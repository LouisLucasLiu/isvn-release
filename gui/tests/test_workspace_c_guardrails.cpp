#include <QFile>
#include <QString>
#include <QTest>

class WorkspaceCGuardrailsTest : public QObject {
    Q_OBJECT

private slots:
    void workspaceCIsPmGatedInShell();
    void storageModelsHaveNoVersionDimension();
    void poolAndSwapUseStorageOnlyCommands();
    void gcUsesConfirmationAndNoImpersonation();
    void gcDryRunPreviewHasNoVersionLabels();
    void dryRunPreviewDoesNotOpenConfirmation();
    void adminAndRenewViewsUseReviewedReadOnlyBackends();
    void dangerZoneIsExplicitlyLabeled();
    void confirmationIsActionSpecific();
    void operationEvidenceHasModeAndStatus();
    void maintenanceOverviewExists();
    void memberViewHidesDestructiveControls();
    void memberFinalExecutionIsQmlGuarded();
    void mutatingCommandDenyPathSetsEvidenceFields();
    void previewCommandDoesNotRequireExecutionAuth();
    void backendBridgeGuardBlocksMutatingMaintenanceCommand();
    void denialEvidenceHasNoRawBackendTerminology();
};

namespace {

QString readFile(const QString& relative_path) {
    QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/") + relative_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString functionBody(const QString& source, const QString& function_name) {
    const qsizetype start = source.indexOf(QStringLiteral("function ") + function_name);
    if (start < 0) return {};
    const qsizetype next = source.indexOf(QStringLiteral("\n    function "), start + 1);
    return next < 0 ? source.mid(start) : source.mid(start, next - start);
}

}  // namespace

void WorkspaceCGuardrailsTest::workspaceCIsPmGatedInShell() {
    const QString main = readFile(QStringLiteral("qml/Main.qml"));
    QVERIFY(!main.isEmpty());
    QVERIFY(main.contains(QStringLiteral("enabled: roleResolver.roleName === \"PM\"")));
    QVERIFY(main.contains(QStringLiteral("pmMode: roleResolver.roleName === \"PM\"")));
    QVERIFY(!main.contains(QStringLiteral("roleSwitch")));
}

void WorkspaceCGuardrailsTest::storageModelsHaveNoVersionDimension() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(QStringLiteral("ListModel { id: refcountDistModel }")));
    QVERIFY(source.contains(QStringLiteral("ListModel { id: evictEventModel }")));
    QVERIFY(source.contains(QStringLiteral("ListModel { id: densityModel }")));
    QVERIFY(source.contains(QStringLiteral("ListModel { id: phantomRefModel }")));
    QVERIFY(!source.contains(QStringLiteral("versionPath")));
    QVERIFY(!source.contains(QStringLiteral("phase:")));
    QVERIFY(!source.contains(QStringLiteral("block:")));
    QVERIFY(!source.contains(QStringLiteral("stage:")));
    QVERIFY(!source.contains(QStringLiteral("child:")));
}

void WorkspaceCGuardrailsTest::poolAndSwapUseStorageOnlyCommands() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    const QString pool_body = functionBody(source, QStringLiteral("loadPoolStat"));
    const QString swap_body = functionBody(source, QStringLiteral("loadSwapHealth"));
    const QString swap_populate = functionBody(source, QStringLiteral("populateSwapHealth"));
    QVERIFY(pool_body.contains(QStringLiteral("[\"pool-stat\"]")));
    QVERIFY(swap_body.contains(QStringLiteral("[\"pool-stat\", \"--evict-events\"")));
    QVERIFY(!pool_body.contains(QStringLiteral("lineage")));
    QVERIFY(!swap_body.contains(QStringLiteral("lineage")));
    QVERIFY(!source.contains(QStringLiteral("ancestry")));
    QVERIFY(!source.contains(QStringLiteral("branches")));
    QVERIFY(swap_populate.contains(QStringLiteral("hash: events[i].hash")));
    QVERIFY(swap_populate.contains(QStringLiteral("evictedAt: events[i].evicted_at")));
    QVERIFY(!swap_populate.contains(QStringLiteral("actor")));
}

