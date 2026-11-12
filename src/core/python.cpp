#include "python.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "project.h"
#include "unravel_python_script.h"   // kPythonScript, made from python/unravel_python.py

namespace Unravel::Python {

QString interpreter()
{
    for (const char *name : {"python3", "python"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty())
            return found;
    }
    return {};
}

namespace {

bool skip(const QFileInfo &dir)
{
    const QString name = dir.fileName();
    static const QStringList names = {"venv", "env", "site-packages", "__pycache__", "node_modules", "build", "dist",
                                      "_build", "htmlcov", "CMakeFiles"};
    if (name.startsWith(QLatin1Char('.')) || names.contains(name) || name.endsWith(QLatin1String(".egg-info")))
        return true;
    // A virtual environment, whatever it's called
    return QFileInfo::exists(dir.filePath() + QStringLiteral("/pyvenv.cfg"))
        || QFileInfo::exists(dir.filePath() + QStringLiteral("/CMakeCache.txt"));
}

void collect(const QString &dir, QStringList &out, int depth)
{
    if (depth > 24)
        return;
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                          QDir::Name);
    for (const QFileInfo &fi : entries) {
        if (fi.isDir()) {
            if (!skip(fi))
                collect(fi.filePath(), out, depth + 1);
        } else if (fi.suffix() == QLatin1String("py") || fi.suffix() == QLatin1String("pyi")) {
            out << QDir::cleanPath(fi.absoluteFilePath());
        }
    }
}

Location location(const QVector<qint32> &files, const QJsonObject &o, const char *line, const char *col)
{
    Location l;
    const int f = o.value(QStringLiteral("file")).toInt(-1);
    l.file = f >= 0 && f < files.size() ? files[f] : -1;
    l.line = o.value(QLatin1String(line)).toInt();
    l.col = o.value(QLatin1String(col)).toInt();
    return l;
}

} // namespace

QStringList sourceFiles(const QString &root)
{
    QStringList out;
    collect(root, out, 0);
    // .pyi stubs only where there is no .py beside them
    QStringList kept;
    for (const QString &f : std::as_const(out)) {
        if (f.endsWith(QLatin1String(".pyi")) && out.contains(f.left(f.size() - 1)))
            continue;
        kept << f;
    }
    return kept;
}

