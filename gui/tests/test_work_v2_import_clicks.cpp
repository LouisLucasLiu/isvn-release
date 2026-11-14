// Import controller test. This is deliberately a QML-controller
// test: it proves the GUI only transports opaque source URLs to the backend via
// stdin, and never invents Import eligibility or Batch membership itself.
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTest>
#include <QVariant>

#include <memory>

class FakeImportCli : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
public:
    bool running() const { return false; }
    QVector<QStringList> calls;
    QStringList stdinPayloads;
    QString previewError;
    bool localReplacement = false;

    Q_INVOKABLE void runJsonWithStdin(const QVariant& arguments, const QString& request) {
        QStringList args;
        for (const QVariant& value : arguments.toList()) args.push_back(value.toString());
        calls.push_back(args);
        stdinPayloads.push_back(request);
        if (args.contains(QStringLiteral("preview"))) {
            if (!previewError.isEmpty()) {
                const QString code = previewError;
                previewError.clear();
                emitError(code);
                return;
            }
            const QVariantMap item{
                {QStringLiteral("source_id"), QStringLiteral("source-0001")},
                {QStringLiteral("source_relative"), QStringLiteral("a.v")},
                {QStringLiteral("destination"), localReplacement ? QStringLiteral("src/top.v")
                                                                    : QStringLiteral("src/rtl/a.v")},
                {QStringLiteral("kind"), QStringLiteral("file")},
                {QStringLiteral("size"), 12},
                {QStringLiteral("destination_state"), localReplacement ? QStringLiteral("local")
                                                                          : QStringLiteral("absent")},
                {QStringLiteral("allowed"), !localReplacement},
                {QStringLiteral("replace_local_available"), localReplacement},
                {QStringLiteral("reason_code"), localReplacement ? QStringLiteral("REPLACE_LOCAL_REQUIRED")
                                                                    : QStringLiteral("OK")},
                {QStringLiteral("reason_params"), QVariantMap{}}};
            emit commandSucceeded(QVariantMap{{QStringLiteral("schema"), QStringLiteral("import/v1")},
                                              {QStringLiteral("items"), QVariantList{item}},
                                              {QStringLiteral("can_apply"), !localReplacement},
                                              {QStringLiteral("plan_hash"), QStringLiteral("import-plan-hash")}});
            return;
        }
        if (args.contains(QStringLiteral("apply"))) {
            const QString destination = localReplacement ? QStringLiteral("src/top.v")
                                                          : QStringLiteral("src/rtl/a.v");
            emit commandSucceeded(QVariantMap{{QStringLiteral("schema"), QStringLiteral("import-result/v1")},
                                              {QStringLiteral("operation_id"), QStringLiteral("op-import-0123")},
                                              {QStringLiteral("status"), QStringLiteral("imported")},
                                              {QStringLiteral("revision"), QVariant{}},
                                              {QStringLiteral("item_count"), 1},
                                              {QStringLiteral("batch_paths"), QVariantList{destination}}});
        }
    }

    Q_INVOKABLE void runJson(const QVariant& arguments) {
        QStringList args;
        for (const QVariant& value : arguments.toList()) args.push_back(value.toString());
        calls.push_back(args);
        if (args.value(0) == QStringLiteral("work-recovery") && args.contains(QStringLiteral("restore"))) {
            emit commandSucceeded(QVariantMap{{QStringLiteral("schema_version"),
                                               QStringLiteral("work-recovery-result/v2")},
                                              {QStringLiteral("operation"), QStringLiteral("import")},
                                              {QStringLiteral("status"), QStringLiteral("rolled_back")}});
        }
    }

signals:
    void runningChanged();
    void commandSucceeded(const QVariant& json);
    void commandFailed(const QString& message);
    void commandFailedJson(const QVariant& json);

