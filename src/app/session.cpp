#include "session.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QQueue>
#include <QtConcurrent>

#include "indexer.h"
#include "python.h"
#include "report.h"

using namespace Unravel;

namespace {
Session *g_session = nullptr;
}

Session::Session(QObject *parent)
    : QObject(parent)
{
    if (!g_session)
        g_session = this;
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, &Session::indexingFinished);
}

Session::~Session()
{
    m_cancel = true;
    m_watcher.waitForFinished();
    if (g_session == this)
        g_session = nullptr;
}

Session *Session::instance()
{
    if (!g_session)
        g_session = new Session(qApp);
    return g_session;
}

QVariantMap Session::queryRelations(int symbol, const QString &query, int depth, bool crossModuleOnly) const
{
    const QStringList supported{QStringLiteral("callers"), QStringLiteral("callees"), QStringLiteral("subclasses"),
        QStringLiteral("overrides"), QStringLiteral("readers"), QStringLiteral("writers"), QStringLiteral("dependencies")};
    if (!m_model || symbol < 0 || symbol >= m_model->symbols.size())
        return {{QStringLiteral("error"), tr("Select a symbol")}};
    if (!supported.contains(query) || depth < 1 || depth > 12)
        return {{QStringLiteral("error"), tr("Unsupported relationship query or depth")}};
    const auto &m = *m_model;
    QSet<qint32> seen{symbol};
    QQueue<QPair<qint32, int>> queue;
    queue.enqueue({symbol, 0});
    QVariantList rows;
    bool truncated = false;
    while (!queue.isEmpty() && !truncated) {
        const auto [current, distance] = queue.dequeue();
        if (distance >= depth)
            continue;
        QVector<QPair<qint32, qint32>> neighbours;
        if (query == QLatin1String("callers") || query == QLatin1String("callees")) {
            const auto calls = query == QLatin1String("callers") ? Analysis::callersOf(m, current, true)
                                                                : Analysis::callsFrom(m, current, true);
            for (const auto &call : calls)
                neighbours.append({call.callee, call.ref});
        } else if (query == QLatin1String("readers") || query == QLatin1String("writers")) {
            for (qint32 ref : m.refsTo[current]) {
                const auto &r = m.refs[ref];
                if (r.from >= 0 && r.kind == RefKind::VarUse
                    && (r.flags & (query == QLatin1String("readers") ? RefRead : RefWrite)))
                    neighbours.append({r.from, ref});
            }
        } else {
            const bool outgoing = query == QLatin1String("dependencies");
            const auto &edges = outgoing ? m.outEdges[current] : m.inEdges[current];
            for (qint32 id : edges) {
                const auto &edge = m.edges[id];
                if ((query == QLatin1String("subclasses") && edge.kind == RefKind::Inherit)
                    || (query == QLatin1String("overrides") && edge.kind == RefKind::Override) || outgoing)
                    neighbours.append({outgoing ? edge.to : edge.from, edge.firstRef});
            }
        }
        for (const auto &[next, ref] : neighbours) {
            if (next < 0 || seen.contains(next) || !m.isProjectSymbol(next))
                continue;
            seen.insert(next);
            if (seen.size() > 1000) {
                truncated = true;
                break;
            }
            queue.enqueue({next, distance + 1});
            const auto from = m.symbols[symbol].where(), to = m.symbols[next].where();
            if (crossModuleOnly && from.valid() && to.valid() && m.files[from.file].module == m.files[to.file].module)
                continue;
            rows.append(QVariantMap{{QStringLiteral("symbol"), next}, {QStringLiteral("name"), m.displayName(next)},
                {QStringLiteral("ref"), ref}, {QStringLiteral("distance"), distance + 1},
                {QStringLiteral("location"), ref >= 0 ? m.locationText(m.refs[ref].loc) : m.locationText(to)}});
        }
    }
    return {{QStringLiteral("rows"), rows}, {QStringLiteral("truncated"), truncated}, {QStringLiteral("error"), QString()}};
}

QVariantList Session::eventConnections() const
{
    QVariantList rows;
    if (!m_model)
        return rows;
    for (const auto &event : m_model->eventConnections) {
        rows.append(QVariantMap{
            {QStringLiteral("signal"), event.signal}, {QStringLiteral("target"), event.target},
            {QStringLiteral("signalName"), event.signal >= 0 ? m_model->displayName(event.signal) : tr("Unresolved signal")},
            {QStringLiteral("targetName"), event.target >= 0 ? m_model->displayName(event.target) : tr("Functor or unresolved target")},
            {QStringLiteral("sender"), event.sender}, {QStringLiteral("receiver"), event.receiver},
            {QStringLiteral("delivery"), event.delivery}, {QStringLiteral("limitation"), event.limitation},
            {QStringLiteral("file"), event.loc.file}, {QStringLiteral("line"), event.loc.line},
            {QStringLiteral("targetFile"), event.targetLoc.file}, {QStringLiteral("targetLine"), event.targetLoc.line}});
    }
    return rows;
}

