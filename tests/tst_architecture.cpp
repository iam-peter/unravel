#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "analysis.h"
#include "architecture.h"

using namespace Unravel;
using namespace Unravel::Architecture;

class TestArchitecture : public QObject
{
    Q_OBJECT

    static CodeModel model()
    {
        CodeModel result;
        result.root = QStringLiteral("/project");
        const qint32 ui = result.addFile(QStringLiteral("/project/src/ui/view.cpp"));
        const qint32 database = result.addFile(QStringLiteral("/project/src/database/store.h"));
        result.files[ui].relative = QStringLiteral("src/ui/view.cpp");
        result.files[ui].project = true;
        result.files[database].relative = QStringLiteral("src/database/store.h");
        result.files[database].project = true;
        result.files[ui].includes.append({database, 12});

        Symbol source;
        source.usr = QStringLiteral("source");
        source.name = QStringLiteral("render");
        source.qualified = QStringLiteral("UI::render");
        source.kind = SymbolKind::Function;
        source.def.file = ui;
        source.def.line = 4;
        source.def.endLine = 20;
        result.symbols.append(source);
        Symbol target;
        target.usr = QStringLiteral("target");
        target.name = QStringLiteral("Store");
        target.qualified = QStringLiteral("Database::Store");
        target.kind = SymbolKind::Class;
        target.decl.file = database;
        target.decl.line = 2;
        result.symbols.append(target);

        Reference reference;
        reference.from = 0;
        reference.to = 1;
        reference.kind = RefKind::TypeUse;
        reference.loc = {ui, 8, 9, 8, 14};
        result.refs.append(reference);
        return result;
    }

    static QByteArray config(const QByteArray &rules, const QByteArray &exceptions = {})
    {
        return QByteArray("{\"version\":1,\"layers\":["
                          "{\"id\":\"ui\",\"name\":\"UI\",\"paths\":[\"src/ui/**\"]},"
                          "{\"id\":\"database\",\"name\":\"Database\",\"paths\":[\"src/database/**\"]}],"
                          "\"rules\":") + rules + ",\"exceptions\":" +
               (exceptions.isEmpty() ? QByteArray("[]") : exceptions) + "}";
    }

private slots:
    void directedForbiddenDependency()
    {
        const Result result = check(model(), config(
            "[{\"id\":\"ui-no-db\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"semantic\"}]"));
        QVERIFY(result.valid);
        QCOMPARE(result.violations.size(), 1);
        const Violation &violation = result.violations.first();
        QCOMPARE(violation.ruleId, QStringLiteral("ui-no-db"));
        QCOMPARE(violation.sourceLayer, QStringLiteral("ui"));
        QCOMPARE(violation.targetLayer, QStringLiteral("database"));
        QCOMPARE(violation.evidence.size(), 1);
        QCOMPARE(violation.evidence.first().line, 8);
        QCOMPARE(violation.evidence.first().sourceSymbol, QStringLiteral("UI::render"));
        QCOMPARE(violation.evidence.first().targetSymbol, QStringLiteral("Database::Store"));
        QCOMPARE(violation.evidence.first().sourceFileId, 0);
        QCOMPARE(violation.evidence.first().targetFileId, 1);
        QVERIFY(result.toJson().contains("\"targetFileId\": 1"));
        QVERIFY(result.toJson().contains("\"defaultSemantics\": \"allow\""));
        QVERIFY(result.toMarkdown().contains(QStringLiteral("ui-no-db")));

        const Result explicitlyAllowed = check(model(), config(
            "[{\"id\":\"ui-no-db\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"semantic\"},"
            "{\"id\":\"ui-db-bridge\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"allow\",\"scope\":\"semantic\"}]"));
        QVERIFY(explicitlyAllowed.valid);
        QVERIFY(explicitlyAllowed.violations.isEmpty());
    }

    void malformedAndAmbiguousConfiguration()
    {
        Result result = check(model(), QByteArray("{\"version\":1,\"layers\":[],\"rules\":[],\"unknown\":true}"));
        QVERIFY(!result.valid);
        QVERIFY(!result.errors.isEmpty());
        QVERIFY(result.violations.isEmpty());

        const QByteArray overlapping =
            "{\"version\":1,\"layers\":["
            "{\"id\":\"ui\",\"name\":\"UI\",\"paths\":[\"src/**\"]},"
            "{\"id\":\"db\",\"name\":\"DB\",\"paths\":[\"src/ui/**\"]}],\"rules\":[]}";
        result = check(model(), overlapping);
        QVERIFY(!result.valid);
        QVERIFY(result.errors.join(QLatin1Char('\n')).contains(QStringLiteral("matches multiple layers")));
        QVERIFY(result.violations.isEmpty());

        result = check(model(), config(
            "[{\"id\":\"bad\",\"from\":\"missing\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"semantic\"}]"));
        QVERIFY(!result.valid);
        QVERIFY(result.errors.join(QLatin1Char('\n')).contains(QStringLiteral("unknown source layer")));
    }

