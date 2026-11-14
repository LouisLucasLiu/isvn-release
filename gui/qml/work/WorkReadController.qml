import QtQuick
import "../common" as Common

// WorkReadController — read-only binding of the Work screen to the real
// CLI read contract (`workspaces --json`, `work-status <version> --json`) through
// the injected single-flight IsvnCli bridge. It exposes the SAME data shape as
// WorkMockModel so the Work components render unchanged, but the data
// originates from the real backend, never from fictional mock data.
//
// It performs NO mutation. All calls are serialized (never runJson while the
// bridge is running); a generation token + per-result version check ensures a
// stale response from an earlier Active selection cannot overwrite a newer one.
QtObject {
    id: root

    property var isvnCli
    property var sessionStore
    // A bounded, independent read lane.  Mutation results are never routed
    // through this controller; it retains its existing active-generation guard
    // for read snapshots only.
    property var operationCoordinator
    property var i18n
    property string projectPath: ""
    property string projectName: ""

    // state: idle | loading | ready | empty | error
    property string state: "idle"
    property string errorText: ""

    // Active Working Version (full repository path) — the source of truth in
    // read mode. Explore Inspected Version is deliberately NOT stored here.
    property string activeVersionId: ""
    property string activeWorkspaceId: ""

// Mock-compatible data surface consumed by the Work components.
    property var identity: ({
        "projectName": root.projectName,
        "workspacePath": "",
        "role": "",
        "buildIdentity": "isvn (read)",
        "backendState": "ready"
    })
    property var localVersions: []
    property var activeFiles: []
// Read mode has no ephemeral batch/import yet.
    readonly property var batchEntries: []
    readonly property var importItems: []
    readonly property string importSourceRoot: ""

    signal snapshotReady()
    // Explore's explicit Set Active route.  Unlike startup restoration, this
    // always rebuilds the authoritative workspace snapshot first.
    signal explicitActiveResolved(bool ok, string version)

    // --- serialization ----------------------------------------------------
    property int _gen: 0
    property string _phase: "idle"        // idle | workspaces | workStatus
    property var _queue: []

    function _enqueue(phase, args) {
        var target = phase === "workStatus" ? activeVersionId : "project-read"
        _queue.push({ "phase": phase, "args": args, "target": target, "gen": _gen })
        _pump()
    }

    function _pump() {
        if (_phase !== "idle")
            return
        if (!isvnCli)
            return
        if (!operationCoordinator && isvnCli.running)
            return
        if (_queue.length === 0)
            return
        var next = _queue.shift()
        _phase = next.phase
        _inflightGen = next.gen
        // A ProductShell always owns the coordinator in production.  Keep the
// Single-flight fallback for an injected read-only bridge
        // (and for a damaged/partial bridge) rather than treating a missing
        // operation lane as a successful read admission.
        if (operationCoordinator && operationCoordinator.beginRead &&
                isvnCli.runJsonForOperation) {
            _inflight = next
            _viaBridge = false
            var id = operationCoordinator.beginRead("work-read-" + next.phase, next.target, next.args)
            _inflight.operationId = id
            Qt.callLater(function() {
                if (!root._inflight || root._inflight.operationId !== id) return
                var record = root.operationCoordinator.operationFor(id)
                if (record && record.state !== "starting" && record.state !== "running" && record.result)
                    root._onCoordinatorResult(id, record.targetVersion, record.result)
            })
            return
        }
        _viaBridge = true
        isvnCli.runJson(next.args)
    }
    property var _inflight: null
    // True while the read in flight went through the shared bridge (runJson).
    // The bridge broadcasts every command's result to every listener, so its
    // signals answer this controller only while such a read is outstanding.
    property bool _viaBridge: false
    // Generation of the read that is currently in flight.
    property int _inflightGen: -1

    // A plain reload.  It is never the answer to an explicit Set Active: a
    // request that is still open ends here, unanswered.
    function refresh() {
        cancelExplicitActivation()
        _startRefresh()
    }

    function _startRefresh() {
        if (!isvnCli) {
            state = "error"
            errorText = "backend bridge unavailable"
            return false
        }
        _gen += 1
        _queue = []
        state = "loading"
        errorText = ""
        _enqueue("workspaces", ["workspaces", "--json"])
        return true
    }

    // Switching Active Working Version (read mode). Validates the target is in
    // the local inventory, persists the hint, then serially refreshes status.
    function selectActive(versionId) {
        if (!versionExists(versionId))
            return false
        activeVersionId = versionId
        var v = versionByRepositoryPath(versionId)
        activeWorkspaceId = v ? v.workspaceId : ""
        if (sessionStore && activeWorkspaceId.length > 0)
            sessionStore.setActiveVersion(activeWorkspaceId, versionId)
        _requestActiveStatus()
        return true
    }

    // An explicit Set Active is answered only by the inventory read it starts
    // here.  It ends unanswered when a read fails, when another reload or
    // request replaces it, or when it is cancelled, so no later reload can
    // complete it on the user's behalf.
    function revalidateAndSelect(versionId) {
        if (!versionId || versionId.length === 0) {
            explicitActiveResolved(false, versionId)
            return false
        }
        cancelExplicitActivation()
        _pendingExplicitActive = versionId
        // Bound before the read is issued.  Without a bridge no read starts
        // and the request matches no answer.
        _pendingExplicitGen = _gen + 1
        if (!_startRefresh())
            _pendingExplicitGen = -1
        return true
    }
    property string _pendingExplicitActive: ""
    property int _pendingExplicitGen: -1

    // Ends an explicit Set Active that has not been answered yet.
    function cancelExplicitActivation() {
        if (_pendingExplicitActive.length === 0)
            return false
        var requested = _pendingExplicitActive
        _pendingExplicitActive = ""
        _pendingExplicitGen = -1
        explicitActiveResolved(false, requested)
        return true
    }

    function _requestActiveStatus() {
        if (activeVersionId.length === 0) {
            activeFiles = []
            state = localVersions.length > 0 ? "ready" : "empty"
            return
        }
        _enqueue("workStatus", ["work-status", activeVersionId, "--json"])
    }

    function versionExists(versionId) {
        for (var i = 0; i < localVersions.length; ++i)
            if (localVersions[i].id === versionId)
                return true
        return false
    }

    function versionByRepositoryPath(versionId) {
        for (var i = 0; i < localVersions.length; ++i)
            if (localVersions[i].id === versionId)
                return localVersions[i]
        return null
    }

    // WorkMockModel-compatible helpers.
    function versionById(id) { return versionByRepositoryPath(id) }
    function humanSize(bytes) {
        if (bytes === undefined || bytes === null || bytes < 0)
            return ""
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB"
    }

    // --- result handling --------------------------------------------------
    function _uiStateColorTone(uiState) { return uiState }

    function _handleWorkspaces(json, gen) {
        var list = []
        var wss = (json && json.workspaces) ? json.workspaces : []
        for (var i = 0; i < wss.length; ++i) {
            var w = wss[i]
            var counts = w.counts || {}
            list.push({
                "id": w.repository_path,
                "workspaceId": w.workspace_id,
                "versionName": w.version,
                "phase": w.phase === null ? "" : w.phase,
                "block": w.block === null ? "" : w.block,
                "stage": w.stage === null ? "" : w.stage,
                "repositoryPath": w.repository_path,
                "localPath": w.working_copy,
                "changedCount": counts.changed || 0,
                "conflictCount": counts.conflict || 0,
                "localCount": counts.local || 0,
                "lastUsedAt": "",
                "health": w.health,
                "isActive": false
            })
        }
        localVersions = list

        if (json && json.project && json.project.name)
            root.projectName = json.project.name

        // Determine Active: keep a still-valid current selection; else restore a
        // saved hint validated against the inventory; else no Active (never a
        // fallback, never an auto-checkout).
        if (activeVersionId.length > 0 && !versionExists(activeVersionId))
            activeVersionId = ""
        if (activeVersionId.length === 0 && sessionStore) {
            var hint = sessionStore.activeHint()
            if (hint && hint.version && versionExists(hint.version)) {
                var v = versionByRepositoryPath(hint.version)
                // Validate the workspace_id matches too (stale-checkout guard).
                if (v && (!hint.workspace_id || hint.workspace_id === v.workspaceId)) {
                    activeVersionId = hint.version
                    activeWorkspaceId = v.workspaceId
                } else if (sessionStore.clearActiveVersion) {
                    sessionStore.clearActiveVersion()
                }
            } else if (hint && hint.version && sessionStore.clearActiveVersion) {
                // Saved hint is stale (no longer checked out): clear it safely.
                sessionStore.clearActiveVersion()
            }
        }
        if (_pendingExplicitActive.length > 0 && gen === _pendingExplicitGen) {
            var requested = _pendingExplicitActive
            _pendingExplicitActive = ""
            _pendingExplicitGen = -1
            if (!versionExists(requested)) {
                explicitActiveResolved(false, requested)
                _markActiveFlag()
                _requestActiveStatus()
                return
            }
            // selectActive persists only after this new workspaces response
            // proves the local checkout and workspace identity still exist.
            selectActive(requested)
            _markActiveFlag()
            explicitActiveResolved(true, requested)
            return
        }
        _markActiveFlag()
        _requestActiveStatus()
    }

    function _markActiveFlag() {
        var list = localVersions.slice()
        for (var i = 0; i < list.length; ++i)
            list[i].isActive = (list[i].id === activeVersionId)
        localVersions = list
    }

    function _handleWorkStatus(json) {
        // Stale-response guard: ignore a status whose version is no longer the
        // Active selection.
        if (!json || json.version !== activeVersionId)
            return
        var files = []
        var entries = json.entries || []
        for (var i = 0; i < entries.length; ++i) {
            var e = entries[i]
            files.push({
                "path": e.path,
                "kind": e.kind,
                "state": e.ui_state === undefined ? "" : e.ui_state,
                "sourceKind": e.source_kind === undefined ? "" : e.source_kind,
                "sizeBytes": (e.size_bytes === null || e.size_bytes === undefined) ? -1 : e.size_bytes,
                "selected": false,
                "availability": e.actions || {}
            })
        }
        activeFiles = files
        state = "ready"
        snapshotReady()
    }

    function _onCoordinatorResult(operationId, targetVersion, json) {
        if (!_inflight || _inflight.operationId !== operationId || _inflight.target !== targetVersion) return
        var phase = _phase
        var gen = _inflightGen
        _phase = "idle"
        _inflight = null
        if (json && json.error) {
            var code = json.error.code ? json.error.code : "SVN_QUERY_FAILED"
            state = "error"
            errorText = code
            _queue = []
            // The failed or dropped read can no longer answer a Set Active.
            cancelExplicitActivation()
            return
        }
        if (phase === "workspaces") root._handleWorkspaces(json, gen)
        else if (phase === "workStatus") root._handleWorkStatus(json)
        root._pump()
    }

    property var _bridge: Connections {
        target: root.isvnCli ? root.isvnCli : null
        function onCommandSucceeded(json) {
            if (!root._viaBridge)
                return
            root._viaBridge = false
            var phase = root._phase
            var gen = root._inflightGen
            root._phase = "idle"
            if (phase === "workspaces")
                root._handleWorkspaces(json, gen)
            else if (phase === "workStatus")
                root._handleWorkStatus(json)
            root._pump()
        }
        function onCommandFailed(message) {
            if (!root._viaBridge)
                return
            root._viaBridge = false
            root._phase = "idle"
            root._queue = []
            root.state = "error"
            root.errorText = message
            root.cancelExplicitActivation()
            root._pump()
        }
    }

    property var _coordinatorBridge: Connections {
        target: root.operationCoordinator ? root.operationCoordinator : null
        ignoreUnknownSignals: true
        function onTargetResult(operationId, targetVersion, result) {
            root._onCoordinatorResult(operationId, targetVersion, result)
        }
    }
}
