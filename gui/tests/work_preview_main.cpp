// work_preview_main — test and development preview & screenshot host for the
// Work shell. It is NOT installed and is NOT the production entry point; the
// production GUI (isvn-gui) loads its shell through the Isvn.Gui module.
//
// It loads gui/qml/shell/WorkPreviewHost.qml directly from the source tree so
// the shell can be rendered/captured without touching the production
// module entry. Screenshot capture, state selection, sizing, language, and
// theme are driven by command-line flags parsed inside WorkPreviewHost.qml:
//   --state=<id> --screenshot=<path> --width=N --height=N --lang=<code> --light
//
// Exit codes: 0 success; 1 QML load failure; 3/4/5/6 capture failures (see
// WorkPreviewHost.qml). This guarantees a hung capture cannot pass silently.
#include "backend/IsvnCli.h"
#include "backend/WorkSessionStore.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("isvn-work-preview"));
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

// Real IsvnCli bridge + session store for the read-mode preview
    // (driven by a fake `isvn` on PATH for deterministic screenshots).
    isvn::gui::IsvnCli cli;
    isvn::gui::WorkSessionStore session_store;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("previewIsvnCli"), &cli);
    engine.rootContext()->setContextProperty(QStringLiteral("previewSessionStore"), &session_store);
    engine.addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                    QStringLiteral("/qml/shell/WorkPreviewHost.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
