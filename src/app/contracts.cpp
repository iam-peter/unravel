#include "contracts.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <limits>
#include <QVariantMap>
#include <QtConcurrent>

#include "session.h"

using namespace Unravel;

Contracts *Contracts::s_instance = nullptr;

Contracts::Contracts(QObject *parent)
    : QObject(parent)
{
    if (!s_instance)
        s_instance = this;
    connect(Session::instance(), &Session::modelChanged, this, &Contracts::modelChanged);
    modelChanged();
}

Contracts::~Contracts()
{
    ++m_generation;
    if (s_instance == this)
        s_instance = nullptr;
}

Contracts *Contracts::instance()
{
    if (!s_instance)
        s_instance = new Contracts(qApp);
    return s_instance;
}

void Contracts::modelChanged()
{
    m_root = Session::instance()->root();
    refresh();
}

void Contracts::refresh()
{
    const quint64 generation = ++m_generation;
    const auto model = Session::instance()->model();
    m_configPath = m_root.isEmpty() ? QString()
        : QDir(m_root).filePath(QStringLiteral(".unravel/architecture.json"));
    m_result = {};
    m_resultModel.reset();
    m_busy = false;
    if (!model) {
        emit changed();
        return;
    }

    if (!QFileInfo::exists(m_configPath)) {
        m_result = Unravel::Architecture::noConfig();
        emit changed();
        return;
    }

    m_busy = true;
    const QString configPath = m_configPath;
    auto *watcher = new QFutureWatcher<Unravel::Architecture::Result>(this);
    connect(watcher, &QFutureWatcher<Unravel::Architecture::Result>::finished,
            this, [this, watcher, generation, model] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        m_result = result;
        m_resultModel = model;
        m_busy = false;
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([model, configPath] {
        return Unravel::Architecture::check(*model, configPath);
    }));
    emit changed();
}

QVariantList Contracts::violations() const
{
    QVariantList list;
    for (const Unravel::Architecture::Violation &violation : m_result.violations) {
        QVariantList evidenceList;
        for (const Unravel::Architecture::Evidence &evidence : violation.evidence) {
            evidenceList.append(QVariantMap{
                {QStringLiteral("sourceFile"), evidence.sourceFile},
                {QStringLiteral("targetFile"), evidence.targetFile},
                {QStringLiteral("sourceLayer"), evidence.sourceLayer},
                {QStringLiteral("targetLayer"), evidence.targetLayer},
                {QStringLiteral("line"), evidence.line},
                {QStringLiteral("sourceSymbol"), evidence.sourceSymbol},
                {QStringLiteral("targetSymbol"), evidence.targetSymbol},
                {QStringLiteral("sourceFileId"), evidence.sourceFileId},
                {QStringLiteral("targetFileId"), evidence.targetFileId},
                {QStringLiteral("sourceSymbolId"), evidence.sourceSymbolId},
                {QStringLiteral("targetSymbolId"), evidence.targetSymbolId},
                {QStringLiteral("referenceId"), evidence.referenceId}
            });
        }
        list.append(QVariantMap{
            {QStringLiteral("ruleId"), violation.ruleId},
            {QStringLiteral("scope"), violation.scope},
            {QStringLiteral("sourceLayer"), violation.sourceLayer},
            {QStringLiteral("targetLayer"), violation.targetLayer},
            {QStringLiteral("evidence"), evidenceList}
        });
    }
    return list;
}

bool Contracts::violatesFiles(qint32 from, qint32 to) const
{
    if (m_busy || !m_result.valid || !m_result.configured || from < 0 || to < 0)
        return false;
    for (const Unravel::Architecture::Violation &violation : m_result.violations) {
        for (const Unravel::Architecture::Evidence &evidence : violation.evidence) {
            if (evidence.sourceFileId == from && evidence.targetFileId == to)
                return true;
        }
    }
    return false;
}

bool Contracts::violatesSymbols(qint32 from, qint32 to) const
{
    if (m_busy || !m_result.valid || !m_result.configured || from < 0 || to < 0)
        return false;
    for (const Unravel::Architecture::Violation &violation : m_result.violations) {
        if (violation.scope != QLatin1String("semantic"))
            continue;
        for (const Unravel::Architecture::Evidence &evidence : violation.evidence) {
            if (evidence.sourceSymbolId == from && evidence.targetSymbolId == to)
                return true;
        }
    }
    return false;
}