private:
    void emitError(const QString& code) {
        const QVariantMap envelope{{QStringLiteral("schema_version"), QStringLiteral("work-error/v1")},
                                   {QStringLiteral("ok"), false},
                                   {QStringLiteral("operation_id"),
                                    code == QStringLiteral("RECOVERY_REQUIRED")
                                        ? QStringLiteral("op-import-0123456789abcdef0123456789abcdef") : QString{}},
                                   {QStringLiteral("error"),
                                    QVariantMap{{QStringLiteral("code"), code},
                                                {QStringLiteral("retryable"), false},
                                                {QStringLiteral("recovery_required"),
                                                 code == QStringLiteral("RECOVERY_REQUIRED")}}}};
        emit commandFailedJson(envelope);
        emit commandFailed(QStringLiteral("operation failed: ") + code);
    }
};

class FakeReadController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
public:
    int refreshCount = 0;
    bool failRefresh = false;
    QString state() const { return state_; }
    Q_INVOKABLE void refresh() {
        ++refreshCount;
        if (failRefresh) { state_ = QStringLiteral("error"); emit stateChanged(); }
        else { state_ = QStringLiteral("ready"); emit snapshotReady(); }
    }
signals:
    void snapshotReady();
    void stateChanged();
private:
    QString state_ = QStringLiteral("idle");
};

class FakeCheckinController : public QObject {
    Q_OBJECT
public:
    QStringList merged;
    QString target;
    Q_INVOKABLE bool mergeImportedPaths(const QVariant& paths, const QString& targetVersion) {
        merged.clear();
        for (const QVariant& path : paths.toList()) merged.push_back(path.toString());
        target = targetVersion;
        return true;
    }
};

class WorkV2ImportClicksTest : public QObject {
    Q_OBJECT
    struct Loaded {
        std::unique_ptr<QQmlEngine> engine;
        QObject* controller = nullptr;
    };

    static std::unique_ptr<Loaded> load(FakeImportCli* cli, FakeReadController* read,
                                        FakeCheckinController* checkin) {
        auto loaded = std::make_unique<Loaded>();
        loaded->engine = std::make_unique<QQmlEngine>();
        loaded->engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(loaded->engine.get(),
                                 QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                     QStringLiteral("/qml/work/ImportController.qml")));
        QObject* object = component.create();
        if (object == nullptr) qFatal("%s", qPrintable(component.errorString()));
        object->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(cli)));
        object->setProperty("readController", QVariant::fromValue(static_cast<QObject*>(read)));
        object->setProperty("checkinController", QVariant::fromValue(static_cast<QObject*>(checkin)));
        object->setProperty("activeVersion", QStringLiteral("P/B/RTL/v1"));
        loaded->controller = object;
        return loaded;
    }

    static void start(QObject* controller, const QString& destination) {
        const QVariant urls = QVariantList{QStringLiteral("file:///external/rtl")};
        QVERIFY(QMetaObject::invokeMethod(controller, "startDrop",
                                          Q_ARG(QVariant, QVariant(destination)), Q_ARG(QVariant, urls)));
        QCoreApplication::processEvents();
    }

private slots:
    void previewUsesStdinAndDoesNotMutate();
    void replaceLocalIsPerItemDefaultOffAndApplyHandsExactPathsToBatch();
    void refreshFailureLeavesImportSuccessfulButBatchUnconfirmed();
    void structuredFailureInvalidatesReview();
    void recoveryUsesOnlyWorkRecoveryRestore();
};

void WorkV2ImportClicksTest::previewUsesStdinAndDoesNotMutate() {
    FakeImportCli cli; FakeReadController read; FakeCheckinController checkin;
    auto loaded = load(&cli, &read, &checkin);
    start(loaded->controller, QStringLiteral("src"));
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("review"));
    QCOMPARE(cli.calls.size(), 1);
    QCOMPARE(cli.calls.first(), (QStringList{QStringLiteral("import"), QStringLiteral("preview"),
                                             QStringLiteral("--request-stdin")}));
    QVERIFY(!cli.calls.first().join(' ').contains(QStringLiteral("/external/rtl")));
    QVERIFY(cli.stdinPayloads.first().contains(QStringLiteral("file:///external/rtl")));
    QCOMPARE(read.refreshCount, 0);
    QVERIFY(checkin.merged.isEmpty());
}

