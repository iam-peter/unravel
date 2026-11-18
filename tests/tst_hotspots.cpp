#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include "hotspots.h"
#include "controlflow.h"
#include "reviewfocus.h"
#include "models.h"
#include "session.h"
#include "views.h"

class TestHotspots : public QObject
{
    Q_OBJECT

private slots:
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
        write(repo.filePath(QStringLiteral("new.cpp")), "int uncommitted() { return 0; }\n");
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
        QCOMPARE(hotspots.forFile(repo.filePath(QStringLiteral("a.cpp")))->commits, 1);
        QCOMPARE(hotspots.forFile(repo.filePath(QStringLiteral("new.cpp")))->commits, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!focus.busy() && focus.count() == 3, 10000);
        const int answer = session.symbolByName(QStringLiteral("answer"));
        const int helper = session.symbolByName(QStringLiteral("helper"));
        QVERIFY(answer >= 0 && helper >= 0);
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

        hotspots.refresh();
        hotspots.cancel();
        QVERIFY(!hotspots.ready());
        QCOMPARE(metrics.data(metrics.index(row(QStringLiteral("a.cpp")), 8), Qt::DisplayRole).toString(),
                 QStringLiteral("Unavailable"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(!hotspots.ready());

        hotspots.setDays(30);
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.busy(), 10000);
        QVERIFY(hotspots.ready());
        QCOMPARE(hotspots.days(), 30);
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
