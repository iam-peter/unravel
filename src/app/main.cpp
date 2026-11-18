#include <QCommandLineParser>
#include <QDir>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

#include <memory>
#include <algorithm>
#include <QtQml/qqmlextensionplugin.h>

#include "Fonts.h"
#include "changes.h"
#include "session.h"
#include "views.h"

Q_IMPORT_QML_PLUGIN(IgittStylePlugin)

namespace {

// The mouse's thumb buttons go back and forth through the symbols looked at,
// as they do through pages in a browser, wherever the pointer is
class NavigationButtons : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *e = static_cast<QMouseEvent *>(event);
            if (e->button() == Qt::BackButton) {
                Session::instance()->back();
                return true;
            }
            if (e->button() == Qt::ForwardButton) {
                Session::instance()->forward();
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Unravel"));
    QGuiApplication::setApplicationName(QStringLiteral("Unravel"));
    QGuiApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));
    QGuiApplication::setDesktopFileName(QStringLiteral("unravel"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Unravel/logo/unravel.svg")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("See the architecture, flows and connections of C and C++ code"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("project"),
                                 QStringLiteral("A project folder or a compile_commands.json to open."));
    // For screenshots and smoke tests
    const QCommandLineOption grabOption(QStringLiteral("grab"), QStringLiteral("Save a screenshot of the window and quit."),
                                        QStringLiteral("png"));
    const QCommandLineOption viewOption(QStringLiteral("view"), QStringLiteral("The view to show first (graph, flow, "
                                        "architecture, matrix, bundles, treemap, metrics, findings)."), QStringLiteral("name"));
    const QCommandLineOption symbolOption(QStringLiteral("symbol"), QStringLiteral("The symbol to show first (qualified name)."),
                                          QStringLiteral("name"));
    const QCommandLineOption delayOption(QStringLiteral("grab-delay"), QStringLiteral("Milliseconds before grabbing."),
                                         QStringLiteral("ms"), QStringLiteral("2500"));
    const QCommandLineOption commitsOption(QStringLiteral("commits"),
                                           QStringLiteral("Commits to show in the Changes view: hashes or their beginnings, "
                                                          "comma-separated; \"worktree\" for what isn't committed."),
                                           QStringLiteral("hashes"));
    const QCommandLineOption foldOption(QStringLiteral("fold"),
                                        QStringLiteral("Folders to show as one node, comma-separated (relative to the project); "
                                                       "\"vendor\" for the third-party folders found."),
                                        QStringLiteral("folders"));
    parser.addOptions({grabOption, viewOption, symbolOption, delayOption, commitsOption, foldOption});
    parser.process(app);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QQuickStyle::setStyle(QStringLiteral("IgittStyle"));
    QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    Fonts::registerBundled();

    Session *session = Session::instance();
    if (parser.isSet(foldOption)) {
        const QStringList folders = parser.value(foldOption).split(QLatin1Char(','), Qt::SkipEmptyParts);
        auto once = std::make_shared<QMetaObject::Connection>();
        *once = QObject::connect(session, &Session::modelChanged, session, [session, folders, once] {
            QObject::disconnect(*once);
            for (const QString &f : folders) {
                if (f == QLatin1String("vendor"))
                    session->foldVendorFolders();
                else
                    session->fold(f);
            }
        });
    }
    Changes *changes = Changes::instance();
    if (parser.isSet(commitsOption)) {
        const QStringList revisions = parser.value(commitsOption).split(QLatin1Char(','), Qt::SkipEmptyParts);
        auto once = std::make_shared<QMetaObject::Connection>();
        *once = QObject::connect(changes, &Changes::commitsChanged, changes, [changes, revisions, once] {
            if (changes->commits().size() > 1) {
                QObject::disconnect(*once);
                changes->selectRevisions(revisions);
            }
        });
    }
    // UNRAVEL_DEBUG=1: what the session does, on stderr
    if (qEnvironmentVariableIsSet("UNRAVEL_DEBUG")) {
        QObject::connect(session, &Session::stateChanged, session, [session] {
            qInfo("%p state %d model %d error '%s'", (void *)session, session->state(), session->hasModel(), qPrintable(session->error()));
            for (const QString &n : session->notes())
                qInfo("  note: %s", qPrintable(n));
        });
    }

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.setInitialProperties({{QStringLiteral("startView"), parser.value(viewOption)},
                                 {QStringLiteral("startSymbol"), parser.value(symbolOption)}});
    engine.loadFromModule("Unravel", "Main");

    for (QObject *root : engine.rootObjects()) {
        if (auto *window = qobject_cast<QQuickWindow *>(root))
            window->installEventFilter(new NavigationButtons(window));
    }

    if (!parser.positionalArguments().isEmpty())
        session->open(QDir::current().absoluteFilePath(parser.positionalArguments().first()));

    // Paired files-baseline/files-overlay runs use identical geometry.
    if (qEnvironmentVariableIsSet("UNRAVEL_PAN_BENCH")) {
        QTimer::singleShot(180000, &app, [] {
            qCritical("Pan benchmark timed out before collecting 240 frames.");
            QCoreApplication::exit(2);
        });
        QObject::connect(session, &Session::stateChanged, &app, [&engine, session] {
            if (session->state() != Session::Ready)
                return;
            QTimer::singleShot(3000, [&engine] {
                const auto roots = engine.rootObjects();
                auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.first());
                if (!window)
                    return;
                // The visible edge layer, through the visual tree
                EdgeLayer *layer = nullptr;
                QList<QQuickItem *> todo = {window->contentItem()};
                while (!todo.isEmpty() && !layer) {
                    QQuickItem *item = todo.takeFirst();
                    if (item->isVisible())
                        layer = qobject_cast<EdgeLayer *>(item);
                    todo += item->childItems();
                }
                if (!layer || !layer->scene()) {
                    qCritical("Pan benchmark requires a visible graph edge layer.");
                    QCoreApplication::exit(2);
                    return;
                }
                const auto mode = qEnvironmentVariable("UNRAVEL_PAN_BENCH");
                const bool paired = mode == QLatin1String("files-baseline") ||
                                    mode == QLatin1String("files-overlay") ||
                                    mode == QLatin1String("files-paired");
                if (paired) {
                    auto *dependencies = qobject_cast<DependencyScene *>(layer->scene());
                    if (!dependencies) {
                        qCritical("Paired pan benchmark requires --view architecture.");
                        QCoreApplication::exit(2);
                        return;
                    }
                    dependencies->setLevel(1);
                }
                QTimer::singleShot(1000, layer, [layer, window, mode, paired] {
                if (!layer->scene() || layer->scene()->edgeCount() == 0) {
                    qCritical("Pan benchmark graph has no edges.");
                    QCoreApplication::exit(2);
                    return;
                }
                if (paired) {
                    for (auto &edge : layer->scene()->edges())
                        edge.forbidden = mode == QLatin1String("files-overlay");
                    emit layer->scene()->pathsChanged();
                }
                auto frames = std::make_shared<int>(-30);
                auto phase = std::make_shared<int>(0);
                auto means = std::make_shared<QVector<double>>();
                auto timer = std::make_shared<QElapsedTimer>();
                auto intervals = std::make_shared<QVector<double>>();
                auto previous = std::make_shared<qint64>(0);
                const double x0 = layer->property("panX").toDouble();
                const double zoom0 = layer->zoom();
                timer->start();
                QObject::connect(window, &QQuickWindow::frameSwapped, layer,
                                 [layer, frames, phase, means, timer, intervals, previous, mode, x0, zoom0] {
                    if (*frames < 0) {
                        ++*frames;
                        if (*frames == 0) {
                            timer->restart();
                            *previous = 0;
                        }
                        layer->setPanX(x0 + *frames * 3);
                        return;
                    }
                    const qint64 now = timer->nsecsElapsed();
                    intervals->append((now - *previous) / 1000000.0);
                    *previous = now;
                    ++*frames;
                    if (*frames == 240) {
                        const bool multiple = mode == QLatin1String("files-paired");
                        const double mean = now / 240000000.0;
                        means->append(mean);
                        std::sort(intervals->begin(), intervals->end());
                        const QJsonObject report{
                            {"benchmark", "pan"}, {"mode", multiple
                                ? (*phase % 2 ? QStringLiteral("files-overlay") : QStringLiteral("files-baseline")) : mode},
                            {"phase", *phase}, {"frames", 240},
                            {"meanMs", mean}, {"p95Ms", intervals->at(227)},
                            {"nodes", layer->scene()->nodes().size()},
                            {"edges", layer->scene()->edgeCount()},
                            {"policy", "240 frame-swapped wall intervals; includes display scheduling, not GPU-only time"}};
                        const auto json = QJsonDocument(report).toJson(QJsonDocument::Compact);
                        fprintf(stdout, "%s\n", json.constData());
                        if (multiple && ++*phase < 6) {
                            for (auto &edge : layer->scene()->edges())
                                edge.forbidden = *phase % 2;
                            emit layer->scene()->pathsChanged();
                            *frames = -30;
                            intervals->clear();
                            layer->setZoom(zoom0);
                            layer->setPanX(x0 - 90);
                            return;
                        }
                        if (multiple) {
                            const double baseline = ((*means)[0] + (*means)[2] + (*means)[4]) / 3;
                            const double overlay = ((*means)[1] + (*means)[3] + (*means)[5]) / 3;
                            const double increase = 100 * (overlay / baseline - 1);
                            const auto summary = QJsonDocument(QJsonObject{
                                {"benchmark", "pan-paired-summary"}, {"baselineMeanMs", baseline},
                                {"overlayMeanMs", overlay}, {"increasePercent", increase},
                                {"targetPass", increase <= 10}}).toJson(QJsonDocument::Compact);
                            fprintf(stdout, "%s\n", summary.constData());
                            QCoreApplication::exit(increase <= 10 ? 0 : 3);
                            return;
                        }
                        QCoreApplication::exit(0);
                        return;
                    }
                    layer->setProperty("panX", x0 + *frames * 3);
                    layer->setProperty("zoom", layer->property("zoom").toDouble() * 1.0005);
                }, Qt::QueuedConnection);
                layer->setProperty("panX", x0 + 1);
                });
            });
        });
    }

    if (parser.isSet(grabOption)) {
        const QString out = parser.value(grabOption);
        const int delay = parser.value(delayOption).toInt();
        auto grab = [&engine, out] {
            const auto roots = engine.rootObjects();
            auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.first());
            const bool saved = window && window->grabWindow().save(out);
            QCoreApplication::exit(saved ? 0 : 1);
        };
        // Once the project is indexed, then a moment for the views to settle
        QObject::connect(session, &Session::stateChanged, &app, [session, grab, delay] {
            if (session->state() == Session::Ready)
                QTimer::singleShot(delay, grab);
        });
        if (parser.positionalArguments().isEmpty() || session->state() == Session::Ready)
            QTimer::singleShot(delay, grab);
    }
    return app.exec();
}
