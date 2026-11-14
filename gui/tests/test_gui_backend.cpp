#include "backend/IsvnCli.h"
#include "backend/RoleResolver.h"
#include "backend/SafeNotifier.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <sys/stat.h>

class GuiBackendTest : public QObject {
    Q_OBJECT

private slots:
    void isvnCliSpawnsCommandAndParsesJson();
    void isvnCliReportsInvalidJsonWithoutFallback();
    void isvnCliDeliversStructuredErrorOnNonzeroExit();
    void isvnCliHumanizesRawNonJsonError();
    void isvnCliWritesImportRequestToStdinOnly();
    void isvnCliOperationsAreTargetPinnedAndMutationsUnlimited();
    void isvnCliBoundedShutdownReapsRunningOperation();
    void notifierRejectsExternalOrRawPayload();
    void roleResolverTreatsIsvnOwnerAsPm();
};

namespace {

QString writeExecutable(QTemporaryDir& dir, const QString& body) {
    const QString path = dir.filePath(QStringLiteral("isvn-fake"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream stream(&file);
    stream << "#!/bin/sh\n" << body << "\n";
    file.close();
    if (::chmod(path.toLocal8Bit().constData(), 0750) != 0) {
        return {};
    }
    return path;
}

}  // namespace

void GuiBackendTest::isvnCliSpawnsCommandAndParsesJson() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString args_file = dir.filePath(QStringLiteral("args.txt"));
    const QString fake =
        writeExecutable(dir, QStringLiteral("printf '%s\\n' \"$@\" > '%1'\necho '{\"ok\":true}'")
                                 .arg(args_file));
    QVERIFY(!fake.isEmpty());

    isvn::gui::IsvnCli cli;
    cli.setExecutable(fake);
    const QVariant result = cli.runJsonBlocking({QStringLiteral("lineage"),
                                                 QStringLiteral("audit")});
    QVERIFY(cli.lastError().isEmpty());
    QVERIFY(result.toMap().value(QStringLiteral("ok")).toBool());

    QFile file(args_file);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString args = QString::fromUtf8(file.readAll());
    QVERIFY(args.contains(QStringLiteral("lineage")));
    QVERIFY(args.contains(QStringLiteral("audit")));
    QVERIFY(args.contains(QStringLiteral("--json")));
}

void GuiBackendTest::isvnCliReportsInvalidJsonWithoutFallback() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fake = writeExecutable(dir, QStringLiteral("echo 'not json'"));
    QVERIFY(!fake.isEmpty());

    isvn::gui::IsvnCli cli;
    cli.setExecutable(fake);
    const QVariant result = cli.runJsonBlocking({QStringLiteral("status")});
    QVERIFY(!result.isValid());
    QVERIFY(cli.lastError().contains(QStringLiteral("JSON")));
}

void GuiBackendTest::isvnCliDeliversStructuredErrorOnNonzeroExit() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A nonzero exit that still prints a work-error/v1 envelope on stdout must
    // reach QML as a structured error, not raw stderr.
    const QString fake = writeExecutable(
        dir, QStringLiteral(
                 "echo '{\"schema_version\":\"work-error/v1\",\"ok\":false,\"operation_id\":\"op1\","
                 "\"error\":{\"code\":\"PLAN_STALE\",\"reason_params\":{},\"retryable\":true,"
                 "\"recovery_required\":false}}'\nexit 1"));
    QVERIFY(!fake.isEmpty());

    isvn::gui::IsvnCli cli;
    cli.setExecutable(fake);
    QSignalSpy structured(&cli, &isvn::gui::IsvnCli::commandFailedJson);
    QSignalSpy failed(&cli, &isvn::gui::IsvnCli::commandFailed);
    cli.runJson({QStringLiteral("work-local-op"), QStringLiteral("apply")});
    QVERIFY(structured.wait(3000) || structured.count() > 0);
    QCOMPARE(structured.count(), 1);
    const QVariantMap json = structured.first().first().toMap();
    QCOMPARE(json.value(QStringLiteral("error")).toMap().value(QStringLiteral("code")).toString(),
             QStringLiteral("PLAN_STALE"));
    QVERIFY(failed.count() >= 1);  // legacy failed also fires, humanized
    QVERIFY(!cli.lastError().contains(QStringLiteral("stderr")));
}

void GuiBackendTest::isvnCliHumanizesRawNonJsonError() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A legacy nonzero exit with non-JSON stderr still humanizes safely and does
    // NOT emit a structured error.
    const QString fake =
        writeExecutable(dir, QStringLiteral("echo 'boom' 1>&2\nexit 1"));
    QVERIFY(!fake.isEmpty());
    isvn::gui::IsvnCli cli;
    cli.setExecutable(fake);
    QSignalSpy structured(&cli, &isvn::gui::IsvnCli::commandFailedJson);
    QSignalSpy failed(&cli, &isvn::gui::IsvnCli::commandFailed);
    cli.runJson({QStringLiteral("status")});
    QVERIFY(failed.wait(3000) || failed.count() > 0);
    QCOMPARE(structured.count(), 0);
    QVERIFY(failed.count() >= 1);
}