QString Session::toLocalPath(const QString &pathOrUrl) const
{
    if (pathOrUrl.startsWith(QLatin1String("file:")))
        return QUrl(pathOrUrl).toLocalFile();
    return pathOrUrl;
}

QString Session::projectName() const
{
    if (m_model)
        return QFileInfo(m_model->root).fileName();
    return QFileInfo(m_projectPath).fileName();
}

QString Session::root() const
{
    return m_model ? m_model->root : QString();
}

QVariantMap Session::stats() const
{
    QVariantMap out;
    if (!m_model)
        return out;
    const Stats st = m_model->stats();
    out[QStringLiteral("files")] = st.projectFiles;
    out[QStringLiteral("allFiles")] = st.files;
    out[QStringLiteral("units")] = st.units;
    out[QStringLiteral("symbols")] = st.symbols;
    out[QStringLiteral("types")] = st.types;
    out[QStringLiteral("functions")] = st.functions;
    out[QStringLiteral("variables")] = st.variables;
    out[QStringLiteral("references")] = qint64(st.references);
    out[QStringLiteral("lines")] = qint64(st.lines);
    out[QStringLiteral("codeLines")] = qint64(st.codeLines);
    out[QStringLiteral("errors")] = st.errors;
    out[QStringLiteral("warnings")] = st.warnings;
    out[QStringLiteral("indexMs")] = qint64(st.indexMs);
    out[QStringLiteral("edges")] = qint64(m_model->edges.size());
    out[QStringLiteral("modules")] = qint64(m_model->modules.size());
    out[QStringLiteral("indexedAt")] = QDateTime::fromMSecsSinceEpoch(m_model->indexedAt);
    out[QStringLiteral("serious")] = m_report.serious;
    out[QStringLiteral("findingWarnings")] = m_report.warnings;
    out[QStringLiteral("findingNotes")] = m_report.notes;
    out[QStringLiteral("findings")] = qint64(m_report.findings.size());
    return out;
}

// --- Settings ----------------------------------------------------------------

QString Session::settingsKey() const
{
    const QByteArray h = QCryptographicHash::hash(m_projectPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("projects/") + QString::fromLatin1(h);
}

QString Session::indexFile() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QByteArray h = QCryptographicHash::hash(m_projectPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(20);
    return dir + QStringLiteral("/indexes/") + QString::fromLatin1(h) + QStringLiteral(".idx");
}

QString Session::cachePath() const
{
    return indexFile();
}

QStringList Session::recentProjects() const
{
    QStringList out;
    const QStringList list = QSettings().value(QStringLiteral("recent")).toStringList();
    for (const QString &p : list) {
        if (QFileInfo::exists(p))
            out << p;
    }
    return out;
}

void Session::addRecent(const QString &path)
{
    QSettings s;
    QStringList list = s.value(QStringLiteral("recent")).toStringList();
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > 12)
        list.removeLast();
    s.setValue(QStringLiteral("recent"), list);
    emit recentChanged();
}

void Session::forgetRecent(const QString &path)
{
    QSettings s;
    QStringList list = s.value(QStringLiteral("recent")).toStringList();
    list.removeAll(path);
    s.setValue(QStringLiteral("recent"), list);
    emit recentChanged();
}

void Session::setExtraArgs(const QString &args)
{
    if (args == m_extraArgs)
        return;
    m_extraArgs = args;
    if (!m_projectPath.isEmpty())
        QSettings().setValue(settingsKey() + QStringLiteral("/extraArgs"), args);
    emit projectChanged();
}

QString Session::editorCommand() const
{
    return QSettings().value(QStringLiteral("editorCommand"), QStringLiteral("code -g {file}:{line}")).toString();
}

void Session::setEditorCommand(const QString &command)
{
    QSettings().setValue(QStringLiteral("editorCommand"), command);
    emit editorCommandChanged();
}

// --- Opening and indexing ----------------------------------------------------

void Session::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit stateChanged();
}

void Session::setProgress(qreal p, const QString &text)
{
    m_progress = p;
    m_progressText = text;
    emit progressChanged();
}

