#include "history.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>

namespace Unravel::History {

namespace {

bool runGit(const QString &repo, const QStringList &args, QByteArray *out, QString *error = nullptr)
{
    QProcess git;
    git.setProgram(QStringLiteral("git"));
    // Plain, stable output: no pager, colours, external diff tools or quoted paths
    git.setArguments(QStringList{QStringLiteral("-C"), repo, QStringLiteral("-c"), QStringLiteral("core.quotepath=off"),
                                 QStringLiteral("--no-pager")}
                     + args);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    git.setProcessEnvironment(env);
    git.start();
    if (!git.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("git could not be started");
        return false;
    }
    QByteArray data;
    while (!git.waitForFinished(100)) {
        data += git.readAllStandardOutput();
        if (git.state() == QProcess::NotRunning)
            break;
    }
    data += git.readAllStandardOutput();
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0) {
        if (error)
            *error = QString::fromLocal8Bit(git.readAllStandardError()).trimmed();
        return false;
    }
    *out = data;
    return true;
}

// The lines a hunk set touches on its new side; a pure deletion touches
// the line after which it was
void touchedLines(const FileDiff &d, QSet<qint32> &lines, QSet<qint32> &added, qint32 *plus, qint32 *minus)
{
    for (const Hunk &h : d.hunks) {
        *plus += h.newCount;
        *minus += h.oldCount;
        if (h.newCount == 0) {
            lines.insert(std::max(1, h.newStart));
            continue;
        }
        for (qint32 l = h.newStart; l < h.newStart + h.newCount; ++l) {
            lines.insert(l);
            if (h.oldCount == 0)
                added.insert(l);
        }
    }
}

qint32 lineCount(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray data = f.readAll();
    return qint32(data.count('\n') + (data.isEmpty() || data.endsWith('\n') ? 0 : 1));
}

} // namespace

QString repositoryRoot(const QString &dir)
{
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
        return {};
    QByteArray out;
    if (!runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, &out))
        return {};
    return QDir::cleanPath(QString::fromUtf8(out).trimmed());
}

QVector<Commit> log(const QString &repo, int limit, const QString &path)
{
    QVector<Commit> commits;
    QStringList args = {QStringLiteral("log"), QStringLiteral("-n"), QString::number(limit),
                        QStringLiteral("--date=short"),
                        QStringLiteral("--format=%H%x1f%h%x1f%an%x1f%ad%x1f%at%x1f%P%x1f%s%x1e")};
    if (!path.isEmpty())
        args << QStringLiteral("--") << path;
    QByteArray out;
    if (!runGit(repo, args, &out))
        return commits;
    for (const QByteArray &record : out.split('\x1e')) {
        const QList<QByteArray> f = record.trimmed().split('\x1f');
        if (f.size() < 7)
            continue;
        Commit c;
        c.hash = QString::fromUtf8(f[0]);
        c.shortHash = QString::fromUtf8(f[1]);
        c.author = QString::fromUtf8(f[2]);
        c.date = QString::fromUtf8(f[3]);
        c.time = f[4].toLongLong();
        c.parents = QString::fromUtf8(f[5]).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        c.subject = QString::fromUtf8(f[6]);
        commits.append(c);
    }
    return commits;
}

QVector<FileDiff> parseDiff(const QByteArray &diff)
{
    QVector<FileDiff> files;
    FileDiff *cur = nullptr;
    auto path = [](const QByteArray &rest) -> QString {
        // "a/dir/file" or "/dev/null"
        const QByteArray p = rest.trimmed();
        if (p == "/dev/null")
            return {};
        return QString::fromUtf8(p.size() > 2 && (p.startsWith("a/") || p.startsWith("b/")) ? p.mid(2) : p);
    };
    auto number = [](const QByteArray &s, qint32 *start, qint32 *count) {
        const qsizetype comma = s.indexOf(',');
        *start = (comma < 0 ? s : s.left(comma)).toInt();
        *count = comma < 0 ? 1 : s.mid(comma + 1).toInt();
    };
    for (const QByteArray &line : diff.split('\n')) {
        if (line.startsWith("diff --git ")) {
            files.append(FileDiff());
            cur = &files.last();
            // Paths from the header, for diffs without ---/+++ (pure renames, binary)
            const QList<QByteArray> parts = line.mid(11).split(' ');
            if (parts.size() >= 2) {
                cur->oldPath = path(parts.first());
                cur->newPath = path(parts.last());
            }
        } else if (!cur) {
            continue;
        } else if (line.startsWith("--- ")) {
            cur->oldPath = path(line.mid(4));
        } else if (line.startsWith("+++ ")) {
            cur->newPath = path(line.mid(4));
        } else if (line.startsWith("rename to ")) {
            cur->newPath = QString::fromUtf8(line.mid(10));
        } else if (line.startsWith("rename from ")) {
            cur->oldPath = QString::fromUtf8(line.mid(12));
        } else if (line.startsWith("new file mode")) {
            cur->oldPath.clear();
        } else if (line.startsWith("deleted file mode")) {
            cur->newPath.clear();
        } else if (line.startsWith("@@ ")) {
            // @@ -a,b +c,d @@
            const QList<QByteArray> parts = line.split(' ');
            if (parts.size() < 3)
                continue;
            Hunk h;
            number(parts[1].mid(1), &h.oldStart, &h.oldCount);
            number(parts[2].mid(1), &h.newStart, &h.newCount);
            cur->hunks.append(h);
        }
    }
    return files;
}

