#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include "hotspots.h"
#include "changes.h"
#include "controlflow.h"
#include "reviewfocus.h"
#include "models.h"
#include "session.h"
#include "views.h"

class TestHotspots : public QObject
{
    Q_OBJECT

private slots:
    void cancellingAndReindexingPreserveSessionStateAndFileIdentity()
    {
        QTemporaryDir storage, repo;
        QVERIFY(storage.isValid() && repo.isValid());
        qputenv("XDG_CACHE_HOME", storage.path().toUtf8());
        auto write = [&](const QString &name, const QByteArray &text) {
            QFile file(repo.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(text), text.size());
        };
        write(QStringLiteral("a.h"), "class A {};\n");
        write(QStringLiteral("main.cpp"), "#include \"a.h\"\nint answer() { return 42; }\n");
        Session session(nullptr);
        bool stopped = false;
        connect(&session, &Session::stateChanged, &session, [&] {
            if (session.state() == Session::Indexing && !stopped) {
                stopped = true;
                session.cancel();
                QCOMPARE(session.state(), Session::Idle);
                QCOMPARE(session.progress(), 0);
                QVERIFY(!session.hasModel());
                session.reindex();
            }
        });
        session.open(repo.path());
        QTRY_VERIFY_WITH_TIMEOUT(stopped && session.state() == Session::Ready, 10000);
        QVERIFY(session.hasModel());
        session.activateFile(session.model()->fileId(repo.filePath(QStringLiteral("a.h"))), 1);
        session.reindex();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::Ready, 10000);
        QCOMPARE(session.model()->files[session.activeFile()].path, repo.filePath(QStringLiteral("a.h")));
        QCOMPARE(session.focusLine(), 1);

