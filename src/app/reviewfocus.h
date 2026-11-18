#pragma once

#include <QAbstractListModel>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "focus.h"

class ReviewFocus : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool changedOnly READ changedOnly WRITE setChangedOnly NOTIFY optionsChanged)
    Q_PROPERTY(bool historical READ historical WRITE setHistorical NOTIFY optionsChanged)
    Q_PROPERTY(bool functionHistory READ functionHistory WRITE setFunctionHistory NOTIFY optionsChanged)
    Q_PROPERTY(bool showAll READ showAll WRITE setShowAll NOTIFY optionsChanged)
    Q_PROPERTY(int impactDepth READ impactDepth WRITE setImpactDepth NOTIFY optionsChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    enum Roles { SymbolRole = Qt::UserRole + 1, NameRole, DetailRole, PriorityRole, FindingsRole, TouchedRole };
    explicit ReviewFocus(QObject *parent = nullptr);
    ~ReviewFocus() override;
    bool busy() const { return m_busy; }
    bool changedOnly() const { return m_changedOnly; }
    bool historical() const { return m_historical; }
    bool functionHistory() const { return m_functionHistory; }
    bool showAll() const { return m_showAll; }
    void setChangedOnly(bool value);
    void setHistorical(bool value);
    void setFunctionHistory(bool value);
    void setShowAll(bool value);
    int impactDepth() const { return m_impactDepth; }
    void setImpactDepth(int value);
    QString summary() const;
    int count() const { return int(m_result.entries.size()); }
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : count(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE QString markdown() const;
    Q_INVOKABLE QString json() const;
    double priorityFor(qint32 symbol) const { return m_priorities.value(symbol, -1); }
    double filePriority(qint32 file) const { return m_filePriorities.value(file, -1); }

signals:
    void optionsChanged();
    void changed();

private:
    void reload();
    bool m_changedOnly = false, m_historical = true, m_functionHistory = false, m_showAll = false;
    int m_impactDepth = 0;
    Unravel::Focus::Result m_result;
    std::shared_ptr<const Unravel::CodeModel> m_model;
    QHash<qint32, int> m_findings;
    QHash<qint32, double> m_priorities, m_filePriorities;
    bool m_busy = false;
    quint64 m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
};