void Session::open(const QString &pathOrUrl)
{
    QString path = QDir::cleanPath(QFileInfo(toLocalPath(pathOrUrl)).absoluteFilePath());
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        m_error = tr("%1 doesn't exist").arg(path);
        emit projectChanged();
        return;
    }
    cancel();
    m_watcher.waitForFinished();
    m_projectPath = path;
    m_error.clear();
    m_notes.clear();
    m_extraArgs = QSettings().value(settingsKey() + QStringLiteral("/extraArgs")).toString();
    m_buildDir = QSettings().value(settingsKey() + QStringLiteral("/buildDir")).toString();
    if (!m_buildDir.isEmpty() && !QFileInfo::exists(m_buildDir + QStringLiteral("/compile_commands.json")))
        m_buildDir.clear();
    m_back.clear();
    m_forward.clear();
    m_active = Place();
    addRecent(path);
    loadBookmarks();
    m_folded = QSettings().value(settingsKey() + QStringLiteral("/folded")).toStringList();
    emit foldedChanged();
    emit projectChanged();
    emit activeChanged();
    startIndexing(true);
}

void Session::reindex()
{
    if (m_projectPath.isEmpty() || m_state == Indexing || m_state == Discovering)
        return;
    startIndexing(false);
}

void Session::stopCMake()
{
    if (!m_cmake)
        return;
    // Its finished signal must not reach us any more
    disconnect(m_cmake, nullptr, this, nullptr);
    m_cmake->kill();
    m_cmake->waitForFinished(2000);
    m_cmake->deleteLater();
    m_cmake = nullptr;
}

void Session::cancel()
{
    m_cancel = true;
    ++m_generation;
    if (m_cmake) {
        stopCMake();
        setState(m_model ? Ready : Idle);
        setProgress(0, QString());
    }
}

void Session::close()
{
    cancel();
    m_watcher.waitForFinished();
    m_model.reset();
    m_report = {};
    m_projectPath.clear();
    m_active = Place();
    m_back.clear();
    m_forward.clear();
    m_revision++;
    setState(Idle);
    emit projectChanged();
    emit modelChanged();
    emit activeChanged();
}

bool Session::isStale(const CodeModel &m)
{
    for (const File &f : m.files) {
        if (!f.project)
            continue;
        const QFileInfo fi(f.path);
        if (!fi.exists() || fi.lastModified().toMSecsSinceEpoch() > m.indexedAt)
            return true;
    }
    // New sources
    QStringList now = Project::sourceFiles(m.root, true);
    if (!Python::interpreter().isEmpty())
        now += Python::sourceFiles(m.root);
    for (const QString &p : now) {
        if (m.fileId(p) < 0)
            return true;
    }
    return false;
}

void Session::startIndexing(bool useCache)
{
    stopCMake();
    m_cancel = false;
    const quint64 generation = ++m_generation;
    setState(Discovering);
    const bool cache = useCache && QFileInfo::exists(indexFile());
    setProgress(0, cache ? tr("Loading the index…") : tr("Looking for a compilation database…"));

    const QFileInfo fi(m_projectPath);
    // A CMake project without a compilation database: let CMake write one
    if (!cache && fi.isDir() && m_buildDir.isEmpty() && Project::findDatabase(m_projectPath).isEmpty()
        && QFileInfo::exists(m_projectPath + QStringLiteral("/CMakeLists.txt"))
        && !QStandardPaths::findExecutable(QStringLiteral("cmake")).isEmpty()) {
        runCMake(m_projectPath);
        return;
    }

    // Loading, indexing, saving and analysing all happen on a worker
    const QString path = m_projectPath;
    const QString buildDir = m_buildDir == QLatin1String("-") ? QString() : m_buildDir;
    const QString indexPath = indexFile();
    const QStringList extra = QProcess::splitCommand(m_extraArgs);
    QPointer<Session> self(this);
    m_watcher.setFuture(QtConcurrent::run([this, self, path, buildDir, indexPath, extra, cache, generation]() -> Result {
        Result r;
        r.generation = generation;
        r.model = std::make_shared<CodeModel>();
        if (cache) {
            if (r.model->load(indexPath)) {
                r.cached = true;
                r.result.ok = true;
                r.report = Analysis::analyze(*r.model);
                r.stale = isStale(*r.model);
                return r;
            }
            r.loadFailed = true;
            return r;
        }
        r.setup = Project::discover(path, extra, buildDir);
        if (!r.setup.error.isEmpty() || (r.setup.units.isEmpty() && r.setup.pythonFiles.isEmpty())) {
            r.result.error = r.setup.error.isEmpty() ? QStringLiteral("Nothing to index") : r.setup.error;
            return r;
        }
        const int units = int(r.setup.units.size()), modules = int(r.setup.pythonFiles.size());
        QMetaObject::invokeMethod(this, [this, self, units, modules, generation] {
            if (self && generation == m_generation) {
                setState(Indexing);
                QStringList what;
                if (units > 0)
                    what << tr("%1 translation units").arg(units);
                if (modules > 0)
                    what << tr("%1 Python files").arg(modules);
                setProgress(0, tr("Indexing %1…").arg(what.join(tr(" and "))));
            }
        }, Qt::QueuedConnection);
        r.result = Indexer::run(r.setup, *r.model, &m_cancel, [this, self, generation](int done, int all, const QString &file) {
            if (!self)
                return;
            const QString name = QFileInfo(file).fileName();
            QMetaObject::invokeMethod(this, [this, done, all, name, generation] {
                if (generation == m_generation)
                    setProgress(all ? qreal(done) / all : 1, tr("%1 of %2 · %3").arg(done).arg(all).arg(name));
            }, Qt::QueuedConnection);
        });
        if (r.result.ok) {
            r.model->save(indexPath);
            r.report = Analysis::analyze(*r.model);
        }
        return r;
    }));
}

