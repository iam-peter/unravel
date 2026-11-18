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
      QCOMPARE(type.value(QStringLiteral("lifetime")).toString(),
               QStringLiteral("not-analyzed"));
    }
    QVERIFY(shapes.contains(QStringLiteral("pointer-type")));
    QVERIFY(shapes.contains(QStringLiteral("lvalue-reference-type")));
    QCOMPARE(facts.value(QStringLiteral("localDefUseSlices"))
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("experimental"));
  }

  void guardedMembersAndQtTransitions() {
    QString error;
    const auto json = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("void update"),
        QStringLiteral("Stateful::update"), &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    const auto facts = QJsonDocument::fromJson(json)
                           .object()["experimentalFacts"]
                           .toObject()["facts"]
                           .toObject();
    const auto assignments = facts["enumAssignments"].toArray();
    QCOMPARE(assignments.size(), 5);
    const auto stateGraph = facts["stateTransitions"].toObject();
    QCOMPARE(stateGraph["targets"].toArray().size(), 1);
    QCOMPARE(stateGraph["edges"].toArray().size(), 5);
    QCOMPARE(assignments.last().toObject()["sourceProvenance"].toString(),
             QStringLiteral("unknown-enum-expression"));
    bool unknownOrigin = false;
    for (const auto value : stateGraph["nodes"].toArray())
      unknownOrigin |=
          value.toObject()["kind"] == QLatin1String("unknown-origin");
    QVERIFY(unknownOrigin);
    QCOMPARE(assignments[0].toObject()["priorState"].toString(),
             QStringLiteral("FlowState::idle"));
    QVERIFY(!assignments[0].toObject()["guard"].toObject().isEmpty());
    for (int i = 1; i < assignments.size(); ++i)
      QCOMPARE(assignments[i].toObject()["priorState"].toString(),
               QStringLiteral("unknown"));
    const auto qt = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("void qt_state_fixture"),
        QStringLiteral("qt_state_fixture"), &error);
    QVERIFY2(!qt.isEmpty(), qPrintable(error));
    const auto qtFacts = QJsonDocument::fromJson(qt)
                             .object()["experimentalFacts"]
                             .toObject()["facts"]
                             .toObject();
    QCOMPARE(qtFacts["qtStateTransitions"].toArray().size(), 2);
    int parents = 0;
    for (const auto edge : qtFacts["ownership"].toObject()["edges"].toArray())
      parents +=
          edge.toObject()["role"].toString() == QLatin1String("qobject-parent");
    QCOMPARE(parents, 2);
  }

  void ownershipPreservesSharedIdentityAndBorrowRoles() {
    QString error;
    const auto json = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("void ownership_fixture"),
        QStringLiteral("ownership_fixture"), &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    const auto graph = QJsonDocument::fromJson(json)
                           .object()["experimentalFacts"]
                           .toObject()["facts"]
                           .toObject()["ownership"]
                           .toObject();
    QHash<QString, QString> labels, targets, roles, kinds;
    QSet<QString> ids;
    for (const auto value : graph["nodes"].toArray()) {
      const auto item = value.toObject();
      QVERIFY(!ids.contains(item["id"].toString()));
      ids.insert(item["id"].toString());
      labels.insert(item["id"].toString(), item["label"].toString());
      kinds.insert(item["id"].toString(), item["kind"].toString());
    }
    for (const auto value : graph["edges"].toArray()) {
      const auto edge = value.toObject();
      targets.insert(labels.value(edge["from"].toString()),
                     edge["to"].toString());
      roles.insert(labels.value(edge["from"].toString()),
                   edge["role"].toString());
      QVERIFY(ids.contains(edge["from"].toString()));
      QVERIFY(ids.contains(edge["to"].toString()));
    }
    QCOMPARE(roles.value("shared"), QStringLiteral("shared-ownership"));
    QCOMPARE(roles.value("sharedRef"), QStringLiteral("borrowing"));
    QCOMPARE(labels.value(targets.value("sharedRef")),
             QStringLiteral("shared"));
    QCOMPARE(targets.value("shared"), targets.value("another"));
    QCOMPARE(targets.value("shared"), targets.value("observer"));
    QCOMPARE(roles.value("observer"), QStringLiteral("observation"));
    QCOMPARE(roles.value("borrow"), QStringLiteral("borrowing"));
    QCOMPARE(roles.value("raw"), QStringLiteral("borrowing"));
    QCOMPARE(roles.value("unique"), QStringLiteral("ownership"));
    QCOMPARE(roles.value("pointer"), QStringLiteral("borrowing"));
    QCOMPARE(targets.value("unique"), targets.value("pointer"));
    QCOMPARE(kinds.value(targets.value("shared")),
             QStringLiteral("allocation-site"));
    QCOMPARE(roles.value("qtShared"), QStringLiteral("shared-ownership"));
    QCOMPARE(targets.value("qtShared"), targets.value("qtCopy"));
    QCOMPARE(targets.value("qtShared"), targets.value("qtWeak"));
    QCOMPARE(kinds.value(targets.value("qtShared")),
             QStringLiteral("allocation-site"));
    QCOMPARE(roles.value("qtWeak"), QStringLiteral("observation"));
    QCOMPARE(roles.value("qtScoped"), QStringLiteral("ownership"));
    QCOMPARE(roles.value("watched"), QStringLiteral("observation"));
    QVERIFY(!graph["unknownTransfers"].toArray().isEmpty());
    const auto fieldsJson = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("void inspect"),
        QStringLiteral("OwnershipFields::inspect"), &error);
    QVERIFY2(!fieldsJson.isEmpty(), qPrintable(error));
    const auto fields = QJsonDocument::fromJson(fieldsJson)
                            .object()["experimentalFacts"]
                            .toObject()["facts"]
                            .toObject()["ownership"]
                            .toObject();
    int contained = 0, unknown = 0;
    for (const auto value : fields["edges"].toArray()) {
      const auto edge = value.toObject();
      contained += edge["from"] == QLatin1String("this") &&
                   edge["role"] == QLatin1String("value-containment");
      unknown += edge["role"] == QLatin1String("unknown");
    }
    QCOMPARE(contained, 5);
    QCOMPARE(unknown, 1);
  }

  void localDefinitionsJoinShadowKillAndUnknownCall() {
    QString error;
    const auto json = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("int local_slice_fixture"),
        QStringLiteral("local_slice_fixture"), &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    const auto graph = QJsonDocument::fromJson(json)
                           .object()["experimentalFacts"]
                           .toObject()["facts"]
                           .toObject()["localDefUseSlices"]
                           .toObject();
    QCOMPARE(graph["status"].toString(), QStringLiteral("experimental"));
    QSet<QString> valueIdentities, nodeIds;
    for (const auto value : graph["variables"].toArray()) {
      const auto variable = value.toObject();
      if (variable["name"] == QLatin1String("value"))
        valueIdentities.insert(variable["id"].toString());
    }
    QCOMPARE(valueIdentities.size(), 2);
    QString joinedId, returnId;
    QSet<QString> branchDefs, unknownDefs;
    QHash<QString, QString> texts;
    for (const auto value : graph["nodes"].toArray()) {
      const auto node = value.toObject();
      const auto id = node["id"].toString();
      nodeIds.insert(id);
      const auto text = node["source"].toObject()["text"].toString();
      texts[id] = text;
      if (text.startsWith("int joined"))
        joinedId = id;
      if (text.startsWith("return"))
        returnId = id;
      if ((text == QLatin1String("value = 10") ||
           text == QLatin1String("value = 20")) &&
          node["kind"] == QLatin1String("definition"))
        branchDefs.insert(id);
      if (node["kind"] == QLatin1String("unknown-call-effect")) {
        unknownDefs.insert(id);
        QVERIFY(node["unknownInfluence"].toBool());
      }
    }
    QVERIFY(!joinedId.isEmpty() && !returnId.isEmpty());
    QCOMPARE(branchDefs.size(), 2);
    QSet<QString> joinInputs, returnInputs;
    for (const auto value : graph["edges"].toArray()) {
      const auto edge = value.toObject();
      QVERIFY(nodeIds.contains(edge["from"].toString()));
      QVERIFY(nodeIds.contains(edge["to"].toString()));
      QVERIFY(edge["source"].toObject()["line"].toInt() > 0);
      if (edge["to"] == joinedId)
        joinInputs.insert(edge["from"].toString());
      if (edge["to"] == returnId)
        returnInputs.insert(edge["from"].toString());
    }
    QCOMPARE(joinInputs, branchDefs);
    QVERIFY(!unknownDefs.intersect(returnInputs).isEmpty());
    for (const auto &id : returnInputs)
      QVERIFY(texts.value(id) != QLatin1String("value = 10") &&
              texts.value(id) != QLatin1String("value = 20"));
  }

  void nestedFunctionBodiesAreOutsideSelectedScope() {
    QString error;
    const auto json = Unravel::Flow::generate(
        m_project.path(), m_source,
        functionLine("void nested_function_fixture"),
        QStringLiteral("nested_function_fixture"), &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    const auto facts = QJsonDocument::fromJson(json)
                           .object()["experimentalFacts"]
                           .toObject()["facts"]
                           .toObject();
    QCOMPARE(facts["enumAssignments"].toArray().size(), 2);
    for (const auto value : facts["ownership"].toObject()["nodes"].toArray())
      QVERIFY(value.toObject()["kind"] != QLatin1String("allocation-site"));
  }

  void emptyFunctionAndCallEvidence() {
    QString error;
    const auto empty = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("void empty_flow_fixture"),
        QStringLiteral("empty_flow_fixture"), &error);
    QVERIFY2(!empty.isEmpty(), qPrintable(error));
    const auto json = Unravel::Flow::generate(
        m_project.path(), m_source, functionLine("int local_slice_fixture"),
        QStringLiteral("local_slice_fixture"), &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    bool found = false;
    for (const auto block :
         QJsonDocument::fromJson(json).object()["blocks"].toArray())
      for (const auto statement : block.toObject()["statements"].toArray()) {
        const auto call = statement.toObject()["call"].toObject();
        if (call["qualifiedName"] == QLatin1String("unknown_effect")) {
          found = true;
          QVERIFY(call["source"].toObject()["line"].toInt() > 0);
        }
      }
    QVERIFY(found);
  }
};

QTEST_GUILESS_MAIN(TestControlFlow)
#include "tst_controlflow.moc"