void WorkspaceCGuardrailsTest::gcUsesConfirmationAndNoImpersonation() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    QVERIFY(source.contains(QStringLiteral("Common.ConfirmDialog")));
    QVERIFY(source.contains(QStringLiteral("gcConfirm.open()")));
    QVERIFY(source.contains(QStringLiteral("gcRunner.runJson")));
    QVERIFY(source.contains(QStringLiteral("resetPreview()")));
    QVERIFY(source.contains(QStringLiteral("Risky maintenance actions ask for confirmation")));
    QVERIFY(!source.contains(QStringLiteral("sudo")));
    QVERIFY(!source.contains(QStringLiteral("setuid")));
    QVERIFY(!source.contains(QStringLiteral("impersonate")));
    QVERIFY(!source.contains(QStringLiteral("runAs")));
}

void WorkspaceCGuardrailsTest::gcDryRunPreviewHasNoVersionLabels() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    const QString gc_body = functionBody(source, QStringLiteral("populatePhantomPreview"));
    QVERIFY(gc_body.contains(QStringLiteral("json.phantom_refs")));
    QVERIFY(gc_body.contains(QStringLiteral("hash: refs[i].hash")));
    QVERIFY(gc_body.contains(QStringLiteral("linkPath: refs[i].link_path")));
    QVERIFY(source.contains(QStringLiteral("[\"gc\", \"--reconcile\", \"--dry-run\"]")));
    QVERIFY(source.contains(QStringLiteral("model.hash.slice(0, 12)")));
    QVERIFY(source.contains(QStringLiteral("model.linkPath")));
    QVERIFY(!gc_body.contains(QStringLiteral("version")));
    QVERIFY(!gc_body.contains(QStringLiteral("branch")));
    QVERIFY(!gc_body.contains(QStringLiteral("stage")));
    QVERIFY(!gc_body.contains(QStringLiteral("child")));
}

void WorkspaceCGuardrailsTest::dryRunPreviewDoesNotOpenConfirmation() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    const QString preview_body = functionBody(source, QStringLiteral("previewReconcile"));
    QVERIFY(preview_body.contains(QStringLiteral("reconcilePreviewRunner.runJson")));
    QVERIFY(preview_body.contains(QStringLiteral("[\"gc\", \"--reconcile\", \"--dry-run\"]")));
    QVERIFY(!preview_body.contains(QStringLiteral("gcConfirm.open")));
    QVERIFY(source.contains(QStringLiteral("Confirm clear")));
    QVERIFY(source.contains(QStringLiteral("onClicked: root.confirmGc([\"gc\", \"--reconcile\"])")));
    QVERIFY(source.contains(QStringLiteral("resetPreview()")));
}

void WorkspaceCGuardrailsTest::adminAndRenewViewsUseReviewedReadOnlyBackends() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    const QString admin_body = functionBody(source, QStringLiteral("loadAdminStatus"));
    const QString pool_body = functionBody(source, QStringLiteral("loadPoolStat"));
    const QString renew_populate = functionBody(source, QStringLiteral("populatePoolStat"));
    QVERIFY(admin_body.contains(QStringLiteral("[\"admin\", \"status\"]")));
    QVERIFY(pool_body.contains(QStringLiteral("[\"pool-stat\"]")));
    QVERIFY(source.contains(QStringLiteral("ListModel { id: renewEventModel }")));
    QVERIFY(renew_populate.contains(QStringLiteral("json.recent_renews")));
    QVERIFY(renew_populate.contains(QStringLiteral("hash: renews[j].hash")));
    QVERIFY(renew_populate.contains(QStringLiteral("actor: renews[j].actor")));
    QVERIFY(renew_populate.contains(QStringLiteral("renewedAt: renews[j].renewed_at")));
    QVERIFY(!admin_body.contains(QStringLiteral("gc")));
    QVERIFY(!admin_body.contains(QStringLiteral("chmod")));
    QVERIFY(!source.contains(QStringLiteral("[\"renew\"")));
    QVERIFY(!renew_populate.contains(QStringLiteral("version")));
    QVERIFY(!renew_populate.contains(QStringLiteral("branch")));
    QVERIFY(!renew_populate.contains(QStringLiteral("stage")));
    QVERIFY(!renew_populate.contains(QStringLiteral("child")));
    QVERIFY(!renew_populate.contains(QStringLiteral("lineage")));
}