bool run(const QString &root, const QStringList &files, CodeModel &model, QString *error, std::atomic<bool> *cancel,
         QMutex *mutex)
{
    const QString python = interpreter();
    if (python.isEmpty()) {
        if (error)
            *error = QStringLiteral("Python files found, but no python3 to read them with");
        return false;
    }
    // The script in a folder of its own, so nothing of the project is found beside it
    QTemporaryDir dir;
    const QString script = dir.filePath(QStringLiteral("unravel_python.py"));
    const QString out = dir.filePath(QStringLiteral("index.json"));
    {
        QFile f(script);
        if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char *>(kPythonScript)) < 0) {
            if (error)
                *error = QStringLiteral("Could not write the Python indexer to %1").arg(script);
            return false;
        }
    }
    QProcess p;
    p.setWorkingDirectory(dir.path());
    p.start(python, {QStringLiteral("-I"), QStringLiteral("-X"), QStringLiteral("utf8"), script, QStringLiteral("--root"), root,
                     QStringLiteral("--out"), out});
    if (!p.waitForStarted(10000)) {
        if (error)
            *error = QStringLiteral("Could not start %1").arg(python);
        return false;
    }
    p.write(files.join(QLatin1Char('\n')).toUtf8());
    p.closeWriteChannel();
    while (!p.waitForFinished(200)) {
        if (cancel && cancel->load()) {
            p.kill();
            p.waitForFinished();
            return false;
        }
        if (p.state() == QProcess::NotRunning)
            break;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) {
            const QStringList lines = QString::fromLocal8Bit(p.readAllStandardError()).trimmed().split(QLatin1Char('\n'));
            *error = QStringLiteral("The Python indexer failed: %1").arg(lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join(QLatin1Char(' ')));
        }
        return false;
    }
    QFile f(out);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseError;
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("The Python indexer wrote no valid JSON: %1").arg(parseError.errorString());
        return false;
    }

    QMutexLocker lock(mutex);
    const QJsonArray jfiles = doc.value(QStringLiteral("files")).toArray();
    QVector<qint32> fileIds;
    for (const QJsonValue &v : jfiles) {
        const qint32 id = model.addFile(QDir::cleanPath(v.toObject().value(QStringLiteral("path")).toString()));
        model.files[id].indexed = true;   // a unit of its own
        fileIds << id;
    }
    for (qsizetype i = 0; i < jfiles.size(); ++i) {
        for (const QJsonValue &inc : jfiles[i].toObject().value(QStringLiteral("includes")).toArray()) {
            const QJsonObject o = inc.toObject();
            const int target = o.value(QStringLiteral("file")).toInt(-1);
            if (target >= 0 && target < fileIds.size())
                model.files[fileIds[i]].includes.append({fileIds[target], o.value(QStringLiteral("line")).toInt()});
        }
    }
    const QJsonArray jsymbols = doc.value(QStringLiteral("symbols")).toArray();
    QVector<qint32> ids;
    ids.reserve(jsymbols.size());
    for (const QJsonValue &v : jsymbols)
        ids << model.addSymbol(v.toObject().value(QStringLiteral("usr")).toString());
    for (qsizetype i = 0; i < jsymbols.size(); ++i) {
        const QJsonObject o = jsymbols[i].toObject();
        Symbol &s = model.symbols[ids[i]];
        s.name = o.value(QStringLiteral("name")).toString();
        s.kind = SymbolKind(o.value(QStringLiteral("kind")).toInt());
        s.flags = quint32(o.value(QStringLiteral("flags")).toInteger());
        s.access = Access(o.value(QStringLiteral("access")).toInt());
        s.signature = o.value(QStringLiteral("signature")).toString();
        s.type = o.value(QStringLiteral("type")).toString();
        const int parent = o.value(QStringLiteral("parent")).toInt(-1);
        s.parent = parent >= 0 && parent < ids.size() ? ids[parent] : -1;
        s.decl = location(fileIds, o, "line", "col");
        s.decl.endLine = s.decl.line;
        s.decl.endCol = s.decl.col + qint32(s.name.toUtf8().size());
        s.def = s.decl;
        s.def.line = o.value(QStringLiteral("defLine")).toInt(s.decl.line);
        s.def.col = 1;
        s.def.endLine = o.value(QStringLiteral("endLine")).toInt(s.def.line);
        s.def.endCol = o.value(QStringLiteral("endCol")).toInt();
        const QJsonObject mo = o.value(QStringLiteral("metrics")).toObject();
        FunctionMetrics &m = s.metrics;
        m.lines = mo.value(QStringLiteral("lines")).toInt();
        m.params = mo.value(QStringLiteral("params")).toInt();
        m.complexity = mo.value(QStringLiteral("complexity")).toInt();
        m.cognitive = mo.value(QStringLiteral("cognitive")).toInt();
        m.nesting = mo.value(QStringLiteral("nesting")).toInt();
        m.statements = mo.value(QStringLiteral("statements")).toInt();
        m.returns = mo.value(QStringLiteral("returns")).toInt();
        m.tokens = mo.value(QStringLiteral("tokens")).toInt();
        m.cloneHash = mo.value(QStringLiteral("cloneHash")).toString().toULongLong();
        m.minHash.clear();
        for (const QJsonValue &h : mo.value(QStringLiteral("minHash")).toArray())
            m.minHash << quint32(h.toInteger());
        // Where the name is, for the code view's links
        if (s.kind != SymbolKind::Namespace) {
            Reference d;
            d.to = ids[i];
            d.kind = RefKind::Declaration;
            d.flags = RefWrite;
            d.loc = s.decl;
            model.refs.append(d);
        }
    }
    for (const QJsonValue &v : doc.value(QStringLiteral("refs")).toArray()) {
        const QJsonObject o = v.toObject();
        const int from = o.value(QStringLiteral("from")).toInt(-1), to = o.value(QStringLiteral("to")).toInt(-1);
        if (to < 0 || to >= ids.size())
            continue;
        Reference r;
        r.from = from >= 0 && from < ids.size() ? ids[from] : -1;
        r.to = ids[to];
        r.kind = RefKind(o.value(QStringLiteral("kind")).toInt());
        r.flags = quint8(o.value(QStringLiteral("flags")).toInt());
        r.loc = location(fileIds, o, "line", "col");
        r.loc.endLine = r.loc.line;
        r.loc.endCol = o.value(QStringLiteral("endCol")).toInt();
        model.refs.append(r);
    }
    for (const QJsonValue &v : doc.value(QStringLiteral("diagnostics")).toArray()) {
        const QJsonObject o = v.toObject();
        Diagnostic d;
        d.severity = Diagnostic::Severity(o.value(QStringLiteral("severity")).toInt(int(Diagnostic::Error)));
        d.message = o.value(QStringLiteral("message")).toString();
        d.loc = location(fileIds, o, "line", "col");
        d.unit = d.loc.file;
        model.diagnostics.append(d);
    }
    return true;
}

} // namespace Unravel::Python
