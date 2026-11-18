#pragma once

#include "includeimpact.h"

#include <QAbstractListModel>
#include <QHash>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

class Session;

class IncludeImpactModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool busy READ busy NOTIFY coverageChanged)
    Q_PROPERTY(bool available READ available NOTIFY coverageChanged)
    Q_PROPERTY(QString coverage READ coverage NOTIFY coverageChanged)
    Q_PROPERTY(int selectedFile READ selectedFile NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectionChanged)
    Q_PROPERTY(int affectedUnitCount READ affectedUnitCount NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList cycleSteps READ cycleSteps NOTIFY selectionChanged)
    Q_PROPERTY(int componentSize READ componentSize NOTIFY selectionChanged)
    Q_PROPERTY(bool showIncludes READ showIncludes WRITE setShowIncludes NOTIFY graphChanged)
    Q_PROPERTY(QVariantMap graphInfo READ graphInfo NOTIFY graphChanged)
    Q_PROPERTY(QVariantList graphEdges READ graphEdges NOTIFY graphChanged)

public:
    explicit IncludeImpactModel(QObject *parent = nullptr);
    ~IncludeImpactModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool busy() const { return m_busy; }
    bool available() const { return m_index && m_index->coverage().available(); }
    QString coverage() const;
    int selectedFile() const { return m_reach.header; }
    QString selectedPath() const { return path(selectedFile()); }
    int affectedUnitCount() const { return m_reach.units.size(); }
    int componentSize() const { return m_cycle.componentSize; }
    QVariantList cycleSteps() const { return stepRows(m_cycle.witness); }
    bool showIncludes() const { return m_showIncludes; }
    void setShowIncludes(bool value);
    QVariantMap graphInfo() const;
    QVariantList graphEdges() const { return stepRows(m_graph.edges); }

    // Unknown/non-ranked files return -1 for overlays such as Treemap.
    Q_INVOKABLE int rankedImpact(int file) const { return m_counts.value(file, -1); }
    Q_INVOKABLE void selectFile(int file);
    Q_INVOKABLE QVariantList units(int start, int count) const;
    Q_INVOKABLE QVariantList witness(int unit) const { return stepRows(m_reach.witness(unit)); }
    Q_INVOKABLE void openEvidence(int file, int line);
    Q_INVOKABLE void expandGraph();

signals:
    void coverageChanged();
    void selectionChanged();
    void graphChanged();

private:
    enum Roles { FileRole = Qt::UserRole + 1, PathRole, AffectedRole, CycleRole };
    void reload();
    void followActive();
    void setFocus(int file);
    void rebuildGraph();
    QString path(int file) const;
    QVariantList stepRows(const QVector<Unravel::IncludeImpact::Step> &steps) const;

    Session *m_session = nullptr;
    std::shared_ptr<const Unravel::CodeModel> m_model;
    std::shared_ptr<const Unravel::IncludeImpact::Index> m_index;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QVector<Unravel::IncludeImpact::HeaderRank> m_ranks;
    QHash<int, int> m_counts;
    Unravel::IncludeImpact::Reach m_reach;
    Unravel::IncludeImpact::Cycle m_cycle;
    Unravel::IncludeImpact::Graph m_graph;
    Unravel::IncludeImpact::Limits m_limits;
    bool m_busy = false;
    bool m_showIncludes = false;
    bool m_openingEvidence = false;
};
