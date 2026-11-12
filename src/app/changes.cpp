#include "changes.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QTextStream>
#include <QtConcurrent>

#include "analysis.h"
#include "session.h"

using namespace Unravel;

namespace {
Changes *g_changes = nullptr;
}

Changes::Changes(QObject *parent)
    : QObject(parent)
{
    if (!g_changes)
        g_changes = this;
    connect(Session::instance(), &Session::modelChanged, this, &Changes::modelChanged);
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, &Changes::computed);
}

Changes *Changes::instance()
{
    if (!g_changes)
        g_changes = new Changes(qApp);
    return g_changes;
}

void Changes::modelChanged()
{
    m_model = Session::instance()->model();
    const QString repo = m_model ? History::repositoryRoot(m_model->root) : QString();
    if (repo != m_repo) {
        m_repo = repo;
        m_selected.clear();
        emit repositoryChanged();
        emit selectionChanged();
    }
    refresh();
    // The ids are the new model's: map the selection again
    compute();
}

void Changes::refresh()
{
    m_log = m_repo.isEmpty() ? QVector<History::Commit>() : History::log(m_repo, 500);
    updateCommitList();
}

void Changes::updateCommitList()
{
    m_commitList.clear();
    if (!m_repo.isEmpty()) {
        QVariantMap work;
        work[QStringLiteral("hash")] = History::kWorkingTree;
        work[QStringLiteral("shortHash")] = QString();
        work[QStringLiteral("subject")] = tr("Uncommitted changes");
        work[QStringLiteral("author")] = QString();
        work[QStringLiteral("date")] = QString();
        work[QStringLiteral("workTree")] = true;
        work[QStringLiteral("selected")] = m_selected.contains(History::kWorkingTree);
        m_commitList << work;
    }
    for (const History::Commit &c : std::as_const(m_log)) {
        QVariantMap e;
        e[QStringLiteral("hash")] = c.hash;
        e[QStringLiteral("shortHash")] = c.shortHash;
        e[QStringLiteral("subject")] = c.subject;
        e[QStringLiteral("author")] = c.author;
        e[QStringLiteral("date")] = c.date;
        e[QStringLiteral("merge")] = c.parents.size() > 1;
        e[QStringLiteral("workTree")] = false;
        e[QStringLiteral("selected")] = m_selected.contains(c.hash);
        m_commitList << e;
    }
    emit commitsChanged();
}

void Changes::setSelection(const QStringList &hashes)
{
    if (hashes == m_selected)
        return;
    m_selected = hashes;
    updateCommitList();
    emit selectionChanged();
    compute();
}

void Changes::select(const QString &hash)
{
    setSelection({hash});
}

void Changes::toggle(const QString &hash)
{
    QStringList s = m_selected;
    if (s.contains(hash))
        s.removeAll(hash);
    else
        s.append(hash);
    setSelection(s);
}

void Changes::selectRange(int fromRow, int toRow)
{
    QStringList s;
    for (int r = std::min(fromRow, toRow); r <= std::max(fromRow, toRow); ++r) {
        if (r >= 0 && r < m_commitList.size())
            s << m_commitList[r].toMap().value(QStringLiteral("hash")).toString();
    }
    setSelection(s);
}

void Changes::selectRevisions(const QStringList &revisions)
{
    QStringList s;
    for (const QString &r : revisions) {
        if (r.compare(QLatin1String("worktree"), Qt::CaseInsensitive) == 0 || r == History::kWorkingTree) {
            s << History::kWorkingTree;
            continue;
        }
        for (const History::Commit &c : std::as_const(m_log)) {
            if (c.hash.startsWith(r)) {
                s << c.hash;
                break;
            }
        }
    }
    setSelection(s);
}

void Changes::clear()
{
    setSelection({});
}

void Changes::compute()
{
    const quint64 generation = ++m_generation;
    if (m_selected.isEmpty() || m_repo.isEmpty() || !m_model) {
        m_touched.clear();
        m_lines.clear();
        m_symbolList.clear();
        m_fileList.clear();
        m_findingList.clear();
        m_summary.clear();
        m_error.clear();
        m_revision++;
        if (m_busy) {
            m_busy = false;
            emit busyChanged();
        }
        emit changesChanged();
        return;
    }
    if (!m_busy) {
        m_busy = true;
        emit busyChanged();
    }
    const QString repo = m_repo;
    const QStringList selected = m_selected;
    const QVector<History::Commit> log = m_log;
    const std::shared_ptr<const CodeModel> model = m_model;
    m_watcher.setFuture(QtConcurrent::run([repo, selected, log, model, generation]() -> Result {
        Result r;
        r.generation = generation;
        r.changes = History::changes(repo, selected, log);
        r.mapping = History::touchedSymbols(*model, r.changes);
        return r;
    }));
}

