#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include "hotspots.h"
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
        write(repo.filePath(QStringLiteral("a.cpp")), "int answer() { return 42; }\n");
        git({QStringLiteral("add"), QStringLiteral("a.cpp")});
        git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")});
        write(repo.filePath(QStringLiteral("new.cpp")), "int uncommitted() { return 0; }\n");
        write(outside.filePath(QStringLiteral("other.cpp")), "int other() { return 1; }\n");

        Session session(nullptr);
        Hotspots hotspots(nullptr);
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
        session.open(outside.path());
        QTRY_VERIFY_WITH_TIMEOUT(session.root() == outside.path(), 15000);
        const QString outsideIndex = session.cachePath();
        QTRY_VERIFY_WITH_TIMEOUT(!hotspots.busy(), 10000);
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(!hotspots.ready());
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