void WorkV2ImportClicksTest::replaceLocalIsPerItemDefaultOffAndApplyHandsExactPathsToBatch() {
    FakeImportCli cli; cli.localReplacement = true;
    FakeReadController read; FakeCheckinController checkin;
    auto loaded = load(&cli, &read, &checkin);
    start(loaded->controller, QStringLiteral("src"));
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("review"));
    QVERIFY(!loaded->controller->property("canConfirm").toBool());
    QVERIFY(QMetaObject::invokeMethod(loaded->controller, "setReplaceLocal",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("src/top.v"))),
                                      Q_ARG(QVariant, QVariant(true))));
    QVERIFY(loaded->controller->property("canConfirm").toBool());
    QVERIFY(QMetaObject::invokeMethod(loaded->controller, "continueToConfirm"));
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("confirm"));
    QVERIFY(QMetaObject::invokeMethod(loaded->controller, "confirmApply"));
    QCoreApplication::processEvents();
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("success"));
    QCOMPARE(cli.calls.size(), 2);
    QVERIFY(cli.calls.at(1).contains(QStringLiteral("apply")));
    QVERIFY(cli.stdinPayloads.at(1).contains(QStringLiteral("\"replace_local\":[\"src/top.v\"]")));
    QCOMPARE(read.refreshCount, 1);
    QCOMPARE(checkin.target, QStringLiteral("P/B/RTL/v1"));
    QCOMPARE(checkin.merged, (QStringList{QStringLiteral("src/top.v")}));
}

void WorkV2ImportClicksTest::refreshFailureLeavesImportSuccessfulButBatchUnconfirmed() {
    FakeImportCli cli; FakeReadController read; read.failRefresh = true; FakeCheckinController checkin;
    auto loaded = load(&cli, &read, &checkin);
    start(loaded->controller, QStringLiteral("src"));
    QVERIFY(loaded->controller->property("canConfirm").toBool());
    QVERIFY(QMetaObject::invokeMethod(loaded->controller, "continueToConfirm"));
    QVERIFY(QMetaObject::invokeMethod(loaded->controller, "confirmApply"));
    QCoreApplication::processEvents();
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("success"));
    QVERIFY(!loaded->controller->property("batchIntegrated").toBool());
    QVERIFY(loaded->controller->property("batchIntegrationPending").toBool());
    QVERIFY(checkin.merged.isEmpty());
}

void WorkV2ImportClicksTest::structuredFailureInvalidatesReview() {
    FakeImportCli cli; cli.previewError = QStringLiteral("SOURCE_SYMLINK_UNSUPPORTED");
    FakeReadController read; FakeCheckinController checkin;
    auto loaded = load(&cli, &read, &checkin);
    start(loaded->controller, QStringLiteral("src"));
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("failed"));
    QCOMPARE(loaded->controller->property("errorCode").toString(), QStringLiteral("SOURCE_SYMLINK_UNSUPPORTED"));
    QVERIFY(!loaded->controller->property("canConfirm").toBool());
    QVERIFY(checkin.merged.isEmpty());
}

void WorkV2ImportClicksTest::recoveryUsesOnlyWorkRecoveryRestore() {
    FakeImportCli cli; cli.previewError = QStringLiteral("RECOVERY_REQUIRED");
    FakeReadController read; FakeCheckinController checkin;
    auto loaded = load(&cli, &read, &checkin);
    start(loaded->controller, QStringLiteral("src"));
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("recovery"));
    QVERIFY(QMetaObject::invokeMethod(loaded->controller, "restoreRecovery"));
    QCoreApplication::processEvents();
    QCOMPARE(cli.calls.last().first(), QStringLiteral("work-recovery"));
    QVERIFY(cli.calls.last().contains(QStringLiteral("restore")));
    QCOMPARE(loaded->controller->property("phase").toString(), QStringLiteral("idle"));
    QVERIFY(checkin.merged.isEmpty());
}

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    WorkV2ImportClicksTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_work_v2_import_clicks.moc"