void Session::runCMake(const QString &root)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/cmake/")
                      + QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    QDir().mkpath(dir);
    setProgress(0, tr("Asking CMake for a compilation database…"));
    auto *cmake = new QProcess(this);
    m_cmake = cmake;
    cmake->setProcessChannelMode(QProcess::MergedChannels);
    QStringList args = {QStringLiteral("-S"), root, QStringLiteral("-B"), dir,
                        QStringLiteral("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"), QStringLiteral("-DBUILD_TESTING=OFF")};
    // So projects using Qt find the Qt unravel was built with
    args << QStringLiteral("-DCMAKE_PREFIX_PATH=") + QLibraryInfo::path(QLibraryInfo::PrefixPath);
    if (!QStandardPaths::findExecutable(QStringLiteral("ninja")).isEmpty())
        args << QStringLiteral("-G") << QStringLiteral("Ninja");
    const quint64 generation = m_generation;
    connect(cmake, &QProcess::finished, this, [this, cmake, dir, generation](int code, QProcess::ExitStatus status) {
        // The process that finished, not whatever m_cmake is by now
        const QString output = QString::fromLocal8Bit(cmake->readAll());
        cmake->deleteLater();
        if (cmake != m_cmake || generation != m_generation)
            return;
        m_cmake = nullptr;
        if (status == QProcess::NormalExit && code == 0 && QFileInfo::exists(dir + QStringLiteral("/compile_commands.json"))) {
            m_buildDir = dir;
            QSettings().setValue(settingsKey() + QStringLiteral("/buildDir"), dir);
            m_notes << tr("CMake wrote a compilation database in %1").arg(dir);
        } else {
            const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            m_notes << tr("CMake could not configure the project; guessing the flags instead. %1")
                           .arg(lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join(QLatin1Char(' ')));
            m_buildDir = QStringLiteral("-");   // don't try again this time
        }
        emit projectChanged();
        startIndexing(false);
    });
    cmake->start(QStringLiteral("cmake"), args);
}

void Session::indexingFinished()
{
    Result r = m_watcher.result();
    if (r.generation != m_generation)
        return;   // for a project or a run that is gone
    if (m_buildDir == QLatin1String("-"))
        m_buildDir.clear();
    m_notes << r.setup.notes;
    if (r.loadFailed) {
        startIndexing(false);
        return;
    }
    if (r.result.cancelled) {
        m_notes << tr("Indexing was stopped");
        setState(m_model ? Ready : Idle);
        setProgress(0, QString());
        emit projectChanged();
        return;
    }
    if (!r.result.ok) {
        m_error = r.result.error;
        setState(m_model ? Ready : Idle);
        emit projectChanged();
        return;
    }
    if (r.result.failedUnits > 0)
        m_notes << tr("clang could not parse %1 of %2 translation units").arg(r.result.failedUnits).arg(r.result.units);
    if (r.cached)
        m_notes << tr("Loaded the index from %1").arg(QDateTime::fromMSecsSinceEpoch(r.model->indexedAt).toString());
    adopt(r.model, std::move(r.report));
    if (m_stale != r.stale) {
        m_stale = r.stale;
        emit staleChanged();
    }
    setProgress(1, QString());
    setState(Ready);
    emit projectChanged();
    if (!r.cached)
        emit message(tr("Indexed %1 files in %2 s").arg(m_model->stats().projectFiles).arg(m_model->indexMs / 1000.0, 0, 'f', 1));
}

