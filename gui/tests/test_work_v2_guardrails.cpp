// test_work_v2_guardrails — source-level guardrails for the Work shell. These
// assert the structure and properties the Work rules depend on (not brittle
// whole-file snapshots) and check that the Work render components represent
// those rules with NO backend binding.
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTest>

class WorkV2GuardrailsTest : public QObject {
    Q_OBJECT

private slots:
    void shellHasFourDestinations();
    void noProjectSelectorExists();
    void activeAndInspectedAreDistinct();
    void localHierarchyIsPhaseBlockStageVersion();
    void workLayoutIsResizableSplit();
    void correctedStateIdsExist();
    void noCheckinPartialResult();
    void confirmRequiresMessage();
    void successUsesOneRevisionForWholeBatch();
    void outcomeUnknownDisablesAutomaticRetry();
    void removeIsIndependentFromBatch();
    void managedImportReplacementCannotBeSelected();
    void startupDoesNotAutoCheckout();
    void newWorkQmlHasNoBackendBinding();
    void legacyMainRemainsDefaultEntry();
// Read-binding guardrails.
    void readControllerIsReadOnlyAndSerialized();
    void productShellWiresReadControllerWithoutRunJson();
    void readModeShowsNonExecutingActions();
};

namespace {

QString read(const QString& relative) {
    QFile file(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QStringList newWorkQmlFiles() {
    return {
        QStringLiteral("qml/shell/ProductShell.qml"),
        QStringLiteral("qml/shell/ProductNavigation.qml"),
        QStringLiteral("qml/shell/TopCommandBar.qml"),
        QStringLiteral("qml/shell/BottomStatusBar.qml"),
        QStringLiteral("qml/work/WorkPage.qml"),
        QStringLiteral("qml/work/WorkHeader.qml"),
        QStringLiteral("qml/work/LocalVersionNavigator.qml"),
        QStringLiteral("qml/work/FileStatusWorkspace.qml"),
        QStringLiteral("qml/work/WorkflowPanel.qml"),
        QStringLiteral("qml/work/CheckinReview.qml"),
        QStringLiteral("qml/work/CheckinConfirm.qml"),
        QStringLiteral("qml/work/CheckinResult.qml"),
        QStringLiteral("qml/work/OutcomeUnknown.qml"),
        QStringLiteral("qml/work/ImportPreview.qml"),
        QStringLiteral("qml/work/OperationStepper.qml"),
        QStringLiteral("qml/work/WorkMockModel.qml"),
        QStringLiteral("qml/work/WorkUiState.qml"),
    };
}

}  // namespace

void WorkV2GuardrailsTest::shellHasFourDestinations() {
    const QString nav = read(QStringLiteral("qml/shell/ProductNavigation.qml"));
    QVERIFY(!nav.isEmpty());
    QVERIFY(nav.contains(QStringLiteral("\"Home\"")));
    QVERIFY(nav.contains(QStringLiteral("\"Work\"")));
    QVERIFY(nav.contains(QStringLiteral("\"Explore\"")));
    QVERIFY(nav.contains(QStringLiteral("\"Maintenance\"")));
}

void WorkV2GuardrailsTest::noProjectSelectorExists() {
    const QString top = read(QStringLiteral("qml/shell/TopCommandBar.qml"));
    const QString shell = read(QStringLiteral("qml/shell/ProductShell.qml"));
    QVERIFY(!top.isEmpty());
    // Fixed, non-editable identity only.
    QVERIFY(top.contains(QStringLiteral("fixedProjectName")));
    QVERIFY(top.contains(QStringLiteral("fixedWorkspacePath")));
    // No project dropdown/switcher/cross-project control anywhere in the shell.
    for (const QString& source : {top, shell}) {
        QVERIFY(!source.contains(QStringLiteral("ComboBox")));
        QVERIFY(!source.contains(QStringLiteral("projectSwitcher")));
        QVERIFY(!source.contains(QStringLiteral("projectDropdown")));
        QVERIFY(!source.contains(QStringLiteral("cross-project")));
    }
}

void WorkV2GuardrailsTest::activeAndInspectedAreDistinct() {
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    QVERIFY(!state.isEmpty());
    QVERIFY(state.contains(QStringLiteral("activeVersionId")));
    QVERIFY(state.contains(QStringLiteral("inspectedVersionId")));
    // They must be separate properties, not aliases of one another.
    QVERIFY(state.contains(QStringLiteral("property string activeVersionId")));
    QVERIFY(state.contains(QStringLiteral("property string inspectedVersionId")));
}

void WorkV2GuardrailsTest::localHierarchyIsPhaseBlockStageVersion() {
    const QString model = read(QStringLiteral("qml/work/WorkMockModel.qml"));
    const QString nav = read(QStringLiteral("qml/work/LocalVersionNavigator.qml"));
    QVERIFY(!model.isEmpty());
    QVERIFY(model.contains(QStringLiteral("\"phase\"")));
    QVERIFY(model.contains(QStringLiteral("\"block\"")));
    QVERIFY(model.contains(QStringLiteral("\"stage\"")));
    QVERIFY(model.contains(QStringLiteral("\"versionName\"")));
    // Navigator renders the Phase/Block/Stage hierarchy line.
    QVERIFY(nav.contains(QStringLiteral(".phase")));
    QVERIFY(nav.contains(QStringLiteral(".block")));
    QVERIFY(nav.contains(QStringLiteral(".stage")));
}

void WorkV2GuardrailsTest::workLayoutIsResizableSplit() {
    const QString page = read(QStringLiteral("qml/work/WorkPage.qml"));
    QVERIFY(!page.isEmpty());
    QVERIFY(page.contains(QStringLiteral("SplitView")));
    QVERIFY(page.contains(QStringLiteral("SplitView.minimumWidth")));
}

void WorkV2GuardrailsTest::correctedStateIdsExist() {
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    QVERIFY(!state.isEmpty());
    for (const char* id : {"normal", "review", "conflictBlocked", "confirm",
                           "checkinSuccess", "checkinFailed", "outcomeUnknown",
                           "importPreview", "createCheckoutFailed"}) {
        QVERIFY2(state.contains(QLatin1Char('"') + QLatin1String(id) + QLatin1Char('"')),
                 id);
    }
}

void WorkV2GuardrailsTest::noCheckinPartialResult() {
    // A partial-batch result component must not exist under any spelling.
    QVERIFY(!QFile::exists(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                           QStringLiteral("/qml/work/PartialResult.qml")));
    QVERIFY(QFile::exists(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                          QStringLiteral("/qml/work/CheckinResult.qml")));
    for (const QString& relative : newWorkQmlFiles()) {
        const QString source = read(relative);
        QVERIFY2(!source.contains(QStringLiteral("PartialResult")), qPrintable(relative));
        QVERIFY2(!source.contains(QStringLiteral("Partial Result")), qPrintable(relative));
    }
}

void WorkV2GuardrailsTest::confirmRequiresMessage() {
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    const QString confirm = read(QStringLiteral("qml/work/CheckinConfirm.qml"));
    QVERIFY(!state.isEmpty());
    // The backend-equivalent guard: an empty/whitespace message is rejected.
    QVERIFY(state.contains(QStringLiteral("trimmed(checkinMessage).length === 0")));
    QVERIFY(confirm.contains(QStringLiteral("required")));
}

void WorkV2GuardrailsTest::successUsesOneRevisionForWholeBatch() {
    const QString result = read(QStringLiteral("qml/work/CheckinResult.qml"));
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    QVERIFY(!result.isEmpty());
    // One shared revision for the whole batch — never per-item revisions and
    // never a fixed placeholder revision.
    QVERIFY(result.contains(QStringLiteral("whole batch")));
    QVERIFY(result.contains(QStringLiteral("successRevision")));
    QVERIFY(!result.contains(QStringLiteral("r1291")));
    QVERIFY(state.contains(QStringLiteral("property string successRevision")));
}

void WorkV2GuardrailsTest::outcomeUnknownDisablesAutomaticRetry() {
    const QString outcome = read(QStringLiteral("qml/work/OutcomeUnknown.qml"));
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    QVERIFY(!outcome.isEmpty());
    // Verify is the only forward action; there is no direct Retry control.
    QVERIFY(outcome.contains(QStringLiteral("verifyOutcomeButton")));
    QVERIFY(!outcome.contains(QStringLiteral("retryButton")));
    QVERIFY(!outcome.contains(QStringLiteral("Retry Now")));
    QVERIFY(state.contains(QStringLiteral("autoRetryEnabled: false")));
}

void WorkV2GuardrailsTest::removeIsIndependentFromBatch() {
    const QString panel = read(QStringLiteral("qml/work/WorkflowPanel.qml"));
    QVERIFY(!panel.isEmpty());
    QVERIFY(panel.contains(QStringLiteral("removeButton")));
    // Remove states, in a user-visible way, that it never enters the batch.
    QVERIFY(panel.contains(QStringLiteral("never enters the Checkin Batch")));
    // Remove must not be wired as a batch add.
    QVERIFY(!panel.contains(QStringLiteral("addToBatch(\"remove\"")));
}

void WorkV2GuardrailsTest::managedImportReplacementCannotBeSelected() {
    const QString importView = read(QStringLiteral("qml/work/ImportPreview.qml"));
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    QVERIFY(!importView.isEmpty());
    // canReplace allows ONLY existing LOCAL; managed is a hard block.
    QVERIFY(state.contains(QStringLiteral("destinationState === \"localExisting\"")));
    QVERIFY(importView.contains(QStringLiteral("importManagedRow")));
    // The Replace Local checkbox exists only for the existing-LOCAL case.
    QVERIFY(importView.contains(QStringLiteral("localExisting ? \"replaceLocalCheckbox\" : \"\"")));
}

void WorkV2GuardrailsTest::startupDoesNotAutoCheckout() {
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    const QString host = read(QStringLiteral("qml/shell/WorkPreviewHost.qml"));
    QVERIFY(!state.isEmpty());
    QVERIFY(state.contains(QStringLiteral("autoCheckoutOnStartup: false")));
    // Neither the state machine nor the preview startup performs a checkout
    // command (the preview host may render a "checkout_failed" status string for
    // the Create composite screenshot, which is not a checkout invocation).
    QVERIFY(!state.contains(QStringLiteral("checkout(")));
    QVERIFY(!host.contains(QStringLiteral("\"checkout\"")));
    QVERIFY(!host.contains(QStringLiteral("startCreateDerived")));
}

void WorkV2GuardrailsTest::newWorkQmlHasNoBackendBinding() {
    // The pure Work render components must never touch the backend bridge; they
    // render only from the injected data model (mock preview or real read
    // model). ProductShell is the shell seam that wires the read controller, so
    // it may hold an isvnCli property but must not itself call runJson; that
    // read-only wiring is covered by the read-binding guardrails.
    for (const QString& relative : newWorkQmlFiles()) {
        const QString source = read(relative);
        QVERIFY2(!source.isEmpty(), qPrintable(relative));
        QVERIFY2(!source.contains(QStringLiteral("runJson")), qPrintable(relative));
        if (relative != QStringLiteral("qml/shell/ProductShell.qml")) {
            QVERIFY2(!source.contains(QStringLiteral("isvnCli")), qPrintable(relative));
            QVERIFY2(!source.contains(QStringLiteral("IsvnCli")), qPrintable(relative));
        }
    }
}

void WorkV2GuardrailsTest::legacyMainRemainsDefaultEntry() {
    const QString mainCpp = read(QStringLiteral("src/main.cpp"));
    QVERIFY(!mainCpp.isEmpty());
    // The Product Shell supersedes the old default: unset/new select the Product
    // Shell, while complete Main.qml remains an explicit runtime rollback.
    QVERIFY(mainCpp.contains(QStringLiteral("QStringLiteral(\"Main\")")));
    QVERIFY(mainCpp.contains(QStringLiteral("ProductShellWindow")));
    QVERIFY(mainCpp.contains(QStringLiteral("ISVN_GUI_SHELL")));
    QVERIFY(!mainCpp.contains(QStringLiteral("WorkPreviewHost")));
    // Legacy Main.qml still exists.
    QVERIFY(QFile::exists(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
}

void WorkV2GuardrailsTest::readControllerIsReadOnlyAndSerialized() {
    const QString c = read(QStringLiteral("qml/work/WorkReadController.qml"));
    QVERIFY(!c.isEmpty());
    // Only the two read commands are ever issued.
    QVERIFY(c.contains(QStringLiteral("\"workspaces\", \"--json\"")));
    QVERIFY(c.contains(QStringLiteral("\"work-status\", activeVersionId, \"--json\"")));
    // No mutation command token appears as a runJson argument literal.
    for (const char* mutation : {"\"checkin\"", "\"edit\"", "\"remove\"", "\"create\"",
                                 "\"checkout\"", "\"import\"", "\"revert\"", "\"delete\""}) {
        QVERIFY2(!c.contains(QLatin1String(mutation)), mutation);
    }
    // Serialized single-flight: never runJson while the bridge is running.
    QVERIFY(c.contains(QStringLiteral("isvnCli.running")));
    QVERIFY(c.contains(QStringLiteral("_phase")));
    // Stale-response protection by generation + version check.
    QVERIFY(c.contains(QStringLiteral("json.version !== activeVersionId")));
    // No auto-checkout / fallback selection intent is documented.
    QVERIFY(c.contains(QStringLiteral("never an auto-checkout")));
}

void WorkV2GuardrailsTest::productShellWiresReadControllerWithoutRunJson() {
    const QString shell = read(QStringLiteral("qml/shell/ProductShell.qml"));
    QVERIFY(!shell.isEmpty());
    QVERIFY(shell.contains(QStringLiteral("WorkReadController")));
    QVERIFY(shell.contains(QStringLiteral("readMode")));
    // The shell wires the controller but never calls the bridge itself.
    QVERIFY(!shell.contains(QStringLiteral("runJson")));
}

void WorkV2GuardrailsTest::readModeShowsNonExecutingActions() {
    const QString panel = read(QStringLiteral("qml/work/WorkflowPanel.qml"));
    QVERIFY(!panel.isEmpty());
    // Mutating actions are disabled under the read-only binding.
    QVERIFY(panel.contains(QStringLiteral("readOnlyBinding")));
    const QString state = read(QStringLiteral("qml/work/WorkUiState.qml"));
    QVERIFY(state.contains(QStringLiteral("property bool readOnlyBinding")));
}

QTEST_MAIN(WorkV2GuardrailsTest)

#include "test_work_v2_guardrails.moc"
