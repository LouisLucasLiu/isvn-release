import QtQuick

// WorkActionController — serialized orchestration of transactional Work
// file actions (Make Editable / Revert / Discard Local), atomic Remove from
// Version, crash recovery, and the Create Derived Version composite flow.
//
// It is orchestration/presentation only: it emits the backend commands
// through the single-flight IsvnCli and never copies, unlinks, symlinks, reverts,
// stages, commits, or recovers files itself. Every destructive action goes
// through a recovery gate → preview/preflight → confirm → apply-with-plan-hash
// pipeline. Checkin and Import are never emitted here.
QtObject {
    id: root

    property var isvnCli
    property var sessionStore
    property var operationCoordinator
    property var i18n
    property var readController          // to refresh work-status after a terminal outcome
    property string activeVersion: ""    // repository path of the Active Working Version
    property string operationTarget: ""  // immutable while a flow is in progress

    // phase: idle | recovery | preview | confirm | applying | result |
    //        outcomeUnknown | manualReview | create
    property string phase: "idle"
    property string manualReviewCode: ""
    property var verifiedRevision: null
    property string action: ""           // make-editable|revert|discard-local|remove|create
    property var previewData: null
    property var resultData: null
    property var recoveryData: null
    property string recoveryOperationId: ""  // must be explicitly selected by operation id
    property string errorCode: ""
    // Remove inputs.
    property string removeMessage: ""
    property bool allowDiscardLocal: false
    property var _paths: []
    // Create composite state.
    property string createVersion: ""
    property int createStep: 0           // 0 create,1 checkout,2 set-active
    property int createFailedStep: -1
    property string createdVersion: ""

    readonly property bool busy: phase === "applying" ||
                                 (_phase !== "idle")
    // A mutation is blocked while recovery is required or an operation runs.
    readonly property bool mutationsBlocked: phase === "recovery" || busy ||
                                             phase === "outcomeUnknown" ||
                                             phase === "manualReview"

    signal activeSwitchGuard()           // emitted when the caller must not switch Active
    // A successful Remove/Discard changes the exact local path set.  The
    // presentation layer uses this target-pinned notification only to discard
    // now-impossible ephemeral Checkin selectors; it does not infer state or
    // issue another filesystem/SVN command.
    signal pathsInvalidated(string targetVersion, var paths)

    // ---- serialization (single-flight) -----------------------------------
    property string _phase: "idle"       // in-flight backend call tag
    property var _queue: []

    function _enqueue(tag, args) { _queue.push({ "tag": tag, "args": args, "target": operationTarget }); _pump() }
    function _pump() {
        if (_phase !== "idle") return
        if (!isvnCli) return
        if (!operationCoordinator && isvnCli.running) return
        if (_queue.length === 0) return
        var next = _queue.shift()
        _phase = next.tag
        if (operationCoordinator) {
            var mutation = next.tag === "apply" || next.tag === "recover" || next.tag === "verify" ||
                           next.tag === "create" || next.tag === "checkout"
            _inflight = next
            var id = mutation
                     ? operationCoordinator.beginMutation("work-" + action, next.target, next.args,
                                                          "", previewData && previewData.plan_hash ? previewData.plan_hash : "")
                     : operationCoordinator.beginRead("work-" + next.tag, next.target, next.args)
            _inflight.operationId = id
            Qt.callLater(function() {
                if (!root._inflight || root._inflight.operationId !== id) return
                var record = root.operationCoordinator.operationFor(id)
                if (record && record.state !== "starting" && record.state !== "running" && record.result)
                    root._onCoordinatorResult(id, record.targetVersion, record.result)
            })
            return
        }
        isvnCli.runJson(next.args)
    }
    property var _inflight: null

    function reset() {
        phase = "idle"; action = ""; previewData = null; resultData = null
        errorCode = ""; removeMessage = ""; allowDiscardLocal = false; _paths = []
        recoveryOperationId = ""
        operationTarget = ""
        createStep = 0; createFailedStep = -1; createdVersion = ""
        if (operationCoordinator) { _phase = "idle"; _queue = []; _inflight = null }
    }

    function _pathArgs(paths) {
        var args = []
        for (var i = 0; i < paths.length; ++i) { args.push("--path"); args.push(paths[i]) }
        return args
    }

    // ---- local operations & remove: recovery gate then preview ------------

    function startLocalAction(op, paths) {
        reset()
        action = op
        operationTarget = activeVersion
        _paths = paths
        errorCode = ""
        _enqueue("recovery-gate", ["work-recovery", "status", "--json"])
    }

    function startRemove(paths) {
        reset()
        action = "remove"
        operationTarget = activeVersion
        _paths = paths
        _enqueue("recovery-gate", ["work-recovery", "status", "--json"])
    }

    function _issuePreview() {
        if (action === "remove") {
            _enqueue("preview", ["remove-batch", "preview", "--from", operationTarget]
                     .concat(_pathArgs(_paths)))
        } else {
            _enqueue("preview", ["work-local-op", "preview", "--action", action, "--in",
                                 operationTarget].concat(_pathArgs(_paths)))
        }
    }

    function confirmLocalApply() {
        if (!previewData) return
        // Set the phase BEFORE issuing the call: the bridge may deliver its result
        // synchronously (tests) or asynchronously (production).
        phase = "applying"
        _enqueue("apply", ["work-local-op", "apply", "--action", action, "--in", operationTarget]
                 .concat(_pathArgs(_paths))
                 .concat(["--expected-plan", previewData.plan_hash]))
    }

    function confirmRemoveApply() {
        if (!previewData) return
        var args = ["remove-batch", "apply", "--from", operationTarget]
            .concat(_pathArgs(_paths))
            .concat(["--expected-plan", previewData.plan_hash, "--message", removeMessage])
        if (allowDiscardLocal) args.push("--allow-discard-local")
        phase = "applying"
        _enqueue("apply", args)
    }

    // ---- recovery ---------------------------------------------------------

    function recover(operationId) {
        var opId = operationId ? String(operationId) : ""
        var selected = null
        var operations = recoveryData && recoveryData.operations ? recoveryData.operations : []
        for (var i = 0; i < operations.length; ++i)
            if (String(operations[i].operation_id || "") === opId) selected = operations[i]
        if (!selected || String(selected.target_version || "") !== operationTarget) {
            errorCode = "OPERATION_NOT_FOUND"
            return false
        }
        recoveryOperationId = opId
        phase = "applying"
        _enqueue("recover", ["work-recovery", "restore", opId, "--json"])
        return true
    }

    // This is entered only after the user selects an exact backend recovery
    // row.  It deliberately cannot manufacture an ID from a UI token.
    function presentRecovery(operation) {
        if (!operation || !operation.operation_id ||
                String(operation.target_version || "") !== activeVersion) {
            errorCode = "OPERATION_NOT_FOUND"
            return false
        }
        operationTarget = String(operation.target_version)
        recoveryData = { "operations": [operation] }
        recoveryOperationId = String(operation.operation_id)
        action = operation.operation ? String(operation.operation) : action
        var manual = operation.state && String(operation.state).toLowerCase().indexOf("manual") >= 0
        manualReviewCode = manual ? "RECOVERY_EVIDENCE_MISMATCH" : ""
        phase = manual ? "manualReview" : "recovery"
        return true
    }

    function verifyOutcome() { return verifyOutcomeFor("") }

    function verifyOutcomeFor(operationId) {
        // Outcome Unknown: the only forward move is positive verification by exact
        // SVN-log evidence of the specific interrupted operation (never a blind
        // re-apply, never path-absence success).
        var opId = operationId || recoveryOperationId
        if ((!opId || opId.length === 0) && resultData && resultData.operation_id)
            opId = resultData.operation_id
        if (!opId || opId.length === 0) { errorCode = "OPERATION_NOT_FOUND"; return false }
        phase = "applying"
        _enqueue("verify", ["work-recovery", "verify", opId, "--json"])
        return true
    }

    // ---- create derived version composite ---------------------------------

    function startCreateDerived(newVersion) {
        reset()
        action = "create"
        operationTarget = activeVersion
        createVersion = newVersion
        phase = "create"
        createStep = 0
        _enqueue("create", ["create", newVersion, "--parent", operationTarget])
    }

    function retryCheckout() {
        // User-triggered only; never calls Create again.
        createFailedStep = -1
        createStep = 1
        _enqueue("checkout", ["checkout", createdVersion])
    }

    // ---- result handling --------------------------------------------------

    function _refreshAfterTerminal() {
        if (readController) readController.refresh()
    }

    // `itemCount` is the number of items the backend reports for the operation;
    // omitted when the operation has no such number.
    function _logActivity(operationType, target, status, revision, summary, itemCount) {
        if (!sessionStore) return
        var rec = {
            "operationId": (resultData && resultData.operation_id) ? resultData.operation_id : ("ui-" + Date.now()),
            "operationType": operationType,
            "target": target,
            "status": status,
            "summary": summary,
            "endTime": Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss")
        }
        if (itemCount !== undefined && itemCount !== null && itemCount >= 0)
            rec.itemCount = String(itemCount)
        if (revision) rec.revision = String(revision)
        sessionStore.appendActivity(rec)
    }

    // Items an applied operation acted on, as the backend result lists them.
    function _appliedItemCount(json) {
        if (json && json.items && json.items.length !== undefined) return json.items.length
        if (json && json.removed_paths && json.removed_paths.length !== undefined)
            return json.removed_paths.length
        return _paths.length
    }

    function _onSucceeded(json) {
        var tag = _phase
        _phase = "idle"
        _inflight = null
        if (tag === "recovery-gate") {
            recoveryData = json
            if (json && json.recovery_required) {
                if (operationCoordinator) operationCoordinator.recordRecoveryOperations(json.operations || [])
                phase = "recovery"
            }
            else { _issuePreview() }
        } else if (tag === "preview") {
            previewData = json
            phase = "confirm"
        } else if (tag === "apply") {
            resultData = json
            phase = "result"
            _logActivity(action, operationTarget,
                         json.status ? json.status : "succeeded",
                         json.revision, action, _appliedItemCount(json))
            // A discarded LOCAL item or an atomic Remove no longer exists at
            // this Version.  Leaving it in the session-only Checkin draft
            // would make the next authoritative Review fail on a path the GUI
            // itself has just removed.  Preserve unrelated selectors and, in
            // particular, never touch a draft for another active Version.
            if (action === "discard-local" || action === "remove")
                pathsInvalidated(operationTarget, _paths.slice())
            _refreshAfterTerminal()
        } else if (tag === "recover" || tag === "verify") {
            resultData = json
            verifiedRevision = (json.revision !== undefined) ? json.revision : null
            if (json.manual_review_required === true) {
                manualReviewCode = json.reason_code ? json.reason_code : "OPERATION_EVIDENCE_INSUFFICIENT"
                phase = "manualReview"
            } else {
                phase = "result"
                if (operationCoordinator)
                    operationCoordinator.resolveRecovery(json.operation_id ? json.operation_id
                                                                           : recoveryOperationId)
            }
            _logActivity("recovery", operationTarget,
                         json.status ? json.status : (json.terminal_state ? json.terminal_state : "recovered"),
                         json.revision, "recovery")
            _refreshAfterTerminal()
        } else if (tag === "create") {
            createdVersion = createVersion
            createStep = 1
            _enqueue("checkout", ["checkout", createVersion])
        } else if (tag === "checkout") {
            createStep = 2
            _enqueue("set-active-refresh", ["workspaces", "--json"])
        } else if (tag === "set-active-refresh") {
            // Only set Active after the new workspace validates in the inventory.
            var found = false
            var wss = (json && json.workspaces) ? json.workspaces : []
            for (var i = 0; i < wss.length; ++i)
                if (wss[i].repository_path === createdVersion) found = true
            if (found && readController) {
                // The composite's private inventory response is evidence that
                // Checkout completed, but WorkReadController must refresh its
                // own authoritative local inventory before persisting Active.
                // Calling selectActive here would consult its stale list and
                // silently leave the prior Version active.
                readController.revalidateAndSelect(createdVersion)
                phase = "result"
                resultData = { "status": "created_checked_out", "target_version": createdVersion }
                _logActivity("create-derived", createdVersion, "succeeded", null, "create+checkout+active")
            } else {
                phase = "result"
                resultData = { "status": "active_validation_failed", "target_version": createdVersion }
            }
        }
        _pump()
    }

    function _onFailedJson(json) {
        var tag = _phase
        _phase = "idle"
        _inflight = null
        var code = (json && json.error && json.error.code) ? json.error.code : "STATE_INCONSISTENT"
        errorCode = code
        if (tag === "create") {
            // Create failed: stop, no checkout, previous Active unchanged.
            createFailedStep = 0
            phase = "result"
            resultData = { "status": "create_failed", "target_version": createVersion }
            _logActivity("create-derived", createVersion, "create_failed", null, "create_failed")
        } else if (tag === "checkout") {
            // Create ok + Checkout failed: retain created version + previous Active.
            createFailedStep = 1
            phase = "result"
            resultData = { "status": "checkout_failed", "target_version": createdVersion }
            _logActivity("create-derived", createdVersion, "checkout_failed", null, "created/checkout_failed")
        } else if (code === "PLAN_STALE") {
            // Re-review: rebuild the plan.
            previewData = null
            _issuePreview()
        } else if (code === "RECOVERY_REQUIRED") {
            phase = "recovery"
            _enqueue("recovery-gate", ["work-recovery", "status", "--json"])
        } else if (code === "OUTCOME_UNKNOWN") {
            phase = "outcomeUnknown"
            recoveryData = json
            recoveryOperationId = json && json.operation_id ? json.operation_id : ""
        } else {
            // MESSAGE_REQUIRED / DISCARD_ACK_REQUIRED / MIXED_SELECTION / etc. stay
            // in confirm so the user can correct; terminal safety errors show result.
            if (code === "MESSAGE_REQUIRED" || code === "DISCARD_ACK_REQUIRED") {
                phase = "confirm"
            } else {
                phase = "result"
                resultData = { "status": "failed" }
            }
        }
        _pump()
    }

    function _onFailed(message) {
        // Non-structured (legacy) failure. Never fall back to a fake success.
        if (_phase === "idle") return
        var tag = _phase
        _phase = "idle"
        _inflight = null
        if (tag === "create") { createFailedStep = 0; phase = "result"
            resultData = { "status": "create_failed" } }
        else if (tag === "checkout") { createFailedStep = 1; phase = "result"
            resultData = { "status": "checkout_failed", "target_version": createdVersion } }
        else { errorCode = "STATE_INCONSISTENT"; phase = "result"; resultData = { "status": "failed" } }
        _pump()
    }

    property var _bridge: Connections {
        target: root.isvnCli ? root.isvnCli : null
        ignoreUnknownSignals: true
        function onCommandSucceeded(json) { root._onSucceeded(json) }
        function onCommandFailedJson(json) { root._onFailedJson(json) }
        function onCommandFailed(message) {
            // commandFailedJson (if any) already advanced _phase; only handle the
            // legacy path when a structured error was not delivered.
            if (root._phase !== "idle") root._onFailed(message)
        }
    }

    function _onCoordinatorResult(operationId, targetVersion, json) {
        var item = _inflight
        if (!item || item.operationId !== operationId || item.target !== targetVersion) return
        if (json && json.error) root._onFailedJson(json)
        else root._onSucceeded(json)
    }

    property var _coordinatorBridge: Connections {
        target: root.operationCoordinator ? root.operationCoordinator : null
        ignoreUnknownSignals: true
        function onTargetResult(operationId, targetVersion, result) {
            root._onCoordinatorResult(operationId, targetVersion, result)
        }
    }

    // ---- localized reason text -------------------------------------------

    function reasonText(code) {
        if (!i18n) return code
        if (code === "PLAN_STALE") return i18n.text("The workspace changed; please review again.",
            "工作區已變更，請重新檢視。", "工作区已变更，请重新查看。")
        if (code === "MESSAGE_REQUIRED") return i18n.text("A removal message is required.",
            "需要填寫移除訊息。", "需要填写移除信息。")
        if (code === "DISCARD_ACK_REQUIRED") return i18n.text("Acknowledge discarding local edits to continue.",
            "請確認將捨棄本機修改以繼續。", "请确认将舍弃本机修改以继续。")
        if (code === "USE_DISCARD_LOCAL") return i18n.text("New local content uses Discard Local, not Remove.",
            "新的本機內容應使用捨棄本機，而非移除。", "新的本机内容应使用舍弃本机，而非移除。")
        if (code === "MIXED_SELECTION") return i18n.text("Separate managed and new local items.",
            "請分開受管與新的本機項目。", "请分开受管与新的本机项目。")
        if (code === "OUTCOME_UNKNOWN") return i18n.text("Repository outcome unknown — verification required.",
            "版本庫結果未知 — 需要驗證。", "版本库结果未知 — 需要验证。")
        if (code === "RECOVERY_REQUIRED") return i18n.text("An interrupted operation must be recovered first.",
            "必須先復原被中斷的操作。", "必须先复原被中断的操作。")
        if (code === "OPERATION_IN_PROGRESS") return i18n.text("That operation is still running. It can be recovered only after it stops.",
            "該操作仍在執行，停止後才能復原。", "该操作仍在执行，停止后才能恢复。")
        if (code === "VERSION_RECOVERY_REQUIRED") return i18n.text("This Version needs recovery before another mutation.",
            "此版本需先復原才能進行其他變更。", "此版本需先恢复才能进行其他变更。")
        if (code === "PROJECT_LEGACY_RECOVERY_REQUIRED") return i18n.text("An older Project recovery record must be resolved first.",
            "必須先處理較舊的專案復原紀錄。", "必须先处理较旧的项目恢复记录。")
        if (code === "OPERATION_TARGET_DIVERGED") return i18n.text("The operation target changed. Manual Review is required.",
            "操作目標已變更，需要人工檢視。", "操作目标已变更，需要人工检查。")
        if (code === "OPERATION_NOT_FOUND") return i18n.text("Select the exact operation before recovery or verification.",
            "請先選取正確操作再進行復原或驗證。", "请先选择正确操作再进行恢复或验证。")
        if (code === "READ_LANE_BUSY") return i18n.text("The bounded read lane is busy. Try Refresh again shortly.",
            "受限讀取通道忙碌，請稍後重新整理。", "受限读取通道忙碌，请稍后刷新。")
        if (code === "REQUEST_LIMIT_EXCEEDED") return i18n.text("The request exceeds the safe size limit.",
            "請求超過安全大小上限。", "请求超过安全大小上限。")
        if (code === "OPERATION_REGISTRY_CORRUPT") return i18n.text("Operation records need Manual Review before another action.",
            "操作紀錄需要人工檢視後才能進行其他動作。", "操作记录需要人工检查后才能进行其他操作。")
        if (code === "INSUFFICIENT_BACKUP_SPACE") return i18n.text("Not enough space for a safe backup.",
            "空間不足以建立安全備份。", "空间不足以建立安全备份。")
        if (code === "CHECKOUT_FAILED") return i18n.text("Checkout failed; the version was still created.",
            "檢出失敗；版本仍已建立。", "检出失败；版本仍已创建。")
        if (code === "OPERATION_EVIDENCE_MISMATCH") return i18n.text("Commit evidence does not match this operation. Manual review required.",
            "提交證據與此操作不符，需人工檢視。", "提交证据与此操作不符，需人工检视。")
        if (code === "OPERATION_EVIDENCE_AMBIGUOUS") return i18n.text("Multiple candidate commits found. Manual review required.",
            "找到多個候選提交，需人工檢視。", "找到多个候选提交，需人工检视。")
        if (code === "OPERATION_EVIDENCE_INSUFFICIENT") return i18n.text("Not enough evidence to attribute the outcome. Manual review required.",
            "證據不足以判定結果，需人工檢視。", "证据不足以判定结果，需人工检视。")
        if (code === "REMOTE_STATE_DIVERGED") return i18n.text("The repository changed independently. Manual review required.",
            "版本庫已被獨立變更，需人工檢視。", "版本库已被独立变更，需人工检视。")
        if (code === "LEGACY_EVIDENCE_INSUFFICIENT") return i18n.text("An older operation lacks verification evidence. Manual review required.",
            "較舊的操作缺少驗證證據，需人工檢視。", "较旧的操作缺少验证证据，需人工检视。")
        if (code === "COMMIT_SCOPE_MISMATCH") return i18n.text("The commit scope did not match the plan. Manual review required.",
            "提交範圍與計畫不符，需人工檢視。", "提交范围与计划不符，需人工检视。")
        if (code === "EXPECTED_AUTHOR_UNAVAILABLE") return i18n.text("The expected author could not be determined. Manual review required.",
            "無法判定預期作者，需人工檢視。", "无法判定预期作者，需人工检视。")
        if (code === "SVN_LOG_QUERY_FAILED" || code === "SVN_LOG_PARSE_FAILED") return i18n.text("Repository evidence could not be read. Try again or seek manual review.",
            "無法讀取版本庫證據，請重試或人工檢視。", "无法读取版本库证据，请重试或人工检视。")
        if (code === "RESERVED_OPERATION_TRAILER") return i18n.text("The message uses reserved operation syntax; please edit it.",
            "訊息使用了保留的操作語法，請修改。", "信息使用了保留的操作语法，请修改。")
        return code
    }
}
