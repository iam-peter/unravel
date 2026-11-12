#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>
#include <QtQml/qqmlextensionplugin.h>

#include "Fonts.h"
#include "session.h"

Q_IMPORT_QML_PLUGIN(IgittStylePlugin)

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
    parser.addOptions({grabOption, viewOption, symbolOption, delayOption});
    parser.process(app);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QQuickStyle::setStyle(QStringLiteral("IgittStyle"));
    QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    Fonts::registerBundled();

    Session *session = Session::instance();
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

    if (!parser.positionalArguments().isEmpty())
        session->open(QDir::current().absoluteFilePath(parser.positionalArguments().first()));

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