void Session::adopt(std::shared_ptr<CodeModel> model, Analysis::Report report)
{
    const QString activeUsr = m_model && m_active.symbol >= 0 ? m_model->symbols[m_active.symbol].usr : QString();
    m_report = std::move(report);
    m_model = model;
    m_revision++;
    // Keep the place across a re-index
    m_back.clear();
    m_forward.clear();
    if (!activeUsr.isEmpty())
        m_active.symbol = m_model->symbolId(activeUsr);
    if (m_active.symbol < 0 && m_active.file >= m_model->files.size())
        m_active = Place();
    emit modelChanged();
    emit activeChanged();
    emit bookmarksChanged();
    emit activeBookmarkedChanged();
    if (m_active.symbol < 0 && m_active.file < 0) {
        const int start = startSymbol();
        if (start >= 0)
            go({start, -1, 0}, false);
    }
}

// --- Navigation ----------------------------------------------------------------

void Session::go(const Place &place, bool record)
{
    if (place == m_active)
        return;
    if (record && (m_active.symbol >= 0 || m_active.file >= 0)) {
        m_back.append(m_active);
        if (m_back.size() > 200)
            m_back.removeFirst();
        m_forward.clear();
    }
    m_active = place;
    emit activeChanged();
    emit activeBookmarkedChanged();
}

void Session::activate(int symbol)
{
    if (!m_model || symbol < 0 || symbol >= m_model->symbols.size())
        return;
    go({symbol, -1, 0}, true);
}

void Session::activateFile(int file, int line)
{
    if (!m_model || file < 0 || file >= m_model->files.size())
        return;
    go({-1, file, line}, true);
}

void Session::activateReference(int ref)
{
    if (!m_model || ref < 0 || ref >= m_model->refs.size())
        return;
    const Reference &r = m_model->refs[ref];
    go({-1, r.loc.file, r.loc.line}, true);
}

void Session::back()
{
    if (m_back.isEmpty())
        return;
    m_forward.append(m_active);
    m_active = m_back.takeLast();
    emit activeChanged();
    emit activeBookmarkedChanged();
}

void Session::forward()
{
    if (m_forward.isEmpty())
        return;
    m_back.append(m_active);
    m_active = m_forward.takeLast();
    emit activeChanged();
    emit activeBookmarkedChanged();
}

QVariantList Session::history() const
{
    QVariantList out;
    if (!m_model)
        return out;
    for (qsizetype i = m_back.size() - 1; i >= 0 && out.size() < 30; --i) {
        const Place &p = m_back[i];
        QVariantMap e;
        e[QStringLiteral("symbol")] = p.symbol;
        e[QStringLiteral("file")] = p.file;
        e[QStringLiteral("name")] = p.symbol >= 0 ? m_model->displayName(p.symbol) : m_model->files[p.file].relative;
        out << e;
    }
    return out;
}

