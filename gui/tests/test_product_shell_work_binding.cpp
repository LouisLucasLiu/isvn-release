// ProductShell real Work binding test.
//
// This exercises the real ProductShell/WorkPage wiring in readMode, rather
// than loading an isolated controller.  The fake bridge provides only typed
// backend JSON and target-pinned operation completions; QML still has no file,
// SVN, shell, hashing, or journal mutation API.
#include "backend/WorkSessionStore.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVariant>

#include <memory>

class FakeWorkOperationCli final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int activeMutations READ activeMutations NOTIFY activeMutationsChanged)
    Q_PROPERTY(int activeReads READ activeReads NOTIFY activeReadsChanged)
    Q_PROPERTY(QString maintenanceRole MEMBER maintenanceRole NOTIFY maintenanceRoleChanged)

public:
    struct Call {
        QString operationId;
        QString target;
        QStringList arguments;
        QString stdinPayload;
        bool mutation = false;
    };

    QVector<Call> calls;
    bool deferImportApply = false;
    bool recoveryRequired = false;
    bool createdDerived = false;
    QString maintenanceRole;

    bool running() const { return false; }
    int activeMutations() const { return 0; }
    int activeReads() const { return 0; }

    Q_INVOKABLE bool runJsonForOperation(const QString& operationId, const QString& target,
                                         const QVariant& arguments, const QString& stdinPayload,
                                         bool mutation) {
        QStringList args;
        for (const QVariant& value : arguments.toList()) args.push_back(value.toString());
        const Call call{operationId, target, args, stdinPayload, mutation};
        calls.push_back(call);
        if (deferImportApply && args.size() >= 2 && args[0] == QStringLiteral("import") &&
            args[1] == QStringLiteral("apply")) {
            deferred_import_ = call;
            return true;
        }
        QTimer::singleShot(0, this, [this, call]() { emit operationSucceeded(
            call.operationId, call.target, responseFor(call)); });
        return true;
    }

    // Legacy Explore's read-only embedded body may issue a diagnostic through
    // CommandRunner.  Keep this fake bridge complete so a Work→Explore action
    // is warning-free; it still performs no process or filesystem operation.
    Q_INVOKABLE void runJson(const QVariant&) {
        QTimer::singleShot(0, this, [this]() { emit commandSucceeded(QVariantMap{}); });
    }

    void finishDeferredImport() {
        if (deferred_import_.operationId.isEmpty()) return;
        const Call call = deferred_import_;
        deferred_import_ = {};
        emit operationSucceeded(call.operationId, call.target, responseFor(call));
    }

    static QStringList selectedPaths(const QStringList& args) {
        QStringList result;
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == QStringLiteral("--path")) result.push_back(args[i + 1]);
        return result;
    }

signals:
    void runningChanged();
    void activeMutationsChanged();
    void activeReadsChanged();
    void operationSucceeded(const QString& operationId, const QString& targetVersion,
                            const QVariant& json);
    void operationFailedJson(const QString& operationId, const QString& targetVersion,
                             const QVariant& envelope);
    void commandSucceeded(const QVariant& json);
    void commandFailed(const QString& message);
    void maintenanceRoleChanged();