void Changes::computed()
{
    const Result r = m_watcher.result();
    if (r.generation != m_generation || !m_model)
        return;   // a selection or a model since
    const CodeModel &m = *m_model;
    m_busy = false;
    m_error = r.changes.error;
    m_touched.clear();
    for (const History::Touched &t : r.mapping.symbols)
        m_touched.insert(t.symbol, t);
    m_lines.clear();
    m_fileList.clear();
    qint32 added = 0, removed = 0;
    QStringList paths = r.changes.files.keys();
    paths.sort();
    for (const QString &path : std::as_const(paths)) {
        const History::FileChange &fc = r.changes.files.value(path);
        const qint32 fid = m.fileId(path);
        if (fid >= 0)
            m_lines.insert(fid, fc.lines);
        added += fc.added;
        removed += fc.removed;
        QVariantMap e;
        e[QStringLiteral("path")] = path;
        e[QStringLiteral("relative")] = fid >= 0 ? m.files[fid].relative : QFileInfo(path).fileName();
        e[QStringLiteral("file")] = fid;
        e[QStringLiteral("added")] = fc.added;
        e[QStringLiteral("removed")] = fc.removed;
        e[QStringLiteral("newFile")] = fc.newFile;
        e[QStringLiteral("indexed")] = fid >= 0;
        e[QStringLiteral("commits")] = fc.commits.join(QStringLiteral(", "));
        int symbols = 0;
        for (const History::Touched &t : r.mapping.symbols) {
            if (m.symbols[t.symbol].where().file == fid)
                ++symbols;
        }
        e[QStringLiteral("symbols")] = symbols;
        m_fileList << e;
    }
    m_symbolList.clear();
    int newCount = 0;
    for (const History::Touched &t : r.mapping.symbols) {
        const Symbol &s = m.symbols[t.symbol];
        QVariantMap e;
        e[QStringLiteral("symbol")] = t.symbol;
        e[QStringLiteral("name")] = s.name;
        e[QStringLiteral("qualified")] = m.displayName(t.symbol);
        e[QStringLiteral("kind")] = int(s.kind);
        e[QStringLiteral("lines")] = t.lines;
        e[QStringLiteral("added")] = t.added;
        e[QStringLiteral("file")] = s.where().file;
        e[QStringLiteral("location")] = m.locationText(s.where());
        e[QStringLiteral("path")] = s.where().file >= 0 ? m.files[s.where().file].relative : QString();
        m_symbolList << e;
        newCount += t.added;
    }
    // What the review found in the changed code
    m_findingList.clear();
    const Analysis::Report &report = Session::instance()->report();
    for (int i = 0; i < report.findings.size(); ++i) {
        const Analysis::Finding &f = report.findings[i];
        bool hit = false;
        for (qint32 s : f.symbols)
            hit = hit || m_touched.contains(s);
        if (!hit && f.symbols.isEmpty()) {
            for (qint32 file : f.files)
                hit = hit || m_lines.contains(file);
        }
        if (!hit)
            continue;
        QVariantMap e;
        e[QStringLiteral("kind")] = int(f.kind);
        e[QStringLiteral("kindName")] = Analysis::findingKindName(f.kind);
        e[QStringLiteral("severity")] = f.severity;
        e[QStringLiteral("title")] = f.title;
        e[QStringLiteral("detail")] = f.detail;
        e[QStringLiteral("symbol")] = f.symbols.isEmpty() ? -1 : f.symbols.first();
        e[QStringLiteral("file")] = f.loc.file;
        e[QStringLiteral("line")] = f.loc.line;
        e[QStringLiteral("location")] = m.locationText(f.loc);
        m_findingList << e;
    }
    const int commits = int(m_selected.size());
    m_summary = tr("%n commits", nullptr, commits) + QStringLiteral(" · ")
              + tr("%n files", nullptr, int(paths.size())) + QStringLiteral(" (+%1 −%2) · ").arg(added).arg(removed)
              + tr("%n symbols", nullptr, int(m_symbolList.size()))
              + (newCount ? tr(", %n new", nullptr, newCount) : QString())
              + (m_findingList.isEmpty() ? QString() : QStringLiteral(" · ") + tr("%n findings", nullptr, int(m_findingList.size())));
    if (!r.mapping.outside.isEmpty())
        m_summary += QStringLiteral(" · ") + tr("%n files not indexed", nullptr, int(r.mapping.outside.size()));
    m_revision++;
    emit busyChanged();
    emit changesChanged();
}

QString Changes::markdown() const
{
    if (!m_model)
        return {};
    QString out;
    QTextStream t(&out);
    t << "# Change review\n\n" << m_summary << "\n\n## Commits\n\n";
    for (const QVariant &v : m_commitList) {
        const QVariantMap c = v.toMap();
        if (!m_selected.contains(c.value(QStringLiteral("hash")).toString()))
            continue;
        t << "- " << (c.value(QStringLiteral("workTree")).toBool() ? QStringLiteral("work tree")
                                                                   : QStringLiteral("`%1`").arg(c.value(QStringLiteral("shortHash")).toString()))
          << " " << c.value(QStringLiteral("subject")).toString() << "\n";
    }
    t << "\n## Files\n\n";
    for (const QVariant &v : m_fileList) {
        const QVariantMap f = v.toMap();
        t << "- `" << f.value(QStringLiteral("relative")).toString() << "` +" << f.value(QStringLiteral("added")).toInt()
          << " −" << f.value(QStringLiteral("removed")).toInt() << (f.value(QStringLiteral("newFile")).toBool() ? " (new)" : "")
          << "\n";
    }
    t << "\n## Symbols\n\n";
    for (const QVariant &v : m_symbolList) {
        const QVariantMap s = v.toMap();
        t << "- " << (s.value(QStringLiteral("added")).toBool() ? "new " : "changed ")
          << Unravel::kindName(SymbolKind(s.value(QStringLiteral("kind")).toInt())) << " `"
          << s.value(QStringLiteral("qualified")).toString() << "` — " << s.value(QStringLiteral("lines")).toInt()
          << " lines, `" << s.value(QStringLiteral("location")).toString() << "`\n";
    }
    if (!m_findingList.isEmpty()) {
        t << "\n## Findings in the changed code\n\n";
        for (const QVariant &v : m_findingList) {
            const QVariantMap f = v.toMap();
            t << "- **" << f.value(QStringLiteral("kindName")).toString() << "**: " << f.value(QStringLiteral("title")).toString()
              << " — " << f.value(QStringLiteral("detail")).toString() << " `" << f.value(QStringLiteral("location")).toString()
              << "`\n";
        }
    }
    return out;
}
