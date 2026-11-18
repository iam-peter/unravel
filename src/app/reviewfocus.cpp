#include "reviewfocus.h"

#include <QJsonDocument>
#include <QFutureWatcher>
#include <QtConcurrent>

#include "changes.h"
#include "hotspots.h"
#include "session.h"

ReviewFocus::ReviewFocus(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(Session::instance(), &Session::modelChanged, this, &ReviewFocus::reload);
    connect(Session::instance(), &Session::staleChanged, this, [this] {
        if (m_functionHistory)
            reload();
    }, Qt::QueuedConnection);
    connect(Hotspots::instance(), &Hotspots::changed, this, &ReviewFocus::reload);
    connect(Changes::instance(), &Changes::changesChanged, this, &ReviewFocus::reload);
    connect(Changes::instance(), &Changes::busyChanged, this, &ReviewFocus::reload);
    reload();
}

ReviewFocus::~ReviewFocus()
{
    if (m_cancel)
        m_cancel->store(true);
}

void ReviewFocus::setChangedOnly(bool value)
{
    if (m_changedOnly == value)
        return;
    m_changedOnly = value;
    emit optionsChanged();
    reload();
}

void ReviewFocus::setHistorical(bool value)
{
    if (m_historical == value)
        return;
    m_historical = value;
    emit optionsChanged();
    reload();
}

void ReviewFocus::setFunctionHistory(bool value)
{
    if (m_functionHistory == value)
        return;
    m_functionHistory = value;
    emit optionsChanged();
    reload();
}

void ReviewFocus::setShowAll(bool value)
{
    if (m_showAll == value)
        return;
    m_showAll = value;
    emit optionsChanged();
    reload();
}

void ReviewFocus::setImpactDepth(int value)
{
    if (m_impactDepth == value)
        return;
    m_impactDepth = value;
    emit optionsChanged();
    reload();
}

void ReviewFocus::reload()
{
    if (m_functionHistory)
        Session::instance()->refreshStaleness();
    if (m_cancel)
        m_cancel->store(true);
    const quint64 generation = ++m_generation;
    beginResetModel();
    m_model = Session::instance()->model();
    m_result = {};
    m_findings.clear();
    m_priorities.clear();
    m_filePriorities.clear();
    m_busy = bool(m_model);
    if (m_model) {
        if (m_functionHistory && Session::instance()->stale()) {
            m_result.functionHistory = true;
            m_result.historical = true;
            m_result.firstParentHistory = true;
            m_result.error = tr("Function hunk estimates require a current index; reindex before enabling them");
            m_busy = false;
            endResetModel();
            emit changed();
            return;
        }
        Unravel::Focus::Options options;
        options.limit = m_showAll ? 0 : 20;
        options.changedOnly = m_changedOnly;
        options.impactDepth = m_impactDepth;
        if (!Changes::instance()->busy()) {
            for (auto it = Changes::instance()->touched().cbegin(); it != Changes::instance()->touched().cend(); ++it)
                options.touched.insert(it.key());
        }
        m_cancel = std::make_shared<std::atomic_bool>(false);
        const auto cancel = m_cancel;
        options.cancel = cancel.get();
        const auto activity = m_historical && Hotspots::instance()->ready()
            ? Hotspots::instance()->activity() : Unravel::History::Activity();
        const auto model = m_model;
        const bool functionHistory = m_functionHistory;
        auto *watcher = new QFutureWatcher<Unravel::Focus::Result>(this);
        connect(watcher, &QFutureWatcher<Unravel::Focus::Result>::finished, this, [this, watcher, generation] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (generation != m_generation)
                return;
            if (m_functionHistory && Session::instance()->refreshStaleness()) {
                reload();
                return;
            }
            beginResetModel();
            m_result = result;
            for (const auto &entry : m_result.entries) {
                m_priorities.insert(entry.symbol, entry.priority);
                const int file = m_model->symbols[entry.symbol].def.file;
                m_filePriorities[file] = std::max(m_filePriorities.value(file, -1), entry.priority);
            }
            m_busy = false;
            endResetModel();
            emit changed();
        });
        watcher->setFuture(QtConcurrent::run([model, activity, options, cancel, functionHistory] {
            Unravel::History::HunkActivity hunks;
            auto rankedOptions = options;
            if (functionHistory) {
                if (!activity.ready()) {
                    hunks.error = QStringLiteral("Function hunk estimates require available file history");
                } else {
                    hunks = Unravel::History::functionHunks(activity.repository, activity.revision,
                        activity.since, activity.until, *model, cancel.get());
                }
                rankedOptions.functionHunks = &hunks;
            }
            return Unravel::Focus::rank(*model, activity.ready() ? &activity : nullptr, rankedOptions);
        }));
        for (const auto &finding : Session::instance()->report().findings) {
            for (qint32 symbol : finding.symbols)
                ++m_findings[symbol];
        }
    }
    endResetModel();
    emit changed();
}

