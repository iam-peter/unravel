#include "controlflow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVariantMap>

#include "layout.h"
#include "session.h"

namespace {
constexpr qsizetype kMaxHelperOutput = 4 * 1024 * 1024;
constexpr int kTimeoutMs = 30000;
} // namespace

ControlFlow *ControlFlow::s_instance = nullptr;

ControlFlow::ControlFlow(QObject *parent) : QObject(parent) {
  if (!s_instance)
    s_instance = this;
  m_process.setProcessChannelMode(QProcess::SeparateChannels);
  m_timeout.setSingleShot(true);
  connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
    m_output += m_process.readAllStandardOutput();
    if (m_output.size() > kMaxHelperOutput) {
      m_error = tr("CFG output exceeded the %1 MiB safety limit.")
                    .arg(kMaxHelperOutput / (1024 * 1024));
      cancel();
    }
  });
  connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
    m_stderr += m_process.readAllStandardError();
    if (m_stderr.size() > 32768)
      m_stderr.truncate(32768);
  });
  connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, &ControlFlow::finish);
  connect(&m_process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
              m_busy = false;
              m_error = tr("Could not start the optional unravel-flow helper.");
              m_timeout.stop();
              emit changed();
            }
          });
  connect(&m_timeout, &QTimer::timeout, this, [this] {
    m_error =
        tr("CFG analysis timed out after %1 seconds.").arg(kTimeoutMs / 1000);
    cancel();
  });
  connect(Session::instance(), &Session::modelChanged, this,
          &ControlFlow::resetForProjectChange);
  connect(Session::instance(), &Session::projectChanged, this,
          &ControlFlow::resetForProjectChange);
  connect(Session::instance(), &Session::staleChanged, this, [this] {
    if (Session::instance()->stale()) {
      resetForProjectChange();
      m_error =
          tr("The indexed project is stale; reindex before building a CFG.");
      emit changed();
    }
  });
  resetForProjectChange();
}

ControlFlow::~ControlFlow() {
  cancel();
  if (s_instance == this)
    s_instance = nullptr;
}

ControlFlow *ControlFlow::instance() {
  if (!s_instance)
    s_instance = new ControlFlow(qApp);
  return s_instance;
}

bool ControlFlow::available() const {
#ifdef Q_OS_WIN
  const QString name = QStringLiteral("unravel-flow.exe");
#else
  const QString name = QStringLiteral("unravel-flow");
#endif
  return QFileInfo(QDir(QCoreApplication::applicationDirPath()).filePath(name))
      .isExecutable();
}

void ControlFlow::resetForProjectChange() {
  ++m_generation;
  if (m_process.state() != QProcess::NotRunning)
    m_process.kill();
  m_timeout.stop();
  m_busy = false;
  m_output.clear();
  m_stderr.clear();
  m_blocks.clear();
  m_edges.clear();
  m_experimentalFacts.clear();
  m_error.clear();
  m_functionName.clear();
  m_root = Session::instance()->root();
  m_revision = Session::instance()->revision();
  emit changed();
}

void ControlFlow::analyze(int fileId, int definitionLine,
                          const QString &qualifiedName) {
  cancel();
  m_blocks.clear();
  m_edges.clear();
  m_experimentalFacts.clear();
  m_error.clear();
  m_functionName.clear();
  const auto model = Session::instance()->model();
  if (!model || fileId < 0 || fileId >= model->files.size() ||
      definitionLine < 1 || qualifiedName.trimmed().isEmpty()) {
    m_error = tr("Select a defined function in the open project.");
    emit changed();
    return;
  }
  if (Session::instance()->stale()) {
    m_error =
        tr("The indexed project is stale; reindex before building a CFG.");
    emit changed();
    return;
  }
  if (!available()) {
    m_error = tr("The optional local unravel-flow helper is not installed "
                 "beside this application.");
    emit changed();
    return;
  }
  if (m_process.state() != QProcess::NotRunning) {
    m_error = tr("The previous CFG helper has not stopped; try again shortly.");
    emit changed();
    return;
  }

  m_root = Session::instance()->root();
  m_revision = Session::instance()->revision();
  const QFileInfo sourceInfo(model->files[fileId].path);
  m_source = sourceInfo.canonicalFilePath();
  if (m_source.isEmpty())
    m_source = sourceInfo.absoluteFilePath();
  m_functionName = qualifiedName;
  m_busy = true;
  m_output.clear();
  m_stderr.clear();
  m_jobGeneration = ++m_generation;
  const QString program = QDir(QCoreApplication::applicationDirPath())
                              .filePath(
#ifdef Q_OS_WIN
                                  QStringLiteral("unravel-flow.exe")
#else
                                  QStringLiteral("unravel-flow")
#endif
                              );
  m_process.start(program,
                  {QStringLiteral("--project"), m_root,
                   QStringLiteral("--file"), m_source, QStringLiteral("--line"),
                   QString::number(definitionLine),
                   QStringLiteral("--qualified-name"), qualifiedName});
  m_timeout.start(kTimeoutMs);
  emit changed();
}