private:
    Call deferred_import_;

    static QVariantMap workspace(const QString& id, const QString& version) {
        const QStringList parts = version.split('/');
        return QVariantMap{{QStringLiteral("workspace_id"), id},
                           {QStringLiteral("phase"), parts.value(0)},
                           {QStringLiteral("block"), parts.value(1)},
                           {QStringLiteral("stage"), parts.value(2)},
                           {QStringLiteral("version"), parts.value(3)},
                           {QStringLiteral("repository_path"), version},
                           {QStringLiteral("working_copy"), QStringLiteral("/disposable/") + version},
                           {QStringLiteral("health"), QStringLiteral("ok")},
                           {QStringLiteral("counts"), QVariantMap{{QStringLiteral("changed"), 2},
                                                                    {QStringLiteral("conflict"), 0},
                                                                    {QStringLiteral("local"), 1}}}};
    }

    QVariantMap workspaces() const {
        QVariantList items{
            workspace(QStringLiteral("ws-a"), QStringLiteral("PPA/CPU/RTL/v1")),
            workspace(QStringLiteral("ws-b"), QStringLiteral("PPA/CPU/SYN/v2")),
            workspace(QStringLiteral("ws-c"), QStringLiteral("PPA/CPU/RTL/v3"))};
        if (createdDerived)
            items.push_back(workspace(QStringLiteral("ws-derived"), QStringLiteral("PPA/CPU/RTL/v1.d")));
        return QVariantMap{{QStringLiteral("schema"), QStringLiteral("workspaces/v1")},
                           {QStringLiteral("project"), QVariantMap{{QStringLiteral("name"), QStringLiteral("preview")}}},
                           {QStringLiteral("workspaces"), items}};
    }

    static QVariantMap workStatus(const QString& target) {
        const QVariantList entries{
            QVariantMap{{QStringLiteral("path"), QStringLiteral("rtl/a.v")},
                        {QStringLiteral("kind"), QStringLiteral("file")},
                        {QStringLiteral("ui_state"), QStringLiteral("modified")},
                        {QStringLiteral("source_kind"), QStringLiteral("LOCAL")},
                        {QStringLiteral("size_bytes"), 11}},
            QVariantMap{{QStringLiteral("path"), QStringLiteral("rtl/local.v")},
                        {QStringLiteral("kind"), QStringLiteral("file")},
                        {QStringLiteral("ui_state"), QStringLiteral("new")},
                        {QStringLiteral("source_kind"), QStringLiteral("LOCAL")},
                        {QStringLiteral("size_bytes"), 12}},
            QVariantMap{{QStringLiteral("path"), QStringLiteral("rtl/managed.v")},
                        {QStringLiteral("kind"), QStringLiteral("file")},
                        {QStringLiteral("ui_state"), QStringLiteral("editable")},
                        {QStringLiteral("source_kind"), QStringLiteral("LINKED")},
                        {QStringLiteral("size_bytes"), 13}}};
        return QVariantMap{{QStringLiteral("schema"), QStringLiteral("work-status/v1")},
                           {QStringLiteral("version"), target},
                           {QStringLiteral("entries"), entries}};
    }

    static QVariantMap checkinPreview(const QStringList& paths) {
        QVariantList items;
        for (const QString& path : paths)
            items.push_back(QVariantMap{{QStringLiteral("path"), path},
                                        {QStringLiteral("kind"), QStringLiteral("file")},
                                        {QStringLiteral("change_kind"), QStringLiteral("update")},
                                        {QStringLiteral("source_kind"), QStringLiteral("LOCAL")},
                                        {QStringLiteral("allowed"), true},
                                        {QStringLiteral("reason_code"), QStringLiteral("OK")}});
        return QVariantMap{{QStringLiteral("schema"), QStringLiteral("checkin-batch/v1")},
                           {QStringLiteral("items"), items},
                           {QStringLiteral("supporting_repository_changes"), QVariantList{}},
                           {QStringLiteral("can_apply"), !items.isEmpty()},
                           {QStringLiteral("plan_hash"), QStringLiteral("checkin-plan")}};
    }

    QVariant responseFor(const Call& call) {
        const QStringList& args = call.arguments;
        if (args.isEmpty()) return {};
        if (args[0] == QStringLiteral("workspaces")) return workspaces();
        if (args[0] == QStringLiteral("work-status")) return workStatus(args.value(1));
        if (args[0] == QStringLiteral("work-recovery") && args.contains(QStringLiteral("status"))) {
            QVariantList operations;
            if (recoveryRequired) {
                operations.push_back(QVariantMap{{QStringLiteral("operation_id"), QStringLiteral("op-recovery-other")},
                                                  {QStringLiteral("operation"), QStringLiteral("checkin-batch")},
                                                  {QStringLiteral("target_version"), QStringLiteral("PPA/CPU/SYN/v2")},
                                                  {QStringLiteral("state"), QStringLiteral("RECOVERY_REQUIRED")}});
                operations.push_back(QVariantMap{{QStringLiteral("operation_id"), QStringLiteral("op-recovery-a")},
                                                  {QStringLiteral("operation"), QStringLiteral("checkin-batch")},
                                                  {QStringLiteral("target_version"), call.target},
                                                  {QStringLiteral("state"), QStringLiteral("RECOVERY_REQUIRED")}});
            }
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("work-recovery/v1")},
                               {QStringLiteral("recovery_required"), recoveryRequired},
                               {QStringLiteral("operations"), operations}};
        }
        if (args[0] == QStringLiteral("work-recovery") && args.contains(QStringLiteral("restore")))
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("work-recovery-result/v2")},
                               {QStringLiteral("operation_id"), args.value(2)},
                               {QStringLiteral("status"), QStringLiteral("manual_review_required")},
                               {QStringLiteral("reason_code"), QStringLiteral("REMOTE_STATE_DIVERGED")},
                               {QStringLiteral("recovery_required"), true},
                               {QStringLiteral("manual_review_required"), true}};
        if (args[0] == QStringLiteral("checkin-batch") && args.contains(QStringLiteral("preview")))
            return checkinPreview(selectedPaths(args));
        if (args[0] == QStringLiteral("checkin-batch") && args.contains(QStringLiteral("apply")))
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("checkin-batch-result/v1")},
                               {QStringLiteral("operation_id"), QStringLiteral("op-checkin-real")},
                               {QStringLiteral("status"), QStringLiteral("committed")},
                               {QStringLiteral("revision"), 4201}};
        if (args[0] == QStringLiteral("work-local-op") && args.contains(QStringLiteral("preview")))
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("work-local-op/v1")},
                               {QStringLiteral("plan_hash"), QStringLiteral("local-plan")}};
        if (args[0] == QStringLiteral("work-local-op") && args.contains(QStringLiteral("apply")))
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("work-local-op-result/v1")},
                               {QStringLiteral("operation_id"), QStringLiteral("op-local-real")},
                               {QStringLiteral("status"), QStringLiteral("succeeded")}};
        if (args[0] == QStringLiteral("remove-batch") && args.contains(QStringLiteral("preview")))
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("remove-batch/v2")},
                               {QStringLiteral("plan_hash"), QStringLiteral("remove-plan")},
                               {QStringLiteral("local_discard_required"), true}};
        if (args[0] == QStringLiteral("remove-batch") && args.contains(QStringLiteral("apply")))
            return QVariantMap{{QStringLiteral("schema_version"), QStringLiteral("remove-batch-result/v2")},
                               {QStringLiteral("operation_id"), QStringLiteral("op-remove-real")},
                               {QStringLiteral("status"), QStringLiteral("succeeded")},
                               {QStringLiteral("revision"), 4202}};
        if (args[0] == QStringLiteral("import") && args.contains(QStringLiteral("preview")))
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("import/v1")},
                               {QStringLiteral("items"), QVariantList{
                                    QVariantMap{{QStringLiteral("source_id"), QStringLiteral("source-0001")},
                                                {QStringLiteral("source_relative"), QStringLiteral("safe.v")},
                                                {QStringLiteral("destination"), QStringLiteral("imported.v")},
                                                {QStringLiteral("allowed"), true},
                                                {QStringLiteral("reason_code"), QStringLiteral("OK")},
                                                {QStringLiteral("replace_local_available"), false}}}},
                               {QStringLiteral("can_apply"), true},
                               {QStringLiteral("plan_hash"), QStringLiteral("import-plan")}};
        if (args[0] == QStringLiteral("import") && args.contains(QStringLiteral("apply")))
            return QVariantMap{{QStringLiteral("schema"), QStringLiteral("import-result/v1")},
                               {QStringLiteral("operation_id"), QStringLiteral("op-import-real")},
                               {QStringLiteral("status"), QStringLiteral("imported")},
                               {QStringLiteral("revision"), QVariant()},
                               {QStringLiteral("item_count"), 1},
                               {QStringLiteral("batch_paths"), QVariantList{QStringLiteral("imported.v")}}};
        if (args[0] == QStringLiteral("create")) { createdDerived = true; return QVariantMap{}; }
        if (args[0] == QStringLiteral("checkout")) return QVariantMap{};
        return QVariantMap{};
    }
};

