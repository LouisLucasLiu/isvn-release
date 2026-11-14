import QtQuick

// ExploreCheckoutController — explicit checkout of one existing repository
// Version that is not yet in this workspace.
//
// The flow is always user-driven: request() validates the target against the
// repository and the local inventory, confirm() issues a single
// `checkout <version>`, and nothing here changes the Active Working Version.
// Only the requested Version is checked out; no parent or related Version is
// fetched.  All commands go through the target-pinned operation lane.
QtObject {
    id: root

    property var operationCoordinator
    property var readController
    property var i18n
    // Workspace root for display in the confirmation; the CLI decides the
    // real location and reports it in its result.
    property string workspaceRoot: ""

    // phase: idle | validating | confirm | running | done | failed
    property string phase: "idle"
    // Fixed from request() until the flow ends; never rebound to the page's
    // current selection.
    property string targetVersion: ""
    property string resultPath: ""
    property string errorCode: ""

    readonly property bool busy: phase === "validating" || phase === "running"
    readonly property bool flowOpen: busy || phase === "confirm"

    property string _step: ""
    property var _inflight: null

    // Same shape the CLI accepts: phase/block/stage/version.
    function wellFormed(version) {
        var text = String(version || "")
        if (text.length === 0 || text.charAt(0) === "/" || text.indexOf("//") >= 0) return false
        var parts = text.split("/")
        if (parts.length !== 4) return false
        for (var i = 0; i < parts.length; ++i) {
            var part = parts[i]
            if (part.length === 0 || part === "." || part === ".." || /\s/.test(part)) return false
        }
        return true
    }

    function expectedPath(version) {
        return workspaceRoot.length > 0 ? workspaceRoot + "/" + version : version
    }

    function request(version) {
        if (flowOpen) return false
        var target = String(version || "").trim()
        targetVersion = target
        resultPath = ""
        errorCode = ""
        if (!operationCoordinator || !readController) return _fail("BACKEND_UNAVAILABLE")
        if (!wellFormed(target)) return _fail("INVALID_VERSION")
        // A Set Active that is still unanswered ends when a checkout is asked
        // for; its answer must not arrive as a consequence of this flow.
        readController.cancelExplicitActivation()
        phase = "validating"
        return _begin("branches", false, ["branches"])
    }

    function confirm() {
        if (phase !== "confirm") return false
        readController.cancelExplicitActivation()
        phase = "running"
        return _begin("checkout", true, ["checkout", targetVersion])
    }

    function cancel() {
        if (phase !== "confirm") return false
        phase = "idle"
        return true
    }

    function dismiss() {
        if (phase !== "done" && phase !== "failed") return false
        phase = "idle"
        errorCode = ""
        return true
    }

    // Versions listed by `isvn workspaces --json` (workspaces/v1), or null when
    // the answer is not that inventory.  An unreadable inventory is never
    // taken to mean that nothing is local.
    function _localVersionsFrom(json) {
        if (!json || typeof json !== "object" || json.schema_version !== "workspaces/v1")
            return null
        var list = json.workspaces
        // CLI output reaches QML through the bridge as a Qt sequence, for which
        // `instanceof Array` holds and Array.isArray() does not.  An object that
        // merely carries a numeric `length` is neither and is rejected.
        if (!(Array.isArray(list) || list instanceof Array))
            return null
        var versions = []
        for (var i = 0; i < list.length; ++i) {
            var entry = list[i]
            if (!entry || typeof entry !== "object" || typeof entry.repository_path !== "string" ||
                    entry.repository_path.length === 0)
                return null
            versions.push(entry.repository_path)
        }
        return versions
    }

    function _fail(code) {
        _step = ""
        _inflight = null
        errorCode = code
        phase = "failed"
        return false
    }

    function _begin(step, mutation, args) {
        _step = step
        var id = mutation
                 ? operationCoordinator.beginMutation("explore-checkout", targetVersion, args, "", "")
                 : operationCoordinator.beginRead("explore-checkout-" + step, targetVersion, args)
        if (!id || id.length === 0) return _fail("BACKEND_UNAVAILABLE")
        _inflight = { "operationId": id, "step": step, "target": targetVersion }
        // A request the bridge refuses is answered before its id is known
        // here, so pick that result up from the coordinator record.
        Qt.callLater(function() {
            if (!root._inflight || root._inflight.operationId !== id) return
            var record = root.operationCoordinator.operationFor(id)
            if (record && record.state !== "starting" && record.state !== "running" && record.result)
                root._onResult(id, record.targetVersion, record.result)
        })
        return true
    }

    function _onResult(operationId, target, json) {
        var item = _inflight
        if (!item || item.operationId !== operationId || item.target !== target) return
        _inflight = null
        if (json && json.error) {
            _fail(json.error.code ? json.error.code : "CHECKOUT_FAILED")
            if (item.step === "checkout" && readController) readController.refresh()
            return
        }
        if (item.step === "branches") {
            var branches = (json && json.branches) ? json.branches : []
            var exists = false
            for (var i = 0; i < branches.length; ++i)
                if (branches[i].path === targetVersion && branches[i].removed !== true) exists = true
            if (!exists) { _fail("VERSION_NOT_IN_REPOSITORY"); return }
            _begin("workspaces", false, ["workspaces"])
        } else if (item.step === "workspaces") {
            var localVersions = _localVersionsFrom(json)
            if (localVersions === null) { _fail("LOCAL_INVENTORY_UNREADABLE"); return }
            if (localVersions.indexOf(targetVersion) >= 0) {
                // The page's inventory was stale; show the real state.
                _fail("ALREADY_LOCAL")
                readController.refresh()
                return
            }
            _step = ""
            phase = "confirm"
        } else if (item.step === "checkout") {
            _step = ""
            if (json && json.result === "checked_out" && json.version === targetVersion) {
                resultPath = json.path ? String(json.path) : ""
                phase = "done"
            } else {
                // Anything other than the CLI's own confirmation is not a success.
                _fail("UNEXPECTED_RESULT")
            }
            // Reload the local inventory only.  Active stays as it was.
            readController.refresh()
        }
    }

    property var _coordinatorBridge: Connections {
        target: root.operationCoordinator ? root.operationCoordinator : null
        ignoreUnknownSignals: true
        function onTargetResult(operationId, targetVersion, result) {
            root._onResult(operationId, targetVersion, result)
        }
    }

    function reasonText(code) {
        if (!i18n) return code
        if (code === "INVALID_VERSION")
            return i18n.text("Enter the version as phase/block/stage/version.",
                             "請以 phase/block/stage/version 的格式輸入版本。",
                             "请以 phase/block/stage/version 的格式输入版本。")
        if (code === "VERSION_NOT_IN_REPOSITORY")
            return i18n.text("This version does not exist in the repository.",
                             "版本庫中沒有這個版本。", "版本库中没有这个版本。")
        if (code === "ALREADY_LOCAL")
            return i18n.text("This version is already checked out in this workspace.",
                             "這個版本已經在此工作區中。", "这个版本已经在此工作区中。")
        if (code === "LOCAL_INVENTORY_UNREADABLE")
            return i18n.text("isvn's list of local versions could not be read, so it is not known whether this version is already here. Nothing was checked out.",
                             "無法讀取 isvn 的本機版本清單，因此不確定這個版本是否已在本機。沒有執行 checkout。",
                             "无法读取 isvn 的本地版本清单，因此不确定这个版本是否已在本地。没有执行 checkout。")
        if (code === "READ_LANE_BUSY")
            return i18n.text("The workspace is being read right now. Try again in a moment.",
                             "正在讀取工作區，請稍後再試。", "正在读取工作区，请稍后再试。")
        if (code === "SVN_QUERY_FAILED" || code === "CHECKOUT_FAILED")
            return i18n.text("isvn could not complete the command and gave no further detail here. Check that the local folder is free and the repository can be reached, then try again. Nothing was activated.",
                             "isvn 無法完成指令，這裡沒有更多細節。請確認本機資料夾沒有被佔用、版本庫可以連線後再試。沒有變更啟用中的版本。",
                             "isvn 无法完成命令，这里没有更多细节。请确认本地文件夹没有被占用、版本库可以连接后再试。没有变更启用中的版本。")
        if (code === "BACKEND_UNAVAILABLE")
            return i18n.text("The isvn command is not available to this window.",
                             "此視窗無法使用 isvn 指令。", "此窗口无法使用 isvn 命令。")
        if (code === "UNEXPECTED_RESULT")
            return i18n.text("isvn did not confirm the checkout. Check the workspace before retrying.",
                             "isvn 沒有確認 checkout 完成。重試前請先檢查工作區。",
                             "isvn 没有确认 checkout 完成。重试前请先检查工作区。")
        return i18n.text("isvn reported a failure (%1). Nothing was activated.",
                         "isvn 回報失敗（%1）。沒有變更啟用中的版本。",
                         "isvn 报告失败（%1）。没有变更启用中的版本。")
                   .replace("%1", function() { return String(code) })
    }
}