void ControlFlow::cancel() {
  if (m_process.state() == QProcess::NotRunning) {
    ++m_generation;
    if (m_busy) {
      m_busy = false;
      m_timeout.stop();
    }
    return;
  }
  ++m_generation;
  m_timeout.stop();
  m_process.kill();
  m_process.waitForFinished(500);
  m_busy = false;
  emit changed();
}

void ControlFlow::finish(int exitCode, QProcess::ExitStatus exitStatus) {
  m_timeout.stop();
  m_output += m_process.readAllStandardOutput();
  m_stderr += m_process.readAllStandardError();
  m_busy = false;
  if (m_jobGeneration != m_generation)
    return;
  if (Session::instance()->root() != m_root ||
      Session::instance()->revision() != m_revision ||
      Session::instance()->stale()) {
    m_blocks.clear();
    m_edges.clear();
    m_experimentalFacts.clear();
    m_error = tr("The project changed or became stale during CFG analysis; the "
                 "result was discarded.");
    emit changed();
    return;
  }
  if (m_output.size() > kMaxHelperOutput) {
    m_error = tr("CFG output exceeded the safety limit.");
    emit changed();
    return;
  }
  if (exitStatus != QProcess::NormalExit || exitCode != 0) {
    m_error = QString::fromUtf8(m_stderr.left(32768)).trimmed();
    if (m_error.isEmpty())
      m_error = tr("Clang CFG analysis failed (exit code %1).").arg(exitCode);
    emit changed();
    return;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(m_output, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
      document.object().value(QStringLiteral("schemaVersion")).toInt() != 1 ||
      document.object().value(QStringLiteral("kind")).toString() !=
          QLatin1String("clang-cfg")) {
    m_error = tr("The helper returned invalid or unsupported CFG JSON.");
    emit changed();
    return;
  }
  const QJsonObject graph = document.object();
  const QJsonObject function =
      graph.value(QStringLiteral("function")).toObject();
  if (function.value(QStringLiteral("file")).toString() != m_source ||
      function.value(QStringLiteral("qualifiedName")).toString() !=
          m_functionName) {
    m_error = tr("The CFG does not match the selected source definition.");
    emit changed();
    return;
  }
  m_blocks = graph.value(QStringLiteral("blocks")).toArray().toVariantList();
  m_edges = graph.value(QStringLiteral("edges")).toArray().toVariantList();
  m_experimentalFacts = graph.value(QStringLiteral("experimentalFacts"))
                            .toObject()
                            .toVariantMap();
  if (m_blocks.isEmpty()) {
    m_error = tr("Clang returned an empty CFG; no graph is displayed.");
    emit changed();
    return;
  }
  emit changed();
}

void ControlFlow::openBlock(int index) {
  if (index < 0 || index >= m_blocks.size())
    return;
  const QVariantMap source =
      m_blocks[index].toMap().value(QStringLiteral("source")).toMap();
  const int line = source.value(QStringLiteral("line")).toInt();
  const auto model = Session::instance()->model();
  if (!model || line < 1)
    return;
  const QString blockFile = QFileInfo(m_source).canonicalFilePath();
  for (int i = 0; i < model->files.size(); ++i) {
    if (QFileInfo(model->files[i].path).canonicalFilePath() == blockFile) {
      Session::instance()->activateFile(i, line);
      return;
    }
  }
}

void ControlFlow::openLocation(const QString &path, int line) {
  if (line < 1)
    return;
  const QString canonicalPath = QFileInfo(path).canonicalFilePath();
  if (canonicalPath.isEmpty())
    return;
  const auto model = Session::instance()->model();
  if (!model)
    return;
  for (int i = 0; i < model->files.size(); ++i) {
    if (QFileInfo(model->files[i].path).canonicalFilePath() == canonicalPath) {
      Session::instance()->activateFile(i, line);
      return;
    }
  }
}

QVariantMap ControlFlow::diagram() const {
  constexpr int maxNodes = 150;
  constexpr int maxEdges = 400;
  if (m_blocks.size() > maxNodes || m_edges.size() > maxEdges)
    return {{QStringLiteral("error"), tr("Diagram limit: %1 blocks / %2 edges. "
                                         "Use the complete evidence lists.")
                                          .arg(maxNodes)
                                          .arg(maxEdges)}};

  QVector<Unravel::Layout::Node> nodes;
  QVector<Unravel::Layout::Link> links;
  QHash<int, int> indices;
  for (int i = 0; i < m_blocks.size(); ++i) {
    indices.insert(m_blocks[i].toMap().value(QStringLiteral("id")).toInt(), i);
    Unravel::Layout::Node node;
    node.w = 260;
    node.h = 86;
    nodes.append(node);
  }
  for (const QVariant &value : m_edges) {
    const QVariantMap edge = value.toMap();
    const int from =
        indices.value(edge.value(QStringLiteral("from")).toInt(), -1);
    const int to = indices.value(edge.value(QStringLiteral("to")).toInt(), -1);
    if (from < 0 || to < 0)
      return {{QStringLiteral("error"),
               tr("Invalid CFG edge endpoint; no diagram rendered.")}};
    Unravel::Layout::Link link;
    link.from = from;
    link.to = to;
    links.append(link);
  }
  Unravel::Layout::Options options;
  options.direction = Unravel::Layout::Direction::TopToBottom;
  options.layerGap = 110;
  options.nodeGap = 60;
  Unravel::Layout::layered(nodes, links, options);
  QVariantList boxes, routes;
  double width = 0, height = 0;
  for (int i = 0; i < nodes.size(); ++i) {
    const auto &node = nodes[i];
    QVariantMap box = m_blocks[i].toMap();
    box.insert(QStringLiteral("index"), i);
    box.insert(QStringLiteral("x"), node.x + 30);
    box.insert(QStringLiteral("y"), node.y + 30);
    box.insert(QStringLiteral("width"), node.w);
    box.insert(QStringLiteral("height"), node.h);
    boxes.append(box);
    width = qMax(width, node.x + node.w + 60);
    height = qMax(height, node.y + node.h + 60);
  }
  for (int i = 0; i < links.size(); ++i) {
    QVariantMap route = m_edges[i].toMap();
    QVariantList points;
    QVector<QPointF> geometry = links[i].points;
    if (links[i].from == links[i].to) {
      const auto &node = nodes[links[i].from];
      geometry = {{node.x + node.w, node.y + node.h * 0.3},
                  {node.x + node.w + 25, node.y + node.h * 0.3},
                  {node.x + node.w + 25, node.y + node.h * 0.7},
                  {node.x + node.w, node.y + node.h * 0.7}};
    }
    for (const QPointF &point : geometry)
      points.append(QVariantMap{{QStringLiteral("x"), point.x() + 30},
                                {QStringLiteral("y"), point.y() + 30}});
    route.insert(QStringLiteral("points"), points);
    routes.append(route);
  }
  return {{QStringLiteral("nodes"), boxes},
          {QStringLiteral("edges"), routes},
          {QStringLiteral("width"), width},
          {QStringLiteral("height"), height}};
}
