import QtQuick

// ImportController orchestration only. It does not inspect URLs,
// traverse, copy, hash, stage, commit, or mutate files.  The backend owns every
// source/destination/collision decision; external URLs remain transient request
// input and are never sent via argv or Activity Log.
QtObject {
    id: root

    property var isvnCli
    property var i18n
    property var readController
    property var checkinController
    property var sessionStore
    property var operationCoordinator
    property string activeVersion: ""
    // Immutable for one Preview → Apply → result flow.  Active navigation may
    // change while it runs, but it must never rebuild an A request as B.
    property string operationTarget: ""

    // idle | previewing | review | confirm | applying | success | failed |
    // recovery | manualReview
    property string phase: "idle"
    property string destination: ""
    property var sourceUrls: []       // transient only; never rendered/persisted
    property var previewData: null
    property var resultData: null
    property string planHash: ""
    property var replaceLocal: ({})
    property string errorCode: ""
    property bool batchIntegrationPending: false
    property bool batchIntegrated: false
    property string recoveryOperationId: ""
    property var _pendingBatchPaths: []
    property bool _awaitingRefresh: false
    property int _generation: 0
    property var _inflight: null

    readonly property bool busy: phase === "previewing" || phase === "applying" || _inflight !== null
    readonly property bool mutationsBlocked: busy || phase === "confirm" || phase === "recovery" ||
                                             phase === "manualReview"
    readonly property var items: previewData && previewData.items ? previewData.items : []
    readonly property int selectedReplacementCount: {
        var count = 0
        for (var i = 0; i < items.length; ++i)
            if (items[i].replace_local_available === true && replaceLocal[items[i].destination] === true)
                ++count
        return count
    }
    readonly property bool canConfirm: {
        if (!previewData) return false
        for (var i = 0; i < items.length; ++i) {
            var item = items[i]
            if (item.allowed === true) continue
            if (item.reason_code === "REPLACE_LOCAL_REQUIRED" && replaceLocal[item.destination] === true)
                continue
            return false
        }
        return items.length > 0
    }

    function reasonText(code) {
        if (!i18n) return code
        var text = {
            "IMPORT_SOURCE_REQUIRED": ["Drop at least one source", "請拖入至少一個來源", "请拖入至少一个来源"],
            "UNSUPPORTED_SOURCE_URL": ["Only local file URLs are supported", "僅支援本機檔案 URL", "仅支持本地文件 URL"],
            "SOURCE_NOT_FOUND": ["The source is no longer available", "來源已不存在", "来源已不存在"],
            "SOURCE_UNREADABLE": ["The source cannot be read", "無法讀取來源", "无法读取来源"],
            "SOURCE_SYMLINK_UNSUPPORTED": ["Source symlinks are not supported", "不支援來源符號連結", "不支持来源符号链接"],
            "SOURCE_SPECIAL_FILE_UNSUPPORTED": ["Source contains an unsupported special file", "來源包含不支援的特殊檔案", "来源包含不支持的特殊文件"],
            "EMPTY_DIRECTORY_UNSUPPORTED": ["Empty directories cannot be imported", "無法匯入空目錄", "无法导入空目录"],
            "OVERLAPPING_SOURCE_ROOTS": ["Remove overlapping source roots", "請移除重疊的來源根目錄", "请移除重叠的来源根目录"],
            "SOURCE_TARGET_OVERLAP": ["A source cannot overlap the Active workspace", "來源不可與使用中的工作區重疊", "来源不可与使用中的工作区重叠"],
            "IMPORT_MAPPING_COLLISION": ["Two sources map to the same destination", "兩個來源映射到相同目標", "两个来源映射到相同目标"],
            "INVALID_DROP_TARGET": ["Drop on a folder row or the Active-root blank area", "請拖放到資料夾列或使用中根目錄空白區", "请拖放到文件夹行或活动根目录空白区"],
            "NO_ACTIVE_VERSION": ["Select an Active version first", "請先選擇使用中的版本", "请先选择活动版本"],
            "TARGET_MISMATCH": ["The Import and Active targets differ", "匯入與使用中目標不一致", "导入与活动目标不一致"],
            "PATH_OUTSIDE_WORKSPACE": ["Destination is outside the Active workspace", "目標不在使用中的工作區內", "目标不在活动工作区内"],
            "MANAGED_DESTINATION_BLOCKED": ["Managed files cannot be replaced by Import", "Import 無法取代受管理檔案", "Import 无法替换受管理文件"],
            "REPLACE_LOCAL_REQUIRED": ["Select Replace Local for this LOCAL file", "請為此 LOCAL 檔案選擇取代", "请为该 LOCAL 文件选择替换"],
            "REPLACE_LOCAL_NOT_ALLOWED": ["This replacement is no longer eligible", "此取代已不再符合資格", "此替换已不再符合条件"],
            "DESTINATION_SYMLINK_BLOCKED": ["Destination topology is unsafe", "目標目錄結構不安全", "目标目录结构不安全"],
            "DESTINATION_TYPE_CONFLICT": ["A destination file/directory type conflicts", "目標檔案／目錄類型衝突", "目标文件／目录类型冲突"],
            "PLAN_STALE": ["Source or destination changed. Preview again.", "來源或目標已變更，請再次預覽。", "来源或目标已变更，请再次预览。"],
            "IMPORT_ENTRY_LIMIT_EXCEEDED": ["The selected source scope is too large", "所選來源範圍過大", "所选来源范围过大"],
            "IMPORT_SIZE_LIMIT_EXCEEDED": ["The source size exceeds the safe limit", "來源大小超過安全上限", "来源大小超过安全上限"],
            "INSUFFICIENT_TARGET_SPACE": ["Free target workspace capacity and try again", "請釋放目標工作區容量後重試", "请释放目标工作区容量后重试"],
            "IMPORT_READ_TIMEOUT": ["Source reading made no progress", "讀取來源沒有進度", "读取来源没有进度"],
            "SELECTOR_LIMIT_EXCEEDED": ["The selected scope is too large", "所選範圍過大", "所选范围过大"],
            "REMOTE_QUERY_LIMIT_EXCEEDED": ["The repository query scope is too large", "版本庫查詢範圍過大", "版本库查询范围过大"],
            "SVN_QUERY_TIMEOUT": ["Repository query timed out", "版本庫查詢逾時", "版本库查询超时"],
            "SVN_OUTPUT_LIMIT_EXCEEDED": ["Repository query output exceeded its safe limit", "版本庫查詢輸出超過安全上限", "版本库查询输出超过安全上限"],
            "SVN_QUERY_FAILED": ["Repository evidence is incomplete", "版本庫證據不完整", "版本库证据不完整"],
            "RECOVERY_REQUIRED": ["Recovery is required before Import.", "Import 前必須先復原。", "Import 前必须先恢复。"],
            "RECOVERY_EVIDENCE_MISMATCH": ["Recovery evidence needs Manual Review", "復原證據需要人工檢視", "恢复证据需要人工检查"],
            "VERSION_RECOVERY_REQUIRED": ["This Version needs recovery before another mutation", "此版本需先復原才能進行其他變更", "此版本需先恢复才能进行其他变更"],
            "PROJECT_LEGACY_RECOVERY_REQUIRED": ["An older Project recovery record must be resolved first", "必須先處理較舊的專案復原紀錄", "必须先处理较旧的项目恢复记录"],
            "OPERATION_TARGET_DIVERGED": ["The operation target changed; Manual Review is required", "操作目標已變更，需要人工檢視", "操作目标已变更，需要人工检查"],
            "OPERATION_NOT_FOUND": ["Select the exact operation first", "請先選取正確操作", "请先选择正确操作"],
            "READ_LANE_BUSY": ["The bounded read lane is busy; Refresh again shortly", "受限讀取通道忙碌，請稍後重新整理", "受限读取通道忙碌，请稍后刷新"],
            "REQUEST_LIMIT_EXCEEDED": ["The request exceeds the safe size limit", "請求超過安全大小上限", "请求超过安全大小上限"],
            "OPERATION_REGISTRY_CORRUPT": ["Operation records need Manual Review before Import", "Import 前需要人工檢視操作紀錄", "Import 前需要人工检查操作记录"],
            "AUTHORIZATION_DENIED": ["The backend denied this operation", "後端拒絕此操作", "后端拒绝此操作"]
        }
        var v = text[code]
        return v ? i18n.text(v[0], v[1], v[2]) : code
    }

    function _request(schema, includeApply) {
        var sources = []
        for (var i = 0; i < sourceUrls.length; ++i)
            sources.push({ "source_url": String(sourceUrls[i]) })
        var request = {
            "schema": schema,
            "target_version": operationTarget,
            "destination": destination,
            "sources": sources
        }
        if (includeApply) {
            var selected = []
            for (var key in replaceLocal)
                if (replaceLocal[key] === true) selected.push(key)
            selected.sort()
            request.expected_plan = planHash
            request.replace_local = selected
        }
        return JSON.stringify(request)
    }

    function _start(tag, args, request) {
        if (!isvnCli || _inflight !== null) return false
        _inflight = { "tag": tag, "generation": _generation, "target": operationTarget }
        if (operationCoordinator) {
            var mutation = tag === "apply" || tag === "recovery"
            var id = mutation
                     ? operationCoordinator.beginMutation("import", operationTarget, args,
                                                          request || "", planHash)
                     : operationCoordinator.beginRead("import-" + tag, operationTarget, args,
                                                       request || "")
            _inflight.operationId = id
            // A fake/test bridge may reject synchronously.  Defer inspection
            // until the operation ID is installed in this immutable context.
            Qt.callLater(function() {
                if (!root._inflight || root._inflight.operationId !== id) return
                var record = root.operationCoordinator.operationFor(id)
                if (record && record.state !== "starting" && record.state !== "running" && record.result)
                    root._onCoordinatorResult(id, record.targetVersion, record.result)
            })
            return id.length > 0
        }
        if (isvnCli.running) { _inflight = null; return false }
        if (request === undefined || request === null)
            isvnCli.runJson(args)
        else
            isvnCli.runJsonWithStdin(args, request)
        return true
    }

    function _onCoordinatorResult(operationId, targetVersion, json) {
        var item = _inflight
        if (!item || item.operationId !== operationId || item.target !== targetVersion) return
        if (json && json.error) root._onFailure(json)
        else root._onSucceeded(json)
    }

    // Called by DropArea.  URLs are passed straight through to backend Preview;
    // QML performs no path canonicalization or filesystem inspection.
    function startDrop(targetDestination, urls) {
        // A hidden/pinned operation on A must not make B unusable.  The
        // coordinator retains A; this visible controller is free to construct
        // B's independent read-only Preview.  This controller's own unfinished
        // flow on the same target and the recovery guard remain strict.
        var blockedHere = mutationsBlocked &&
                          (!operationCoordinator || operationTarget === activeVersion)
        if (blockedHere || activeVersion.length === 0) {
            errorCode = activeVersion.length === 0 ? "NO_ACTIVE_VERSION" : "RECOVERY_REQUIRED"
            return false
        }
        // The visible controller may now prepare B while its prior A Apply is
        // still owned by the coordinator.  Detach only the presentation
        // callback; the A operation record and its later result remain intact
        // and target-addressable in Operation Center.
        if (operationCoordinator && _inflight && _inflight.target !== activeVersion)
            _inflight = null
        var values = []
        for (var i = 0; urls && i < urls.length; ++i) values.push(String(urls[i]))
        if (values.length === 0) { errorCode = "IMPORT_SOURCE_REQUIRED"; return false }
        _generation++
        destination = targetDestination || ""
        operationTarget = activeVersion
        sourceUrls = values
        previewData = null
        resultData = null
        planHash = ""
        replaceLocal = ({})
        batchIntegrationPending = false
        batchIntegrated = false
        _pendingBatchPaths = []
        _awaitingRefresh = false
        errorCode = ""
        phase = "previewing"
        return _start("preview", ["import", "preview", "--request-stdin"], _request("import-request/v1", false))
    }

    function setReplaceLocal(path, selected) {
        var next = {}
        for (var key in replaceLocal) next[key] = replaceLocal[key]
        if (selected) next[path] = true
        else delete next[path]
        replaceLocal = next
    }

    function continueToConfirm() {
        if (!canConfirm) return false
        phase = "confirm"
        return true
    }

    function confirmApply() {
        if (phase !== "confirm" || !canConfirm || planHash.length === 0) return false
        phase = "applying"
        return _start("apply", ["import", "apply", "--request-stdin"],
                      _request("import-apply-request/v1", true))
    }

    function reset() {
        _generation++
        destination = ""; sourceUrls = []; previewData = null; resultData = null
        operationTarget = ""
        planHash = ""; replaceLocal = ({}); errorCode = ""; phase = "idle"; _inflight = null
        batchIntegrationPending = false; batchIntegrated = false; _pendingBatchPaths = []; _awaitingRefresh = false
        recoveryOperationId = ""
    }

    function _stale(item) {
        return !item || item.generation !== _generation || item.target !== operationTarget
    }

    function _activity(status, count) {
        if (!sessionStore) return
        sessionStore.appendActivity({
            "operationId": resultData && resultData.operation_id ? resultData.operation_id : ("ui-" + Date.now()),
            "operationType": "import",
            "target": operationTarget + (destination.length > 0 ? "/" + destination : ""),
            "status": status,
            "summary": "import " + count + " files",
            "itemCount": String(count),
            "endTime": Qt.formatDateTime(new Date(), "yyyy-MM-dd HH:mm:ss"),
            "revision": null
        })
    }

    function _mergeBatchPaths() {
        if (!_awaitingRefresh && !_pendingBatchPaths.length) return
        var paths = _pendingBatchPaths
        _pendingBatchPaths = []
        _awaitingRefresh = false
        if (checkinController && checkinController.mergeImportedPaths(paths, operationTarget)) {
            batchIntegrated = true
            batchIntegrationPending = false
        } else {
            // Local Import has already completed.  Do not invent a Batch review
            // from stale data; retain only truthful "Refresh/manual Add" state.
            batchIntegrated = false
            batchIntegrationPending = true
        }
    }

    function restoreRecovery() {
        if (phase !== "recovery" || recoveryOperationId.length === 0) return false
        return _start("recovery", ["work-recovery", "restore", recoveryOperationId, "--json"], null)
    }

    // Import recovery never resumes an external source.  It opens only a
    // destination-only journal selected by its exact backend operation ID.
    function presentRecovery(operation) {
        if (!operation || !operation.operation_id ||
                String(operation.target_version || "") !== activeVersion) {
            errorCode = "OPERATION_NOT_FOUND"
            return false
        }
        operationTarget = String(operation.target_version)
        recoveryOperationId = String(operation.operation_id)
        var manual = operation.state && String(operation.state).toLowerCase().indexOf("manual") >= 0
        errorCode = manual ? "RECOVERY_EVIDENCE_MISMATCH" : "RECOVERY_REQUIRED"
        phase = manual ? "manualReview" : "recovery"
        return true
    }

    function _onSucceeded(json) {
        var inflight = _inflight
        _inflight = null
        if (_stale(inflight)) return
        if (inflight.tag === "preview") {
            previewData = json
            planHash = json && json.plan_hash ? String(json.plan_hash) : ""
            phase = "review"
            return
        }
        if (inflight.tag === "apply") {
            resultData = json
            if (json && json.status === "imported") {
                phase = "success"
                _activity("succeeded", Number(json.item_count || 0))
                _pendingBatchPaths = json.batch_paths ? json.batch_paths : []
                batchIntegrationPending = true
                batchIntegrated = false
                if (readController && readController.refresh) {
                    // Wait for an authoritative new status snapshot before
                    // adding exact returned paths to the ephemeral Batch.
                    _awaitingRefresh = true
                    readController.refresh()
                } else {
                    _mergeBatchPaths()
                }
            } else {
                phase = "failed"
                _activity("rolled_back", 0)
            }
        } else if (inflight.tag === "recovery") {
            // Restore is terminal local cleanup only.  The external source was
            // intentionally not kept, so the user must Drop/Preview again.
            recoveryOperationId = ""
            sourceUrls = []
            previewData = null
            planHash = ""
            phase = "idle"
        }
    }

    function _onFailure(json) {
        var inflight = _inflight
        _inflight = null
        if (_stale(inflight)) return
        var error = json && json.error ? json.error : {}
        errorCode = error.code ? String(error.code) : "PLAN_STALE"
        recoveryOperationId = json && json.operation_id ? String(json.operation_id) : ""
        if (errorCode === "RECOVERY_EVIDENCE_MISMATCH" || error.manual_review_required === true)
            phase = "manualReview"
        else if (error.recovery_required === true || errorCode === "RECOVERY_REQUIRED")
            phase = "recovery"
        else phase = "failed"
        // A failed preview must not leave a prior plan eligible for Confirm.
        if (inflight.tag === "preview") { previewData = null; planHash = "" }
    }

    property var _bridge: Connections {
        target: root.isvnCli ? root.isvnCli : null
        ignoreUnknownSignals: true
        function onCommandSucceeded(json) { root._onSucceeded(json) }
        function onCommandFailedJson(json) { root._onFailure(json) }
        function onCommandFailed(message) {
            if (root._inflight !== null && root._inflight.tag !== "") {
                // Structured errors arrive first; this only covers legacy/no-JSON failure.
                if (root._inflight !== null) {
                    root.errorCode = root.errorCode.length > 0 ? root.errorCode : "PLAN_STALE"
                    root._inflight = null
                    root.phase = "failed"
                }
            }
        }
    }

    property var _coordinatorBridge: Connections {
        target: root.operationCoordinator ? root.operationCoordinator : null
        ignoreUnknownSignals: true
        function onTargetResult(operationId, targetVersion, result) {
            root._onCoordinatorResult(operationId, targetVersion, result)
        }
    }

    property var _readBridge: Connections {
        target: root.readController ? root.readController : null
        ignoreUnknownSignals: true
        function onSnapshotReady() {
            if (root._awaitingRefresh) root._mergeBatchPaths()
        }
        function onStateChanged() {
            if (root._awaitingRefresh && root.readController && root.readController.state === "error") {
                root._awaitingRefresh = false
                root.batchIntegrated = false
                root.batchIntegrationPending = true
            }
        }
    }
}