void GuiBackendTest::isvnCliWritesImportRequestToStdinOnly() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString argsFile = dir.filePath(QStringLiteral("args.txt"));
    const QString stdinFile = dir.filePath(QStringLiteral("stdin.json"));
    const QString fake = writeExecutable(
        dir, QStringLiteral("printf '%s\\n' \"$@\" > '%1'\ncat > '%2'\necho '{\"ok\":true}'")
                 .arg(argsFile, stdinFile));
    QVERIFY(!fake.isEmpty());
    isvn::gui::IsvnCli cli;
    cli.setExecutable(fake);
    QSignalSpy succeeded(&cli, &isvn::gui::IsvnCli::commandSucceeded);
    const QString request =
        QStringLiteral("{\"schema\":\"import-request/v1\",\"sources\":[{\"source_url\":\"file:///external/rtl\"}]}");
    cli.runJsonWithStdin({QStringLiteral("import"), QStringLiteral("preview"),
                          QStringLiteral("--request-stdin")}, request);
    QVERIFY(succeeded.wait(3000) || succeeded.count() > 0);
    QFile args(argsFile);
    QVERIFY(args.open(QIODevice::ReadOnly));
    QVERIFY(!QString::fromUtf8(args.readAll()).contains(QStringLiteral("/external/rtl")));
    QFile payload(stdinFile);
    QVERIFY(payload.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(payload.readAll()), request);
}

void GuiBackendTest::isvnCliOperationsAreTargetPinnedAndMutationsUnlimited() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fake = writeExecutable(dir, QStringLiteral("sleep 1\necho '{\"ok\":true}'"));
    QVERIFY(!fake.isEmpty());
    isvn::gui::IsvnCli cli;
    cli.setExecutable(fake);
    QSignalSpy succeeded(&cli, &isvn::gui::IsvnCli::operationSucceeded);
    QSignalSpy failed(&cli, &isvn::gui::IsvnCli::operationFailedJson);
    const QVariantList command{QStringLiteral("import"), QStringLiteral("apply")};

    // Mutations are neither limited in number nor serialized per Version: two
    // on the same Version and three more on other Versions all start.
    const QStringList targets{QStringLiteral("P/B/RTL/A"), QStringLiteral("P/B/RTL/A"),
                              QStringLiteral("P/B/RTL/B"), QStringLiteral("P/B/RTL/C"),
                              QStringLiteral("P/B/RTL/D")};
    for (int i = 0; i < targets.size(); ++i) {
        QVERIFY(cli.runJsonForOperation(QStringLiteral("op-%1").arg(i), targets.at(i), command,
                                        QString(), true));
    }
    QCOMPARE(cli.activeMutations(), 5);
    QCOMPARE(failed.count(), 0);

    // One read runs beside the mutations; an overlapping second read is
    // dropped rather than queued.
    QVERIFY(cli.runJsonForOperation(QStringLiteral("read-b"), QStringLiteral("P/B/RTL/B"),
                                    QVariantList{QStringLiteral("work-status")}, QString(), false));
    QCOMPARE(cli.activeReads(), 1);
    QVERIFY(!cli.runJsonForOperation(QStringLiteral("read-c"), QStringLiteral("P/B/RTL/C"),
                                     QVariantList{QStringLiteral("work-status")}, QString(), false));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.last().at(2).toMap().value(QStringLiteral("error")).toMap()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("READ_LANE_BUSY"));

    QTRY_COMPARE_WITH_TIMEOUT(succeeded.count(), 6, 6000);
    QSet<QString> completed;
    for (const auto& record : succeeded) {
        completed.insert(record.at(0).toString() + QStringLiteral("|") + record.at(1).toString());
    }
    for (int i = 0; i < targets.size(); ++i) {
        QVERIFY(completed.contains(QStringLiteral("op-%1|").arg(i) + targets.at(i)));
    }
    QVERIFY(completed.contains(QStringLiteral("read-b|P/B/RTL/B")));
    QCOMPARE(cli.activeMutations(), 0);
    QCOMPARE(cli.activeReads(), 0);
}

void GuiBackendTest::isvnCliBoundedShutdownReapsRunningOperation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fake = writeExecutable(dir, QStringLiteral("sleep 5\necho '{\"ok\":true}'"));
    QVERIFY(!fake.isEmpty());
    isvn::gui::IsvnCli cli;
    cli.setExecutable(fake);
    QSignalSpy failed(&cli, &isvn::gui::IsvnCli::operationFailedJson);
    QVERIFY(cli.runJsonForOperation(QStringLiteral("shutdown-op"), QStringLiteral("A"),
                                    QVariantList{QStringLiteral("import"), QStringLiteral("apply")}));
    QCOMPARE(cli.activeMutations(), 1);
    cli.requestBoundedShutdown(10);
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 1500);
    QCOMPARE(cli.activeMutations(), 0);
}

void GuiBackendTest::notifierRejectsExternalOrRawPayload() {
    isvn::gui::SafeNotifier notifier;
    QVERIFY(!notifier.notify(QStringLiteral("Import"), QStringLiteral("Completed"),
                             QStringLiteral("/external/sentinel")));
    QVERIFY(!notifier.notify(QStringLiteral("Import\nraw"), QStringLiteral("Completed"),
                             QStringLiteral("phase/block/stage/version")));
}

void GuiBackendTest::roleResolverTreatsIsvnOwnerAsPm() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("project/.isvn")));
    QFile config(root.filePath(QStringLiteral("project/.isvn/config")));
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&config);
    stream << "project_group = " << QFileInfo(root.path()).group() << "\n";
    config.close();

    isvn::gui::RoleResolver resolver;
    const QVariantMap result = resolver.resolve(root.filePath(QStringLiteral("project")));
    QVERIFY(result.value(QStringLiteral("allowed")).toBool());
    QCOMPARE(result.value(QStringLiteral("role")).toString(), QStringLiteral("PM"));
    QVERIFY(!result.value(QStringLiteral("currentUser")).toString().isEmpty());
}

QTEST_MAIN(GuiBackendTest)

#include "test_gui_backend.moc"