qint32 mapLine(const QVector<Hunk> &hunks, qint32 line, bool *changedSince)
{
    if (changedSince)
        *changedSince = false;
    qint32 offset = 0;
    for (const Hunk &h : hunks) {
        if (h.oldCount > 0 && line >= h.oldStart && line < h.oldStart + h.oldCount) {
            if (changedSince)
                *changedSince = true;
            return std::max(1, h.newStart);
        }
        const qint32 lastOld = h.oldCount > 0 ? h.oldStart + h.oldCount - 1 : h.oldStart;
        if (lastOld < line)
            offset += h.newCount - h.oldCount;
        else
            break;
    }
    return std::max(1, line + offset);
}

ChangeSet changes(const QString &repo, const QStringList &commits, const QVector<Commit> &known)
{
    ChangeSet set;
    set.commits = commits;
    const QString top = QDir::cleanPath(repo) + QLatin1Char('/');
    for (const QString &hash : commits) {
        QByteArray out;
        QString error;
        if (hash == kWorkingTree) {
            // Staged and unstaged, against the last commit
            if (!runGit(repo, {QStringLiteral("diff"), QStringLiteral("-U0"), QStringLiteral("--no-color"),
                               QStringLiteral("--no-ext-diff"), QStringLiteral("-M"), QStringLiteral("HEAD")}, &out, &error)) {
                set.error = error;
                continue;
            }
            for (const FileDiff &d : parseDiff(out)) {
                if (d.newPath.isEmpty())
                    continue;
                FileChange &fc = set.files[top + d.newPath];
                touchedLines(d, fc.lines, fc.addedLines, &fc.added, &fc.removed);
                fc.newFile = fc.newFile || d.oldPath.isEmpty();
                fc.commits << QStringLiteral("work tree");
            }
            // New files git doesn't know yet
            QByteArray untracked;
            if (runGit(repo, {QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
                              QStringLiteral("-z")}, &untracked)) {
                for (const QByteArray &p : untracked.split('\0')) {
                    if (p.isEmpty())
                        continue;
                    const QString abs = top + QString::fromUtf8(p);
                    FileChange &fc = set.files[abs];
                    const qint32 n = lineCount(abs);
                    for (qint32 l = 1; l <= n; ++l) {
                        fc.lines.insert(l);
                        fc.addedLines.insert(l);
                    }
                    fc.added += n;
                    fc.newFile = true;
                    fc.commits << QStringLiteral("untracked");
                }
            }
            continue;
        }
        // The commit against its first parent (the root commit against nothing)
        QStringList parents;
        for (const Commit &c : known) {
            if (c.hash == hash || c.shortHash == hash)
                parents = c.parents;
        }
        if (parents.isEmpty()) {
            QByteArray p;
            if (runGit(repo, {QStringLiteral("rev-list"), QStringLiteral("--parents"), QStringLiteral("-n1"), hash}, &p))
                parents = QString::fromUtf8(p).trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts).mid(1);
        }
        const QStringList common = {QStringLiteral("-U0"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"),
                                    QStringLiteral("-M")};
        const bool ok = parents.isEmpty()
            ? runGit(repo, QStringList{QStringLiteral("show"), QStringLiteral("--format=")} + common + QStringList{hash}, &out, &error)
            : runGit(repo, QStringList{QStringLiteral("diff")} + common + QStringList{parents.first(), hash}, &out, &error);
        if (!ok) {
            set.error = error;
            continue;
        }
        const QVector<FileDiff> diffs = parseDiff(out);
        // Then from the commit to the work tree, for the files it touched,
        // so its lines are where they are now
        QStringList paths;
        for (const FileDiff &d : diffs) {
            if (!d.newPath.isEmpty())
                paths << d.newPath;
        }
        QHash<QString, FileDiff> since;
        if (!paths.isEmpty()) {
            QByteArray forward;
            if (runGit(repo, QStringList{QStringLiteral("diff")} + common + QStringList{hash, QStringLiteral("--")} + paths, &forward)) {
                for (const FileDiff &d : parseDiff(forward))
                    since.insert(d.oldPath.isEmpty() ? d.newPath : d.oldPath, d);
            }
        }
        const QString shortHash = hash.left(9);
        for (const FileDiff &d : diffs) {
            if (d.newPath.isEmpty())
                continue;   // deleted by the commit: nothing of it is left
            QString now = d.newPath;
            const FileDiff *forward = nullptr;
            const auto it = since.constFind(d.newPath);
            if (it != since.cend()) {
                if (it->newPath.isEmpty())
                    continue;   // deleted since
                now = it->newPath;
                forward = &*it;
            }
            QSet<qint32> lines, added;
            qint32 plus = 0, minus = 0;
            touchedLines(d, lines, added, &plus, &minus);
            FileChange &fc = set.files[top + now];
            for (qint32 l : std::as_const(lines)) {
                const qint32 m = forward ? mapLine(forward->hunks, l) : l;
                fc.lines.insert(m);
                if (added.contains(l))
                    fc.addedLines.insert(m);
            }
            fc.added += plus;
            fc.removed += minus;
            fc.newFile = fc.newFile || d.oldPath.isEmpty();
            if (!fc.commits.contains(shortHash))
                fc.commits << shortHash;
        }
    }
    return set;
}

