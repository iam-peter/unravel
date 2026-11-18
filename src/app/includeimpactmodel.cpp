#include "includeimpactmodel.h"

#include "session.h"

#include <QFutureWatcher>
#include <QScopedValueRollback>
#include <QtConcurrentRun>

using namespace Unravel;
using namespace Unravel::IncludeImpact;

IncludeImpactModel::IncludeImpactModel(QObject *parent)
    : QAbstractListModel(parent), m_session(Session::instance())
{
    if (m_session) {
        connect(m_session, &Session::modelChanged, this, &IncludeImpactModel::reload);
        connect(m_session, &Session::activeChanged, this, &IncludeImpactModel::followActive);
        reload();
    }
}

IncludeImpactModel::~IncludeImpactModel()
{
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
}

void IncludeImpactModel::reload()
{
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
    m_cancel.reset();

    beginResetModel();
    m_model = m_session->model();
    m_index.reset();
    m_ranks.clear();
    m_counts.clear();
    m_reach = {};
    m_cycle = {};
    m_limits = {};
    m_graph = {};
    m_busy = bool(m_model);
    endResetModel();
    emit coverageChanged();
    emit selectionChanged();
    emit graphChanged();
    if (!m_model)
        return;

    struct Result {
        std::shared_ptr<const Index> index;
        QVector<HeaderRank> ranks;
    };
    auto *watcher = new QFutureWatcher<Result>(this);
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, cancel] {
        watcher->deleteLater();
        if (cancel != m_cancel)
            return;
        auto [index, ranks] = watcher->result();
        beginResetModel();
        m_busy = false;
        m_index = std::move(index);
        m_ranks = std::move(ranks);
        for (const HeaderRank &row : std::as_const(m_ranks))
            m_counts.insert(row.file, row.affectedUnits);
        endResetModel();
        const int active = m_session->activeFile();
        if (available() && m_index->supportsFile(active))
            setFocus(active);
        else if (!m_ranks.isEmpty())
            setFocus(m_ranks.first().file); // preview without moving Session navigation
        emit coverageChanged();
    });
    watcher->setFuture(QtConcurrent::run([model = m_model, cancel] {
        if (cancel->load(std::memory_order_relaxed))
            return Result{};
        auto index = std::make_shared<const Index>(*model);
        auto ranks = index->ranking(cancel.get());
        return Result{std::move(index), std::move(ranks)};
    }));
}

int IncludeImpactModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_ranks.size();
}

QVariant IncludeImpactModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_ranks.size())
        return {};
    const HeaderRank &row = m_ranks[index.row()];
    switch (role) {
    case FileRole: return row.file;
    case PathRole: return path(row.file);
    case AffectedRole: return row.affectedUnits;
    case CycleRole: return row.inCycle;
    default: return {};
    }
}

QHash<int, QByteArray> IncludeImpactModel::roleNames() const
{
    return {{FileRole, "fileId"}, {PathRole, "filePath"},
            {AffectedRole, "estimatedUnits"}, {CycleRole, "inCycle"}};
}

QString IncludeImpactModel::coverage() const
{
    if (m_busy)
        return tr("Analyzing recorded includes…");
    if (!m_index)
        return tr("Open an indexed project to analyze includes.");
    return m_index->coverage().description();
}

QString IncludeImpactModel::path(int file) const
{
    if (!m_model || file < 0 || file >= m_model->files.size())
        return {};
    return m_model->files[file].relative;
}

void IncludeImpactModel::followActive()
{
    const int file = m_session->activeFile();
    if (!m_openingEvidence && available() && m_index->supportsFile(file))
        setFocus(file);
}

void IncludeImpactModel::selectFile(int file)
{
    if (!available() || !m_index->supportsFile(file))
        return;
    setFocus(file);
    m_session->activateFile(file);
}

void IncludeImpactModel::setFocus(int file)
{
    if (file == selectedFile())
        return;
    m_reach = m_index->affected(file);
    m_cycle = m_index->cycle(file);
    m_limits = {};
    rebuildGraph();
    emit selectionChanged();
}

QVariantList IncludeImpactModel::units(int start, int count) const
{
    QVariantList result;
    if (start < 0 || start >= m_reach.units.size() || count <= 0)
        return result;
    const int size = qMin<qsizetype>(count, m_reach.units.size() - start);
    result.reserve(size);
    for (int i = start; i < start + size; ++i) {
        const int file = m_reach.units[i];
        result.append(QVariantMap{{"fileId", file}, {"filePath", path(file)}});
    }
    return result;
}

QVariantList IncludeImpactModel::stepRows(const QVector<Step> &steps) const
{
    QVariantList result;
    result.reserve(steps.size());
    for (const Step &step : steps) {
        result.append(QVariantMap{{"from", step.from}, {"to", step.to}, {"line", step.line},
                                  {"fromPath", path(step.from)}, {"toPath", path(step.to)}});
    }
    return result;
}

void IncludeImpactModel::openEvidence(int file, int line)
{
    if (!available() || !m_index->supportsFile(file))
        return;
    // Jump in the shared code view without moving the root of this explanation.
    const QScopedValueRollback keepFocus(m_openingEvidence, true);
    m_session->activateFile(file, qMax(0, line));
}

void IncludeImpactModel::setShowIncludes(bool value)
{
    if (value == m_showIncludes)
        return;
    m_showIncludes = value;
    m_limits = {};
    rebuildGraph();
}

void IncludeImpactModel::expandGraph()
{
    if (m_graph.depthTruncated)
        m_limits.depth += 2;
    if (m_graph.nodesTruncated)
        m_limits.nodes += 40;
    rebuildGraph();
}

void IncludeImpactModel::rebuildGraph()
{
    m_graph = available() ? m_index->graph(selectedFile(),
        m_showIncludes ? Direction::Includes : Direction::Includers, m_limits) : Graph{};
    emit graphChanged();
}

QVariantMap IncludeImpactModel::graphInfo() const
{
    return {{"nodeCount", m_graph.files.size()}, {"edgeCount", m_graph.edges.size()},
            {"depthTruncated", m_graph.depthTruncated}, {"nodesTruncated", m_graph.nodesTruncated},
            {"depthLimit", m_limits.depth}, {"nodeLimit", m_limits.nodes}};
}
