#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include "flow.h"

class TestControlFlow : public QObject {
  Q_OBJECT

private:
  QTemporaryDir m_project;
  QString m_source;

  int functionLine(const QString &name) const {
    QFile source(m_source);
    if (!source.open(QIODevice::ReadOnly))
      return 0;
    const QStringList lines =
        QString::fromUtf8(source.readAll()).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
      if (lines[i].contains(name))
        return i + 1;
    }
    return 0;
  }

private slots:
  void initTestCase() {
    QVERIFY(m_project.isValid());
    m_source = m_project.filePath(QStringLiteral("flow.cpp"));
    QFile fixture(QStringLiteral(UNRAVEL_FLOW_FIXTURE));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QByteArray contents = fixture.readAll();
    QFile copy(m_source);
    QVERIFY(copy.open(QIODevice::WriteOnly));
    QCOMPARE(copy.write(contents), qint64(contents.size()));
    copy.close();

    QJsonArray entries;
    entries.append(QJsonObject{
        {QStringLiteral("directory"), m_project.path()},
        {QStringLiteral("file"), m_source},
        {QStringLiteral("command"),
         QStringLiteral("/usr/bin/clang++ -std=c++20 -c %1 -o flow.o")
             .arg(m_source)}});
    QFile database(m_project.filePath(QStringLiteral("compile_commands.json")));
    QVERIFY(database.open(QIODevice::WriteOnly));
    QCOMPARE(database.write(QJsonDocument(entries).toJson()),
             qint64(QJsonDocument(entries).toJson().size()));
  }

  void serializesClangCfgFacts() {
    QString error;
    const QByteArray json = Unravel::Flow::generate(
        m_project.path(), m_source, 1, QStringLiteral("flow_fixture"), &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    const QJsonObject graph = QJsonDocument::fromJson(json).object();
    QCOMPARE(graph.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(graph.value(QStringLiteral("kind")).toString(),
             QStringLiteral("clang-cfg"));
    QCOMPARE(graph.value(QStringLiteral("function"))
                 .toObject()
                 .value(QStringLiteral("qualifiedName"))
                 .toString(),
             QStringLiteral("flow_fixture"));
    QVERIFY(graph.value(QStringLiteral("blocks")).toArray().size() >= 8);

    bool hasReturn = false;
    bool hasLoopBackedge = false;
    bool hasSwitchFallthrough = false;
    bool hasBranch = false;
    bool hasSwitchBranch = false;
    int explicitReturnCount = 0;
    int earlyReturnCount = 0;
    QSet<int> blockIds;
    for (const QJsonValue &block :
         graph.value(QStringLiteral("blocks")).toArray()) {
      const QJsonObject item = block.toObject();
      blockIds.insert(item.value(QStringLiteral("id")).toInt());
      hasReturn |= item.value(QStringLiteral("earlyReturn")).toBool();
      explicitReturnCount +=
          item.value(QStringLiteral("explicitReturn")).toBool();
      earlyReturnCount += item.value(QStringLiteral("earlyReturn")).toBool();
    }
    for (const QJsonValue &value :
         graph.value(QStringLiteral("edges")).toArray()) {
      const QString kind =
          value.toObject().value(QStringLiteral("kind")).toString();
      hasLoopBackedge |= kind == QLatin1String("loop-backedge");
      hasSwitchFallthrough |= kind == QLatin1String("switch-fallthrough");
      hasBranch |= kind == QLatin1String("branch");
      hasSwitchBranch |=
          kind == QLatin1String("branch") &&
          value.toObject().value(QStringLiteral("branchType")).toString() ==
              QLatin1String("switch");
    }
    QVERIFY(hasReturn);
    QCOMPARE(explicitReturnCount, 2);
    QCOMPARE(earlyReturnCount, 1);
    QVERIFY(hasLoopBackedge);
    QVERIFY(hasSwitchFallthrough);
    QVERIFY(hasBranch);
    QVERIFY(hasSwitchBranch);
    for (const QJsonValue &value :
         graph.value(QStringLiteral("edges")).toArray()) {
      const QJsonObject edge = value.toObject();
      QVERIFY(blockIds.contains(edge.value(QStringLiteral("from")).toInt()));
      QVERIFY(blockIds.contains(edge.value(QStringLiteral("to")).toInt()));
    }
    const QJsonObject policies =
        graph.value(QStringLiteral("policies")).toObject();
    QCOMPARE(policies.value(QStringLiteral("exceptionEdges")).toString(),
             QStringLiteral("omitted"));
    QCOMPARE(policies.value(QStringLiteral("coroutines")).toString(),
             QStringLiteral("rejected"));
  }

  void selectorMustMatchDefinition() {
    QString error;
    const QByteArray json =
        Unravel::Flow::generate(m_project.path(), m_source, 1,
                                QStringLiteral("not_a_function"), &error);
    QVERIFY(json.isEmpty());
    QVERIFY(error.contains(QStringLiteral("No definition matches")));
  }

  void guessedCompileFlagsAreNotAccepted() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    const QString source = project.filePath(QStringLiteral("flow.cpp"));
    QVERIFY(QFile::copy(m_source, source));
    QString error;
    const QByteArray json = Unravel::Flow::generate(
        project.path(), source, 1, QStringLiteral("flow_fixture"), &error);
    QVERIFY(json.isEmpty());
    QVERIFY(error.contains(QStringLiteral("compile_commands.json")));
  }

  void experimentalFactsStayEvidenceLimited() {
    QString error;
    const QByteArray json = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("enum_flow_fixture"),
        QStringLiteral("enum_flow_fixture"), &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    const QJsonObject experimental =
        QJsonDocument::fromJson(json)
            .object()
            .value(QStringLiteral("experimentalFacts"))
            .toObject();
    const QJsonObject facts =
        experimental.value(QStringLiteral("facts")).toObject();
    const QJsonArray assignments =
        facts.value(QStringLiteral("enumAssignments")).toArray();
    QCOMPARE(assignments.size(), 3);
    for (const QJsonValue &value : assignments) {
      const QJsonObject assignment = value.toObject();
      QCOMPARE(assignment.value(QStringLiteral("enumType")).toString(),
               QStringLiteral("FlowState"));
      QCOMPARE(assignment.value(QStringLiteral("sourceProvenance")).toString(),
               QStringLiteral("explicit-enumerator"));
      QCOMPARE(
          assignment.value(QStringLiteral("priorStateProvenance")).toString(),
          QStringLiteral("unknown"));
    }
    const QJsonArray types =
        facts.value(QStringLiteral("pointerReferenceTypeObservations"))
            .toArray();
    QVERIFY(types.size() >= 2);
    QSet<QString> shapes;
    for (const QJsonValue &value : types) {
      const QJsonObject type = value.toObject();
      shapes.insert(type.value(QStringLiteral("typeShape")).toString());
      QCOMPARE(type.value(QStringLiteral("ownershipRole")).toString(),
               QStringLiteral("unknown"));
      QCOMPARE(type.value(QStringLiteral("borrowRole")).toString(),
               QStringLiteral("unknown"));
      QCOMPARE(type.value(QStringLiteral("lifetime")).toString(),
               QStringLiteral("not-analyzed"));
    }
    QVERIFY(shapes.contains(QStringLiteral("pointer-type")));
    QVERIFY(shapes.contains(QStringLiteral("lvalue-reference-type")));
    QCOMPARE(experimental.value(QStringLiteral("localDefUseSlices")).toString(),
             QStringLiteral("not implemented; no slice emitted"));
    QCOMPARE(facts.value(QStringLiteral("localDefUseSlices"))
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("not-implemented"));
  }
};

QTEST_GUILESS_MAIN(TestControlFlow)
#include "tst_controlflow.moc"