int Session::startSymbol() const
{
    if (!m_model)
        return -1;
    int best = -1, bestScore = -1;
    for (int i = 0; i < m_model->symbols.size(); ++i) {
        const Symbol &s = m_model->symbols[i];
        if (s.has(External) || s.has(Generated))
            continue;
        if (s.has(EntryPoint) && s.has(Defined))
            return i;
        if (isType(s.kind) && s.kind != SymbolKind::Typedef) {
            const int score = s.fanIn + s.fanOut + int(s.children.size());
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
    }
    return best;
}

// --- Bookmarks -------------------------------------------------------------------

void Session::loadBookmarks()
{
    m_bookmarkUsrs = QSettings().value(settingsKey() + QStringLiteral("/bookmarks")).toStringList();
    emit bookmarksChanged();
}

void Session::saveBookmarks() const
{
    QSettings().setValue(settingsKey() + QStringLiteral("/bookmarks"), m_bookmarkUsrs);
}

QVariantList Session::bookmarks() const
{
    QVariantList out;
    if (!m_model)
        return out;
    for (const QString &usr : m_bookmarkUsrs) {
        const int id = m_model->symbolId(usr);
        if (id < 0)
            continue;
        const Symbol &s = m_model->symbols[id];
        QVariantMap e;
        e[QStringLiteral("id")] = id;
        e[QStringLiteral("qualified")] = m_model->displayName(id);
        e[QStringLiteral("kind")] = int(s.kind);
        e[QStringLiteral("location")] = m_model->locationText(s.where());
        out << e;
    }
    return out;
}

bool Session::activeBookmarked() const
{
    if (!m_model || m_active.symbol < 0)
        return false;
    return m_bookmarkUsrs.contains(m_model->symbols[m_active.symbol].usr);
}

void Session::toggleBookmark()
{
    if (!m_model || m_active.symbol < 0)
        return;
    const QString usr = m_model->symbols[m_active.symbol].usr;
    if (m_bookmarkUsrs.contains(usr))
        m_bookmarkUsrs.removeAll(usr);
    else
        m_bookmarkUsrs.append(usr);
    saveBookmarks();
    emit bookmarksChanged();
    emit activeBookmarkedChanged();
}

void Session::removeBookmark(int index)
{
    const QVariantList list = bookmarks();
    if (index < 0 || index >= list.size())
        return;
    m_bookmarkUsrs.removeAll(m_model->symbols[list[index].toMap().value(QStringLiteral("id")).toInt()].usr);
    saveBookmarks();
    emit bookmarksChanged();
}

// --- Facts ---------------------------------------------------------------------

QString Session::kindName(int kind) const
{
    return Unravel::kindName(SymbolKind(kind));
}

QVariantMap Session::symbolInfo(int id) const
{
    QVariantMap out;
    if (!m_model || id < 0 || id >= m_model->symbols.size())
        return out;
    const CodeModel &m = *m_model;
    const Symbol &s = m.symbols[id];
    out[QStringLiteral("id")] = id;
    out[QStringLiteral("name")] = s.name;
    out[QStringLiteral("qualified")] = m.displayName(id);
    out[QStringLiteral("signature")] = s.signature;
    out[QStringLiteral("type")] = s.type;
    out[QStringLiteral("kind")] = int(s.kind);
    out[QStringLiteral("kindName")] = Unravel::kindName(s.kind);
    out[QStringLiteral("access")] = int(s.access);
    out[QStringLiteral("external")] = s.has(External);
    out[QStringLiteral("defined")] = s.has(Defined);
    out[QStringLiteral("virtual")] = s.has(Virtual);
    out[QStringLiteral("pure")] = s.has(PureVirtual);
    out[QStringLiteral("static")] = s.has(Static);
    out[QStringLiteral("const")] = s.has(Const);
    out[QStringLiteral("template")] = s.has(Template);
    out[QStringLiteral("abstract")] = s.has(Abstract);
    out[QStringLiteral("implicit")] = s.has(Implicit);
    out[QStringLiteral("fromMacro")] = s.has(FromMacro);
    out[QStringLiteral("entryPoint")] = s.has(EntryPoint);
    out[QStringLiteral("isType")] = isType(s.kind);
    out[QStringLiteral("isFunction")] = isFunction(s.kind);
    out[QStringLiteral("isVariable")] = isVariable(s.kind);
    out[QStringLiteral("parent")] = s.parent;
    out[QStringLiteral("parentName")] = s.parent >= 0 ? m.displayName(s.parent) : QString();
    const Location w = s.where();
    out[QStringLiteral("file")] = w.file;
    out[QStringLiteral("line")] = w.line;
    out[QStringLiteral("location")] = m.locationText(w);
    out[QStringLiteral("declLocation")] = m.locationText(s.decl);
    out[QStringLiteral("defLocation")] = m.locationText(s.def);
    out[QStringLiteral("lines")] = s.metrics.lines;
    out[QStringLiteral("complexity")] = s.metrics.complexity;
    out[QStringLiteral("cognitive")] = s.metrics.cognitive;
    out[QStringLiteral("nesting")] = s.metrics.nesting;
    out[QStringLiteral("params")] = s.metrics.params;
    out[QStringLiteral("statements")] = s.metrics.statements;
    out[QStringLiteral("fanIn")] = s.fanIn;
    if (isFunction(s.kind)) {
        out[QStringLiteral("callers")] = qint64(Analysis::callersOf(m, id).size());
        out[QStringLiteral("callees")] = qint64(Analysis::callsFrom(m, id).size());
    }
    out[QStringLiteral("fanOut")] = s.fanOut;
    out[QStringLiteral("references")] = qint64(m.refsTo[id].size());
    out[QStringLiteral("children")] = qint64(s.children.size());
    out[QStringLiteral("unused")] = Analysis::looksUnused(m, id);
    out[QStringLiteral("bookmarked")] = m_bookmarkUsrs.contains(s.usr);
    // Findings that name it
    QStringList findings;
    for (const Analysis::Finding &f : m_report.findings) {
        if (f.symbols.contains(id))
            findings << Analysis::findingKindName(f.kind) + QStringLiteral(": ") + f.detail;
    }
    out[QStringLiteral("findings")] = findings;
    return out;
}

QVariantMap Session::fileInfo(int file) const
{
    QVariantMap out;
    if (!m_model || file < 0 || file >= m_model->files.size())
        return out;
    const File &f = m_model->files[file];
    out[QStringLiteral("id")] = file;
    out[QStringLiteral("path")] = f.path;
    out[QStringLiteral("relative")] = f.relative;
    out[QStringLiteral("name")] = QFileInfo(f.path).fileName();
    out[QStringLiteral("module")] = f.module;
    out[QStringLiteral("project")] = f.project;
    out[QStringLiteral("generated")] = f.generated;
    out[QStringLiteral("header")] = f.header;
    out[QStringLiteral("indexed")] = f.indexed;
    out[QStringLiteral("lines")] = f.lines;
    out[QStringLiteral("codeLines")] = f.codeLines;
    out[QStringLiteral("includes")] = qint64(f.includes.size());
    out[QStringLiteral("includedBy")] = qint64(f.includedBy.size());
    out[QStringLiteral("symbols")] = qint64(f.symbols.size());
    int errors = 0;
    for (const Diagnostic &d : m_model->diagnostics) {
        if (d.loc.file == file && d.severity >= Diagnostic::Error)
            errors++;
    }
    out[QStringLiteral("errors")] = errors;
    return out;
}

QVariantMap Session::referenceInfo(int ref) const
{
    QVariantMap out;
    if (!m_model || ref < 0 || ref >= m_model->refs.size())
        return out;
    const Reference &r = m_model->refs[ref];
    out[QStringLiteral("id")] = ref;
    out[QStringLiteral("from")] = r.from;
    out[QStringLiteral("to")] = r.to;
    out[QStringLiteral("fromName")] = r.from >= 0 ? m_model->displayName(r.from) : QStringLiteral("(file level)");
    out[QStringLiteral("toName")] = r.to >= 0 ? m_model->displayName(r.to) : QString();
    out[QStringLiteral("kind")] = int(r.kind);
    out[QStringLiteral("kindName")] = refKindName(r.kind);
    if (r.kind == RefKind::Call && (r.flags & RefAddressOf))
        out[QStringLiteral("kindName")] = tr("Function address (not execution)");
    else if (r.kind == RefKind::Call && (r.flags & RefDynamic))
        out[QStringLiteral("kindName")] = tr("Possible virtual call");
    out[QStringLiteral("file")] = r.loc.file;
    out[QStringLiteral("line")] = r.loc.line;
    out[QStringLiteral("location")] = m_model->locationText(r.loc);
    out[QStringLiteral("write")] = bool(r.flags & RefWrite);
    out[QStringLiteral("implicit")] = bool(r.flags & RefImplicit);
    out[QStringLiteral("addressOf")] = bool(r.flags & RefAddressOf);
    out[QStringLiteral("dynamic")] = bool(r.flags & RefDynamic);
    return out;
}

QVariantList Session::references(int symbol, int limit) const
{
    QVariantList out;
    if (!m_model || symbol < 0 || symbol >= m_model->symbols.size())
        return out;
    for (qint32 r : m_model->refsTo[symbol]) {
        if (out.size() >= limit)
            break;
        out << referenceInfo(r);
    }
    return out;
}

int Session::symbolByName(const QString &qualified) const
{
    if (!m_model)
        return -1;
    for (int i = 0; i < m_model->symbols.size(); ++i) {
        if (m_model->symbols[i].qualified == qualified && !m_model->symbols[i].has(External))
            return i;
    }
    return -1;
}

QVariantList Session::moduleMetrics() const
{
    QVariantList out;
    if (!m_model)
        return out;
    for (const Analysis::ModuleMetrics &mm : Analysis::moduleMetrics(*m_model)) {
        QVariantMap e;
        e[QStringLiteral("name")] = mm.name;
        e[QStringLiteral("files")] = mm.files;
        e[QStringLiteral("lines")] = mm.lines;
        e[QStringLiteral("classes")] = mm.classes;
        e[QStringLiteral("functions")] = mm.functions;
        e[QStringLiteral("afferent")] = mm.afferent;
        e[QStringLiteral("efferent")] = mm.efferent;
        e[QStringLiteral("instability")] = mm.instability;
        e[QStringLiteral("abstractness")] = mm.abstractness;
        e[QStringLiteral("distance")] = mm.distance;
        e[QStringLiteral("complexity")] = mm.complexity;
        out << e;
    }
    return out;
}

QVariantMap Session::classMetrics(int symbol) const
{
    QVariantMap e;
    if (!m_model || symbol < 0 || symbol >= m_model->symbols.size())
        return e;
    const Analysis::ClassMetrics c = Analysis::classMetrics(*m_model, symbol);
    e[QStringLiteral("methods")] = c.methods;
    e[QStringLiteral("fields")] = c.fields;
    e[QStringLiteral("publicMethods")] = c.publicMethods;
    e[QStringLiteral("wmc")] = c.wmc;
    e[QStringLiteral("lines")] = c.lines;
    e[QStringLiteral("depth")] = c.depth;
    e[QStringLiteral("children")] = c.children;
    e[QStringLiteral("coupling")] = c.coupling;
    e[QStringLiteral("usedBy")] = c.usedBy;
    e[QStringLiteral("cohesion")] = c.cohesion;
    return e;
}

QString Session::reportMarkdown() const
{
    if (!m_model)
        return QString();
    return Reporting::markdown(*m_model, m_report);
}

bool Session::saveReport(const QString &pathOrUrl) const
{
    QFile f(toLocalPath(pathOrUrl));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(reportMarkdown().toUtf8());
    return true;
}

QVariantMap Session::activeInfo() const
{
    if (m_active.symbol >= 0)
        return symbolInfo(m_active.symbol);
    QVariantMap out = fileInfo(m_active.file);
    if (!out.isEmpty()) {
        out[QStringLiteral("isFile")] = true;
        out[QStringLiteral("line")] = m_active.line;   // where to show it
        out[QStringLiteral("file")] = m_active.file;
    }
    return out;
}

void Session::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

void Session::openInEditor(int file, int line) const
{
    if (!m_model || file < 0 || file >= m_model->files.size())
        return;
    const QString path = m_model->files[file].path;
    QString command = editorCommand().trimmed();
    if (command.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    QStringList parts = QProcess::splitCommand(command);
    for (QString &p : parts) {
        p.replace(QStringLiteral("{file}"), path);
        p.replace(QStringLiteral("{line}"), QString::number(std::max(1, line)));
    }
    const QString program = parts.takeFirst();
    if (!QProcess::startDetached(program, parts))
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Session::openFolder(int file) const
{
    if (!m_model || file < 0 || file >= m_model->files.size())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_model->files[file].path).absolutePath()));
}

