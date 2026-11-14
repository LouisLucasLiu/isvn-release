import QtQuick

// CheckinBatchController — serialized orchestration of the atomic Work
// Checkin Batch: explicit selection → backend preview (authoritative concrete
// Batch) → Review → Confirm → one exact SVN commit → Success / Failed / Outcome
// Unknown / Manual Review.
//
// It is orchestration/presentation only. It NEVER copies, stages, commits, adds,
// reverts, recovers, or touches the filesystem/SVN. Every backend fact (folder
// expansion, eligibility, provenance, plan identity, commit, verification) comes
// from the single-flight IsvnCli. It emits ONLY `checkin-batch` and
// `work-recovery` commands — never the legacy per-file `checkin`, never `import`,
// and never N sequential checkins.
QtObject {
    id: root

    property var isvnCli
    property var sessionStore
    property var operationCoordinator
    property var i18n
    property var readController
    property string activeVersion: ""      // repository path of the Active Working Version
    // Frozen when a Version's ephemeral Batch begins. Navigation may change
    // Active, but it cannot silently retarget this concrete selector/plan.
    property string batchTarget: ""

    // phase: idle | recovery | previewing | review | confirm | applying |
    //        success | failed | outcomeUnknown | manualReview
    property string phase: "idle"

    // The user's explicit checked selectors (stable project-relative path
    // identity, NEVER a row index). The UI owns this list; Add to Batch sends
    // exactly these — it must never substitute "all eligible rows".
    property var selectedPaths: []

    // Authoritative backend response (concrete Batch). QML never recomputes it.
    property var previewData: null
    property var resultData: null
    property var recoveryData: null
    property string recoveryOperationId: ""
    property string planHash: ""
    property string checkinMessage: ""
    property bool messageInvalid: false
    property string errorCode: ""
    property string manualReviewCode: ""
    property var verifiedRevision: null

    // Target-switch guard state.
    property string pendingActiveSwitch: ""
    property bool requiresBatchDecision: false

    // Derived read models over previewData (authoritative concrete Batch).
    readonly property var concreteItems: previewData && previewData.items ? previewData.items : []
    readonly property var supportingDirs: previewData && previewData.supporting_repository_changes
                                          ? previewData.supporting_repository_changes : []
    readonly property bool canApply: previewData ? (previewData.can_apply === true) : false
    readonly property int itemCount: concreteItems.length
    readonly property bool hasBlockingConflict: {
        for (var i = 0; i < concreteItems.length; ++i)
            if (concreteItems[i].reason_code === "CONFLICT_BLOCKED") return true
        return false
    }
    readonly property bool batchEmpty: itemCount === 0 && selectedPaths.length === 0

    readonly property bool busy: phase === "applying" || phase === "previewing" ||
                                 _phase !== "idle"
    // Batch editing and other Work mutation are frozen while an outcome is
    // unresolved or an operation is in flight.
    readonly property bool mutationsBlocked: phase === "recovery" || phase === "outcomeUnknown" ||
                                             phase === "manualReview" || busy

    signal activeSwitchGuard()

    // ---- single-flight serialization with generation/target guard ----------
    property string _phase: "idle"
    property int _generation: 0            // bumped when the Batch/target is invalidated
    property var _queue: []

    function _enqueue(tag, args) {
        _queue.push({ "tag": tag, "args": args, "gen": _generation, "target": batchTarget })
        _pump()
    }
    function _pump() {
        if (_phase !== "idle") return
        if (!isvnCli) return
        if (!operationCoordinator && isvnCli.running) return
        if (_queue.length === 0) return
        var next = _queue.shift()
        _inflight = next
        _phase = next.tag
        if (operationCoordinator) {
            var mutation = next.tag === "apply" || next.tag === "verify" || next.tag === "recover"
            var id = mutation
                     ? operationCoordinator.beginMutation("checkin-batch", next.target, next.args,
                                                          "", planHash)
                     : operationCoordinator.beginRead("checkin-" + next.tag, next.target, next.args)
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

    function _pathArgs(paths) {
        var args = []
        for (var i = 0; i < paths.length; ++i) { args.push("--path"); args.push(paths[i]) }
        return args
    }

    function _bumpGeneration() { _generation = _generation + 1 }

    // Drafts are presentation-only.  They deliberately retain selectors and
    // message by stable Version ID, never a backend plan: returning to a
    // Version always rebuilds Preview before Confirm can be enabled.
    function _saveDraft() {
        if (!operationCoordinator || !batchTarget) return
        operationCoordinator.updateDraft(batchTarget, {
            "selectedPaths": selectedPaths.slice(),
            "checkinMessage": checkinMessage,
            "review": null
        })
    }

    function _activateDraft(target) {
        if (!operationCoordinator || !target || batchTarget === target) return
        _saveDraft()
        var draft = operationCoordinator.draftFor(target)
        _bumpGeneration()
        batchTarget = target
        selectedPaths = draft.selectedPaths ? draft.selectedPaths.slice() : []
        checkinMessage = draft.checkinMessage ? draft.checkinMessage : ""
        previewData = null
        resultData = null
        planHash = ""
        errorCode = ""
        manualReviewCode = ""
        phase = "idle"
        _queue = []
        _phase = "idle"
        _inflight = null
    }

    function resetBatch() {
        _bumpGeneration()
        selectedPaths = []
        previewData = null; resultData = null; planHash = ""
        checkinMessage = ""; messageInvalid = false; errorCode = ""
        manualReviewCode = ""; verifiedRevision = null
        recoveryOperationId = ""
        pendingActiveSwitch = ""; requiresBatchDecision = false
        batchTarget = ""
        phase = "idle"
    }

    // Leave a Review/Confirm/failed presentation without manufacturing a
    // result.  The explicit selectors remain in the target-keyed session
    // draft; any later Review is rebuilt authoritatively by the backend.
    function dismissPresentation() {
        if (phase === "outcomeUnknown" || phase === "manualReview" || phase === "recovery") return false
        previewData = null
        resultData = null
        planHash = ""
        errorCode = ""
        messageInvalid = false
        if (phase !== "applying" && _phase === "idle") phase = "idle"
        _saveDraft()
        return true
    }

    // ---- selection & preview ----------------------------------------------

    // The UI sets the explicit checked selector set. Changing the selection
    // invalidates any existing Review/plan.
    function setSelection(paths) {
        if (batchTarget.length === 0) batchTarget = activeVersion
        else if (operationCoordinator && batchTarget !== activeVersion) _activateDraft(activeVersion)
        selectedPaths = paths
        _saveDraft()
        if (phase === "review" || phase === "confirm") { previewData = null; planHash = "" }
    }

    // Called only after a target-pinned local transaction reports that its
    // exact paths were removed.  This updates the owning Version's ephemeral
    // draft, never the currently visible Version merely because it happens to
    // be Active when the asynchronous result arrives.
    function pruneInvalidatedPaths(paths, targetVersion) {
        if (!targetVersion || !paths) return false
        var removed = {}
        for (var r = 0; r < paths.length; ++r) {
            var path = String(paths[r])
            if (path.length > 0) removed[path] = true
        }
        if (Object.keys(removed).length === 0) return true

        if (operationCoordinator && targetVersion !== activeVersion) {
            var hiddenDraft = operationCoordinator.draftFor(targetVersion)
            var hiddenPaths = hiddenDraft.selectedPaths ? hiddenDraft.selectedPaths : []
            var hiddenKept = []
            for (var h = 0; h < hiddenPaths.length; ++h)
                if (!removed[String(hiddenPaths[h])]) hiddenKept.push(hiddenPaths[h])
            operationCoordinator.updateDraft(targetVersion, {
                "selectedPaths": hiddenKept,
                "checkinMessage": hiddenDraft.checkinMessage || "",
                "review": null
            })
            return true
        }

        if (batchTarget !== targetVersion) return false
        var kept = []
        for (var i = 0; i < selectedPaths.length; ++i)
            if (!removed[String(selectedPaths[i])]) kept.push(selectedPaths[i])
        selectedPaths = kept
        previewData = null
        resultData = null
        planHash = ""
        messageInvalid = false
        if (phase === "review" || phase === "confirm" || phase === "failed") phase = "idle"
        _saveDraft()
        return true
    }

// Import success hands back exact project-relative paths only. This
    // merges that explicit list into the same-target ephemeral Batch, removes
    // any old Review/hash, and never substitutes an all-eligible selection.
    function mergeImportedPaths(paths, targetVersion) {
        if (!targetVersion || !paths) return false
        // Import A may finish while the user is working on B.  Update only A's
        // session draft; do not activate A or overwrite B's visible selectors,
        // message, Review, or target.  Returning to A later restores the draft
        // through the normal Active-version change path.
        if (operationCoordinator && targetVersion !== activeVersion) {
            var hiddenDraft = operationCoordinator.draftFor(targetVersion)
            var hiddenSeen = {}
            var hiddenMerged = []
            var hiddenPaths = hiddenDraft.selectedPaths ? hiddenDraft.selectedPaths : []
            for (var h = 0; h < hiddenPaths.length; ++h) {
                var hiddenExisting = String(hiddenPaths[h])
                if (hiddenExisting.length > 0 && !hiddenSeen[hiddenExisting]) {
                    hiddenSeen[hiddenExisting] = true
                    hiddenMerged.push(hiddenExisting)
                }
            }
            for (var p = 0; p < paths.length; ++p) {
                var hiddenImported = String(paths[p])
                if (hiddenImported.length > 0 && !hiddenSeen[hiddenImported]) {
                    hiddenSeen[hiddenImported] = true
                    hiddenMerged.push(hiddenImported)
                }
            }
            operationCoordinator.updateDraft(targetVersion, {
                "selectedPaths": hiddenMerged,
                "checkinMessage": "",
                "review": null
            })
            return true
        }
        if (batchTarget.length === 0) batchTarget = targetVersion
        else if (operationCoordinator && batchTarget !== targetVersion) _activateDraft(targetVersion)
        if (targetVersion !== batchTarget || !paths) return false
        var seen = {}
        var merged = []
        for (var i = 0; i < selectedPaths.length; ++i) {
            var existing = String(selectedPaths[i])
            if (!seen[existing]) { seen[existing] = true; merged.push(existing) }
        }
        for (var j = 0; j < paths.length; ++j) {
            var imported = String(paths[j])
            if (imported.length > 0 && !seen[imported]) {
                seen[imported] = true
                merged.push(imported)
            }
        }
        _bumpGeneration()
        selectedPaths = merged
        _saveDraft()
        previewData = null
        planHash = ""
        checkinMessage = ""
        messageInvalid = false
        if (phase === "review" || phase === "confirm" || phase === "failed")
            phase = "idle"
        return true
    }

    // Add to Batch: recovery-gate, then backend preview of EXACTLY the checked
    // selectors. It never calls a helper that returns every eligible row.
    function addToBatch() {
        if (selectedPaths.length === 0) { errorCode = "SELECTION_REQUIRED"; return }
        errorCode = ""
        if (batchTarget.length === 0) batchTarget = activeVersion
        else if (operationCoordinator && batchTarget !== activeVersion) _activateDraft(activeVersion)
        _saveDraft()
        _enqueue("recovery-gate", ["work-recovery", "status", "--json"])
    }

    function _issuePreview() {
        phase = "previewing"
        _enqueue("preview", ["checkin-batch", "preview", batchTarget]
                 .concat(_pathArgs(selectedPaths)))
    }

    // Remove one concrete item before Confirm: the selector set shrinks and the
    // plan is re-previewed (the old plan hash is invalidated).
    function removeItem(path) {
        var next = []
        for (var i = 0; i < selectedPaths.length; ++i)
            if (selectedPaths[i] !== path) next.push(selectedPaths[i])
        selectedPaths = next
        previewData = null; planHash = ""
        if (next.length === 0) { phase = "idle"; return }
        _issuePreview()
    }

    function continueToConfirm() {
        // A blocking conflict (or any non-applicable Batch) cannot reach Confirm.
        if (!canApply) return false
        phase = "confirm"
        return true
    }

    function confirmApply() {
        if (!previewData || !canApply) return false
        if (String(checkinMessage).replace(/^\s+|\s+$/g, "").length === 0) {
            messageInvalid = true
            errorCode = "MESSAGE_REQUIRED"
            return false
        }
        messageInvalid = false
        // Set phase BEFORE issuing so a synchronous bridge cannot race the guard
        // (also prevents a double activation from emitting a second apply).
        if (phase === "applying") return false
        phase = "applying"
        _saveDraft()
        _enqueue("apply", ["checkin-batch", "apply", batchTarget]
                 .concat(_pathArgs(selectedPaths))
                 .concat(["--expected-plan", planHash, "--message", checkinMessage]))
        return true
    }

    // ---- outcome-unknown verification --------------------------------------

    function verifyOutcome() { return verifyOutcomeFor("") }

    function verifyOutcomeFor(operationId) {
        var opId = operationId || recoveryOperationId
        if (resultData && resultData.operation_id) opId = resultData.operation_id
        else if ((!opId || opId.length === 0) && recoveryData && recoveryData.operation_id)
            opId = recoveryData.operation_id
        if (!opId || opId.length === 0) { errorCode = "OPERATION_NOT_FOUND"; return false }
        phase = "applying"
        _enqueue("verify", ["work-recovery", "verify", opId, "--json"])
        return true
    }

    function recover(operationId) {
        var opId = operationId ? String(operationId) : ""
        var selected = null
        var operations = recoveryData && recoveryData.operations ? recoveryData.operations : []
        for (var i = 0; i < operations.length; ++i)
            if (String(operations[i].operation_id || "") === opId) selected = operations[i]
        if (!selected || String(selected.target_version || "") !== batchTarget) {
            errorCode = "OPERATION_NOT_FOUND"
            return false
        }
        recoveryOperationId = opId
        phase = "applying"
        _enqueue("recover", ["work-recovery", "restore", opId, "--json"])
        return true
    }

    // Exact recovery selection only: never infer from a journal-list index.
    function presentRecovery(operation) {
        if (!operation || !operation.operation_id ||
                String(operation.target_version || "") !== activeVersion) {
            errorCode = "OPERATION_NOT_FOUND"
            return false
        }
        batchTarget = String(operation.target_version)
        recoveryData = { "operations": [operation] }
        recoveryOperationId = String(operation.operation_id)
        var manual = operation.state && String(operation.state).toLowerCase().indexOf("manual") >= 0
        manualReviewCode = manual ? "RECOVERY_EVIDENCE_MISMATCH" : ""
        phase = manual ? "manualReview" : "recovery"
        return true
    }

    // ---- target-switch guard ----------------------------------------------

    // A non-empty Batch is bound to its target; switching Active requires an
    // explicit Discard Batch / Cancel. The Batch is NEVER silently retargeted.
    function requestActiveSwitch(versionId) {
        if (!batchEmpty || mutationsBlocked) {
            pendingActiveSwitch = versionId
            requiresBatchDecision = true
            activeSwitchGuard()
            return false
        }
        activeVersion = versionId
        return true
    }

    function discardBatchAndSwitch() {
        if (pendingActiveSwitch.length === 0) return false
        var target = pendingActiveSwitch
        resetBatch()
        activeVersion = target
        return true
    }

    function cancelActiveSwitch() {
        pendingActiveSwitch = ""
        requiresBatchDecision = false
    }

    // ---- backend response handling ----------------------------------------

    function _stale(item) {
        // Discard any response whose generation/target no longer matches: a stale
        // read can never mutate the current Batch or re-enable a denied action.
        return !item || item.gen !== _generation || item.target !== batchTarget
    }

    function _logActivity(status, revision) {
        if (!sessionStore) return
        var rec = {
            "operationId": (resultData && resultData.operation_id) ? resultData.operation_id
                                                                   : ("ui-" + Date.now()),
            "operationType": "checkin-batch",
            "target": batchTarget,
            "status": status,
            "summary": "checkin-batch " + itemCount + " items",
            "itemCount": String(itemCount),
            "endTime": Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss")
        }
        if (revision) rec.revision = String(revision)
        sessionStore.appendActivity(rec)
    }

    function _refreshAfterTerminal() { if (readController) readController.refresh() }

    function _onSucceeded(json) {
        var item = _inflight
        var tag = _phase
        _phase = "idle"; _inflight = null
        if (_stale(item)) { _pump(); return }
        if (tag === "recovery-gate") {
            recoveryData = json
            if (json && json.recovery_required) {
                if (operationCoordinator) operationCoordinator.recordRecoveryOperations(json.operations || [])
                phase = "recovery"
            }
            else _issuePreview()
        } else if (tag === "preview") {
            previewData = json
            planHash = json && json.plan_hash ? json.plan_hash : ""
            phase = "review"
        } else if (tag === "apply") {
            resultData = json
            if (json && json.status === "committed") {
                verifiedRevision = json.revision !== undefined ? json.revision : null
                phase = "success"
                _logActivity("committed", json.revision)
                _refreshAfterTerminal()
                // Committed Batch items are cleared after the status refresh.
                selectedPaths = []
                _saveDraft()
            } else {
                // status "failed": pre-commit rollback, no revision, batch retained.
                phase = "failed"
                planHash = ""   // invalidate the old plan; a new Review is required
                _logActivity("failed", null)
            }
        } else if (tag === "verify" || tag === "recover") {
            resultData = json
            verifiedRevision = json.revision !== undefined ? json.revision : null
            if (json.status === "verified_committed" || json.status === "verified_not_committed") {
                if (operationCoordinator)
                    operationCoordinator.resolveRecovery(json.operation_id ? json.operation_id
                                                                           : recoveryOperationId)
            }
            if (json.status === "verified_committed") {
                phase = "success"
                selectedPaths = []
                _saveDraft()
                _refreshAfterTerminal()
            } else if (json.status === "verified_not_committed") {
                phase = "failed"
                planHash = ""
            } else {
                manualReviewCode = json.reason_code ? json.reason_code
                                                    : "OPERATION_EVIDENCE_INSUFFICIENT"
                phase = "manualReview"
            }
            _logActivity(json.status ? json.status : "verified", json.revision)
        }
        _pump()
    }

    function _onFailedJson(json) {
        var item = _inflight
        var tag = _phase
        _phase = "idle"; _inflight = null
        if (_stale(item)) { _pump(); return }
        var code = (json && json.error && json.error.code) ? json.error.code : "STATE_INCONSISTENT"
        errorCode = code
        if (code === "PLAN_STALE") {
            previewData = null; planHash = ""
            _issuePreview()                      // rebuild the plan → Review again
        } else if (code === "RECOVERY_REQUIRED") {
            phase = "recovery"
            _enqueue("recovery-gate", ["work-recovery", "status", "--json"])
        } else if (code === "OUTCOME_UNKNOWN") {
            phase = "outcomeUnknown"
            recoveryData = json
            recoveryOperationId = json && json.operation_id ? json.operation_id : ""
        } else if (code === "OPERATION_EVIDENCE_MISMATCH" ||
                   code === "OPERATION_EVIDENCE_AMBIGUOUS" ||
                   code === "OPERATION_EVIDENCE_INSUFFICIENT" ||
                   code === "REMOTE_STATE_DIVERGED" ||
                   code === "LEGACY_EVIDENCE_INSUFFICIENT" ||
                   code === "COMMIT_SCOPE_MISMATCH" ||
                   code === "EXPECTED_AUTHOR_UNAVAILABLE" ||
                   // A bounded query failure after COMMITTING is not a failed
                   // pre-commit plan.  Preserve the journal and block any
                   // retry until recovery can obtain complete evidence.
                   code === "RECOVERY_QUERY_INCOMPLETE") {
            manualReviewCode = code
            phase = "manualReview"
        } else if (code === "MESSAGE_REQUIRED" || code === "RESERVED_OPERATION_TRAILER") {
            phase = "confirm"                    // correctable in Confirm
        } else if (code === "SELECTION_REQUIRED" || code === "FOLDER_EMPTY") {
            phase = "review"
        } else {
            // Eligibility/provenance/containment blockers: retain the batch, show
            // the reason, invalidate the plan; the user corrects and re-reviews.
            // In particular, a timeout/limit/typed-query failure has no complete
            // backend plan behind it, so an old Review must never stay confirmable.
            phase = "failed"
            previewData = null; planHash = ""
            resultData = { "status": "failed", "reason_code": code }
        }
        _pump()
    }

    function _onFailed(message) {
        if (_phase === "idle") return
        var item = _inflight
        _phase = "idle"; _inflight = null
        if (_stale(item)) { _pump(); return }
        errorCode = "STATE_INCONSISTENT"
        phase = "failed"
        resultData = { "status": "failed" }
        _pump()
    }

    property var _bridge: Connections {
        target: root.isvnCli ? root.isvnCli : null
        ignoreUnknownSignals: true
        function onCommandSucceeded(json) { root._onSucceeded(json) }
        function onCommandFailedJson(json) { root._onFailedJson(json) }
        function onCommandFailed(message) {
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

    onCheckinMessageChanged: _saveDraft()
    onActiveVersionChanged: {
        // An admitted Apply/Verify keeps its immutable target and presentation
        // context.  A switch to B must not clear A's result callback; B can use
        // the other bounded controller lanes while A remains target-addressable
        // in the Operation Center.
        if (operationCoordinator && activeVersion && batchTarget && batchTarget !== activeVersion && !busy)
            _activateDraft(activeVersion)
    }

    // ---- localized reason text (EN / zh_TW / zh_CN) ------------------------

    function reasonText(code) {
        if (!i18n) return code
        if (code === "SELECTION_REQUIRED") return i18n.text("Check at least one item to add to the Batch.",
            "請至少勾選一個項目加入批次。", "请至少勾选一个项目加入批次。")
        if (code === "FOLDER_EMPTY") return i18n.text("The selected folder has no checkin-eligible item.",
            "所選資料夾沒有可簽入的項目。", "所选文件夹没有可签入的项目。")
        if (code === "SELECTOR_LIMIT_EXCEEDED") return i18n.text("Too many selected paths. Choose a narrower set.",
            "選取路徑過多，請縮小選取範圍。", "选择路径过多，请缩小选择范围。")
        if (code === "LOCAL_SCAN_LIMIT_EXCEEDED") return i18n.text("The selected local scope is too large. Choose a narrower folder or set.",
            "所選本機範圍過大，請選擇較小的資料夾或集合。", "所选本地范围过大，请选择较小的文件夹或集合。")
        if (code === "BATCH_ITEM_LIMIT_EXCEEDED") return i18n.text("The Batch contains too many items. Narrow the selection.",
            "批次包含過多項目，請縮小選取範圍。", "批次包含过多项目，请缩小选择范围。")
        if (code === "REMOTE_QUERY_LIMIT_EXCEEDED") return i18n.text("The selected scope exceeds the safe repository-query limit. Narrow it and try again.",
            "所選範圍超過安全的版本庫查詢上限，請縮小後重試。", "所选范围超过安全的版本库查询上限，请缩小后重试。")
        if (code === "SVN_QUERY_TIMEOUT") return i18n.text("Preview query timed out. Refresh or choose a narrower selection.",
            "預覽查詢逾時，請重新整理或縮小選取範圍。", "预览查询超时，请刷新或缩小选择范围。")
        if (code === "SVN_OUTPUT_LIMIT_EXCEEDED") return i18n.text("Preview output exceeded the safe limit. Choose a narrower selection.",
            "預覽輸出超過安全上限，請縮小選取範圍。", "预览输出超过安全上限，请缩小选择范围。")
        if (code === "SVN_QUERY_FAILED") return i18n.text("The repository query did not complete. Refresh before reviewing again.",
            "版本庫查詢未完整完成，請重新整理後再檢視。", "版本库查询未完整完成，请刷新后再查看。")
        if (code === "RECOVERY_QUERY_INCOMPLETE") return i18n.text("Recovery verification is incomplete. Verify again after the repository is available; do not retry Checkin.",
            "復原驗證尚未完成。版本庫可用後請再次驗證，請勿重試簽入。", "恢复验证尚未完成。版本库可用后请再次验证，请勿重试签入。")
        if (code === "NO_ACTIVE_VERSION") return i18n.text("Select or check out an Active version first.",
            "請先選擇或檢出一個作用中版本。", "请先选择或检出一个活动版本。")
        if (code === "TARGET_MISMATCH") return i18n.text("The Batch and Active target differ.",
            "批次與作用中目標不一致。", "批次与活动目标不一致。")
        if (code === "PATH_OUTSIDE_WORKSPACE") return i18n.text("A path is outside the working copy; blocked.",
            "路徑不在工作副本內，已封鎖。", "路径不在工作副本内，已封锁。")
        if (code === "PATH_NOT_FOUND") return i18n.text("The item is missing; refresh or remove it.",
            "項目已遺失，請重新整理或移除。", "项目已丢失，请刷新或移除。")
        if (code === "UNSUPPORTED_FILE_TYPE") return i18n.text("Unsupported or special content; blocked.",
            "不支援或特殊內容，已封鎖。", "不支持或特殊内容，已封锁。")
        if (code === "NOT_CHECKIN_ELIGIBLE") return i18n.text("Nothing to submit for this item.",
            "此項目沒有可提交的變更。", "此项目没有可提交的更改。")
        if (code === "CONFLICT_BLOCKED") return i18n.text("Resolve or remove the conflicted item to continue.",
            "請解決或移除衝突項目以繼續。", "请解决或移除冲突项目以继续。")
        if (code === "REMOVAL_REQUIRES_REMOVE") return i18n.text("Use the Remove action for missing/removed content.",
            "遺失/已移除的內容請改用移除功能。", "丢失/已移除的内容请改用移除功能。")
        if (code === "MANAGED_OVERWRITE_BLOCKED") return i18n.text("Overwriting a managed path is blocked.",
            "覆寫受管路徑已封鎖。", "覆写受管路径已封锁。")
        if (code === "EDIT_PROVENANCE_REQUIRED") return i18n.text("Revert, then Make Editable to check in this managed file.",
            "請先還原再設為可編輯，才能簽入此受管檔案。", "请先还原再设为可编辑，才能签入此受管文件。")
        if (code === "EDIT_PROVENANCE_STALE") return i18n.text("The editable provenance no longer matches; revert and recover.",
            "可編輯來源證明已不符，請還原並復原。", "可编辑来源证明已不符，请还原并恢复。")
        if (code === "MESSAGE_REQUIRED") return i18n.text("Enter a Checkin Message.",
            "請輸入簽入訊息。", "请输入签入信息。")
        if (code === "RESERVED_OPERATION_TRAILER") return i18n.text("The message uses reserved operation syntax; edit it.",
            "訊息使用了保留的操作語法，請修改。", "信息使用了保留的操作语法，请修改。")
        if (code === "PLAN_STALE") return i18n.text("The workspace changed; please review again.",
            "工作區已變更，請重新檢視。", "工作区已变更，请重新查看。")
        if (code === "RECOVERY_REQUIRED") return i18n.text("An interrupted operation must be verified/recovered first.",
            "必須先驗證/復原被中斷的操作。", "必须先验证/恢复被中断的操作。")
        if (code === "OPERATION_IN_PROGRESS") return i18n.text("That operation is still running. It can be verified only after it stops.",
            "該操作仍在執行，停止後才能驗證。", "该操作仍在执行，停止后才能验证。")
        if (code === "VERSION_RECOVERY_REQUIRED") return i18n.text("This Version needs recovery before another mutation.",
            "此版本需先復原才能進行其他變更。", "此版本需先恢复才能进行其他变更。")
        if (code === "PROJECT_LEGACY_RECOVERY_REQUIRED") return i18n.text("An older Project recovery record must be resolved first.",
            "必須先處理較舊的專案復原紀錄。", "必须先处理较旧的项目恢复记录。")
        if (code === "OPERATION_TARGET_DIVERGED") return i18n.text("The operation target changed. Manual Review is required.",
            "操作目標已變更，需要人工檢視。", "操作目标已变更，需要人工检查。")
        if (code === "OPERATION_NOT_FOUND") return i18n.text("Select the exact operation before verification.",
            "請先選取正確操作再進行驗證。", "请先选择正确操作再进行验证。")
        if (code === "READ_LANE_BUSY") return i18n.text("The bounded read lane is busy. Refresh again shortly.",
            "受限讀取通道忙碌，請稍後重新整理。", "受限读取通道忙碌，请稍后刷新。")
        if (code === "REQUEST_LIMIT_EXCEEDED") return i18n.text("The request exceeds the safe size limit.",
            "請求超過安全大小上限。", "请求超过安全大小上限。")
        if (code === "OPERATION_REGISTRY_CORRUPT") return i18n.text("Operation records need Manual Review before Checkin.",
            "簽入前需要人工檢視操作紀錄。", "签入前需要人工检查操作记录。")
        if (code === "AUTHORIZATION_DENIED") return i18n.text("The backend denied this operation.",
            "後端拒絕了此操作。", "后端拒绝了此操作。")
        if (code === "OUTCOME_UNKNOWN") return i18n.text("Repository outcome unknown — verify before any retry.",
            "版本庫結果未知 — 請先驗證再重試。", "版本库结果未知 — 请先验证再重试。")
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
        return code
    }
}
