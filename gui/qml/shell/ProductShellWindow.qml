import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// Default Product Shell root. The ProductShell and its coordinators remain
// alive when this window is hidden; only an explicit normal exit destroys it.
ApplicationWindow {
    id: root
    objectName: "productShellWindow"
    width: 1440
    height: 860
    minimumWidth: 1120
    minimumHeight: 720
    visible: true
    title: "isvn"
    property bool hiddenForBackground: false
    property var persistedTerminal: ({})
    property bool hiddenTerminalNotified: false
    // Preserve context-property bindings at the window boundary.  Passing a
    // same-named context property directly to a child property resolves to the
    // child property itself in QML and creates a binding loop.
    property var cliBridge: isvnCli
    property var sessionStoreBridge: workSessionStore
    property var roleResolverBridge: roleResolver
    property var buildInfoBridge: buildInfo
    property var launchContextBridge: launchContext

    ProductShell {
        id: shell
        objectName: "defaultProductShell"
        anchors.fill: parent
        readMode: true
        isvnCli: root.cliBridge
        sessionStore: root.sessionStoreBridge
        roleResolver: root.roleResolverBridge
        projectPath: root.roleResolverBridge ? root.roleResolverBridge.projectDir : ""
        buildInfo: root.buildInfoBridge
        workspaceRoot: root.launchContextBridge && root.launchContextBridge.workspaceRoot
                       ? root.launchContextBridge.workspaceRoot : ""
    }

    function runningOperations() {
        return shell.operationCoordinator.activeMutationCount > 0
    }
    function unresolvedTerminal() {
        var all = shell.operationCoordinator.allOperations()
        for (var i = 0; i < all.length; ++i) {
            var state = all[i].state
            if (state === "failed" || state === "unknown" || state === "manualReview" || state === "recovery")
                return true
        }
        return false
    }
    // Safety net for the Activity Log: a change whose flow is no longer on
    // screen still gets a record.  Reads are not activity.  The record is keyed
    // by the backend operation id when the result names one, so the detailed
    // record that the flow's own controller writes for the same operation
    // replaces this one instead of being listed beside it.
    function persistTerminal(record) {
        if (!record || record.mutation !== true) return
        // Create Derived records its own outcome once, not once per step.
        if (record.operationKind === "work-create") return
        if ((record.state !== "success" && record.state !== "failed" &&
             record.state !== "unknown" && record.state !== "manualReview" &&
             record.state !== "recovery") || persistedTerminal[record.operationId]) return
        var next = {}
        for (var key in persistedTerminal) next[key] = persistedTerminal[key]
        next[record.operationId] = true
        persistedTerminal = next
        var backendId = record.result && record.result.operation_id
                        ? String(record.result.operation_id) : ""
        workSessionStore.appendActivity({
            "operationId": backendId.length > 0 ? backendId : record.operationId,
            "operationType": record.operationKind,
            "target": record.targetVersion,
            "status": record.state,
            "summary": "terminal operation result",
            "endTime": Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss")
        })
    }
    function evaluateHiddenTerminalPolicy() {
        if (!hiddenForBackground || runningOperations()) return
        // Safe Activity persistence occurs before a successful hidden exit.
        var operations = shell.operationCoordinator.allOperations()
        var target = operations.length > 0 ? operations[0].targetVersion : ""
        if (!hiddenTerminalNotified) {
            hiddenTerminalNotified = true
            safeNotifier.notify("isvn operation",
                                unresolvedTerminal() ? "Needs attention" : "Completed",
                                target, "")
        }
        if (!unresolvedTerminal()) Qt.callLater(Qt.quit)
        // Failed/Unknown/Manual/Recovery intentionally stay hidden and are
        // reactivated by the same private instance endpoint; no auto-focus.
    }
    function runInBackground() {
        hiddenForBackground = true
        hiddenTerminalNotified = false
        root.hide()
        evaluateHiddenTerminalPolicy()
    }
    function reactivate() {
        hiddenForBackground = false
        hiddenTerminalNotified = false
        root.showNormal()
        root.raise()
        root.requestActivate()
        shell.openOperationCenter()
    }
    function requestForceExit() {
        var operations = shell.operationCoordinator.allOperations()
        for (var i = 0; i < operations.length; ++i) {
            var operation = operations[i]
            if (operation.state === "running" || operation.state === "starting") {
                workSessionStore.appendActivity({
                    "operationId": operation.operationId,
                    "operationType": operation.operationKind,
                    "target": operation.targetVersion,
                    "status": "shutdown_requested",
                    "summary": "bounded force-exit requested",
                    "endTime": Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss")
                })
            }
        }
        isvnCli.requestBoundedShutdown(250)
        forceExitDialog.close()
        // Keep the event loop alive through the documented grace bound so the
        // C++ termination timer can reap/kill children before the GUI exits.
        forceExitTimer.start()
    }

    onClosing: function(close) {
        if (runningOperations()) {
            close.accepted = false
            closeOperationsDialog.open()
        }
    }

    Connections {
        target: shell.operationCoordinator
        function onOperationChanged(operationId, targetVersion) {
            var record = shell.operationCoordinator.operationFor(operationId)
            root.persistTerminal(record)
            Qt.callLater(root.evaluateHiddenTerminalPolicy)
        }
    }

    Timer {
        id: forceExitTimer
        interval: 300
        repeat: false
        onTriggered: Qt.quit()
    }
    Connections {
        target: isvnCli
        function onOperationLaneChanged() { Qt.callLater(root.evaluateHiddenTerminalPolicy) }
    }

    Dialog {
        id: closeOperationsDialog
        objectName: "closeOperationsDialog"
        modal: true
        focus: true
        title: shell.i18n.text("Operations are still running", "操作仍在執行", "操作仍在执行")
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, root.width - 64)
        contentItem: ColumnLayout {
            spacing: shell.theme.gapMd
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: shell.i18n.text("Running operations will keep their target Version. You can safely hide this window and reopen it later.",
                                      "執行中的操作會保留原目標版本；可安全隱藏視窗並稍後重新開啟。",
                                      "运行中的操作会保留原目标版本；可安全隐藏窗口并稍后重新打开。")
                color: shell.theme.textSecondary
            }
            Repeater {
                model: shell.operationCoordinator.allOperations()
                delegate: Label {
                    required property var modelData
                    visible: modelData.state === "running" || modelData.state === "starting"
                    text: modelData.operationKind + " · " + modelData.targetVersion
                    color: shell.theme.textPrimary
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: shell.i18n.text("Return to Operations", "返回操作", "返回操作"); onClicked: closeOperationsDialog.close() }
                Button {
                    id: backgroundButton
                    objectName: "runInBackgroundButton"
                    text: shell.i18n.text("Run in Background", "在背景執行", "在后台运行")
                    focus: true
                    onClicked: { closeOperationsDialog.close(); root.runInBackground() }
                }
                Button {
                    objectName: "forceExitButton"
                    text: shell.i18n.text("Force Exit", "強制離開", "强制退出")
                    onClicked: { closeOperationsDialog.close(); forceExitDialog.open() }
                }
            }
        }
    }

    Dialog {
        id: forceExitDialog
        objectName: "forceExitConfirmDialog"
        modal: true
        focus: true
        title: shell.i18n.text("Force Exit?", "強制離開？", "强制退出？")
        anchors.centerIn: Overlay.overlay
        width: Math.min(540, root.width - 64)
        contentItem: ColumnLayout {
            spacing: shell.theme.gapMd
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: shell.i18n.text("Child processes will be stopped after a bounded grace period. The next launch will discover journals and offer recovery; no rollback is claimed now.",
                                      "子程序會在受限寬限期後停止。下次啟動會偵測 journal 並提供復原；現在不宣稱已復原。",
                                      "子进程会在受限宽限期后停止。下次启动会检测 journal 并提供恢复；现在不宣称已恢复。")
                color: shell.theme.textSecondary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: shell.i18n.text("Cancel", "取消", "取消"); focus: true; onClicked: forceExitDialog.close() }
                Button { objectName: "forceExitConfirmButton"; text: shell.i18n.text("Force Exit", "強制離開", "强制退出"); onClicked: root.requestForceExit() }
            }
        }
    }
}