QString ReviewFocus::summary() const
{
    if (m_busy)
        return tr("Computing Review Focus...");
    if (!m_result.error.isEmpty())
        return m_result.functionHistory
            ? tr("Function-hunk estimate unavailable: %1").arg(m_result.error) : m_result.error;
    QString history = m_result.functionHistory
        ? tr("first-parent non-merge function-hunk estimate; side-branch and merge commits are not counted (file proxy counts all reachable non-merge commits)")
        : m_result.historical ? (m_result.followedRenames
            ? tr("file-history proxy over all reachable non-merge commits; Git --follow heuristic")
            : tr("file-history proxy over all reachable non-merge commits; renames not followed"))
                             : tr("static priority (no history used)");
    QString caveats = m_result.partial ? tr(" | partial shallow history") : QString();
    if (m_result.functionHistory) {
        caveats += tr(" | %1 first-parent commits used for line mapping").arg(m_result.mappingCommits);
        caveats += tr(" | post-revision/current-tree changes are mapping-only, not counted as commits");
        if (m_result.postRevisionChangedFiles > 0)
            caveats += tr(" (%1 files)").arg(m_result.postRevisionChangedFiles);
    } else if (m_result.historical) {
        caveats += tr(" | working-tree changes not counted");
    }
    return tr("%1 of %2 functions | %3 | heuristic v1, not a defect probability%4")
        .arg(count()).arg(m_result.eligible)
        .arg(history, caveats);
}

QHash<int, QByteArray> ReviewFocus::roleNames() const
{
    return {{SymbolRole, "symbol"}, {NameRole, "name"}, {DetailRole, "detail"},
            {PriorityRole, "priority"}, {FindingsRole, "findings"}, {TouchedRole, "touched"}};
}

QVariant ReviewFocus::data(const QModelIndex &index, int role) const
{
    if (!m_model || !index.isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const auto &entry = m_result.entries[index.row()];
    const auto &symbol = m_model->symbols[entry.symbol];
    switch (role) {
    case SymbolRole: return entry.symbol;
    case NameRole: return m_model->displayName(entry.symbol);
    case PriorityRole: return entry.priority;
    case FindingsRole: return m_findings.value(entry.symbol);
    case TouchedRole: return entry.touched;
    case DetailRole:
        return tr("%1 | McCabe %2, cognitive %3 | %4 neighbours | C=%5, H=%6, D=%7%8")
            .arg(m_model->locationText(symbol.def)).arg(symbol.metrics.complexity).arg(symbol.metrics.cognitive)
            .arg(entry.neighbours).arg(entry.complexity, 0, 'f', 2).arg(entry.history, 0, 'f', 2)
            .arg(entry.dependency, 0, 'f', 2)
            .arg(m_result.functionHistory
                ? tr(" | %1 hunk commits, %2 mapped lines, %3 ambiguous commits%4")
                    .arg(entry.hunkCommits).arg(entry.hunkLines).arg(entry.ambiguousHunks)
                    .arg(entry.hunkAmbiguity.isEmpty() ? QString() : tr(" (%1)").arg(entry.hunkAmbiguity.join(QStringLiteral(", "))))
                : m_result.historical ? tr(" | %1 file commits (proxy)").arg(entry.commits) : QString());
    }
    return {};
}

QString ReviewFocus::markdown() const
{
    if (m_busy)
        return tr("Review Focus is still computing");
    return m_model ? Unravel::Focus::markdown(*m_model, m_result) : QString();
}

QString ReviewFocus::json() const
{
    if (m_busy)
        return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("error"), tr("Review Focus is still computing")}}).toJson());
    return m_model ? QString::fromUtf8(QJsonDocument(Unravel::Focus::json(*m_model, m_result)).toJson()) : QString();
}
