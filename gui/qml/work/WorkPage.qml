import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// WorkPage — the Work screen anatomy. Normal state uses a resizable three-region
// SplitView (Local Version Navigator | File Status Workspace | Workflow Panel).
// Operation states keep the navigator visible and merge the center/right into a
// single focused operation workspace while preserving target/version context.
Item {
    id: root

    property var theme
    property var i18n
    property var workState
    property var workModel
    // Present only in real-read mode; routes Active switching through
    // the backend read controller (which validates + persists + refreshes).
    property var readController: null
    readonly property string readState: readController ? readController.state : "ready"
    // Transactional action orchestration (read mode only).
    property var actionController: null
    // Atomic Checkin Batch orchestration (read mode only).
    property var checkinController: null
    // External Import orchestration (read mode only).
    property var importController: null
    // The target-keyed, long-lived coordinator remains alive above
    // this page and exposes busy/result state per Version rather than per
    // currently visible Active Version.
    property var operationCoordinator: null
    signal inspectInExploreRequested(string versionId)

    // The controller reports a successful transaction using its immutable
    // target Version.  Pruning a stale Checkin selector is session-only UI
    // hygiene; it cannot retarget or mutate a different Version.
    Connections {
        target: root.actionController
        function onPathsInvalidated(targetVersion, paths) {
            if (root.checkinController)
                root.checkinController.pruneInvalidatedPaths(paths, targetVersion)
        }
    }

    readonly property bool normal: root.workState
                                   && root.workState.stateId === root.workState.stateNormal

    function isOperation() {
        return root.workState && root.workState.stateId !== root.workState.stateNormal
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        WorkHeader {
            Layout.fillWidth: true
            Layout.preferredHeight: root.theme ? root.theme.workHeaderHeight : 104
            theme: root.theme
            i18n: root.i18n
            workState: root.workState
            workModel: root.workModel
            realBinding: root.actionController !== null
            canInspect: root.readController ? root.readController.activeVersionId.length > 0 : true
            canCreateDerived: root.actionController
                              ? (root.readController && root.readController.activeVersionId.length > 0 &&
                                 !root.actionController.mutationsBlocked)
                              : true
            onInspectInExploreRequested: root.inspectInExploreRequested(root.readController
                                                                          ? root.readController.activeVersionId
                                                                          : (root.workState ? root.workState.activeVersionId : ""))
            onCreateDerivedRequested: createDerivedDialog.open()
        }

        SplitView {
            id: split
            objectName: "workSplitView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: root.theme ? root.theme.gapMd : 12
                color: "transparent"
                Rectangle {
                    anchors.centerIn: parent
                    width: 1
                    height: parent.height
                    color: root.theme ? root.theme.border : "#263946"
                }
            }

            LocalVersionNavigator {
                objectName: "localVersionNavigator"
                SplitView.preferredWidth: 260
                SplitView.minimumWidth: 210
                SplitView.maximumWidth: 340
                theme: root.theme
                i18n: root.i18n
                workState: root.workState
                workModel: root.workModel
                operationCoordinator: root.operationCoordinator
                onVersionActivateRequested: function(versionId) {
                    // A Version-scoped operation stays pinned to its own
                    // immutable target.  Switching to B must never cancel or
                    // retarget an already-admitted A operation.
                    if (root.readController)
                        root.readController.selectActive(versionId)
                    else
                        root.workState.requestActiveSwitch(versionId)
                }
            }

            // Normal state: File Status (dominant) + Workflow Panel, resizable.
            SplitView {
                id: centerRight
                objectName: "workCenterRight"
                visible: root.normal
                SplitView.fillWidth: true
                SplitView.minimumWidth: 560
                orientation: Qt.Horizontal

                handle: Rectangle {
                    implicitWidth: root.theme ? root.theme.gapMd : 12
                    color: "transparent"
                    Rectangle {
                        anchors.centerIn: parent
                        width: 1
                        height: parent.height
                        color: root.theme ? root.theme.border : "#263946"
                    }
                }

                FileStatusWorkspace {
                    objectName: "fileStatusWorkspace"
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 420
                    theme: root.theme
                    i18n: root.i18n
                    workState: root.workState
                    workModel: root.workModel
                    checkinController: root.checkinController
                    onSelectionRequested: function(paths) {
                        if (root.checkinController) root.checkinController.setSelection(paths)
                    }
                    onImportRequested: function(dest, urls) {
                        if (root.importController) root.importController.startDrop(dest, urls)
                    }
                }

                WorkflowPanel {
                    SplitView.preferredWidth: 380
                    SplitView.minimumWidth: 320
                    SplitView.maximumWidth: 460
                    theme: root.theme
                    i18n: root.i18n
                    workState: root.workState
                    workModel: root.workModel
                    checkinController: root.checkinController
                    actionController: root.actionController
                    operationCoordinator: root.operationCoordinator
                    activeVersion: root.readController ? root.readController.activeVersionId
                                                       : (root.workState ? root.workState.activeVersionId : "")
                    onReviewCheckinRequested: {
                        if (root.checkinController) root.checkinController.addToBatch()
                        else root.workState.beginReview()
                    }
                    onAddToBatchRequested: {
                        if (root.checkinController) root.checkinController.addToBatch()
                    }
                    onCreateDerivedRequested: {
                        if (root.actionController) createDerivedDialog.open()
                        else createDerivedDialog.open()
                    }
                    onInspectInExploreRequested: root.inspectInExploreRequested(root.readController
                                                                                  ? root.readController.activeVersionId
                                                                                  : (root.workState ? root.workState.activeVersionId : ""))
                    onRemoveRequested: function(paths) {
                        if (root.actionController) root.actionController.startRemove(paths)
                        else removeDialog.open()
                    }
                    onMakeEditableRequested: function(paths) {
                        if (root.actionController) root.actionController.startLocalAction("make-editable", paths)
                    }
                    onRevertRequested: function(paths) {
                        if (root.actionController) root.actionController.startLocalAction("revert", paths)
                    }
                    onDiscardLocalRequested: function(paths) {
                        if (root.actionController) root.actionController.startLocalAction("discard-local", paths)
                    }
                }
            }

            // Operation states: a single focused operation workspace.
            Item {
                objectName: "operationArea"
                visible: root.isOperation()
                SplitView.fillWidth: true
                SplitView.minimumWidth: 560

                Loader {
                    anchors.fill: parent
                    active: root.isOperation()
                    sourceComponent: {
                        if (!root.workState)
                            return null
                        var s = root.workState.stateId
                        if (s === root.workState.stateReview || s === root.workState.stateConflictBlocked)
                            return reviewComponent
                        if (s === root.workState.stateConfirm)
                            return confirmComponent
                        if (s === root.workState.stateCheckinSuccess || s === root.workState.stateCheckinFailed)
                            return resultComponent
                        if (s === root.workState.stateOutcomeUnknown)
                            return outcomeComponent
                        if (s === root.workState.stateImportPreview)
                            return importComponent
                        if (s === root.workState.stateCreateCheckoutFailed)
                            return createResultComponent
                        return null
                    }
                }
            }
        }
    }

    // Manual Refresh control (read mode). No aggressive polling occurs.
    Common.ActionButton {
        objectName: "workRefreshButton"
        visible: root.readController
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: root.theme ? root.theme.gapSm : 8
        anchors.rightMargin: root.theme ? root.theme.gapXl : 24
        z: 5
        theme: root.theme
        implicitHeight: 28
        text: root.i18n ? root.i18n.text("Refresh", "重新整理", "刷新") : "Refresh"
        onClicked: if (root.readController) root.readController.refresh()
    }

    // Read-mode loading / empty / error overlay. It never falls back
    // to mock data on a real read failure.
    Rectangle {
        objectName: "readStateOverlay"
        anchors.fill: parent
        visible: root.readController && root.readState !== "ready"
        color: root.theme ? root.theme.workSurface0 : "#0a1823"

        Common.EmptyState {
            anchors.centerIn: parent
            width: Math.min(520, parent.width - 80)
            theme: root.theme
            title: root.readState === "loading"
                   ? (root.i18n ? root.i18n.text("Loading…", "載入中…", "载入中…") : "Loading…")
                   : root.readState === "empty"
                     ? (root.i18n ? root.i18n.text("No local working versions", "沒有本機工作版本", "没有本机工作版本") : "No local working versions")
                     : (root.i18n ? root.i18n.text("Could not read Work data", "無法讀取工作資料", "无法读取工作数据") : "Could not read Work data")
            message: root.readState === "error" && root.readController
                     ? root.readController.errorText
                     : (root.readState === "empty"
                        ? (root.i18n ? root.i18n.text("Check out a version to begin, then Refresh.", "先檢出一個版本，然後重新整理。", "先检出一个版本，然后刷新。") : "Check out a version to begin, then Refresh.")
                        : "")
            actionText: root.readState === "error"
                        ? (root.i18n ? root.i18n.text("Refresh", "重新整理", "刷新") : "Refresh") : ""
            onActionRequested: if (root.readController) root.readController.refresh()
        }
    }

    // Transactional action overlay (confirm/apply/result/recovery/outcome/create).
    WorkOperationOverlay {
        objectName: "workOperationOverlay"
        anchors.fill: parent
        z: 8
        theme: root.theme
        i18n: root.i18n
        controller: root.actionController
        activeVersion: root.workState ? root.workState.activeVersionId : ""
    }

    // Bounded-query failures are controller-owned structured states.
    // They deliberately overlay the existing Work surface rather than allowing
    // an old Review/Confirm to remain actionable.
    CheckinBatchOverlay {
        objectName: "checkinBatchOverlay"
        anchors.fill: parent
        z: 9
        theme: root.theme
        i18n: root.i18n
        controller: root.checkinController
        activeVersion: root.workState ? root.workState.activeVersionId : ""
    }

    ImportOverlay {
        objectName: "importOverlay"
        anchors.fill: parent
        z: 10
        theme: root.theme
        i18n: root.i18n
        controller: root.importController
        activeVersion: root.workState ? root.workState.activeVersionId : ""
    }

    Component { id: reviewComponent
        CheckinReview { theme: root.theme; i18n: root.i18n; workState: root.workState; workModel: root.workModel } }
    Component { id: confirmComponent
        CheckinConfirm { theme: root.theme; i18n: root.i18n; workState: root.workState; workModel: root.workModel } }
    Component { id: resultComponent
        CheckinResult { theme: root.theme; i18n: root.i18n; workState: root.workState; workModel: root.workModel } }
    Component { id: outcomeComponent
        OutcomeUnknown { theme: root.theme; i18n: root.i18n; workState: root.workState; workModel: root.workModel } }
    Component { id: importComponent
        ImportPreview { theme: root.theme; i18n: root.i18n; workState: root.workState; workModel: root.workModel } }

    // Create Derived Version composite result (Create -> Checkout -> Set Active).
    // Create succeeded but Checkout failed: keep the created version, keep the
    // previous Active Working Version, expose Retry Checkout / Inspect in Explore.
    Component {
        id: createResultComponent
        Common.ShellPanel {
            theme: root.theme
            ColumnLayout {
                anchors.fill: parent
                spacing: root.theme ? root.theme.gapMd : 12
                OperationStepper {
                    Layout.fillWidth: true
                    theme: root.theme
                    steps: ["Create", "Checkout", "Set Active"]
                    currentStep: 1
                    failedStep: 1
                }
                Common.SeverityBanner {
                    Layout.fillWidth: true
                    theme: root.theme
                    severity: "warn"
                    title: root.i18n ? root.i18n.text("Version created · Checkout failed", "版本已建立 · 檢出失敗", "版本已创建 · 检出失败") : "Version created · Checkout failed"
                    message: root.i18n ? root.i18n.text("The repository version was created but Checkout failed. The created version is retained and the previous Active Working Version is unchanged.",
                                                        "版本庫版本已建立，但檢出失敗。已保留新建立的版本，且先前的使用中工作版本維持不變。",
                                                        "版本库版本已创建，但检出失败。已保留新创建的版本，且先前的使用中工作版本维持不变。")
                                       : "The repository version was created but Checkout failed. The created version is retained and the previous Active Working Version is unchanged."
                }
                Common.KeyValueGrid {
                    Layout.fillWidth: true
                    theme: root.theme
                    monoValue: true
                    keyWidth: 160
                    rows: [
                        { key: (root.i18n ? root.i18n.text("Created version", "已建立版本", "已创建版本") : "Created version"), value: (root.workState ? root.workState.createdVersionId : "") },
                        { key: (root.i18n ? root.i18n.text("Active (unchanged)", "使用中（未變更）", "使用中（未变更）") : "Active (unchanged)"), value: (root.workState ? root.workState.activeVersionId : "") }
                    ]
                }
                RowLayout {
                    Layout.fillWidth: true
                    Common.ActionButton { theme: root.theme; kind: "primary"
                        text: root.i18n ? root.i18n.text("Retry Checkout", "重試檢出", "重试检出") : "Retry Checkout"
                        onClicked: root.workState.backToWork() }
                    Common.ActionButton { theme: root.theme
                        text: root.i18n ? root.i18n.text("Inspect in Explore", "在探索檢視", "在探索查看") : "Inspect in Explore"
                        onClicked: { root.workState.inspectedVersionId = root.workState.createdVersionId; root.workState.backToWork() } }
                    Item { Layout.fillWidth: true }
                    Common.ActionButton { theme: root.theme
                        text: root.i18n ? root.i18n.text("Back to Work", "返回工作", "返回工作") : "Back to Work"
                        onClicked: root.workState.backToWork() }
                }
                Item { Layout.fillHeight: true }
            }
        }
    }

    // Switching Active with a non-empty Batch requires an explicit discard/cancel
    // decision; the batch is never silently retargeted.
    Connections {
        target: root.workState
        function onRequiresBatchDecisionChanged() {
            if (root.workState.requiresBatchDecision)
                batchSwitchDialog.open()
        }
    }

    Common.ConfirmDialog {
        id: batchSwitchDialog
        objectName: "batchSwitchDialog"
        anchors.centerIn: Overlay.overlay
        i18n: root.i18n
        theme: root.theme
        confirmLabel: root.i18n ? root.i18n.text("Discard batch & switch", "捨棄批次並切換", "舍弃批次并切换") : "Discard batch & switch"
        body: root.i18n ? root.i18n.text("The current Checkin Batch is bound to its target version. Switching the Active Working Version requires discarding the batch or cancelling.",
                                         "目前的簽入批次綁定其目標版本。切換使用中工作版本需要捨棄批次或取消。",
                                         "当前的签入批次绑定其目标版本。切换使用中工作版本需要舍弃批次或取消。")
                        : "The current Checkin Batch is bound to its target version. Switching the Active Working Version requires discarding the batch or cancelling."
        confirmAction: function() { root.workState.discardBatchAndSwitch() }
        onRejected: root.workState.cancelActiveSwitch()
    }

    Common.ConfirmDialog {
        id: createDerivedDialog
        objectName: "createDerivedDialog"
        anchors.centerIn: Overlay.overlay
        i18n: root.i18n
        theme: root.theme
        confirmLabel: root.i18n ? root.i18n.text("Create → Checkout → Set Active", "建立 → 檢出 → 設為使用中", "创建 → 检出 → 设为使用中") : "Create → Checkout → Set Active"
        body: root.i18n ? root.i18n.text("Create Derived Version performs three steps: create the repository version, checkout locally, then set it as the Active Working Version. Set Active only happens after checkout succeeds.",
                                         "建立衍生版本會執行三個步驟：建立版本庫版本、於本機檢出，然後設為使用中工作版本。只有在檢出成功後才會設為使用中。",
                                         "创建衍生版本会执行三个步骤：创建版本库版本、于本机检出，然后设为使用中工作版本。只有在检出成功后才会设为使用中。")
                        : "Create Derived Version performs three steps: create the repository version, checkout locally, then set it as the Active Working Version. Set Active only happens after checkout succeeds."
    // Preview-only fallback. Default ProductShell invokes the transactional
        // controller composite above and never enters this WorkUiState branch.
        confirmAction: function() {
            if (root.actionController)
                root.actionController.startCreateDerived(root.workState.activeVersionId + ".d")
            else
                root.workState.markCreateCheckoutFailed()
        }
    }

    Common.ConfirmDialog {
        id: removeDialog
        objectName: "removeDialog"
        anchors.centerIn: Overlay.overlay
        i18n: root.i18n
        theme: root.theme
        confirmLabel: root.i18n ? root.i18n.text("Remove", "移除", "移除") : "Remove"
            body: root.i18n ? root.i18n.text("Remove is a separate high-risk atomic operation with its own required message; it never enters the Checkin Batch. No action is performed from this preview.",
                                              "移除是獨立的高風險原子操作，需自己的必填訊息；不會進入簽入批次。此預覽不會執行任何動作。",
                                              "移除是独立的高风险原子操作，需自己的必填信息；不会进入签入批次。此预览不会执行任何动作。")
                                 : "Remove is a separate high-risk atomic operation with its own required message; it never enters the Checkin Batch. No action is performed from this preview."
        confirmAction: function() {}
    }
}
