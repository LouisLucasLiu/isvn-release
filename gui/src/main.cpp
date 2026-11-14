#include "backend/GuiInstanceCoordinator.h"
#include "backend/IsvnCli.h"
#include "backend/LaunchPaths.h"
#include "backend/RoleResolver.h"
#include "backend/SafeNotifier.h"
#include "backend/WorkSessionStore.h"
#include "version.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>
#include <QTimer>

namespace {

enum class ShellSelection { New, Legacy, Invalid };

ShellSelection selectShell(QString* error) {
    const QByteArray raw = qgetenv("ISVN_GUI_SHELL");
    if (!qEnvironmentVariableIsSet("ISVN_GUI_SHELL") || raw == "new") return ShellSelection::New;
    if (raw == "legacy") return ShellSelection::Legacy;
    if (error != nullptr) {
        *error = QStringLiteral("ISVN_GUI_SHELL must be exactly 'new' or 'legacy'");
    }
    return ShellSelection::Invalid;
}

QString canonicalStartPath() {
    const QFileInfo info(QDir::currentPath());
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName(QStringLiteral("isvn"));
    QGuiApplication::setOrganizationName(QStringLiteral("isvn"));
    QGuiApplication::setApplicationName(QStringLiteral("isvn-gui"));
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QString shell_error;
    const ShellSelection shell = selectShell(&shell_error);
    if (shell == ShellSelection::Invalid) {
        QTextStream(stderr) << shell_error << Qt::endl;
        return 2;
    }
    const QString shell_mode = shell == ShellSelection::New ? QStringLiteral("new")
                                                              : QStringLiteral("legacy");

    // The GUI drives only the CLI shipped next to it.  Without that CLI no
    // page is loaded; a different `isvn` found on PATH is never used instead.
    QString cli_error;
    const QString paired_cli =
        isvn::gui::pairedCliPath(QCoreApplication::applicationDirPath(), &cli_error);
    if (paired_cli.isEmpty()) {
        QTextStream(stderr) << "isvn-gui: " << cli_error << Qt::endl
                            << "isvn-gui: refusing to start without its own isvn CLI" << Qt::endl;
        return 5;
    }

    isvn::gui::RoleResolver role_resolver;
    role_resolver.refresh(canonicalStartPath());
    // The canonical project directory is preferred.  A denied/not-project
    // launch still gets a stable cwd scope, but never a Project page root.
    const QString instance_identity = role_resolver.projectDir().isEmpty()
                                          ? canonicalStartPath()
                                          : role_resolver.projectDir();
    isvn::gui::GuiInstanceCoordinator instance;
    QString instance_error;
    const auto instance_result = instance.acquire(instance_identity, shell_mode, &instance_error);
    if (instance_result == isvn::gui::GuiInstanceCoordinator::AcquireResult::ActivatedExisting) {
        QTextStream(stdout) << "isvn-gui activation acknowledged" << Qt::endl;
        return 0;
    }
    if (instance_result == isvn::gui::GuiInstanceCoordinator::AcquireResult::ModeConflict) {
        QTextStream(stderr) << instance_error << Qt::endl;
        return 3;
    }
    if (instance_result != isvn::gui::GuiInstanceCoordinator::AcquireResult::Owner) {
        QTextStream(stderr) << instance_error << Qt::endl;
        return 4;
    }

    isvn::gui::IsvnCli cli;
    cli.setExecutable(paired_cli);
    cli.setMaintenanceRole(role_resolver.roleName());
    isvn::gui::WorkSessionStore session_store;
    session_store.setProjectKey(instance_identity);
    isvn::gui::SafeNotifier notifier;

    // Identity of this GUI binary, taken from the same compiled-in values the
    // CLI reports with `isvn version`.
    const QString app_version = QString::fromLatin1(isvn::version::appVersion());
    const QString git_commit = QString::fromLatin1(isvn::version::gitCommit());
    const QString build_type = QString::fromLatin1(isvn::version::buildType());
    QVariantMap build_info;
    build_info.insert(QStringLiteral("program"), QStringLiteral("isvn-gui"));
    build_info.insert(QStringLiteral("version"), app_version);
    build_info.insert(QStringLiteral("commit"), git_commit);
    build_info.insert(QStringLiteral("buildTime"), QString::fromLatin1(isvn::version::buildTime()));
    build_info.insert(QStringLiteral("buildType"), build_type);
    build_info.insert(QStringLiteral("qt"), QString::fromLatin1(QT_VERSION_STR));
    build_info.insert(QStringLiteral("buildIdentity"),
                      QStringLiteral("isvn-gui %1 · %2 · %3").arg(app_version, git_commit, build_type));
    build_info.insert(QStringLiteral("shellMode"), shell_mode);
    build_info.insert(QStringLiteral("rootIdentity"),
                      shell == ShellSelection::New ? QStringLiteral("ProductShellWindow")
                                                   : QStringLiteral("LegacyMain"));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("isvnCli"), &cli);
    engine.rootContext()->setContextProperty(QStringLiteral("roleResolver"), &role_resolver);
    engine.rootContext()->setContextProperty(QStringLiteral("workSessionStore"), &session_store);
    engine.rootContext()->setContextProperty(QStringLiteral("safeNotifier"), &notifier);
    engine.rootContext()->setContextProperty(QStringLiteral("buildInfo"), build_info);
    engine.rootContext()->setContextProperty(QStringLiteral("guiShellMode"), shell_mode);
    QVariantMap launch_context;
    launch_context.insert(QStringLiteral("cliExecutable"), paired_cli);
    launch_context.insert(QStringLiteral("workspaceRoot"),
                          isvn::gui::workspaceRootFor(canonicalStartPath()));
    engine.rootContext()->setContextProperty(QStringLiteral("launchContext"), launch_context);

    const QString root = shell == ShellSelection::Legacy
                             ? QStringLiteral("Main")
                             : (role_resolver.allowed() ? QStringLiteral("ProductShellWindow")
                                                         : QStringLiteral("AccessDeniedWindow"));
    engine.loadFromModule("Isvn.Gui", root);
    if (engine.rootObjects().isEmpty()) return 1;

    QObject::connect(&instance, &isvn::gui::GuiInstanceCoordinator::activationRequested, &app,
                     [&engine]() {
                         if (engine.rootObjects().isEmpty()) return;
                         QMetaObject::invokeMethod(engine.rootObjects().constFirst(), "reactivate");
                     });

    // Test-only root identity evidence.  It is intentionally opt-in and
    // contains no Project or operation data.
    if (qEnvironmentVariableIntValue("ISVN_GUI_TEST_EXIT_AFTER_ROOT") != 0) {
        QTextStream(stdout) << "isvn-gui root=" << build_info.value(QStringLiteral("rootIdentity")).toString()
                            << Qt::endl
                            << "isvn-gui build=" << build_info.value(QStringLiteral("buildIdentity")).toString()
                            << Qt::endl
                            << "isvn-gui cli=" << paired_cli << Qt::endl;
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
