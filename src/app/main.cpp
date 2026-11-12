#include <QCommandLineParser>
#include <QDir>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

#include <memory>
#include <QtQml/qqmlextensionplugin.h>

#include "Fonts.h"
#include "changes.h"
#include "session.h"

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

    // UNRAVEL_PAN_BENCH=1: once indexed, pan the visible graph for 240 frames
    // and print the mean time per frame, for measuring the edge rendering
    if (qEnvironmentVariableIsSet("UNRAVEL_PAN_BENCH")) {
        QObject::connect(session, &Session::stateChanged, &app, [&engine, session] {
            if (session->state() != Session::Ready)
                return;
            QTimer::singleShot(3000, [&engine] {
                const auto roots = engine.rootObjects();
                auto *window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.first());
                if (!window)
                    return;
                // The visible edge layer, through the visual tree
                QQuickItem *layer = nullptr;
                QList<QQuickItem *> todo = {window->contentItem()};
                while (!todo.isEmpty() && !layer) {
                    QQuickItem *item = todo.takeFirst();
                    if (QByteArray(item->metaObject()->className()) == "EdgeLayer" && item->isVisible())
                        layer = item;
                    todo += item->childItems();
                }
                if (!layer)
                    return;
                auto frames = std::make_shared<int>(0);
                auto timer = std::make_shared<QElapsedTimer>();
                const double x0 = layer->property("panX").toDouble();
                timer->start();
                QObject::connect(window, &QQuickWindow::frameSwapped, layer, [layer, frames, timer, x0] {
                    if (*frames == 240) {
                        fprintf(stderr, "pan: %.2f ms per frame\n", timer->elapsed() / 240.0);
                        QCoreApplication::exit(0);
                        return;
                    }
                    ++*frames;
                    layer->setProperty("panX", x0 + *frames * 3);
                    layer->setProperty("zoom", layer->property("zoom").toDouble() * 1.0005);
                }, Qt::QueuedConnection);
                layer->setProperty("panX", x0 + 1);
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