void WorkspaceCGuardrailsTest::dangerZoneIsExplicitlyLabeled() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // Danger Zone must be explicitly labeled in the UI
    QVERIFY(source.contains(QStringLiteral("Danger Zone")));
    QVERIFY(source.contains(QStringLiteral("危險區域")));
    // Destructive action buttons must appear after the Danger Zone label
    const qsizetype dangerPos = source.indexOf(QStringLiteral("Danger Zone"));
    const qsizetype reclaimPos = source.indexOf(QStringLiteral("gcReclaimButton"));
    const qsizetype fullGcPos = source.indexOf(QStringLiteral("gcFullButton"));
    QVERIFY(dangerPos >= 0);
    QVERIFY(reclaimPos > dangerPos);
    QVERIFY(fullGcPos > dangerPos);
    // Preview button must appear before the Danger Zone (safe action comes first)
    const qsizetype previewPos = source.indexOf(QStringLiteral("gcPreviewButton"));
    QVERIFY(previewPos >= 0);
    QVERIFY(previewPos < dangerPos);
}

void WorkspaceCGuardrailsTest::confirmationIsActionSpecific() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // confirmGc must set action-specific dialog content before opening
    const QString body = functionBody(source, QStringLiteral("confirmGc"));
    QVERIFY(!body.isEmpty());
    QVERIFY(body.contains(QStringLiteral("pendingGcDialogTitle")));
    QVERIFY(body.contains(QStringLiteral("pendingGcDialogBody")));
    QVERIFY(body.contains(QStringLiteral("pendingGcConfirmLabel")));
    QVERIFY(body.contains(QStringLiteral("gcConfirm.open()")));
    // Each action type must have a distinct title
    QVERIFY(source.contains(QStringLiteral("Confirm Cleanup Sweep")));
    QVERIFY(source.contains(QStringLiteral("Confirm Cache Reclaim")));
    QVERIFY(source.contains(QStringLiteral("Confirm Full GC")));
    // The dialog must bind to the dynamic properties
    QVERIFY(source.contains(QStringLiteral("title: root.pendingGcDialogTitle")));
    QVERIFY(source.contains(QStringLiteral("body: root.pendingGcDialogBody")));
    QVERIFY(source.contains(QStringLiteral("confirmLabel: root.pendingGcConfirmLabel")));
}

void WorkspaceCGuardrailsTest::operationEvidenceHasModeAndStatus() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // Operation Evidence section must exist with user-facing label
    QVERIFY(source.contains(QStringLiteral("Operation Evidence")));
    QVERIFY(source.contains(QStringLiteral("操作憑據")));
    // Structured evidence properties must be declared
    QVERIFY(source.contains(QStringLiteral("property string lastOperationMode")));
    QVERIFY(source.contains(QStringLiteral("property string lastOperationStatus")));
    QVERIFY(source.contains(QStringLiteral("property string lastOperationTime")));
    // populateGcResult must fill the structured evidence fields
    const QString gc_result = functionBody(source, QStringLiteral("populateGcResult"));
    QVERIFY(!gc_result.isEmpty());
    QVERIFY(gc_result.contains(QStringLiteral("lastOperationMode")));
    QVERIFY(gc_result.contains(QStringLiteral("lastOperationStatus")));
    QVERIFY(gc_result.contains(QStringLiteral("lastOperationTime")));
    // Mode must distinguish preview from execute
    QVERIFY(gc_result.contains(QStringLiteral("Preview (no data modified)")));
    QVERIFY(gc_result.contains(QStringLiteral("Execute (data may have changed)")));
}

