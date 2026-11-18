#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "analysis.h"
#include "architecture.h"

class Contracts : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool valid READ valid NOTIFY changed)
    Q_PROPERTY(bool configured READ configured NOTIFY changed)
    Q_PROPERTY(QString configPath READ configPath NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList violations READ violations NOTIFY changed)
    Q_PROPERTY(QVariantList unassignedFiles READ unassignedFiles NOTIFY changed)

public:
    explicit Contracts(QObject *parent);
    ~Contracts() override;

    static Contracts *instance();
    static Contracts *create(QQmlEngine *, QJSEngine *)
    {
        Contracts *contracts = instance();
        QJSEngine::setObjectOwnership(contracts, QJSEngine::CppOwnership);
        return contracts;
    }

    bool busy() const { return m_busy; }
    bool valid() const { return m_result.valid; }
    bool configured() const { return m_result.configured; }
    QString configPath() const { return m_configPath; }
    QString error() const { return m_result.errors.join(QLatin1Char('\n')); }
    QVariantList violations() const;
    QVariantList unassignedFiles() const;

    // Directed CodeModel file IDs, matching evidence from either scope.
    bool violatesFiles(qint32 from, qint32 to) const;
    // Directed CodeModel symbol IDs; only semantic evidence with both symbols.
    bool violatesSymbols(qint32 from, qint32 to) const;
    // Call on the GUI thread and copy the result into scene-graph input; this
    // method reads the active result and never consults Session/controller state.
    QSet<qint32> violatingLinks(const Unravel::CodeModel &model,
                                const Unravel::Analysis::DepGraph &graph,
                                int moduleDepth = 0) const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reload() { refresh(); }
    Q_INVOKABLE void openEvidence(int violation, int evidence);
    Q_INVOKABLE void openUnassigned(int index);
    Q_INVOKABLE QString reportJson() const;
    Q_INVOKABLE QString reportMarkdown() const;

signals:
    void changed();

private:
    void modelChanged();
    static Contracts *s_instance;

    QString m_root;
    QString m_configPath;
    bool m_busy = false;
    quint64 m_generation = 0;
    Unravel::Architecture::Result m_result;
    std::shared_ptr<const Unravel::CodeModel> m_resultModel;
};