class ProductShellWorkBindingTest final : public QObject {
    Q_OBJECT

    struct Loaded {
        std::unique_ptr<QQmlEngine> engine;
        std::unique_ptr<QObject> root;
        QObject* read = nullptr;
        QObject* checkin = nullptr;
        QObject* action = nullptr;
        QObject* import = nullptr;
        QObject* fileStatus = nullptr;
        QObject* coordinator = nullptr;
    };

    static bool invokeClick(QObject* object) {
        return object != nullptr && QMetaObject::invokeMethod(object, "clicked");
    }

    static QStringList paths(const QStringList& args) { return FakeWorkOperationCli::selectedPaths(args); }

    static const FakeWorkOperationCli::Call* lastCall(const FakeWorkOperationCli& cli,
                                                       const QString& command,
                                                       const QString& phase = QString()) {
        for (auto it = cli.calls.crbegin(); it != cli.calls.crend(); ++it) {
            if (it->arguments.value(0) == command &&
                (phase.isEmpty() || it->arguments.contains(phase))) return &*it;
        }
        return nullptr;
    }

    std::unique_ptr<Loaded> load(FakeWorkOperationCli* cli, QObject* sessionStore = nullptr) {
        auto loaded = std::make_unique<Loaded>();
        loaded->engine = std::make_unique<QQmlEngine>();
        loaded->engine->addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(loaded->engine.get(),
                                QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                    QStringLiteral("/qml/shell/ProductShell.qml")));
        loaded->root.reset(component.create());
        if (!loaded->root) qFatal("%s", qPrintable(component.errorString()));
        if (sessionStore != nullptr)
            loaded->root->setProperty("sessionStore", QVariant::fromValue(sessionStore));
        loaded->root->setProperty("isvnCli", QVariant::fromValue(static_cast<QObject*>(cli)));
        loaded->root->setProperty("readMode", true);
        loaded->read = loaded->root->findChild<QObject*>(QStringLiteral("workReadController"));
        loaded->checkin = loaded->root->findChild<QObject*>(QStringLiteral("checkinBatchController"));
        loaded->action = loaded->root->findChild<QObject*>(QStringLiteral("workActionController"));
        loaded->import = loaded->root->findChild<QObject*>(QStringLiteral("importController"));
        loaded->fileStatus = loaded->root->findChild<QObject*>(QStringLiteral("fileStatusWorkspace"));
        loaded->coordinator = loaded->root->findChild<QObject*>(QStringLiteral("workOperationCoordinator"));
        if (!loaded->read || !loaded->checkin || !loaded->action || !loaded->import ||
            !loaded->fileStatus || !loaded->coordinator)
            qFatal("ProductShell did not create the required real Work controllers");
        if (!QMetaObject::invokeMethod(loaded->read, "refresh"))
            qFatal("WorkReadController.refresh was unavailable");
        for (int attempt = 0; attempt < 50 &&
             loaded->read->property("state").toString() != QStringLiteral("ready"); ++attempt)
            QTest::qWait(10);
        if (loaded->read->property("state").toString() != QStringLiteral("ready"))
            qFatal("WorkReadController did not produce a ready snapshot");
        if (!QMetaObject::invokeMethod(loaded->read, "selectActive",
                                       Q_ARG(QVariant, QVariant(QStringLiteral("PPA/CPU/RTL/v1")))))
            qFatal("WorkReadController.selectActive was unavailable");
        for (int attempt = 0; attempt < 50 &&
             loaded->read->property("activeVersionId").toString() != QStringLiteral("PPA/CPU/RTL/v1"); ++attempt)
            QTest::qWait(10);
        if (loaded->read->property("activeVersionId").toString() != QStringLiteral("PPA/CPU/RTL/v1"))
            qFatal("Active version was not selected");
        for (int attempt = 0; attempt < 50 &&
             loaded->read->property("state").toString() != QStringLiteral("ready"); ++attempt)
            QTest::qWait(10);
        if (loaded->read->property("state").toString() != QStringLiteral("ready"))
            qFatal("Active Work status was not ready");
        return loaded;
    }

    static void select(QObject* fileStatus, const QString& path) {
        QVERIFY(QMetaObject::invokeMethod(fileStatus, "toggleSelection",
                                          Q_ARG(QVariant, QVariant(path)),
                                          Q_ARG(QVariant, QVariant(true))));
        QCoreApplication::processEvents();
    }