        write(QStringLiteral("b.h"), "class B {};\n");
        write(QStringLiteral("main.cpp"), "#include \"b.h\"\nint answer() { return 42; }\n");
        QVERIFY(QFile::remove(repo.filePath(QStringLiteral("a.h"))));
        session.reindex();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::Ready, 10000);
        QCOMPARE(session.activeFile(), -1);
        QVERIFY(session.activeSymbol() >= 0);
        const QString cache = session.cachePath();
        session.close();
        QVERIFY(QFile::remove(cache));
    }

    void compactCfgPreservesBranchesCallsAndCycles()
    {
        const QVariantMap source{{QStringLiteral("file"), QStringLiteral("fixture.cpp")},
                                 {QStringLiteral("line"), 2}};
        const QVariantMap call{{QStringLiteral("call"), QVariantMap{
            {QStringLiteral("qualifiedName"), QStringLiteral("callee")},
            {QStringLiteral("source"), source}}}};
        QVariantList blocks;
        for (int i = 0; i < 6; ++i)
            blocks.append(QVariantMap{{QStringLiteral("id"), i},
                {QStringLiteral("source"), source},
                {QStringLiteral("statements"), QVariantList{call}}});
        auto entry = blocks[0].toMap();
        entry[QStringLiteral("entry")] = true;
        blocks[0] = entry;
        auto branch = blocks[3].toMap();
        branch[QStringLiteral("terminatorKind")] = QStringLiteral("IfStmt");
        blocks[3] = branch;
        QVariantList edges;
        auto link = [&](int from, int to, const QString &kind = QStringLiteral("flow")) {
            edges.append(QVariantMap{{QStringLiteral("from"), from},
                {QStringLiteral("to"), to}, {QStringLiteral("kind"), kind}});
        };
        link(0, 1); link(1, 2); link(2, 3);
        link(3, 4, QStringLiteral("branch"));
        link(3, 5, QStringLiteral("branch"));
        const auto compact = ControlFlow::compactGraph(blocks, edges);
        QVERIFY(!compact.contains(QStringLiteral("error")));
        const auto regions = compact.value(QStringLiteral("blocks")).toList();
        QCOMPARE(regions.size(), 4);
        bool merged = false;
        QSet<int> retained;
        for (const auto &value : regions) {
            const auto region = value.toMap();
            const auto ids = region.value(QStringLiteral("blockIds")).toList();
            for (const auto &id : ids)
                retained.insert(id.toInt());
            if (ids.size() == 3) {
                merged = true;
                QCOMPARE(ids, (QVariantList{1, 2, 3}));
                QCOMPARE(region.value(QStringLiteral("statements")).toList().size(), 3);
                QCOMPARE(region.value(QStringLiteral("terminatorKind")).toString(), QStringLiteral("IfStmt"));
                QCOMPARE(region.value(QStringLiteral("index")).toInt(), 1);
            }
        }
        QVERIFY(merged);
        QCOMPARE(retained.size(), 6);
        QCOMPARE(compact.value(QStringLiteral("edges")).toList().size(), 3);
        edges.clear();
        link(1, 2); link(2, 1);
        const auto cycle = ControlFlow::compactGraph(blocks.mid(1, 2), edges);
        QCOMPARE(cycle.value(QStringLiteral("edges")).toList().size(), 1);
        QVERIFY(!ControlFlow::compactGraph(blocks.mid(1, 1), edges)
                     .value(QStringLiteral("error")).toString().isEmpty());
    }

    void historyViews()
    {
        QTemporaryDir storage, repo, outside;
        QVERIFY(storage.isValid() && repo.isValid() && outside.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("UnravelTests"));
        QCoreApplication::setApplicationName(QStringLiteral("Hotspots"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, storage.path());
        qputenv("XDG_CACHE_HOME", storage.path().toUtf8());
        auto write = [&](const QString &path, const QByteArray &text) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(text), text.size());
        };
        auto git = [&](const QStringList &args) {
            QProcess process;
            process.setWorkingDirectory(repo.path());
            auto env = QProcessEnvironment::systemEnvironment();
            const QString date = QStringLiteral("@%1 +0000").arg(QDateTime::currentSecsSinceEpoch() - 86400);
            env.insert(QStringLiteral("GIT_AUTHOR_DATE"), date);
            env.insert(QStringLiteral("GIT_COMMITTER_DATE"), date);
            process.setProcessEnvironment(env);
            process.start(QStringLiteral("git"), QStringList{
                QStringLiteral("-c"), QStringLiteral("user.name=Test"),
                QStringLiteral("-c"), QStringLiteral("user.email=test@example.test"),
                QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false"),
                QStringLiteral("-c"), QStringLiteral("core.hooksPath=") + storage.path()} + args);
            QVERIFY(process.waitForFinished(10000));
            const QByteArray error = process.readAllStandardError();
            QVERIFY2(process.exitCode() == 0, error.constData());
        };
        git({QStringLiteral("init"), QStringLiteral("-q")});
        write(repo.filePath(QStringLiteral("a.cpp")), "int helper() { return 42; } int answer() { return helper(); }\n");
        git({QStringLiteral("add"), QStringLiteral("a.cpp")});
        git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")});
        write(repo.filePath(QStringLiteral("compile_commands.json")),
            QJsonDocument(QJsonArray{QJsonObject{
                {QStringLiteral("directory"), repo.path()},
                {QStringLiteral("file"), repo.filePath(QStringLiteral("a.cpp"))},
                {QStringLiteral("arguments"), QJsonArray{"clang++", "-std=c++20", "-c", "a.cpp"}}},
                QJsonObject{
                {QStringLiteral("directory"), repo.path()},
                {QStringLiteral("file"), repo.filePath(QStringLiteral("new.cpp"))},
                {QStringLiteral("arguments"), QJsonArray{"clang++", "-std=c++20", "-c", "new.cpp"}}}}).toJson());
        write(repo.filePath(QStringLiteral("new.cpp")), "int uncommitted() { return 0; }\n");
        git({QStringLiteral("add"), QStringLiteral("new.cpp")});
        write(outside.filePath(QStringLiteral("other.cpp")), "int other() { return 1; }\n");

        Session session(nullptr);
        Hotspots hotspots(nullptr);
        ReviewFocus focus;
        ControlFlow flow;
        focus.setShowAll(true);
        MetricsTable metrics;
        metrics.setMode(MetricsTable::Files);
        TreemapView map;
        map.setWidth(600);
        map.setHeight(400);
        map.setColorMetric(TreemapView::Churn);
        session.open(repo.path());
        QTRY_VERIFY_WITH_TIMEOUT(session.hasModel(), 15000);
        const QString repoIndex = session.cachePath();
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.busy(), 10000);
        QVERIFY2(hotspots.ready(), qPrintable(hotspots.error()));
        QCOMPARE(hotspots.days(), 180);
        // Checked first: a path that doesn't match the repository's fails
        // here rather than crashing the run
        const auto *a = hotspots.forFile(repo.filePath(QStringLiteral("a.cpp")));
        const auto *added = hotspots.forFile(repo.filePath(QStringLiteral("new.cpp")));
        QVERIFY(a && added);
        QCOMPARE(a->commits, 1);
        QCOMPARE(added->commits, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy() && focus.count() == 3, 10000);
        const int answer = session.symbolByName(QStringLiteral("answer"));
        const int helper = session.symbolByName(QStringLiteral("helper"));
        QVERIFY(answer >= 0 && helper >= 0);
        focus.setFunctionHistory(true);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy(), 10000);
        QVERIFY2(focus.summary().contains(QStringLiteral("first-parent non-merge function-hunk estimate")),
                 qPrintable(focus.summary()));
        QVERIFY(focus.summary().contains(QStringLiteral("post-revision/current-tree changes are mapping-only")));
        const QJsonObject functionReport = QJsonDocument::fromJson(focus.json().toUtf8()).object();
        QCOMPARE(functionReport.value(QStringLiteral("mode")).toString(),
                 QStringLiteral("historical-function-hunk-estimate"));
        QVERIFY(functionReport.value(QStringLiteral("historySemantics")).toString()
                    .contains(QStringLiteral("merge commits are not counted")));
        QVERIFY(functionReport.value(QStringLiteral("postRevisionChangedFiles")).toInt() > 0);
        focus.setFunctionHistory(false);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy(), 10000);

        const auto definition = session.model()->symbols[answer].def;
        hotspots.loadBlame(definition.file, definition.line);
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.blameBusy(), 10000);
        QVERIFY2(hotspots.blameText().contains(QStringLiteral("initial")), qPrintable(hotspots.blameText()));
        QVERIFY(QDir().mkpath(repo.filePath(QStringLiteral("nested"))));
        const auto nestedBlame = Unravel::History::blame(repo.filePath(QStringLiteral("nested")),
                                                       repo.filePath(QStringLiteral("a.cpp")), 1);
        QVERIFY2(nestedBlame.error.isEmpty(), qPrintable(nestedBlame.error));
        QVERIFY(nestedBlame.text.contains(QStringLiteral("initial")));
        QCOMPARE(Unravel::History::blame(repo.path(), repo.filePath(QStringLiteral("a.cpp")), 0).text, QString());
        QVERIFY(!Unravel::History::blame(repo.path(), repo.filePath(QStringLiteral("a.cpp")), 0).error.isEmpty());

        flow.analyze(definition.file, definition.line, QStringLiteral("answer"));
        if (flow.available()) {
            QVERIFY(flow.busy());
            flow.cancel();
            QVERIFY(!flow.busy());
            QCoreApplication::processEvents();
            QVERIFY(flow.blocks().isEmpty());
            flow.analyze(definition.file, definition.line, QStringLiteral("answer"));
            QTRY_VERIFY_WITH_TIMEOUT(!flow.busy(), 10000);
            QVERIFY2(flow.error().isEmpty(), qPrintable(flow.error()));
            QVERIFY(!flow.diagram().value(QStringLiteral("nodes")).toList().isEmpty());
            const auto local = flow.experimentalFacts().value(QStringLiteral("facts")).toMap()
                                   .value(QStringLiteral("localDefUseSlices")).toMap();
            const auto nodes = local.value(QStringLiteral("nodes")).toList();
            QVERIFY(!nodes.isEmpty());
            const auto seed = nodes.first().toMap().value(QStringLiteral("id")).toString();
            const auto backward = flow.slice(seed, false);
            QVERIFY(backward.value(QStringLiteral("error")).toString().isEmpty());
            QVERIFY(!backward.value(QStringLiteral("nodes")).toList().isEmpty());
            QVERIFY(!flow.slice(QStringLiteral("missing"), false)
                         .value(QStringLiteral("error")).toString().isEmpty());
            QVERIFY(!flow.evidenceDiagram(backward.value(QStringLiteral("nodes")).toList(),
                                          backward.value(QStringLiteral("edges")).toList())
                         .contains(QStringLiteral("error")));
        } else {
            QVERIFY(!flow.busy());
            QVERIFY(!flow.error().isEmpty());
        }
        const auto relations = session.queryRelations(answer, QStringLiteral("callees"), 1);
        QVERIFY(relations.value(QStringLiteral("error")).toString().isEmpty());
        QCOMPARE(relations.value(QStringLiteral("rows")).toList().size(), 1);
        QCOMPARE(relations.value(QStringLiteral("rows")).toList().first().toMap().value(QStringLiteral("symbol")).toInt(), helper);
        QVERIFY(session.queryRelations(answer, QStringLiteral("callees"), 1, true).value(QStringLiteral("rows")).toList().isEmpty());
        QVERIFY(!session.queryRelations(answer, QStringLiteral("unknown")).value(QStringLiteral("error")).toString().isEmpty());
        focus.setChangedOnly(true);
        focus.setHistorical(false);
        focus.setChangedOnly(false);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy() && focus.count() == 3, 10000);
        QCOMPARE(QJsonDocument::fromJson(focus.json().toUtf8()).object().value(QStringLiteral("mode")).toString(), QStringLiteral("static"));
        QVERIFY(focus.priorityFor(answer) > 0);

        auto row = [&](const QString &name) {
            for (int i = 0; i < metrics.rowCount(); ++i) {
                if (metrics.rowInfo(i).value(QStringLiteral("name")).toString() == name)
                    return i;
            }
            return -1;
        };
        const int tracked = row(QStringLiteral("a.cpp"));
        QVERIFY(tracked >= 0);
        QCOMPARE(metrics.data(metrics.index(tracked, 8), Qt::DisplayRole).toString(), QStringLiteral("1"));
        QCOMPARE(metrics.data(metrics.index(tracked, 9), Qt::DisplayRole).toString(), QStringLiteral("1"));
        QCOMPARE(metrics.data(metrics.index(tracked, 10), Qt::DisplayRole).toString(), QStringLiteral("0"));
        QVERIFY(metrics.toCsv().contains(QStringLiteral("revision=") + hotspots.activity().revision));
        QVERIFY(metrics.toCsv().contains(QStringLiteral("renames not followed")));
        QTRY_VERIFY_WITH_TIMEOUT(metrics.data(metrics.index(row(QStringLiteral("a.cpp")), 13), MetricsTable::ValueRole).toDouble() > 0, 10000);

        bool foundFile = false;
        for (int y = 0; y < 400 && !foundFile; y += 5) {
            for (int x = 0; x < 600 && !foundFile; x += 5) {
                map.hover(x, y);
                const auto info = map.hoveredInfo();
                if (info.value(QStringLiteral("name")).toString() == QLatin1String("a.cpp")) {
                    QCOMPARE(info.value(QStringLiteral("symbol")).toInt(), -1);
                    QCOMPARE(info.value(QStringLiteral("value")).toInt(), 1);
                    foundFile = true;
                }
            }
        }
        QVERIFY(foundFile);

        focus.setHistorical(true);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy(), 10000);
        QTest::qWait(20);
        write(repo.filePath(QStringLiteral("a.cpp")), "int helper() { return 43; } int answer() { return helper(); }\n");
        focus.setFunctionHistory(true);
        QTRY_VERIFY_WITH_TIMEOUT(session.stale(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy(), 15000);
        QVERIFY(focus.summary().contains(QStringLiteral("Function-hunk estimate unavailable")));
        QVERIFY(focus.summary().contains(QStringLiteral("current index")));
        const QJsonObject staleFunctionReport = QJsonDocument::fromJson(focus.json().toUtf8()).object();
        QCOMPARE(staleFunctionReport.value(QStringLiteral("mode")).toString(),
                 QStringLiteral("historical-function-hunk-estimate"));
        QVERIFY(!staleFunctionReport.value(QStringLiteral("error")).toString().isEmpty());

        focus.setFunctionHistory(false);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy(), 10000);
        QVERIFY(focus.summary().contains(QStringLiteral("file-history proxy over all reachable non-merge commits")));

        hotspots.refresh();
        hotspots.cancel();
        QVERIFY(!hotspots.ready());
        QCOMPARE(metrics.data(metrics.index(row(QStringLiteral("a.cpp")), 8), Qt::DisplayRole).toString(),
                 QStringLiteral("Unavailable"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(!hotspots.ready());

        const int indexedRevision = session.revision();
        hotspots.setDays(30);
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.busy(), 10000);
        QVERIFY(hotspots.ready());
        QCOMPARE(hotspots.days(), 30);
        QCOMPARE(session.revision(), indexedRevision);
        // A refresh from the first project must not be published in the second.
        hotspots.refresh();
        hotspots.loadBlame(definition.file, definition.line);
        if (flow.available())
            flow.analyze(definition.file, definition.line, QStringLiteral("answer"));
        session.open(outside.path());
        QTRY_VERIFY_WITH_TIMEOUT(session.root() == outside.path(), 15000);
        const QString outsideIndex = session.cachePath();
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.busy(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy() && focus.count() == 1, 10000);
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(!hotspots.ready());
        QVERIFY(!hotspots.blameBusy());
        QVERIFY(hotspots.blameText().isEmpty());
        QVERIFY(!flow.busy());
        QVERIFY(flow.blocks().isEmpty());
        QVERIFY(hotspots.error().isEmpty());
        QVERIFY(!hotspots.forFile(repo.filePath(QStringLiteral("a.cpp"))));
        QCOMPARE(hotspots.days(), 180);
        QVERIFY(!metrics.toCsv().contains(QStringLiteral("revision=")));
        session.open(repo.path());
        QTRY_VERIFY_WITH_TIMEOUT(session.root() == repo.path(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.busy(), 10000);
        QVERIFY(hotspots.ready());
        QCOMPARE(hotspots.days(), 30);
        session.reindex();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::Ready, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.busy() && !Changes::instance()->busy() && !focus.busy(), 10000);
        QVERIFY(!session.stale());
        focus.setFunctionHistory(true);
        QVERIFY(focus.busy());
        QThreadPool::globalInstance()->waitForDone();
        QTest::qSleep(5);
        write(repo.filePath(QStringLiteral("a.cpp")),
              "// moved\nint helper() { return 43; } int answer() { return helper(); }\n");
        QTRY_VERIFY_WITH_TIMEOUT(session.stale() && !focus.busy(), 10000);
        QCOMPARE(focus.count(), 0);
        QVERIFY(focus.summary().contains(QStringLiteral("current index")));
        session.close();
        QVERIFY(!hotspots.ready());
        QThreadPool::globalInstance()->waitForDone();
        if (QFile::exists(repoIndex))
            QVERIFY(QFile::remove(repoIndex));
        if (QFile::exists(outsideIndex))
            QVERIFY(QFile::remove(outsideIndex));
    }
};

QTEST_MAIN(TestHotspots)
#include "tst_hotspots.moc"
