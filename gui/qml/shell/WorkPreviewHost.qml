import QtQuick
import QtQuick.Controls
import "." as Shell

// WorkPreviewHost — test and development host for the Work shell.
//
// It is deliberately NOT registered in the production Isvn.Gui module and is
// never the production entry point (isvn-gui loads its shell from that module).
// It selects a deterministic Work state by ID, optionally captures a screenshot
// of a NAMED QML-engine-owned Item (never window.contentItem, which cannot be
// captured reliably), and cannot hang: capture failure or a timeout exits with
// a nonzero code.
ApplicationWindow {
    id: window

    property string screenshotPath: ""
    property int settleMs: 700
    property int watchdogMs: 8000

    width: 1600
    height: 900
    minimumWidth: 1200
    minimumHeight: 720
    visible: true
    title: "isvn Work preview"
    color: shell.theme.workSurface0

    property bool readMode: false
    property string seedActive: ""
    property string seedActiveWs: ""
    property string sessionFile: ""
    property string opState: ""   // seed an operation overlay for screenshots
    property string checkinState: "" // seed a controller-owned Checkin error overlay
    property string importState: "" // seed controller-only Import states
    property string concurrencyState: "" // seed target-keyed operation states

// Deterministically seed the action controller into a transactional phase so the
    // operation overlay can be captured without a live backend round-trip.
    function seedOperation(op) {
        var ac = shell.actionController
        if (op === "makeEditableConfirm") {
            ac.action = "make-editable"; ac.previewData = { "plan_hash": "H", "ready": true }; ac.phase = "confirm"
        } else if (op === "discardConfirm") {
            ac.action = "discard-local"; ac.previewData = { "plan_hash": "H", "ready": true }; ac.phase = "confirm"
        } else if (op === "revertConfirm") {
            ac.action = "revert"; ac.previewData = { "plan_hash": "H", "ready": true }; ac.phase = "confirm"
        } else if (op === "removeConfirm") {
            ac.action = "remove"; ac.previewData = { "plan_hash": "H", "local_discard_required": true }; ac.phase = "confirm"
        } else if (op === "result") {
            ac.action = "remove"; ac.resultData = { "status": "succeeded", "revision": 1302 }; ac.phase = "result"
        } else if (op === "outcome") {
            ac.action = "remove"; ac.recoveryData = { "operation_id": "op-r" }; ac.phase = "outcomeUnknown"
        } else if (op === "verifiedResult") {
            ac.action = "remove"; ac.verifiedRevision = 1302
            ac.resultData = { "status": "verified_committed", "revision": 1302, "author": "member1" }
            ac.phase = "result"
        } else if (op === "manualReview") {
            ac.action = "remove"; ac.manualReviewCode = "REMOTE_STATE_DIVERGED"; ac.phase = "manualReview"
        } else if (op === "recovery") {
            ac.recoveryData = { "recovery_required": true, "operations": [
                { "operation_id": "op-x", "operation": "revert", "target_version": "Phase_2/block_cpu/SYN/v1.7", "state": "RECOVERY_REQUIRED" } ] }
            ac.phase = "recovery"
        } else if (op === "createFail") {
            ac.action = "create"; ac.createStep = 1; ac.createFailedStep = 1
            ac.resultData = { "status": "checkout_failed", "target_version": "Phase_2/block_cpu/SYN/v1.7.d" }
            ac.phase = "result"
        }
    }

    function seedCheckin(state) {
        var cc = shell.checkinController
        cc.selectedPaths = ["very/long/selected/path/for/bounded/checkin/example.v"]
        cc.activeVersion = "Phase_2/block_cpu/SYN/v1.7"
        if (state === "timeout") {
            cc.errorCode = "SVN_QUERY_TIMEOUT"; cc.phase = "failed"
        } else if (state === "limit") {
            cc.errorCode = "LOCAL_SCAN_LIMIT_EXCEEDED"; cc.phase = "failed"
        } else if (state === "recoveryIncomplete") {
            cc.recoveryData = { "operation_id": "op-checkin-batch-preview-evidence" }
            cc.manualReviewCode = "RECOVERY_QUERY_INCOMPLETE"; cc.phase = "manualReview"
        }
    }

    function seedImport(state) {
        var ic = shell.importController
        ic.activeVersion = "Phase_2/block_cpu/SYN/v1.7"
        ic.destination = "src"
        ic.previewData = { "plan_hash": "import-preview-hash", "items": [
            { "source_id": "source-0001", "source_relative": "rtl/非常長的模組名稱.v",
              "destination": "src/rtl/非常長的模組名稱.v", "kind": "file", "size": 2048,
              "allowed": true, "replace_local_available": false, "reason_code": "OK" }
        ] }
        ic.planHash = "import-preview-hash"
        if (state === "managed") {
            ic.previewData.items[0].allowed = false
            ic.previewData.items[0].reason_code = "MANAGED_DESTINATION_BLOCKED"
            ic.phase = "review"
        } else if (state === "replace") {
            ic.previewData.items[0].destination = "src/rtl/local.v"
            ic.previewData.items[0].allowed = false
            ic.previewData.items[0].replace_local_available = true
            ic.previewData.items[0].reason_code = "REPLACE_LOCAL_REQUIRED"
            ic.phase = "review"
        } else if (state === "symlink") {
            ic.previewData = null; ic.planHash = ""
            ic.errorCode = "SOURCE_SYMLINK_UNSUPPORTED"; ic.phase = "failed"
        } else if (state === "confirm") {
            ic.previewData.items[0].destination = "src/rtl/local.v"
            ic.previewData.items[0].replace_local_available = true
            ic.previewData.items[0].reason_code = "REPLACE_LOCAL_REQUIRED"
            ic.previewData.items[0].allowed = false
            ic.replaceLocal = { "src/rtl/local.v": true }
            ic.phase = "confirm"
        } else if (state === "success") {
            ic.resultData = { "operation_id": "op-import-0123456789abcdef0123456789abcdef",
                              "status": "imported", "item_count": 1,
                              "revision": null, "batch_paths": ["src/rtl/非常長的模組名稱.v"] }
            ic.phase = "success"
        } else if (state === "failed") {
            ic.previewData = null; ic.planHash = ""
            ic.errorCode = "PLAN_STALE"; ic.phase = "failed"
        } else if (state === "recovery") {
            ic.previewData = null; ic.planHash = ""
            ic.errorCode = "RECOVERY_REQUIRED"; ic.phase = "recovery"
        } else {
            ic.phase = "review"
        }
    }

    function seedConcurrency(state) {
        if (!state || state.length === 0) return
        var coordinator = shell.operationCoordinator
        var targetA = "Phase_2/block_cpu/SYN/v1.7"
        var targetB = "Phase_2/block_cpu/SYN/v1.6"
        coordinator.operationsById = {
            "ui-op-a": { "operationId": "ui-op-a", "operationKind": "checkin-batch",
                         "targetVersion": targetA, "planHash": "preview-a", "mutation": true,
                         "state": state === "unknown" ? "unknown" : state === "recovery" || state === "legacy" ? "manualReview" : state === "success" ? "success" : "running",
                         "result": null, "errorCode": state === "unknown" ? "OUTCOME_UNKNOWN" :
                                      state === "recovery" ? "VERSION_RECOVERY_REQUIRED" :
                                      state === "legacy" ? "PROJECT_LEGACY_RECOVERY_REQUIRED" : "" },
            "ui-op-b": { "operationId": "ui-op-b", "operationKind": "import",
                         "targetVersion": targetB, "planHash": "preview-b", "mutation": true,
                         "state": state === "legacy" ? "failed" : "running", "result": null,
                         "errorCode": state === "legacy" ? "PROJECT_LEGACY_RECOVERY_REQUIRED" : "" }
        }
        // Mock-preview only: A remains visibly pinned while B is active.
        shell.workState.activeVersionId = "v1.6"
        shell.openOperationCenter()
    }

    Shell.ProductShell {
        id: shell
        objectName: "productShell"
        anchors.fill: parent
        readMode: window.readMode
        operationCenterInlineForPreview: window.concurrencyState.length > 0
        isvnCli: window.readMode && typeof previewIsvnCli !== "undefined" ? previewIsvnCli : null
        sessionStore: window.readMode && typeof previewSessionStore !== "undefined" ? previewSessionStore : null
    }

    function applyStartupState(stateName) {
        // Any unknown value falls back to the normal state deterministically.
        shell.currentPage = "Work"
        if (!window.readMode)
            shell.workState.loadState(stateName)
    }

    function fail(code, reason) {
        console.error("work_preview: " + reason)
        Qt.exit(code)
    }

    function capture() {
        var ok = shell.grabToImage(function(result) {
            if (!result) {
                window.fail(3, "grabToImage produced no result")
                return
            }
            var saved = result.saveToFile(window.screenshotPath)
            Qt.exit(saved ? 0 : 5)
        })
        if (!ok)
            window.fail(6, "grabToImage could not be started")
    }

    Timer {
        id: settleTimer
        interval: window.settleMs
        repeat: false
        onTriggered: window.capture()
    }

    Timer {
        id: importSeedTimer
        interval: 120
        repeat: false
        onTriggered: window.seedImport(window.importState)
    }

    Timer {
        id: checkinSeedTimer
        interval: 100
        repeat: false
        onTriggered: window.seedCheckin(window.checkinState)
    }

    Timer {
        id: watchdog
        interval: window.watchdogMs
        repeat: false
        onTriggered: window.fail(4, "capture watchdog timeout")
    }

    Timer {
        id: seedTimer
        interval: 1600
        repeat: false
        onTriggered: window.seedOperation(window.opState)
    }

    Component.onCompleted: {
        var args = Qt.application.arguments
        var state = "normal"
        for (var i = 0; i < args.length; ++i) {
            var a = args[i]
            if (a.indexOf("--state=") === 0)
                state = a.substring(8)
            else if (a.indexOf("--screenshot=") === 0)
                screenshotPath = a.substring(13)
            else if (a.indexOf("--width=") === 0)
                width = parseInt(a.substring(8))
            else if (a.indexOf("--height=") === 0)
                height = parseInt(a.substring(9))
            else if (a.indexOf("--lang=") === 0)
                shell.i18n.language = a.substring(7)
            else if (a === "--light")
                shell.theme.dark = false
            else if (a === "--read")
                window.readMode = true
            else if (a.indexOf("--project=") === 0)
                shell.projectPath = a.substring(10)
            else if (a.indexOf("--active=") === 0)
                seedActive = a.substring(9)
            else if (a.indexOf("--activews=") === 0)
                seedActiveWs = a.substring(11)
            else if (a.indexOf("--sessionfile=") === 0)
                sessionFile = a.substring(14)
            else if (a.indexOf("--op=") === 0)
                opState = a.substring(5)
            else if (a.indexOf("--checkin=") === 0)
                checkinState = a.substring(10)
            else if (a.indexOf("--import=") === 0)
                importState = a.substring(9)
            else if (a.indexOf("--concurrency=") === 0)
                concurrencyState = a.substring(14)
        }
        shell.previewMode = checkinState.length > 0 || importState.length > 0
        applyStartupState(state)
        if (window.readMode) {
            // Real reads spawn a subprocess; give the serialized refresh time to
            // complete before capturing.
            settleMs = 2500
            if (typeof previewSessionStore !== "undefined") {
                if (sessionFile.length > 0)
                    previewSessionStore.storageFile = sessionFile
                previewSessionStore.projectKey = shell.projectPath
                previewSessionStore.resetWorkSession()
                if (seedActive.length > 0 && seedActiveWs.length > 0)
                    previewSessionStore.setActiveVersion(seedActiveWs, seedActive)
            }
            shell.readController.refresh()
            if (opState.length > 0) {
                settleMs = 3000
                seedTimer.start()
            }
        }
        if (checkinState.length > 0)
            checkinSeedTimer.start()
        if (importState.length > 0)
            importSeedTimer.start()
        if (concurrencyState.length > 0)
            Qt.callLater(function() { window.seedConcurrency(window.concurrencyState) })
        if (screenshotPath.length > 0) {
            settleTimer.start()
            watchdog.start()
        }
    }
}