private slots:
    void defaultShellSelectionCheckinReviewConfirmResult();
    void defaultShellLocalAndRemoveUseExactSelection();
    void defaultShellImportRoutesExactBatchPathsToOriginalTarget();
    void defaultShellCreateCheckoutSetActiveUsesRealComposite();
    void defaultShellRecoveryManualReviewIsExactTargetPinned();
    void defaultShellFiltersAndExploreInspectionUseLiveReadState();
    void defaultShellActivityLogListsWhatWasDone();
};

void ProductShellWorkBindingTest::defaultShellSelectionCheckinReviewConfirmResult() {
    FakeWorkOperationCli cli;
    auto loaded = load(&cli);
    select(loaded->fileStatus, QStringLiteral("rtl/a.v"));
    QCOMPARE(loaded->checkin->property("selectedPaths").toList(),
             QVariantList{QStringLiteral("rtl/a.v")});

    QObject* add = loaded->root->findChild<QObject*>(QStringLiteral("addToBatchButton"));
    QVERIFY(invokeClick(add));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("review"));
    const auto* preview = lastCall(cli, QStringLiteral("checkin-batch"), QStringLiteral("preview"));
    QVERIFY(preview != nullptr);
    QCOMPARE(paths(preview->arguments), (QStringList{QStringLiteral("rtl/a.v")}));

    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("checkinContinueButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("confirm"));
    QObject* message = loaded->root->findChild<QObject*>(QStringLiteral("checkinMessageArea"));
    QVERIFY(message != nullptr);
    message->setProperty("text", QStringLiteral("GUI real batch"));
    QCoreApplication::processEvents();
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("checkinConfirmApplyButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("success"));
    const auto* apply = lastCall(cli, QStringLiteral("checkin-batch"), QStringLiteral("apply"));
    QVERIFY(apply != nullptr);
    QCOMPARE(paths(apply->arguments), (QStringList{QStringLiteral("rtl/a.v")}));
    QVERIFY(!apply->arguments.contains(QStringLiteral("checkin"))); // never legacy command
    QCOMPARE(loaded->checkin->property("verifiedRevision").toInt(), 4201);
}

void ProductShellWorkBindingTest::defaultShellLocalAndRemoveUseExactSelection() {
    FakeWorkOperationCli cli;
    auto loaded = load(&cli);

    select(loaded->fileStatus, QStringLiteral("rtl/managed.v"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("makeEditableButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("confirm"));
    const auto* localPreview = lastCall(cli, QStringLiteral("work-local-op"), QStringLiteral("preview"));
    QVERIFY(localPreview != nullptr);
    QCOMPARE(paths(localPreview->arguments), (QStringList{QStringLiteral("rtl/managed.v")}));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("opApplyButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("result"));

    QVERIFY(QMetaObject::invokeMethod(loaded->action, "reset"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("revertButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("confirm"));
    const auto* revert = lastCall(cli, QStringLiteral("work-local-op"), QStringLiteral("preview"));
    QVERIFY(revert != nullptr);
    QVERIFY(revert->arguments.contains(QStringLiteral("revert")));
    QCOMPARE(paths(revert->arguments), (QStringList{QStringLiteral("rtl/managed.v")}));
    QVERIFY(QMetaObject::invokeMethod(loaded->action, "reset"));

    QVERIFY(QMetaObject::invokeMethod(loaded->fileStatus, "toggleSelection",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("rtl/managed.v"))),
                                      Q_ARG(QVariant, QVariant(false))));
    select(loaded->fileStatus, QStringLiteral("rtl/local.v"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("discardLocalButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("confirm"));
    const auto* discard = lastCall(cli, QStringLiteral("work-local-op"), QStringLiteral("preview"));
    QVERIFY(discard != nullptr);
    QVERIFY(discard->arguments.contains(QStringLiteral("discard-local")));
    QCOMPARE(paths(discard->arguments), (QStringList{QStringLiteral("rtl/local.v")}));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("opApplyButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("result"));
    // A true backend success removes only the discarded target's stale
    // Checkin selector.  A later Review must not contain a path the user just
    // discarded through this default-shell action.
    QTRY_COMPARE(loaded->checkin->property("selectedPaths").toList(), QVariantList{});
    QVERIFY(QMetaObject::invokeMethod(loaded->action, "reset"));

    select(loaded->fileStatus, QStringLiteral("rtl/local.v"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("removeButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("confirm"));
    const auto* removePreview = lastCall(cli, QStringLiteral("remove-batch"), QStringLiteral("preview"));
    QVERIFY(removePreview != nullptr);
    QCOMPARE(paths(removePreview->arguments), (QStringList{QStringLiteral("rtl/local.v")}));
    QObject* removeMessage = loaded->root->findChild<QObject*>(QStringLiteral("removeMessageArea"));
    QVERIFY(removeMessage != nullptr);
    removeMessage->setProperty("text", QStringLiteral("GUI exact remove"));
    QObject* discardAck = loaded->root->findChild<QObject*>(QStringLiteral("discardAckCheckbox"));
    QObject* removeApplyButton = loaded->root->findChild<QObject*>(QStringLiteral("removeApplyButton"));
    QVERIFY(discardAck != nullptr && removeApplyButton != nullptr);
    QCOMPARE(discardAck->property("checked").toBool(), false);
    QCOMPARE(removeApplyButton->property("enabled").toBool(), false);
    discardAck->setProperty("checked", true);
    QTRY_COMPARE(removeApplyButton->property("enabled").toBool(), true);
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("removeApplyButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("result"));
    const auto* removeApply = lastCall(cli, QStringLiteral("remove-batch"), QStringLiteral("apply"));
    QVERIFY(removeApply != nullptr);
    QCOMPARE(paths(removeApply->arguments), (QStringList{QStringLiteral("rtl/local.v")}));
    QVERIFY(removeApply->arguments.contains(QStringLiteral("--allow-discard-local")));
}

void ProductShellWorkBindingTest::defaultShellImportRoutesExactBatchPathsToOriginalTarget() {
    FakeWorkOperationCli cli;
    auto loaded = load(&cli);
    cli.deferImportApply = true;

    const QVariant urls = QVariantList{QStringLiteral("file:///transient/external/rtl")};
    QVERIFY(QMetaObject::invokeMethod(loaded->fileStatus, "requestImport",
                                      Q_ARG(QVariant, QVariant(QStringLiteral(""))), Q_ARG(QVariant, urls)));
    QTRY_COMPARE(loaded->import->property("phase").toString(), QStringLiteral("review"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("importContinueButton"))));
    QTRY_COMPARE(loaded->import->property("phase").toString(), QStringLiteral("confirm"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("importConfirmApplyButton"))));
    QTRY_COMPARE(loaded->import->property("phase").toString(), QStringLiteral("applying"));

    // A keeps its pinned Import while the user switches to B and executes a
    // separate real Checkin mutation through another coordinator lane.
    QVERIFY(QMetaObject::invokeMethod(loaded->read, "selectActive",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("PPA/CPU/SYN/v2")))));
    QTRY_COMPARE(loaded->read->property("activeVersionId").toString(), QStringLiteral("PPA/CPU/SYN/v2"));
    select(loaded->fileStatus, QStringLiteral("rtl/a.v"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("addToBatchButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("review"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("checkinContinueButton"))));
    QObject* message = loaded->root->findChild<QObject*>(QStringLiteral("checkinMessageArea"));
    QVERIFY(message != nullptr);
    message->setProperty("text", QStringLiteral("B mutation"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("checkinConfirmApplyButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("success"));
    QCOMPARE(loaded->read->property("activeVersionId").toString(), QStringLiteral("PPA/CPU/SYN/v2"));

    cli.finishDeferredImport();
    QTRY_COMPARE(loaded->import->property("phase").toString(), QStringLiteral("success"));
    QCOMPARE(loaded->read->property("activeVersionId").toString(), QStringLiteral("PPA/CPU/SYN/v2"));
    // Import A does not overwrite B's visible selection/draft.  Its exact
    // returned batch_paths are retained only under A's target key.
    QCOMPARE(loaded->checkin->property("selectedPaths").toList(), QVariantList{});
    QVariant draft;
    for (int attempt = 0; attempt < 50; ++attempt) {
        QVERIFY(QMetaObject::invokeMethod(loaded->coordinator, "draftFor", Q_RETURN_ARG(QVariant, draft),
                                          Q_ARG(QVariant, QVariant(QStringLiteral("PPA/CPU/RTL/v1")))));
        if (draft.toMap().value(QStringLiteral("selectedPaths")).toList() ==
            QVariantList{QStringLiteral("imported.v")}) break;
        QTest::qWait(10);
    }
    QCOMPARE(draft.toMap().value(QStringLiteral("selectedPaths")).toList(),
             QVariantList{QStringLiteral("imported.v")});

    bool sawImportA = false;
    bool sawCheckinB = false;
    const QVariantList operations = loaded->coordinator->property("operationsById").toMap().values();
    for (const QVariant& value : operations) {
        const QVariantMap op = value.toMap();
        if (op.value(QStringLiteral("operationKind")).toString() == QStringLiteral("import") &&
            op.value(QStringLiteral("targetVersion")).toString() == QStringLiteral("PPA/CPU/RTL/v1")) sawImportA = true;
        if (op.value(QStringLiteral("operationKind")).toString() == QStringLiteral("checkin-batch") &&
            op.value(QStringLiteral("targetVersion")).toString() == QStringLiteral("PPA/CPU/SYN/v2")) sawCheckinB = true;
    }
    QVERIFY(sawImportA && sawCheckinB);
}

void ProductShellWorkBindingTest::defaultShellCreateCheckoutSetActiveUsesRealComposite() {
    FakeWorkOperationCli cli;
    auto loaded = load(&cli);
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("createDerivedVersionButton"))));
    QObject* dialog = loaded->root->findChild<QObject*>(QStringLiteral("createDerivedDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(QMetaObject::invokeMethod(dialog, "confirm"));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("result"));
    QTRY_COMPARE(loaded->read->property("activeVersionId").toString(), QStringLiteral("PPA/CPU/RTL/v1.d"));
    const auto* create = lastCall(cli, QStringLiteral("create"));
    const auto* checkout = lastCall(cli, QStringLiteral("checkout"));
    const auto* validation = lastCall(cli, QStringLiteral("workspaces"));
    QVERIFY(create != nullptr && checkout != nullptr && validation != nullptr);
    QCOMPARE(create->arguments.value(1), QStringLiteral("PPA/CPU/RTL/v1.d"));
    QCOMPARE(checkout->arguments.value(1), QStringLiteral("PPA/CPU/RTL/v1.d"));
}

void ProductShellWorkBindingTest::defaultShellRecoveryManualReviewIsExactTargetPinned() {
    FakeWorkOperationCli cli;
    cli.recoveryRequired = true;
    auto loaded = load(&cli);
    select(loaded->fileStatus, QStringLiteral("rtl/a.v"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("addToBatchButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("recovery"));

    // Move to B before opening A's durable recovery record.  The Operation
    // Center must route the exact A ID, rather than selecting the decoy B row
    // or treating the visible Active Version as authority.
    QVERIFY(QMetaObject::invokeMethod(loaded->read, "selectActive",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("PPA/CPU/SYN/v2")))));
    QTRY_COMPARE(loaded->read->property("activeVersionId").toString(), QStringLiteral("PPA/CPU/SYN/v2"));
    QVariant record;
    QVERIFY(QMetaObject::invokeMethod(loaded->coordinator, "operationFor", Q_RETURN_ARG(QVariant, record),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("op-recovery-a")))));
    QVERIFY(record.toMap().isEmpty() == false);
    QVERIFY(QMetaObject::invokeMethod(loaded->root.get(), "openTargetOperation",
                                      Q_ARG(QVariant, record)));
    QTRY_COMPARE(loaded->read->property("activeVersionId").toString(), QStringLiteral("PPA/CPU/RTL/v1"));
    QTRY_COMPARE(loaded->checkin->property("recoveryOperationId").toString(), QStringLiteral("op-recovery-a"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("checkinRecoverButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("manualReview"));
    const auto* restore = lastCall(cli, QStringLiteral("work-recovery"), QStringLiteral("restore"));
    QVERIFY(restore != nullptr);
    QCOMPARE(restore->arguments.value(2), QStringLiteral("op-recovery-a"));
}

void ProductShellWorkBindingTest::defaultShellFiltersAndExploreInspectionUseLiveReadState() {
    FakeWorkOperationCli cli;
    auto loaded = load(&cli);
    QTRY_COMPARE(loaded->fileStatus->property("filteredCount").toInt(), 3);
    loaded->fileStatus->setProperty("activeFilter", QStringLiteral("Editable"));
    QTRY_COMPARE(loaded->fileStatus->property("filteredCount").toInt(), 1);
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("inspectInExploreButton"))));
    QTRY_COMPARE(loaded->root->property("currentPage").toString(), QStringLiteral("Explore"));
    QObject* inspected = loaded->root->findChild<QObject*>(QStringLiteral("exploreInspectedVersion"));
    QVERIFY(inspected != nullptr);
    QCOMPARE(inspected->property("text").toString(), QStringLiteral("PPA/CPU/RTL/v1"));
}

// The Activity Log lists the operations that were really run, with the number
// of items each one acted on; it is not a fixed sample line.
void ProductShellWorkBindingTest::defaultShellActivityLogListsWhatWasDone() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    isvn::gui::WorkSessionStore store;
    store.setStorageFile(dir.filePath(QStringLiteral("work-session.ini")));
    store.setProjectKey(QStringLiteral("/disposable/project"));
    // Read records that an older build stored are not activity.
    QVERIFY(store.appendActivity(QVariantMap{{QStringLiteral("operationId"), QStringLiteral("ui-op-read")},
                                             {QStringLiteral("operationType"), QStringLiteral("work-read-workStatus")},
                                             {QStringLiteral("target"), QStringLiteral("PPA/CPU/RTL/v1")},
                                             {QStringLiteral("status"), QStringLiteral("success")}}));

    FakeWorkOperationCli cli;
    auto loaded = load(&cli, &store);
    QCOMPARE(loaded->root->property("activityRecords").toList().size(), 0);

    select(loaded->fileStatus, QStringLiteral("rtl/managed.v"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("makeEditableButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("confirm"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("opApplyButton"))));
    QTRY_COMPARE(loaded->action->property("phase").toString(), QStringLiteral("result"));

    QTRY_COMPARE(loaded->root->property("activityRecords").toList().size(), 1);
    const auto title = [&loaded](int index) {
        QVariant text;
        QMetaObject::invokeMethod(loaded->root.get(), "activityTitle", Q_RETURN_ARG(QVariant, text),
                                  Q_ARG(QVariant, loaded->root->property("activityRecords").toList().at(index)));
        return text.toString();
    };
    const auto detail = [&loaded](int index) {
        QVariant text;
        QMetaObject::invokeMethod(loaded->root.get(), "activityDetail", Q_RETURN_ARG(QVariant, text),
                                  Q_ARG(QVariant, loaded->root->property("activityRecords").toList().at(index)));
        return text.toString();
    };
    QObject* i18n = loaded->root->findChild<QObject*>(QStringLiteral("workI18n"));
    QVERIFY(i18n != nullptr);
    i18n->setProperty("language", QStringLiteral("en"));
    QCOMPARE(title(0), QStringLiteral("Make Editable \u00b7 1 item \u00b7 done"));
    QVERIFY(detail(0).startsWith(QStringLiteral("PPA/CPU/RTL/v1")));
    i18n->setProperty("language", QStringLiteral("zh_TW"));
    QCOMPARE(title(0), QStringLiteral("\u8a2d\u70ba\u53ef\u7de8\u8f2f \u00b7 1 \u500b\u9805\u76ee \u00b7 \u5b8c\u6210"));
    i18n->setProperty("language", QStringLiteral("en"));
    QVERIFY(QMetaObject::invokeMethod(loaded->action, "reset"));

    // A committed Checkin is listed above it, with its revision.
    QVERIFY(QMetaObject::invokeMethod(loaded->fileStatus, "toggleSelection",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("rtl/managed.v"))),
                                      Q_ARG(QVariant, QVariant(false))));
    select(loaded->fileStatus, QStringLiteral("rtl/a.v"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("addToBatchButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("review"));
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("checkinContinueButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("confirm"));
    QObject* message = loaded->root->findChild<QObject*>(QStringLiteral("checkinMessageArea"));
    QVERIFY(message != nullptr);
    message->setProperty("text", QStringLiteral("activity log check"));
    QCoreApplication::processEvents();
    QVERIFY(invokeClick(loaded->root->findChild<QObject*>(QStringLiteral("checkinConfirmApplyButton"))));
    QTRY_COMPARE(loaded->checkin->property("phase").toString(), QStringLiteral("success"));

    QTRY_COMPARE(loaded->root->property("activityRecords").toList().size(), 2);
    QCOMPARE(title(0), QStringLiteral("Checkin \u00b7 1 item \u00b7 committed \u00b7 r4201"));
    QCOMPARE(title(1), QStringLiteral("Make Editable \u00b7 1 item \u00b7 done"));
}

QTEST_MAIN(ProductShellWorkBindingTest)
#include "test_product_shell_work_binding.moc"