QSet<qint32> Contracts::violatingLinks(const CodeModel &model,
                                       const Analysis::DepGraph &graph,
                                       int moduleDepth) const
{
    QSet<qint32> links;
    if (m_busy || !m_result.valid || !m_result.configured || !m_resultModel
        || m_resultModel.get() != &model)
        return links;

    QSet<qint32> violatingReferences;
    for (const Architecture::Violation &violation : m_result.violations) {
        if (violation.scope != QLatin1String("semantic"))
            continue;
        for (const Architecture::Evidence &evidence : violation.evidence) {
            if (evidence.referenceId >= 0)
                violatingReferences.insert(evidence.referenceId);
        }
    }

    for (qint32 linkId = 0; linkId < graph.links.size(); ++linkId) {
        const Analysis::DepGraph::Link &link = graph.links[linkId];
        for (qint32 reference : link.refs) {
            if (violatingReferences.contains(reference)) {
                links.insert(linkId);
                break;
            }
        }
    }

    if (graph.level == Analysis::Level::Class)
        return links;

    auto nodeForFile = [&](qint32 fileId) {
        if (fileId < 0 || fileId >= model.files.size())
            return qint32(-1);
        const File &file = model.files[fileId];

        qint32 foldedNode = -1;
        qsizetype foldedPathLength = std::numeric_limits<qsizetype>::max();
        for (qint32 node = 0; node < graph.folder.size(); ++node) {
            const QString &folder = graph.folder[node];
            if (folder.isEmpty()
                || (file.module != folder && !file.module.startsWith(folder + QLatin1Char('/')))
                || folder.size() >= foldedPathLength)
                continue;
            foldedNode = node;
            foldedPathLength = folder.size();
        }
        if (foldedNode >= 0)
            return foldedNode;

        if (graph.level == Analysis::Level::File) {
            for (qint32 node = 0; node < graph.ids.size(); ++node) {
                if (graph.folder[node].isEmpty() && graph.ids[node] == fileId)
                    return node;
            }
            return qint32(-1);
        }

        const QString module = Analysis::moduleAt(file.module, moduleDepth);
        const QString moduleName = module.isEmpty() ? QStringLiteral("(root)") : module;
        for (qint32 node = 0; node < graph.names.size(); ++node) {
            if (graph.folder[node].isEmpty() && graph.names[node] == moduleName)
                return node;
        }
        return qint32(-1);
    };

    for (const Architecture::Violation &violation : m_result.violations) {
        if (violation.scope != QLatin1String("includes"))
            continue;
        for (const Architecture::Evidence &evidence : violation.evidence) {
            const qint32 from = nodeForFile(evidence.sourceFileId);
            const qint32 to = nodeForFile(evidence.targetFileId);
            const qint32 linkId = graph.linkIndex(from, to);
            if (linkId >= 0)
                links.insert(linkId);
        }
    }
    return links;
}

QVariantList Contracts::unassignedFiles() const
{
    QVariantList list;
    const auto model = Session::instance()->model();
    if (!model)
        return list;
    QHash<QString, int> fileIds;
    for (qint32 file = 0; file < model->files.size(); ++file)
        fileIds.insert(QDir::fromNativeSeparators(model->files[file].relative), file);
    for (const QString &path : m_result.unassignedFiles) {
        QVariantMap item{{QStringLiteral("path"), path},
                         {QStringLiteral("file"), fileIds.value(path, -1)}};
        list.append(item);
    }
    return list;
}

void Contracts::openEvidence(int violation, int evidence)
{
    if (violation < 0 || violation >= m_result.violations.size())
        return;
    const auto &items = m_result.violations[violation].evidence;
    if (evidence < 0 || evidence >= items.size())
        return;
    const auto &item = items[evidence];
    Session::instance()->activateFile(item.sourceFileId, item.line);
}

void Contracts::openUnassigned(int index)
{
    const QVariantList files = unassignedFiles();
    if (index < 0 || index >= files.size())
        return;
    const int file = files[index].toMap().value(QStringLiteral("file")).toInt();
    if (file >= 0)
        Session::instance()->activateFile(file);
}

QString Contracts::reportJson() const
{
    return QString::fromUtf8(m_result.toJson());
}

QString Contracts::reportMarkdown() const
{
    return m_result.toMarkdown();
}