void WorkspaceCGuardrailsTest::maintenanceOverviewExists() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // Maintenance Control Center header
    QVERIFY(source.contains(QStringLiteral("Maintenance Control Center")));
    QVERIFY(source.contains(QStringLiteral("維護控制中心")));
    // Maintenance Overview card must exist
    QVERIFY(source.contains(QStringLiteral("Maintenance Overview")));
    QVERIFY(source.contains(QStringLiteral("維護總覽")));
    // Overview must show health indicators
    QVERIFY(source.contains(QStringLiteral("Storage")));
    QVERIFY(source.contains(QStringLiteral("Capacity health")));
    QVERIFY(source.contains(QStringLiteral("Record chain")));
    QVERIFY(source.contains(QStringLiteral("Last operation")));
}

void WorkspaceCGuardrailsTest::memberViewHidesDestructiveControls() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // MEMBER empty state must be shown when not PM
    QVERIFY(source.contains(QStringLiteral("visible: !root.pmMode")));
    QVERIFY(source.contains(QStringLiteral("Destructive maintenance actions are hidden for this role.")));
    QVERIFY(source.contains(QStringLiteral("此角色不顯示破壞性維護動作。")));
    // PM-only content must be gated with pmMode check
    QVERIFY(source.contains(QStringLiteral("visible: root.pmMode")));
    // All destructive buttons must be inside the pmMode-gated layout
    // (verified by position: gcReclaimButton and gcFullButton appear after pmMode gating)
    const qsizetype pmGatePos = source.indexOf(QStringLiteral("visible: root.pmMode"));
    const qsizetype reclaimPos = source.indexOf(QStringLiteral("gcReclaimButton"));
    const qsizetype fullGcPos = source.indexOf(QStringLiteral("gcFullButton"));
    QVERIFY(pmGatePos >= 0);
    QVERIFY(reclaimPos > pmGatePos);
    QVERIFY(fullGcPos > pmGatePos);
}

void WorkspaceCGuardrailsTest::memberFinalExecutionIsQmlGuarded() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // runPendingGc is the final QML execution point triggered by ConfirmDialog.confirmAction.
    // It must re-check pmMode so a direct invocation without the UI flow is also blocked.
    const QString body = functionBody(source, QStringLiteral("runPendingGc"));
    QVERIFY(!body.isEmpty());
    // PM re-check must appear before gcRunner.runJson
    const qsizetype pmCheckPos = body.indexOf(QStringLiteral("if (!pmMode)"));
    const qsizetype runJsonPos = body.indexOf(QStringLiteral("gcRunner.runJson"));
    QVERIFY(pmCheckPos >= 0);
    QVERIFY(runJsonPos > pmCheckPos);
    // Denial must call showMaintenanceAuthDenied, not just statusMessage
    QVERIFY(body.contains(QStringLiteral("showMaintenanceAuthDenied")));
    // The guard must return before gcRunner.runJson on non-PM path
    QVERIFY(body.contains(QStringLiteral("return")));
}

void WorkspaceCGuardrailsTest::mutatingCommandDenyPathSetsEvidenceFields() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // showMaintenanceAuthDenied must set all required Operation Evidence fields
    const QString body = functionBody(source, QStringLiteral("showMaintenanceAuthDenied"));
    QVERIFY(!body.isEmpty());
    QVERIFY(body.contains(QStringLiteral("lastOperationReady = true")));
    QVERIFY(body.contains(QStringLiteral("lastOperationMode")));
    QVERIFY(body.contains(QStringLiteral("lastOperationStatus")));
    QVERIFY(body.contains(QStringLiteral("lastOperationTime")));
    QVERIFY(body.contains(QStringLiteral("lastOperationSummary")));
    // Required exact evidence strings per task brief
    QVERIFY(body.contains(QStringLiteral("blocked")));
    QVERIFY(body.contains(QStringLiteral("Not authorized")));
    QVERIFY(body.contains(QStringLiteral("PM authorization is required for this maintenance action.")));
}