Mapping touchedSymbols(const CodeModel &m, const ChangeSet &changes)
{
    Mapping out;
    QHash<qint32, Touched> bySymbol;
    QHash<qint32, qint32> extentLines;
    for (auto it = changes.files.cbegin(); it != changes.files.cend(); ++it) {
        const qint32 fid = m.fileId(it.key());
        if (fid < 0) {
            out.outside << it.key();
            continue;
        }
        out.lineCount[fid] = qint32(it->lines.size());
        // What is declared on each line here: fields, variables, macros,
        // declarations of functions defined elsewhere
        QHash<qint32, QVector<qint32>> declaredOn;
        for (qint32 s : m.files[fid].symbols) {
            const Symbol &sym = m.symbols[s];
            if (sym.has(FromMacro) || sym.has(Implicit) || sym.kind == SymbolKind::Namespace)
                continue;
            if (sym.decl.file == fid)
                declaredOn[sym.decl.line].append(s);
            if (sym.def.file == fid && sym.def.line != sym.decl.line && !isFunction(sym.kind) && !isType(sym.kind))
                declaredOn[sym.def.line].append(s);
        }
        for (qint32 line : it->lines) {
            QVector<qint32> hit;
            const qint32 inner = m.innermostAt(fid, line);
            // A declaration on the line, unless it is the start of a definition spanning it
            for (qint32 s : declaredOn.value(line)) {
                if (s != inner)
                    hit.append(s);
            }
            if (hit.isEmpty() && inner >= 0)
                hit.append(inner);
            for (qint32 s : std::as_const(hit)) {
                Touched &t = bySymbol[s];
                t.symbol = s;
                t.lines++;
                if (it->addedLines.contains(line))
                    extentLines[s]++;
            }
        }
    }
    for (auto it = bySymbol.begin(); it != bySymbol.end(); ++it) {
        const Symbol &s = m.symbols[it.key()];
        const qint32 span = s.def.valid() && s.def.endLine >= s.def.line ? s.def.endLine - s.def.line + 1 : 1;
        it->added = extentLines.value(it.key()) >= std::max(1, span * 9 / 10);
        out.symbols.append(*it);
    }
    std::sort(out.symbols.begin(), out.symbols.end(), [&](const Touched &a, const Touched &b) {
        const Location x = m.symbols[a.symbol].where(), y = m.symbols[b.symbol].where();
        return std::tie(x.file, x.line) < std::tie(y.file, y.line);
    });
    return out;
}

} // namespace Unravel::History
