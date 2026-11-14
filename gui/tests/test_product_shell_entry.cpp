#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

struct ProcessResult {
    int exitCode = -1;
    QByteArray stdOut;
    QByteArray stdErr;
};

class ProductShellEntryTest final : public QObject {
    Q_OBJECT

    static ProcessResult run(const QString& shell) {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        QTemporaryDir runtime;
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("ISVN_GUI_TEST_EXIT_AFTER_ROOT"), QStringLiteral("1"));
        environment.insert(QStringLiteral("ISVN_GUI_TEST_RUNTIME_DIR"), runtime.path());
        if (shell.isNull()) environment.remove(QStringLiteral("ISVN_GUI_SHELL"));
        else environment.insert(QStringLiteral("ISVN_GUI_SHELL"), shell);
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(QStringLiteral(ISVN_GUI_PROJECT_DIR));
        process.start(QStringLiteral(ISVN_GUI_BINARY));
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished();
        }
        return {process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError()};
    }

    static QString lineValue(const QByteArray& output, const QByteArray& key) {
        for (const QByteArray& line : output.split('\n'))
            if (line.startsWith(key)) return QString::fromUtf8(line.mid(key.size())).trimmed();
        return {};
    }

    static bool writeScript(const QString& path, const QByteArray& body) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write(body);
        file.close();
        return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner);
    }

    // A private copy of the GUI binary in its build-tree position (<root>/gui),
    // so the directory above it can be arranged per test.
    static bool copyGui(const QString& root) {
        if (!QDir(root).mkpath(QStringLiteral("gui"))) return false;
        const QString target = root + QStringLiteral("/gui/isvn-gui");
        if (!QFile::copy(QStringLiteral(ISVN_GUI_BINARY), target)) return false;
        return QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                 QFileDevice::ExeOwner);
    }

    // An `isvn` earlier on PATH that records any invocation.  The GUI must
    // never run it.  `runtime_dir` holds the instance endpoint, whose socket
    // path is length-limited, so it is passed as given rather than resolved.
    static QProcessEnvironment trappedEnvironment(const QString& scratch, const QString& runtime_dir,
                                                  QString* trap_log) {
        QDir(scratch).mkpath(QStringLiteral("path-trap"));
        QDir(scratch).mkpath(QStringLiteral("home"));
        *trap_log = scratch + QStringLiteral("/path-trap/called.log");
        writeScript(scratch + QStringLiteral("/path-trap/isvn"),
                    "#!/bin/sh\necho \"$@\" >> '" + trap_log->toUtf8() + "'\necho '{}'\n");
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PATH"), scratch + QStringLiteral("/path-trap:") +
                                                       environment.value(QStringLiteral("PATH")));
        environment.insert(QStringLiteral("HOME"), scratch + QStringLiteral("/home"));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), scratch + QStringLiteral("/home/.config"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("ISVN_GUI_TEST_RUNTIME_DIR"), runtime_dir);
        environment.remove(QStringLiteral("ISVN_GUI_SHELL"));
        return environment;
    }

