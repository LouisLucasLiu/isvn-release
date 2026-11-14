#include "backend/LaunchPaths.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class LaunchPathsTest final : public QObject {
    Q_OBJECT

    static bool writeFile(const QString& path, QFileDevice::Permissions permissions) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write("#!/bin/sh\nexit 0\n");
        file.close();
        return file.setPermissions(permissions);
    }

    static constexpr QFileDevice::Permissions kExecutable =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    static constexpr QFileDevice::Permissions kPlain =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;

private slots:
    void pairedCliIsTheSiblingOneDirectoryUp() {
        QTemporaryDir bundle;
        QVERIFY(bundle.isValid());
        const QString root = QDir(bundle.path()).canonicalPath();
        // Both layouts place the GUI binary one directory below the CLI.
        for (const QString& gui_dir : {QStringLiteral("gui"), QStringLiteral("libexec")}) {
            QVERIFY(QDir(root).mkpath(gui_dir));
        }
        QVERIFY(writeFile(root + QStringLiteral("/isvn"), kExecutable));
        QString error;
        QCOMPARE(isvn::gui::pairedCliPath(root + QStringLiteral("/gui"), &error),
                 root + QStringLiteral("/isvn"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(isvn::gui::pairedCliPath(root + QStringLiteral("/libexec"), &error),
                 root + QStringLiteral("/isvn"));
    }

    void missingCliIsAnErrorNotAFallback() {
        QTemporaryDir bundle;
        const QString root = QDir(bundle.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("gui")));
        QString error;
        QVERIFY(isvn::gui::pairedCliPath(root + QStringLiteral("/gui"), &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("not found")), qPrintable(error));
        QVERIFY(error.contains(root + QStringLiteral("/isvn")));
    }

    void nonExecutableCliIsRejected() {
        QTemporaryDir bundle;
        const QString root = QDir(bundle.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("gui")));
        QVERIFY(writeFile(root + QStringLiteral("/isvn"), kPlain));
        QString error;
        QVERIFY(isvn::gui::pairedCliPath(root + QStringLiteral("/gui"), &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("not executable")), qPrintable(error));
    }

    void directoryNamedIsvnIsRejected() {
        QTemporaryDir bundle;
        const QString root = QDir(bundle.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("gui")));
        QVERIFY(QDir(root).mkpath(QStringLiteral("isvn")));
        QString error;
        QVERIFY(isvn::gui::pairedCliPath(root + QStringLiteral("/gui"), &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("not a regular file")), qPrintable(error));
    }

    void symbolicLinkCliIsRejected() {
        QTemporaryDir bundle;
        QTemporaryDir elsewhere;
        const QString root = QDir(bundle.path()).canonicalPath();
        const QString outside = QDir(elsewhere.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("gui")));
        QVERIFY(writeFile(outside + QStringLiteral("/isvn"), kExecutable));
        QVERIFY(QFile::link(outside + QStringLiteral("/isvn"), root + QStringLiteral("/isvn")));
        QString error;
        QVERIFY(isvn::gui::pairedCliPath(root + QStringLiteral("/gui"), &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("symbolic link")), qPrintable(error));
    }

    void danglingSymbolicLinkCliIsRejected() {
        QTemporaryDir bundle;
        const QString root = QDir(bundle.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("gui")));
        QVERIFY(QFile::link(root + QStringLiteral("/absent"), root + QStringLiteral("/isvn")));
        QString error;
        QVERIFY(isvn::gui::pairedCliPath(root + QStringLiteral("/gui"), &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("symbolic link")), qPrintable(error));
    }

    void guiDirectoryReachedThroughALinkResolvesToItsRealParent() {
        // A release slot is entered through a `bin` link; the pair must come
        // from the real slot directory, not from beside the link.
        QTemporaryDir bundle;
        const QString root = QDir(bundle.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("slot/libexec")));
        QVERIFY(QDir(root).mkpath(QStringLiteral("other")));
        QVERIFY(writeFile(root + QStringLiteral("/slot/isvn"), kExecutable));
        QVERIFY(writeFile(root + QStringLiteral("/other/isvn"), kExecutable));
        QVERIFY(QFile::link(root + QStringLiteral("/slot/libexec"), root + QStringLiteral("/other/libexec")));
        QString error;
        QCOMPARE(isvn::gui::pairedCliPath(root + QStringLiteral("/other/libexec"), &error),
                 root + QStringLiteral("/slot/isvn"));
    }

    void missingGuiDirectoryIsAnError() {
        QString error;
        QVERIFY(isvn::gui::pairedCliPath(QStringLiteral("/nonexistent/isvn-gui-dir"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void workspaceRootIsTheNearestDirectoryWithLocalState() {
        QTemporaryDir area;
        const QString root = QDir(area.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("ws/.isvn_local")));
        QVERIFY(QDir(root).mkpath(QStringLiteral("ws/P/B/S/V")));
        QCOMPARE(isvn::gui::workspaceRootFor(root + QStringLiteral("/ws")), root + QStringLiteral("/ws"));
        QCOMPARE(isvn::gui::workspaceRootFor(root + QStringLiteral("/ws/P/B/S/V")),
                 root + QStringLiteral("/ws"));
        QVERIFY(isvn::gui::workspaceRootFor(root).isEmpty());
        QVERIFY(isvn::gui::workspaceRootFor(root + QStringLiteral("/absent")).isEmpty());
    }

    void fileNamedLocalStateDoesNotMakeAWorkspace() {
        QTemporaryDir area;
        const QString root = QDir(area.path()).canonicalPath();
        QVERIFY(QDir(root).mkpath(QStringLiteral("plain")));
        QVERIFY(writeFile(root + QStringLiteral("/plain/.isvn_local"), kPlain));
        QVERIFY(isvn::gui::workspaceRootFor(root + QStringLiteral("/plain")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(LaunchPathsTest)
#include "test_launch_paths.moc"
