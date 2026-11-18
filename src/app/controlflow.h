#pragma once

#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class ControlFlow final : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON

  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool available READ available CONSTANT)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString functionName READ functionName NOTIFY changed)
  Q_PROPERTY(QVariantList blocks READ blocks NOTIFY changed)
  Q_PROPERTY(QVariantList edges READ edges NOTIFY changed)
  Q_PROPERTY(
      QVariantMap experimentalFacts READ experimentalFacts NOTIFY changed)

public:
  explicit ControlFlow(QObject *parent = nullptr);
  ~ControlFlow() override;

  static ControlFlow *instance();
  static ControlFlow *create(QQmlEngine *, QJSEngine *) {
    ControlFlow *flow = instance();
    QJSEngine::setObjectOwnership(flow, QJSEngine::CppOwnership);
    return flow;
  }

  bool busy() const { return m_busy; }
  bool available() const;
  QString error() const { return m_error; }
  QString functionName() const { return m_functionName; }
  QVariantList blocks() const { return m_blocks; }
  QVariantList edges() const { return m_edges; }
  QVariantMap experimentalFacts() const { return m_experimentalFacts; }

  Q_INVOKABLE void analyze(int fileId, int definitionLine,
                           const QString &qualifiedName);
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void openBlock(int index);
  Q_INVOKABLE void openLocation(const QString &path, int line);
  Q_INVOKABLE QVariantMap diagram(bool collapse = false) const;
  Q_INVOKABLE QVariantMap slice(const QString &node, bool forward) const;
  Q_INVOKABLE QVariantMap evidenceDiagram(const QVariantList &nodes,
                                          const QVariantList &edges) const;
  static QVariantMap compactGraph(const QVariantList &blocks,
                                  const QVariantList &edges);

signals:
  void changed();

private:
  void finish(int exitCode, QProcess::ExitStatus exitStatus);
  void resetForProjectChange();
  static ControlFlow *s_instance;

  QProcess m_process;
  QTimer m_timeout;
  QByteArray m_output;
  QByteArray m_stderr;
  QString m_error;
  QString m_functionName;
  QVariantList m_blocks;
  QVariantList m_edges;
  QVariantMap m_experimentalFacts;
  QString m_root;
  QString m_source;
  int m_revision = -1;
  int m_definitionLine = 0;
  bool m_busy = false;
  quint64 m_generation = 0;
  quint64 m_jobGeneration = 0;
};