private slots:
    void exactShellSelectionAndRollback() {
        const auto unset = run(QString());
        QCOMPARE(unset.exitCode, 0);
        QVERIFY(unset.stdOut.contains("root=ProductShellWindow"));
        QVERIFY2(unset.stdErr.isEmpty(), unset.stdErr.constData());
        const auto modern = run(QStringLiteral("new"));
        QCOMPARE(modern.exitCode, 0);
        QVERIFY(modern.stdOut.contains("root=ProductShellWindow"));
        QVERIFY2(modern.stdErr.isEmpty(), modern.stdErr.constData());
        const auto legacy = run(QStringLiteral("legacy"));
        QCOMPARE(legacy.exitCode, 0);
        QVERIFY(legacy.stdOut.contains("root=LegacyMain"));
        const auto empty = run(QStringLiteral(""));
        QCOMPARE(empty.exitCode, 2);
        QVERIFY(empty.stdErr.contains("ISVN_GUI_SHELL"));
        const auto invalid = run(QStringLiteral(" new"));
        QCOMPARE(invalid.exitCode, 2);
        QVERIFY(invalid.stdErr.contains("ISVN_GUI_SHELL"));
    }

    void buildIdentityMatchesThePairedCli() {
        const auto started = run(QString());
        QCOMPARE(started.exitCode, 0);
        const QString expected_cli =
            QFileInfo(QFileInfo(QStringLiteral(ISVN_GUI_BINARY)).canonicalPath() + QStringLiteral("/../isvn"))
                .canonicalFilePath();
        QVERIFY(!expected_cli.isEmpty());
        QCOMPARE(lineValue(started.stdOut, "isvn-gui cli="), expected_cli);

        QProcess cli;
        cli.start(expected_cli, {QStringLiteral("version"), QStringLiteral("--json")});
        QVERIFY(cli.waitForFinished(5000));
        QCOMPARE(cli.exitCode(), 0);
        const QJsonObject identity = QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
        QVERIFY(!identity.value(QStringLiteral("version")).toString().isEmpty());
        const QString expected_identity =
            QStringLiteral("isvn-gui %1 · %2 · %3")
                .arg(identity.value(QStringLiteral("version")).toString(),
                     identity.value(QStringLiteral("commit")).toString(),
                     identity.value(QStringLiteral("build_type")).toString());
        QCOMPARE(lineValue(started.stdOut, "isvn-gui build="), expected_identity);
    }

    void refusesToStartWithoutItsOwnCli() {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString root = QDir(scratch.path()).canonicalPath();
        QVERIFY(copyGui(root + QStringLiteral("/bundle")));
        QString trap_log;
        QTemporaryDir runtime;
        QProcessEnvironment environment = trappedEnvironment(root, runtime.path(), &trap_log);
        environment.insert(QStringLiteral("ISVN_GUI_TEST_EXIT_AFTER_ROOT"), QStringLiteral("1"));

        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start(root + QStringLiteral("/bundle/gui/isvn-gui"));
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 5);
        const QByteArray err = process.readAllStandardError();
        QVERIFY2(err.contains("paired isvn CLI not found"), err.constData());
        QVERIFY2(err.contains((root + QStringLiteral("/bundle/isvn")).toUtf8()), err.constData());
        // No shell root was loaded and the PATH `isvn` was not run.
        QVERIFY(!process.readAllStandardOutput().contains("root="));
        QVERIFY(!QFile::exists(trap_log));

        // A CLI that is present but not executable is refused the same way.
        QFile plain(root + QStringLiteral("/bundle/isvn"));
        QVERIFY(plain.open(QIODevice::WriteOnly));
        plain.write("#!/bin/sh\n");
        plain.close();
        QVERIFY(plain.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QProcess second;
        second.setProcessEnvironment(environment);
        second.setWorkingDirectory(root);
        second.start(root + QStringLiteral("/bundle/gui/isvn-gui"));
        QVERIFY(second.waitForFinished(10000));
        QCOMPARE(second.exitCode(), 5);
        QVERIFY(second.readAllStandardError().contains("not executable"));
        QVERIFY(!QFile::exists(trap_log));
    }

    void runsItsOwnCliAndNeverTheOneOnPath() {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString root = QDir(scratch.path()).canonicalPath();
        QVERIFY(copyGui(root + QStringLiteral("/bundle")));
        const QString paired_log = root + QStringLiteral("/paired-called.log");
        QVERIFY(writeScript(
            root + QStringLiteral("/bundle/isvn"),
            "#!/bin/sh\necho \"$@\" >> '" + paired_log.toUtf8() +
                "'\ncase \"$1\" in\n"
                "  workspaces) echo '{\"schema_version\":\"workspaces/v1\",\"project\":"
                "{\"name\":\"entry-test\"},\"workspaces\":[],\"issues\":[]}' ;;\n"
                "  *) echo '{}' ;;\nesac\n"));
        // A directory whose `.isvn` is owned by this user is a project the GUI opens.
        QVERIFY(QDir(root).mkpath(QStringLiteral("project/.isvn")));
        QFile config(root + QStringLiteral("/project/.isvn/config"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("project_group = none\n");
        config.close();

        QString trap_log;
        QTemporaryDir runtime;
        const QProcessEnvironment environment = trappedEnvironment(root, runtime.path(), &trap_log);
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root + QStringLiteral("/project"));
        process.start(root + QStringLiteral("/bundle/gui/isvn-gui"));
        QVERIFY(process.waitForStarted(10000));
        // The shell reads the workspace inventory as soon as it is up.
        for (int attempt = 0; attempt < 150 && !QFile::exists(paired_log); ++attempt) QTest::qWait(100);
        const bool paired_called = QFile::exists(paired_log);
        QTest::qWait(300);
        process.terminate();
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished();
        }
        QVERIFY2(paired_called, process.readAllStandardError().constData());
        QFile log(paired_log);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.readAll().contains("workspaces"));
        QVERIFY(!QFile::exists(trap_log));
    }
};

QTEST_MAIN(ProductShellEntryTest)
#include "test_product_shell_entry.moc"