    void includeAndSemanticScopesAreIndependent()
    {
        const Result includeOnly = check(model(), config(
            "[{\"id\":\"headers-only\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"includes\"}]"));
        QVERIFY(includeOnly.valid);
        QCOMPARE(includeOnly.violations.size(), 1);
        QCOMPARE(includeOnly.violations.first().scope, QStringLiteral("includes"));
        QCOMPARE(includeOnly.violations.first().evidence.first().line, 12);
        QVERIFY(includeOnly.violations.first().evidence.first().sourceSymbol.isEmpty());

        const Result both = check(model(), config(
            "[{\"id\":\"semantic\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"semantic\"},"
            "{\"id\":\"include\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"includes\"}]"));
        QVERIFY(both.valid);
        QCOMPARE(both.violations.size(), 2);
        QCOMPARE(both.violations.at(0).scope, QStringLiteral("includes"));
        QCOMPARE(both.violations.at(1).scope, QStringLiteral("semantic"));
    }

    void exactReasonedExceptionSuppressesOnlyItsFiles()
    {
        const QByteArray exceptions =
            "[{\"id\":\"migration-bridge\",\"rule\":\"ui-no-db\","
            "\"source\":\"src/ui/view.cpp\",\"target\":\"src/database/store.h\","
            "\"reason\":\"Temporary migration adapter\"}]";
        const Result result = check(model(), config(
            "[{\"id\":\"ui-no-db\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"semantic\"}]", exceptions));
        QVERIFY(result.valid);
        QVERIFY(result.violations.isEmpty());
        QCOMPARE(result.acceptedExceptions.size(), 1);
        QCOMPARE(result.acceptedExceptions.first().exceptionId, QStringLiteral("migration-bridge"));
        QVERIFY(result.toMarkdown().contains(QStringLiteral("Temporary migration adapter")));
        QVERIFY(result.toJson().contains("\"acceptedExceptions\""));

        CodeModel withOtherFile = model();
        const qint32 otherFile = withOtherFile.addFile(QStringLiteral("/project/src/ui/other.cpp"));
        withOtherFile.files[otherFile].relative = QStringLiteral("src/ui/other.cpp");
        withOtherFile.files[otherFile].project = true;
        Symbol otherSource;
        otherSource.usr = QStringLiteral("other-source");
        otherSource.name = QStringLiteral("refresh");
        otherSource.qualified = QStringLiteral("UI::refresh");
        otherSource.kind = SymbolKind::Function;
        otherSource.def.file = otherFile;
        otherSource.def.line = 2;
        otherSource.def.endLine = 9;
        withOtherFile.symbols.append(otherSource);
        Reference otherReference;
        otherReference.from = 2;
        otherReference.to = 1;
        otherReference.kind = RefKind::TypeUse;
        otherReference.loc = {otherFile, 4, 5, 4, 10};
        withOtherFile.refs.append(otherReference);
        const Result exact = check(withOtherFile, config(
            "[{\"id\":\"ui-no-db\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"semantic\"}]", exceptions));
        QVERIFY(exact.valid);
        QCOMPARE(exact.violations.size(), 1);
        QCOMPARE(exact.violations.first().evidence.first().sourceFile,
                 QStringLiteral("src/ui/other.cpp"));
        QCOMPARE(exact.acceptedExceptions.size(), 1);
    }

    void semanticEvidenceMapsToDirectedGraphLink()
    {
        CodeModel fixture = model();
        fixture.files[0].module = QStringLiteral("src/ui");
        fixture.files[1].module = QStringLiteral("src/database");
        fixture.moduleIndex.insert(QStringLiteral("src/ui"), 0);
        fixture.moduleIndex.insert(QStringLiteral("src/database"), 1);
        const Result result = check(fixture, config(
            "[{\"id\":\"ui-no-db\",\"from\":\"ui\",\"to\":\"database\","
            "\"effect\":\"forbid\",\"scope\":\"semantic\"}]"));
        QVERIFY(result.valid);
        QCOMPARE(result.violations.size(), 1);
        const qint32 referenceId = result.violations.first().evidence.first().referenceId;
        QVERIFY(referenceId >= 0);

        const Analysis::DepGraph graph = Analysis::dependencyGraph(fixture, Analysis::Level::File);
        const qint32 fromNode = graph.ids.indexOf(0);
        const qint32 toNode = graph.ids.indexOf(1);
        QVERIFY(fromNode >= 0 && toNode >= 0);
        const qint32 linkId = graph.linkIndex(fromNode, toNode);
        QVERIFY(linkId >= 0);
        QVERIFY(graph.links[linkId].refs.contains(referenceId));
    }
};

QTEST_GUILESS_MAIN(TestArchitecture)
#include "tst_architecture.moc"
