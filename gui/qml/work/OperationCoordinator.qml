import QtQuick

// OperationCoordinator — session-only, target-keyed operation
// routing.  It deliberately owns neither filesystem/SVN policy nor durable
// recovery authority: every Apply is validated by the CLI backend.  Its job is
// to keep a completion for Version A attached to A after the user switches to
// B, and to retain only safe ephemeral drafts for the life of this GUI process.
// It never refuses or queues an operation because another one is running.
Item {
    id: root
    visible: false
    width: 0
    height: 0

    property var isvnCli: null
    property string activeVersion: ""
    property var draftsByVersion: ({})
    property var operationsById: ({})
    property int sequence: 0

    readonly property int activeMutationCount: isvnCli && isvnCli.activeMutations !== undefined
                                             ? isvnCli.activeMutations : 0
    readonly property int activeReadCount: isvnCli && isvnCli.activeReads !== undefined
                                         ? isvnCli.activeReads : 0
    readonly property int operationCount: allOperations().length

    signal operationChanged(string operationId, string targetVersion)
    signal targetResult(string operationId, string targetVersion, var result)

    function _copy(object) {
        var result = {}
        for (var key in object) result[key] = object[key]
        return result
    }

    function _token(prefix) {
        sequence += 1
        return prefix + "-" + Date.now() + "-" + sequence
    }

    function draftFor(versionId) {
        if (!versionId || versionId.length === 0) return ({})
        if (!draftsByVersion[versionId]) {
            var next = _copy(draftsByVersion)
            next[versionId] = {
                "selectedPaths": [], "checkinMessage": "", "replaceAcknowledgements": ({}),
                "pendingBatchPaths": [], "review": null
            }
            draftsByVersion = next
        }
        return draftsByVersion[versionId]
    }

    function updateDraft(versionId, patch) {
        var previous = draftFor(versionId)
        var draft = _copy(previous)
        for (var key in patch) draft[key] = patch[key]
        var next = _copy(draftsByVersion)
        next[versionId] = draft
        draftsByVersion = next
    }

    function operationFor(operationId) { return operationsById[operationId] || null }

    function operationsForVersion(versionId) {
        var result = []
        for (var id in operationsById) {
            var operation = operationsById[id]
            if (operation.targetVersion === versionId) result.push(operation)
        }
        result.sort(function(a, b) { return String(a.operationId).localeCompare(String(b.operationId)) })
        return result
    }

    function allOperations() {
        var result = []
        for (var id in operationsById) result.push(operationsById[id])
        result.sort(function(a, b) { return String(a.operationId).localeCompare(String(b.operationId)) })
        return result
    }

    // Progress display only: an operation on this Version is still running.
    function hasRunningOperation(versionId) {
        var operations = operationsForVersion(versionId)
        for (var i = 0; i < operations.length; ++i)
            if (operations[i].state === "running") return true
        return false
    }

    // An interrupted or unverified operation on this Version has to be
    // recovered or verified before the Version is changed again.
    function needsRecovery(versionId) {
        var operations = operationsForVersion(versionId)
        for (var i = 0; i < operations.length; ++i) {
            var state = operations[i].state
            if (state === "recovery" || state === "unknown" || state === "manualReview") return true
        }
        return false
    }

    function beginMutation(kind, targetVersion, arguments, stdinPayload, planHash) {
        return _begin(kind, targetVersion, arguments, stdinPayload || "", planHash || "", true)
    }

    function beginRead(kind, targetVersion, arguments, stdinPayload) {
        return _begin(kind, targetVersion, arguments, stdinPayload || "", "", false)
    }

    function _begin(kind, targetVersion, arguments, stdinPayload, planHash, mutation) {
        if (!isvnCli || !targetVersion || targetVersion.length === 0) return ""
        // The coordinator may show a progress badge for the target, but it
        // never holds a request back because another operation is running.
        // Double-click prevention remains in the individual controller's own
        // in-flight UI state.
        var operationId = _token("ui-op")
        var record = {
            "operationId": operationId,
            "operationKind": kind,
            "targetVersion": targetVersion,
            "planHash": planHash,
            "mutation": mutation,
            "state": "starting",
            "result": null,
            "errorCode": ""
        }
        var next = _copy(operationsById)
        next[operationId] = record
        operationsById = next
        // The bridge's operation API does not queue.  A false return is
        // represented by its immediate, target-pinned structured failure.
        var started = isvnCli.runJsonForOperation(operationId, targetVersion, arguments,
                                                  stdinPayload, mutation)
        if (started) {
            record = _copy(record)
            record.state = "running"
            next = _copy(operationsById)
            next[operationId] = record
            operationsById = next
            operationChanged(operationId, targetVersion)
        }
        return operationId
    }

    function _replace(operationId, update) {
        var existing = operationFor(operationId)
        if (!existing) return null
        var record = _copy(existing)
        for (var key in update) record[key] = update[key]
        var next = _copy(operationsById)
        next[operationId] = record
        operationsById = next
        operationChanged(operationId, record.targetVersion)
        return record
    }

    // A recovery or verification that reached a final result clears every record of
    // that backend operation, so the Version can be changed again.
    function resolveRecovery(backendOperationId) {
        if (!backendOperationId || String(backendOperationId).length === 0) return
        var wanted = String(backendOperationId)
        var next = _copy(operationsById)
        var cleared = []
        for (var id in operationsById) {
            var record = operationsById[id]
            var backend = record.result && record.result.operation_id
                          ? String(record.result.operation_id) : ""
            if (id === wanted || backend === wanted) {
                delete next[id]
                cleared.push(record)
            }
        }
        if (cleared.length === 0) return
        operationsById = next
        for (var i = 0; i < cleared.length; ++i)
            operationChanged(cleared[i].operationId, cleared[i].targetVersion)
    }

    function acknowledge(operationId) {
        var record = operationFor(operationId)
        if (!record || record.state === "recovery" || record.state === "unknown" ||
                record.state === "manualReview") return false
        var next = _copy(operationsById)
        delete next[operationId]
        operationsById = next
        operationChanged(operationId, record.targetVersion)
        return true
    }

    // Durable recovery discovery is backend evidence.  Preserve every journal
    // under its exact backend operation ID so no caller can infer a recovery
    // target from list ordering or the current Active Version.
    function recordRecoveryOperations(operations) {
        var next = _copy(operationsById)
        var changed = []
        for (var i = 0; operations && i < operations.length; ++i) {
            var source = operations[i]
            var id = source && source.operation_id ? String(source.operation_id) : ""
            var target = source && source.target_version ? String(source.target_version) : ""
            if (id.length === 0 || target.length === 0) continue
            var sourceState = source.state ? String(source.state).toLowerCase() : ""
            var state = sourceState.indexOf("manual") >= 0 ? "manualReview" : "recovery"
            next[id] = {
                "operationId": id,
                "operationKind": source.operation ? String(source.operation) : "work-recovery",
                "targetVersion": target,
                "planHash": "",
                "mutation": true,
                "state": state,
                "result": source,
                "errorCode": state === "manualReview" ? "RECOVERY_EVIDENCE_MISMATCH" : "RECOVERY_REQUIRED",
                "legacyProjectBlocking": source.legacy_project_blocking === true
            }
            changed.push(id)
        }
        if (changed.length > 0) {
            operationsById = next
            for (var j = 0; j < changed.length; ++j)
                operationChanged(changed[j], next[changed[j]].targetVersion)
        }
    }

    Connections {
        target: root.isvnCli
// Older read-only test bridges intentionally expose only the read-model
        // single-flight signals.  They are not an operation-capable executor,
        // so connecting this long-lived coordinator must stay silent and let
        // WorkReadController use its proven legacy read fallback.
        ignoreUnknownSignals: true
        function onOperationSucceeded(operationId, targetVersion, json) {
            var existing = root.operationFor(operationId)
            if (!existing || existing.targetVersion !== targetVersion) return
            var state = "success"
            if (json && json.manual_review_required) state = "manualReview"
            else if (json && json.recovery_required) state = "recovery"
            else if (json && json.status === "outcome_unknown") state = "unknown"
            var record = root._replace(operationId, { "state": state, "result": json,
                                                       "errorCode": "" })
            if (record && record.operationKind === "import" && state === "success" &&
                    json && json.batch_paths) {
                var draft = root.draftFor(targetVersion)
                root.updateDraft(targetVersion, { "pendingBatchPaths": json.batch_paths,
                                                   "review": null })
            } else if (record && record.operationKind === "checkin-batch" && state === "success" &&
                       json && json.status === "committed") {
                // A background successful Batch must clear only its own
                // session draft.  It cannot leave a stale reviewed selector
                // behind for a later Active switch, and it cannot touch B.
                root.updateDraft(targetVersion, { "selectedPaths": [], "checkinMessage": "",
                                                   "review": null })
            }
            root.targetResult(operationId, targetVersion, json)
        }
        function onOperationFailedJson(operationId, targetVersion, envelope) {
            var existing = root.operationFor(operationId)
            if (!existing || existing.targetVersion !== targetVersion) return
            var error = envelope && envelope.error ? envelope.error : ({})
            var code = error.code || "SVN_QUERY_FAILED"
            var state = error.recovery_required ? "recovery" : "failed"
            root._replace(operationId, { "state": state, "result": envelope, "errorCode": code })
            root.targetResult(operationId, targetVersion, envelope)
        }
    }
}