QVariantList Session::diagnostics(int limit) const
{
    QVariantList out;
    if (!m_model)
        return out;
    for (const Diagnostic &d : m_model->diagnostics) {
        if (out.size() >= limit)
            break;
        QVariantMap e;
        e[QStringLiteral("severity")] = int(d.severity);
        e[QStringLiteral("message")] = d.message;
        e[QStringLiteral("category")] = d.category;
        e[QStringLiteral("file")] = d.loc.file;
        e[QStringLiteral("line")] = d.loc.line;
        e[QStringLiteral("location")] = m_model->locationText(d.loc);
        e[QStringLiteral("unit")] = d.unit >= 0 ? m_model->files[d.unit].relative : QString();
        out << e;
    }
    return out;
}

bool Session::writeText(const QString &pathOrUrl, const QString &text) const
{
    QFile f(toLocalPath(pathOrUrl));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(text.toUtf8()) >= 0;
}

// --- Folded folders ------------------------------------------------------------------

QStringList Session::vendorFolders() const
{
    return m_model ? Analysis::vendorFolders(*m_model) : QStringList();
}

void Session::setFolded(QStringList folders)
{
    folders.removeAll(QString());
    folders.removeDuplicates();
    folders.sort();
    if (folders == m_folded)
        return;
    m_folded = folders;
    if (!m_projectPath.isEmpty())
        QSettings().setValue(settingsKey() + QStringLiteral("/folded"), m_folded);
    emit foldedChanged();
}