void WorkspaceCGuardrailsTest::previewCommandDoesNotRequireExecutionAuth() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    // previewReconcile runs the dry-run command without going through runPendingGc
    const QString body = functionBody(source, QStringLiteral("previewReconcile"));
    QVERIFY(!body.isEmpty());
    QVERIFY(body.contains(QStringLiteral("reconcilePreviewRunner.runJson")));
    QVERIFY(body.contains(QStringLiteral("[\"gc\", \"--reconcile\", \"--dry-run\"]")));
    // Preview must not call runPendingGc (which is the mutating path)
    QVERIFY(!body.contains(QStringLiteral("runPendingGc")));
    QVERIFY(!body.contains(QStringLiteral("gcRunner.runJson")));
    QVERIFY(!body.contains(QStringLiteral("gcConfirm.open")));
    // showMaintenanceAuthDenied must not be in the preview path
    QVERIFY(!body.contains(QStringLiteral("showMaintenanceAuthDenied")));
}

void WorkspaceCGuardrailsTest::backendBridgeGuardBlocksMutatingMaintenanceCommand() {
    // Verify the C++ IsvnCli backend has a bridge-level guard for mutating maintenance.
    const QString header = readFile(QStringLiteral("src/backend/IsvnCli.h"));
    QVERIFY(!header.isEmpty());
    // maintenanceRole property must be declared so QML can wire the role
    QVERIFY(header.contains(QStringLiteral("maintenanceRole")));
    QVERIFY(header.contains(QStringLiteral("setMaintenanceRole")));
    QVERIFY(header.contains(QStringLiteral("maintenanceRoleChanged")));
    // isMutatingMaintenanceCommand must be declared
    QVERIFY(header.contains(QStringLiteral("isMutatingMaintenanceCommand")));

    const QString impl = readFile(QStringLiteral("src/backend/IsvnCli.cpp"));
    QVERIFY(!impl.isEmpty());
    // isMutatingMaintenanceCommand must classify "gc" without --dry-run as mutating
    QVERIFY(impl.contains(QStringLiteral("isMutatingMaintenanceCommand")));
    QVERIFY(impl.contains(QStringLiteral("\"gc\"")));
    QVERIFY(impl.contains(QStringLiteral("--dry-run")));
    // The guard must use the maintenance_role_ check and emit commandFailed without spawning
    QVERIFY(impl.contains(QStringLiteral("maintenance-auth-denied")));
    QVERIFY(impl.contains(QStringLiteral("maintenance_role_")));
    // Main.qml must wire the roleResolver to isvnCli.maintenanceRole
    const QString mainQml = readFile(QStringLiteral("qml/Main.qml"));
    QVERIFY(!mainQml.isEmpty());
    QVERIFY(mainQml.contains(QStringLiteral("maintenanceRole")));
    QVERIFY(mainQml.contains(QStringLiteral("roleResolver.roleName")));
}

void WorkspaceCGuardrailsTest::denialEvidenceHasNoRawBackendTerminology() {
    const QString source = readFile(QStringLiteral("qml/workspaceC/WorkspaceC.qml"));
    QVERIFY(!source.isEmpty());
    const QString body = functionBody(source, QStringLiteral("showMaintenanceAuthDenied"));
    QVERIFY(!body.isEmpty());
    // Denial text must not expose raw backend terms to the user
    QVERIFY(!body.contains(QStringLiteral("gc --reconcile")));
    QVERIFY(!body.contains(QStringLiteral("--reclaim")));
    QVERIFY(!body.contains(QStringLiteral("pool-stat")));
    QVERIFY(!body.contains(QStringLiteral("pool_root")));
    QVERIFY(!body.contains(QStringLiteral("meta.db")));
    QVERIFY(!body.contains(QStringLiteral("copy-from")));
    QVERIFY(!body.contains(QStringLiteral("maintenance-auth-denied")));
    // No impersonation or privilege escalation in denial path
    QVERIFY(!body.contains(QStringLiteral("sudo")));
    QVERIFY(!body.contains(QStringLiteral("setuid")));
    QVERIFY(!body.contains(QStringLiteral("runAs")));
}

QTEST_MAIN(WorkspaceCGuardrailsTest)

#include "test_workspace_c_guardrails.moc"