void Session::fold(const QString &folder)
{
    // Folding a folder takes in folded folders inside it
    QStringList f;
    for (const QString &x : std::as_const(m_folded)) {
        if (x != folder && !x.startsWith(folder + QLatin1Char('/')))
            f << x;
    }
    f << folder;
    setFolded(f);
}

void Session::unfold(const QString &folder)
{
    QStringList f = m_folded;
    f.removeAll(folder);
    setFolded(f);
}

QString Session::foldOfFile(int file) const
{
    if (!m_model || file < 0 || file >= m_model->files.size() || !m_model->files[file].project)
        return {};
    return Analysis::foldedFolderOf(m_model->files[file].module, m_folded);
}

QStringList Session::foldersOfFile(int file) const
{
    QStringList out;
    if (!m_model || file < 0 || file >= m_model->files.size() || !m_model->files[file].project)
        return out;
    QStringList parts = m_model->files[file].module.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    while (!parts.isEmpty()) {
        out << parts.join(QLatin1Char('/'));
        parts.removeLast();
    }
    return out;
}

void Session::foldVendorFolders()
{
    setFolded(m_folded + vendorFolders());
}

void Session::unfoldAll()
{
    setFolded({});
}

QString Session::namespaceOf(int symbol) const
{
    if (!m_model || symbol < 0 || symbol >= m_model->symbols.size())
        return {};
    for (qint32 p = m_model->symbols[m_model->nodeOwner(symbol)].parent; p >= 0; p = m_model->symbols[p].parent) {
        if (m_model->symbols[p].kind == SymbolKind::Namespace)
            return m_model->displayName(p);
    }
    return {};
}
