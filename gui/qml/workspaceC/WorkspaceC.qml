import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

Item {
    id: root

    property bool pmMode: false
    property var i18n: ({ text: function(en, zhTw, zhCn) { return zhTw } })
    property var theme: ({ bgBase: "#0E1116", bgPanel: "#161B22", bgElevated: "#1C232D", border: "#2A3441", textPrimary: "#E6EDF3", textSecondary: "#8B98A5", accent: "#3FB6C7", accentWarn: "#E0A030", accentDanger: "#E5534B", accentOk: "#3FB950", radius: 6, gapXs: 4, gapSm: 8, gapMd: 12, gapLg: 16, gapXl: 24, fontSection: 15, fontBody: 13, fontData: 12, fontCaption: 11, monoFont: "monospace" })
    signal statusMessage(string message)

    property real dedupRatio: 1.0
    property string usedBytes: "0"
    property string capacityBytes: "0"
    property string objectCount: "0"
    property string refCount: "0"
    property string physicalBytes: "0"
    property string logicalBytes: "0"
    property bool atimeUsable: false
    property bool swapCritical: false
    property string swapThreshold: ""
    property string verifyState: root.i18n.text("not checked", "尚未檢查", "尚未检查")
    property int verifyCode: -1
    property string verifyDetail: ""
    property string verifyExpectedHash: ""
    property string verifyActualHash: ""
    property string verifyActor: ""
    property string verifyTime: ""
    property var pendingGcArgs: []
    property string pendingGcDialogTitle: ""
    property string pendingGcDialogBody: ""
    property string pendingGcConfirmLabel: ""
    property int phantomCount: 0
    property bool previewReady: false
    property string renewedObjectCount: "0"
    property string renewedBytes: "0"
    property string adminLineageState: root.i18n.text("not loaded", "尚未載入", "尚未载入")
    property string adminScaleSummary: ""
    property string adminBackupSummary: ""
    property int refcountDistCount: refcountDistModel.count
    property int evictEventCount: evictEventModel.count
    property int densityCount: densityModel.count
    property int renewEventCount: renewEventModel.count
    property int adminPathCount: adminPathModel.count
    property int adminPermissionCount: adminPermissionModel.count
    property bool lastOperationReady: false
    property bool lastGcPreviewMode: false
    property string lastGcUnmatchedLinks: "0"
    property string lastGcRefsRemoved: "0"
    property string lastGcObjectsReclaimed: "0"
    property string lastGcObjectsForceEvicted: "0"
    property string lastGcBytesFreed: "0"
    property string lastGcLowWatermark: root.i18n.text("Unknown", "未知", "未知")
    property bool refreshAllInProgress: false
    property string lastOperationMode: ""
    property string lastOperationStatus: ""
    property string lastOperationTime: ""
    property string lastOperationSummary: ""

    ListModel { id: refcountDistModel }
    ListModel { id: evictEventModel }
    ListModel { id: densityModel }
    ListModel { id: phantomRefModel }
    ListModel { id: renewEventModel }
    ListModel { id: adminPathModel }
    ListModel { id: adminPermissionModel }

    Common.CommandRunner {
        id: poolStatRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.populatePoolStat(json)
            if (root.refreshAllInProgress)
                root.loadSwapHealth()
        }
        onFailed: function(message) {
            root.refreshAllInProgress = false
            root.statusMessage(message)
        }
    }

    Common.CommandRunner {
        id: swapRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.populatePoolStat(json)
            root.populateSwapHealth(json)
            if (root.refreshAllInProgress)
                root.loadAdminStatus()
        }
        onFailed: function(message) {
            root.refreshAllInProgress = false
            root.statusMessage(message)
        }
    }

    Common.CommandRunner {
        id: verifyRunner
        i18n: root.i18n
        onSucceeded: function(json) { root.populateVerify(json) }
        onFailed: function(message) { root.statusMessage(message) }
    }

    Common.CommandRunner {
        id: gcRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.populateGcResult(json)
            root.resetPreview()
            root.statusMessage(root.i18n.text("GC operation completed", "GC 操作完成", "GC 操作完成"))
            root.loadSwapHealth()
        }
        onFailed: function(message) { root.statusMessage(message) }
    }

    Common.CommandRunner {
        id: reconcilePreviewRunner
        i18n: root.i18n
        onSucceeded: function(json) { root.populatePhantomPreview(json) }
        onFailed: function(message) { root.statusMessage(message) }
    }

    Common.CommandRunner {
        id: adminStatusRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.populateAdminStatus(json)
            root.refreshAllInProgress = false
        }
        onFailed: function(message) {
            root.refreshAllInProgress = false
            root.statusMessage(message)
        }
    }

    Common.ConfirmDialog {
        id: gcConfirm
        objectName: "gcConfirmDialog"
        i18n: root.i18n
        theme: root.theme
        title: root.pendingGcDialogTitle
        body: root.pendingGcDialogBody
        confirmLabel: root.pendingGcConfirmLabel
        confirmAction: root.runPendingGc
    }

    function loadPoolStat() {
        poolStatRunner.runJson(["pool-stat"])
    }

    function loadAdminStatus() {
        adminStatusRunner.runJson(["admin", "status"])
    }

    function loadSwapHealth() {
        swapRunner.runJson(["pool-stat", "--evict-events", "--evict-threshold", String(thresholdSpin.value)])
    }

    function loadVerify() {
        verifyRunner.runJson(["lineage", "verify"])
    }

    function refreshAll() {
        if (!pmMode) {
            statusMessage(root.i18n.text("Maintenance tools are available to PM users only.",
                                         "維護工具僅 PM 可使用。",
                                         "维护工具仅 PM 可使用。"))
            return
        }
        refreshAllInProgress = true
        loadPoolStat()
    }

    function confirmGc(args) {
        if (!pmMode) {
            statusMessage(root.i18n.text("Maintenance tools are available to PM users only.",
                                         "維護工具僅 PM 可使用。",
                                         "维护工具仅 PM 可使用。"))
            return
        }
        pendingGcArgs = args
        var cmd = args.join(" ")
        if (cmd.indexOf("--reconcile") >= 0) {
            pendingGcDialogTitle = root.i18n.text("Confirm Cleanup Sweep", "確認清理掃描", "确认清理扫描")
            pendingGcDialogBody = root.i18n.text(
                "Remove unmatched cache entries identified in the preview. Review the preview result before continuing. Runs as the current GUI user.",
                "移除預覽發現的未匹配快取項目。請先確認預覽結果再繼續。將以目前 GUI 使用者身分執行。",
                "移除预览发现的未匹配缓存项目。请先确认预览结果再继续。将以当前 GUI 用户身份执行。")
            pendingGcConfirmLabel = root.i18n.text("Confirm Cleanup Sweep", "確認清理掃描", "确认清理扫描")
        } else if (cmd.indexOf("--reclaim") >= 0) {
            pendingGcDialogTitle = root.i18n.text("Confirm Cache Reclaim", "確認快取回收", "确认缓存回收")
            pendingGcDialogBody = root.i18n.text(
                "Reclaim cached objects to reduce storage below the configured high watermark. Objects at or below the low watermark will be retained. Runs as the current GUI user.",
                "回收快取物件以降低存量至設定的高水位以下。低水位以下的物件將保留。將以目前 GUI 使用者身分執行。",
                "回收缓存对象以将存量降低至设置的高水位以下。低水位以下的对象将保留。将以当前 GUI 用户身份执行。")
            pendingGcConfirmLabel = root.i18n.text("Confirm Cache Reclaim", "確認快取回收", "确认缓存回收")
        } else {
            pendingGcDialogTitle = root.i18n.text("Confirm Full GC", "確認完整 GC", "确认完整 GC")
            pendingGcDialogBody = root.i18n.text(
                "Run a full garbage collection sweep. This is the highest-impact maintenance action. Review storage status and preview results before continuing. Runs as the current GUI user.",
                "執行完整 GC 清理。這是影響最大的維護動作。繼續前請先確認存量狀態與預覽結果。將以目前 GUI 使用者身分執行。",
                "执行完整 GC 清理。这是影响最大的维护操作。继续前请先确认存量状态与预览结果。将以当前 GUI 用户身份执行。")
            pendingGcConfirmLabel = root.i18n.text("Confirm Full GC", "確認完整 GC", "确认完整 GC")
        }
        gcConfirm.open()
    }

    function showMaintenanceAuthDenied(actionLabel) {
        lastOperationReady = true
        lastOperationMode = root.i18n.text("blocked", "已阻擋", "已阻挡")
        lastOperationStatus = root.i18n.text("Not authorized", "未授權", "未授权")
        lastOperationTime = Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm")
        lastOperationSummary = root.i18n.text(
            "PM authorization is required for this maintenance action.",
            "此維護動作需要 PM 授權。",
            "此维护操作需要 PM 授权。")
        statusMessage(root.i18n.text(
            "Maintenance action blocked: PM authorization required.",
            "維護動作已阻擋：需要 PM 授權。",
            "维护操作已阻挡：需要 PM 授权。"))
    }

    function runPendingGc() {
        if (!pmMode) {
            showMaintenanceAuthDenied(root.i18n.text("GC maintenance", "GC 維護", "GC 维护"))
            return
        }
        gcRunner.runJson(pendingGcArgs)
    }

    function previewReconcile() {
        if (!pmMode) {
            statusMessage(root.i18n.text("Maintenance tools are available to PM users only.",
                                         "維護工具僅 PM 可使用。",
                                         "维护工具仅 PM 可使用。"))
            return
        }
        reconcilePreviewRunner.runJson(["gc", "--reconcile", "--dry-run"])
    }

    function populatePoolStat(json) {
        dedupRatio = Number(json.dedup_ratio || 1.0)
        usedBytes = String(json.used_bytes || 0)
        capacityBytes = String(json.capacity_bytes || 0)
        objectCount = String(json.object_count || 0)
        refCount = String(json.ref_count || 0)
        physicalBytes = String(json.physical_bytes || 0)
        logicalBytes = String(json.logical_ref_bytes || 0)
        renewedObjectCount = String(json.renewed_object_count || 0)
        renewedBytes = String(json.renewed_bytes || 0)
        atimeUsable = json.atime_usable === true
        refcountDistModel.clear()
        var dist = json.refcount_dist || {}
        var keys = Object.keys(dist).sort(function(a, b) { return Number(a) - Number(b) })
        for (var i = 0; i < keys.length; ++i)
            refcountDistModel.append({ refcount: keys[i], count: dist[keys[i]] })

        renewEventModel.clear()
        var renews = json.recent_renews || []
        for (var j = 0; j < renews.length; ++j) {
            renewEventModel.append({
                hash: renews[j].hash || "",
                actor: renews[j].actor || "",
                renewedAt: renews[j].renewed_at || "",
                refcount: renews[j].refcount || 0,
                size: renews[j].size || 0
            })
        }
        statusMessage(root.i18n.text("Cache pool refreshed", "快取池已刷新", "缓存池已刷新"))
    }

    function populateAdminStatus(json) {
        adminPathModel.clear()
        adminPermissionModel.clear()
        var paths = json.paths || []
        for (var i = 0; i < paths.length; ++i) {
            adminPathModel.append({
                name: paths[i].name || "",
                path: paths[i].path || "",
                exists: paths[i].exists === true,
                reachable: paths[i].reachable === true,
                availableBytes: String(paths[i].available_bytes || 0),
                mountRoot: paths[i].mount_root || ""
            })
        }
        var permissions = json.permissions || []
        for (var j = 0; j < permissions.length; ++j) {
            adminPermissionModel.append({
                name: permissions[j].name || "",
                path: permissions[j].path || "",
                mode: permissions[j].mode || "",
                expectedMode: permissions[j].expected_mode || "",
                group: permissions[j].group || "",
                matches: permissions[j].matches_expected === true
            })
        }
        var lineage = json.lineage || {}
        adminLineageState = lineage.intact === true ? root.i18n.text("Record chain intact", "紀錄鏈完整", "记录链完整") :
                            (lineage.intact === false ? root.i18n.text("Record chain needs review", "紀錄鏈需要確認", "记录链需要确认") :
                                                        root.i18n.text("Record chain unknown", "紀錄鏈狀態未知", "记录链状态未知"))
        var scale = json.scale || {}
        adminScaleSummary = "phase=" + (scale.phase_count || 0) +
                            " block=" + (scale.block_count || 0) +
                            " stage=" + (scale.stage_count || 0) +
                            " version=" + (scale.version_count || 0) +
                            " member=" + (scale.member_count || 0) +
                            " rev=" + (scale.revision_count || 0)
        var backup = json.backup || {}
        adminBackupSummary = backup.available === true ?
                             ((backup.latest || "").length > 0 ? backup.latest : root.i18n.text("Available", "可用", "可用")) :
                             root.i18n.text("No backup record found", "未找到備份紀錄", "未找到备份纪录")
        statusMessage(root.i18n.text("Admin overview loaded", "管理總覽已載入", "管理总览已载入"))
    }

    function populateSwapHealth(json) {
        evictEventModel.clear()
        densityModel.clear()
        var events = json.events || []
        for (var i = 0; i < events.length; ++i) {
            evictEventModel.append({
                hash: events[i].hash || "",
                evictedAt: events[i].evicted_at || ""
            })
        }
        var density = json.density || []
        for (var j = 0; j < density.length; ++j) {
            densityModel.append({
                window: density[j].window || "",
                count: density[j].count || 0
            })
        }
        swapThreshold = json.threshold === null || json.threshold === undefined ? root.i18n.text("Unset", "未設定", "未设置") : String(json.threshold)
        swapCritical = json.critical === true
        statusMessage(root.i18n.text("Capacity health loaded", "容量健康已載入", "容量健康已载入"))
    }

    function populateVerify(json) {
        verifyCode = json.intact === true ? 1 : 0
        verifyState = verifyCode === 1 ? root.i18n.text("Record chain intact", "紀錄鏈完整", "记录链完整") :
                                         root.i18n.text("Record chain needs review", "紀錄鏈需要確認", "记录链需要确认")
        var detail = json.detail || {}
        verifyExpectedHash = detail.expected_hash === null || detail.expected_hash === undefined ? "" : String(detail.expected_hash)
        verifyActualHash = detail.actual_hash === null || detail.actual_hash === undefined ? "" : String(detail.actual_hash)
        verifyActor = detail.actor === null || detail.actor === undefined ? "" : String(detail.actor)
        verifyTime = detail.ts === null || detail.ts === undefined ? "" : String(detail.ts)
        verifyDetail = detail.message || ""
        statusMessage(root.i18n.text("Integrity check loaded", "完整性檢查已載入", "完整性检查已载入"))
    }

    function populateGcResult(json) {
        var refs = json.phantom_refs || []
        lastOperationReady = true
        lastGcPreviewMode = json.dry_run === true
        lastGcUnmatchedLinks = String(refs.length)
        lastGcRefsRemoved = String(json.refs_removed || 0)
        lastGcObjectsReclaimed = String(json.objects_reclaimed || 0)
        lastGcObjectsForceEvicted = String(json.objects_force_evicted || 0)
        lastGcBytesFreed = String(json.bytes_freed || 0)
        lastGcLowWatermark = json.low_watermark_reached === true ? root.i18n.text("Yes", "是", "是") :
                             (json.low_watermark_reached === false ? root.i18n.text("No", "否", "否") :
                                                                      root.i18n.text("Unknown", "未知", "未知"))
        lastOperationMode = lastGcPreviewMode ?
            root.i18n.text("Preview (no data modified)", "預覽（未修改資料）", "预览（未修改数据）") :
            root.i18n.text("Execute (data may have changed)", "執行（資料可能已異動）", "执行（数据可能已变更）")
        lastOperationStatus = lastGcPreviewMode ?
            root.i18n.text("Preview complete", "預覽完成", "预览完成") :
            (Number(lastGcObjectsReclaimed) > 0 || Number(lastGcRefsRemoved) > 0 ?
                root.i18n.text("Objects reclaimed", "已回收物件", "已回收对象") :
                root.i18n.text("No action taken", "未執行動作", "未执行动作"))
        lastOperationTime = Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm")
        lastOperationSummary = lastGcUnmatchedLinks + root.i18n.text(" candidates, ", " 筆候選，", " 个候选，") +
                               root.formatBytes(root.lastGcBytesFreed) + root.i18n.text(" freed", " 已釋放", " 已释放")
    }

    function populatePhantomPreview(json) {
        phantomRefModel.clear()
        var refs = json.phantom_refs || []
        for (var i = 0; i < refs.length; ++i) {
            phantomRefModel.append({
                hash: refs[i].hash || "",
                linkPath: refs[i].link_path || ""
            })
        }
        phantomCount = refs.length
        previewReady = true
        populateGcResult(json)
        statusMessage(root.i18n.text("GC preview found ", "GC 預覽找到 ", "GC 预览找到 ") +
                      phantomCount +
                      root.i18n.text(" unmatched object links", " 筆未匹配物件連結", " 笔未匹配对象链接"))
    }

    function resetPreview() {
        phantomRefModel.clear()
        phantomCount = 0
        previewReady = false
    }

    function formatBytes(value) {
        var number = Number(value || 0)
        if (number >= 1099511627776)
            return (number / 1099511627776).toFixed(1) + " TiB"
        if (number >= 1073741824)
            return (number / 1073741824).toFixed(1) + " GiB"
        if (number >= 1048576)
            return (number / 1048576).toFixed(1) + " MiB"
        if (number >= 1024)
            return (number / 1024).toFixed(1) + " KiB"
        return String(number) + " B"
    }

    function percentString() {
        var capacity = Number(capacityBytes || 0)
        if (capacity <= 0)
            return root.i18n.text("Capacity limit not configured", "未設定容量上限", "未设置容量上限")
        return ((Number(usedBytes || 0) / capacity) * 100.0).toFixed(1) + "%"
    }

    function cachePoolRows() {
        return [
            { key: root.i18n.text("Dedup ratio", "去重率", "去重率"), value: root.dedupRatio.toFixed(2) },
            { key: root.i18n.text("Used capacity", "已用容量", "已用容量"), value: root.formatBytes(root.usedBytes) + " / " + root.formatBytes(root.capacityBytes) },
            { key: root.i18n.text("Capacity", "容量", "容量"), value: root.percentString() },
            { key: root.i18n.text("Objects", "物件", "对象"), value: root.objectCount },
            { key: root.i18n.text("Object links", "物件連結", "对象链接"), value: root.refCount },
            { key: root.i18n.text("Physical size", "實體大小", "实体大小"), value: root.formatBytes(root.physicalBytes) },
            { key: root.i18n.text("Logical linked size", "邏輯連結大小", "逻辑链接大小"), value: root.formatBytes(root.logicalBytes) }
        ]
    }

    function capacityRows() {
        return [
            { key: root.i18n.text("Status", "狀態", "状态"), value: root.swapCritical ? root.i18n.text("Critical", "需要注意", "需要注意") : root.i18n.text("Normal", "正常", "正常") },
            { key: root.i18n.text("Threshold", "門檻", "阈值"), value: root.swapThreshold.length > 0 ? root.swapThreshold : root.i18n.text("Unset", "未設定", "未设置") },
            { key: root.i18n.text("Events", "事件", "事件"), value: String(root.evictEventCount) }
        ]
    }

    function integrityRows() {
        return [
            { key: root.i18n.text("Record chain status", "紀錄鏈狀態", "记录链状态"), value: root.verifyState },
            { key: root.i18n.text("Expected hash", "預期 hash", "预期 hash"), value: root.verifyExpectedHash.length > 0 ? root.verifyExpectedHash : "—" },
            { key: root.i18n.text("Actual hash", "實際 hash", "实际 hash"), value: root.verifyActualHash.length > 0 ? root.verifyActualHash : "—" },
            { key: root.i18n.text("Actor", "人員", "人员"), value: root.verifyActor.length > 0 ? root.verifyActor : "—" },
            { key: root.i18n.text("Time", "時間", "时间"), value: root.verifyTime.length > 0 ? root.verifyTime : "—" },
            { key: root.i18n.text("Detail", "細節", "细节"), value: root.verifyDetail.length > 0 ? root.verifyDetail : "—" }
        ]
    }

    function lastOperationRows() {
        return [
            { key: root.i18n.text("Preview mode", "預覽模式", "预览模式"), value: root.lastGcPreviewMode ? root.i18n.text("Yes", "是", "是") : root.i18n.text("No", "否", "否") },
            { key: root.i18n.text("Unmatched object links", "未匹配物件連結", "未匹配对象链接"), value: root.lastGcUnmatchedLinks },
            { key: root.i18n.text("Links removed", "已移除連結", "已移除链接"), value: root.lastGcRefsRemoved },
            { key: root.i18n.text("Objects reclaimed", "已回收物件", "已回收对象"), value: root.lastGcObjectsReclaimed },
            { key: root.i18n.text("Objects force-evicted", "強制回收物件", "强制回收对象"), value: root.lastGcObjectsForceEvicted },
            { key: root.i18n.text("Bytes freed", "釋放容量", "释放容量"), value: root.formatBytes(root.lastGcBytesFreed) },
            { key: root.i18n.text("Low watermark reached", "已達低水位", "已达低水位"), value: root.lastGcLowWatermark }
        ]
    }

    ScrollView {
        id: cScroll
        anchors.fill: parent
        anchors.margins: root.theme.gapSm
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: cScroll.availableWidth
            spacing: root.theme.gapSm

            // ── Header: Maintenance Control Center ──────────────────────────
            Common.ShellPanel {
                theme: root.theme
                elevated: true
                Layout.fillWidth: true
                Layout.preferredHeight: 58

                RowLayout {
                    anchors.fill: parent
                    spacing: root.theme.gapMd

                    Label {
                        text: root.i18n.text("Maintenance Control Center", "維護控制中心", "维护控制中心")
                        color: root.theme.textPrimary
                        font.pixelSize: root.theme.fontSection
                        font.bold: true
                        Layout.preferredWidth: 160
                        elide: Text.ElideRight
                    }

                    Common.RoleBadge {
                        theme: root.theme
                        role: root.pmMode ? "PM" : "MEMBER"
                    }

                    Label {
                        text: root.i18n.text("PM operations for cache, capacity, integrity, and cleanup.",
                                             "PM 用於快取、容量、完整性與清理的維護操作。",
                                             "PM 用于缓存、容量、完整性与清理的维护操作。")
                        color: root.theme.textSecondary
                        font.pixelSize: root.theme.fontCaption
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Common.StatusDot {
                        theme: root.theme
                        state: root.pmMode ? "ok" : "danger"
                        label: root.pmMode ? root.i18n.text("PM-only", "僅 PM 可用", "仅 PM 可用") :
                                             root.i18n.text("PM required", "需要 PM", "需要 PM")
                        Layout.preferredWidth: 120
                    }

                    Common.ActionButton {
                        theme: root.theme
                        text: root.i18n.text("Refresh", "重新整理", "刷新")
                        kind: "secondary"
                        visible: root.pmMode
                        onClicked: root.refreshAll()
                    }

                    Common.ActionButton {
                        objectName: "lineageVerifyButton"
                        theme: root.theme
                        text: root.i18n.text("Integrity check", "完整性檢查", "完整性检查")
                        kind: "secondary"
                        visible: root.pmMode
                        onClicked: root.loadVerify()
                    }

                    Common.ActionButton {
                        objectName: "gcPreviewButton"
                        theme: root.theme
                        text: root.i18n.text("GC preview", "GC 預覽", "GC 预览")
                        kind: "primary"
                        visible: root.pmMode
                        onClicked: root.previewReconcile()
                    }
                }
            }

            // ── Maintenance Overview (PM only) ───────────────────────────────
            Common.ShellPanel {
                visible: root.pmMode
                theme: root.theme
                title: root.i18n.text("Maintenance Overview", "維護總覽", "维护总览")
                subtitle: root.i18n.text("Quick health summary. Refresh sections below for current data.",
                                         "快速健康摘要。重新整理下方各區塊以取得最新資料。",
                                         "快速健康摘要。刷新下方各区块以获取最新数据。")
                Layout.fillWidth: true
                Layout.preferredHeight: 78

                RowLayout {
                    anchors.fill: parent
                    spacing: root.theme.gapXl

                    Common.StatusDot {
                        theme: root.theme
                        state: Number(root.capacityBytes) > 0
                               ? (Number(root.usedBytes) / Number(root.capacityBytes) > 0.9 ? "warn" : "ok")
                               : "neutral"
                        label: root.i18n.text("Storage", "存量", "存量") + ": " +
                               (Number(root.capacityBytes) > 0 ? root.percentString() :
                                root.i18n.text("not loaded", "尚未載入", "尚未载入"))
                        Layout.preferredWidth: 200
                    }

                    Common.StatusDot {
                        theme: root.theme
                        state: root.swapCritical ? "danger" : (root.evictEventCount > 0 ? "warn" : "ok")
                        label: root.i18n.text("Capacity health", "容量健康", "容量健康") + ": " +
                               (root.swapCritical ? root.i18n.text("Critical", "需要注意", "需要注意") :
                                root.i18n.text("Normal", "正常", "正常"))
                        Layout.preferredWidth: 200
                    }

                    Common.StatusDot {
                        theme: root.theme
                        state: root.verifyCode === 1 ? "ok" : (root.verifyCode === 0 ? "warn" : "neutral")
                        label: root.i18n.text("Record chain", "紀錄鏈", "记录链") + ": " + root.verifyState
                        Layout.fillWidth: true
                    }

                    Common.StatusDot {
                        theme: root.theme
                        state: !root.lastOperationReady ? "neutral" : (root.lastGcPreviewMode ? "ok" : "warn")
                        label: root.i18n.text("Last operation", "上次操作", "上次操作") + ": " +
                               (root.lastOperationReady ? root.lastOperationMode :
                                root.i18n.text("None", "無", "无"))
                        Layout.preferredWidth: 260
                    }
                }
            }

            // ── MEMBER: no-access state ──────────────────────────────────────
            Common.ShellPanel {
                visible: !root.pmMode
                theme: root.theme
                elevated: true
                Layout.fillWidth: true
                Layout.preferredHeight: 220

                Common.EmptyState {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - root.theme.gapXl * 2, 620)
                    theme: root.theme
                    title: root.i18n.text("Maintenance tools are available to PM users only.",
                                          "維護工具僅 PM 可使用。",
                                          "维护工具仅 PM 可使用。")
                    message: root.i18n.text("Destructive maintenance actions are hidden for this role.",
                                            "此角色不顯示破壞性維護動作。",
                                            "此角色不显示破坏性维护动作。")
                }
            }

            // ── PM main content ──────────────────────────────────────────────
            RowLayout {
                visible: root.pmMode
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: root.theme.gapMd

                // Left column: diagnostics and storage panels
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 680
                    spacing: root.theme.gapMd

                    // ── Storage / Cache Review ──────────────────────────────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.theme.gapMd

                        Common.ShellPanel {
                            theme: root.theme
                            title: root.i18n.text("Cache Pool", "快取池", "缓存池")
                            subtitle: root.i18n.text("Cache facts only: capacity, object counts, and object links.",
                                                     "僅顯示快取容量、物件數與物件連結。",
                                                     "仅显示缓存容量、对象数与对象链接。")
                            Layout.fillWidth: true
                            Layout.preferredHeight: 288

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: root.theme.gapSm

                                RowLayout {
                                    Layout.fillWidth: true
                                    Common.StatusDot {
                                        theme: root.theme
                                        state: Number(root.capacityBytes) > 0 ? "ok" : "neutral"
                                        label: Number(root.capacityBytes) > 0 ? root.percentString() :
                                                                               root.i18n.text("Capacity limit not configured", "未設定容量上限", "未设置容量上限")
                                        Layout.fillWidth: true
                                    }
                                    Common.ActionButton {
                                        objectName: "loadPoolStatButton"
                                        theme: root.theme
                                        text: root.i18n.text("Refresh", "重新整理", "刷新")
                                        kind: "secondary"
                                        onClicked: root.loadPoolStat()
                                    }
                                }

                                Common.KeyValueGrid {
                                    theme: root.theme
                                    rows: root.cachePoolRows()
                                    keyWidth: 150
                                    Layout.fillWidth: true
                                }

                                Label {
                                    text: root.i18n.text("Link count distribution", "連結數分布", "链接数分布")
                                    color: root.theme.textPrimary
                                    font.pixelSize: root.theme.fontData
                                    font.bold: true
                                    Layout.fillWidth: true
                                }

                                Flow {
                                    Layout.fillWidth: true
                                    spacing: root.theme.gapSm
                                    Repeater {
                                        model: refcountDistModel
                                        Common.StatusDot {
                                            theme: root.theme
                                            state: "neutral"
                                            label: model.refcount + "× " + model.count
                                        }
                                    }
                                }
                            }
                        }

                        Common.ShellPanel {
                            theme: root.theme
                            title: root.i18n.text("Retention Activity", "保留活動", "保留活动")
                            subtitle: root.i18n.text("Recently retained objects, shown as hash-level facts.",
                                                     "最近保留的物件，只顯示 hash 層級事實。",
                                                     "最近保留的对象，只显示 hash 层级事实。")
                            Layout.fillWidth: true
                            Layout.preferredHeight: 288

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: root.theme.gapSm

                                RowLayout {
                                    Layout.fillWidth: true
                                    Common.StatusDot {
                                        theme: root.theme
                                        state: Number(root.renewedObjectCount) > 0 ? "active" : "neutral"
                                        label: root.i18n.text("Recently retained objects", "最近保留物件", "最近保留对象") +
                                               ": " + root.renewedObjectCount + " / " + root.formatBytes(root.renewedBytes)
                                        Layout.fillWidth: true
                                    }
                                    Common.ActionButton {
                                        objectName: "renewStatsButton"
                                        theme: root.theme
                                        text: root.i18n.text("Refresh", "重新整理", "刷新")
                                        kind: "secondary"
                                        onClicked: root.loadPoolStat()
                                    }
                                }

                                Common.EmptyState {
                                    visible: renewEventModel.count === 0
                                    theme: root.theme
                                    title: root.i18n.text("No recent retention activity.", "最近沒有保留活動。", "最近没有保留活动。")
                                    Layout.fillWidth: true
                                }

                                ListView {
                                    visible: renewEventModel.count > 0
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    model: renewEventModel
                                    spacing: root.theme.gapXs
                                    delegate: Rectangle {
                                        width: ListView.view.width
                                        height: 44
                                        radius: root.theme.radius
                                        color: root.theme.bgElevated
                                        border.color: root.theme.border

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: root.theme.gapSm
                                            anchors.rightMargin: root.theme.gapSm
                                            spacing: root.theme.gapSm

                                            Common.HashPill {
                                                theme: root.theme
                                                hash: model.hash
                                                prefixLength: 10
                                            }

                                            Label {
                                                text: model.actor
                                                color: root.theme.textPrimary
                                                font.pixelSize: root.theme.fontData
                                                elide: Text.ElideRight
                                                Layout.preferredWidth: 86
                                            }

                                            Label {
                                                text: root.i18n.text("Retained at", "保留時間", "保留时间") + ": " + model.renewedAt
                                                color: root.theme.textSecondary
                                                font.pixelSize: root.theme.fontData
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }

                                            Label {
                                                text: root.i18n.text("links=", "連結=", "链接=") + model.refcount + "  " + root.formatBytes(model.size)
                                                color: root.theme.textSecondary
                                                font.pixelSize: root.theme.fontData
                                                elide: Text.ElideRight
                                                Layout.preferredWidth: 112
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ── Health & Diagnostics: Capacity Health ───────────────
                    Common.ShellPanel {
                        theme: root.theme
                        title: root.i18n.text("Capacity Health", "容量健康", "容量健康")
                        subtitle: root.i18n.text("Eviction activity and event density without version or file relationships.",
                                                 "回收活動與事件密度，不顯示版本或檔案關係。",
                                                 "回收活动与事件密度，不显示版本或文件关系。")
                        Layout.fillWidth: true
                        Layout.preferredHeight: 260

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: root.theme.gapSm

                            RowLayout {
                                Layout.fillWidth: true

                                Common.SeverityBanner {
                                    theme: root.theme
                                    severity: root.swapCritical ? "danger" : "ok"
                                    title: root.swapCritical ? root.i18n.text("Critical", "需要注意", "需要注意") :
                                                               root.i18n.text("Normal", "正常", "正常")
                                    message: root.swapCritical ?
                                             root.i18n.text("Eviction density crossed the configured threshold.",
                                                            "回收事件密度超過設定門檻。",
                                                            "回收事件密度超过设置阈值。") :
                                             root.i18n.text("Eviction density is below the configured threshold.",
                                                            "回收事件密度低於設定門檻。",
                                                            "回收事件密度低于设置阈值。")
                                    compact: true
                                    Layout.fillWidth: true
                                }

                                Label {
                                    text: root.i18n.text("Threshold", "門檻", "阈值")
                                    color: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                }

                                SpinBox {
                                    id: thresholdSpin
                                    from: 0
                                    to: 100000
                                    value: 10
                                    Layout.preferredWidth: 96
                                }

                                Common.ActionButton {
                                    objectName: "loadSwapHealthButton"
                                    theme: root.theme
                                    text: root.i18n.text("Refresh", "重新整理", "刷新")
                                    kind: "secondary"
                                    onClicked: root.loadSwapHealth()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: root.theme.gapMd

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true

                                    Common.SectionHeader {
                                        theme: root.theme
                                        title: root.i18n.text("Event density", "事件密度", "事件密度")
                                        compact: true
                                    }

                                    Common.KeyValueGrid {
                                        theme: root.theme
                                        rows: root.capacityRows()
                                        keyWidth: 88
                                        Layout.fillWidth: true
                                    }

                                    Repeater {
                                        model: densityModel
                                        Common.StatusDot {
                                            theme: root.theme
                                            state: model.count > 0 ? "warn" : "neutral"
                                            label: model.window + ": " + model.count
                                            Layout.fillWidth: true
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true

                                    Common.SectionHeader {
                                        theme: root.theme
                                        title: root.i18n.text("Eviction Events", "回收事件", "回收事件")
                                        compact: true
                                    }

                                    Common.EmptyState {
                                        visible: evictEventModel.count === 0
                                        theme: root.theme
                                        title: root.i18n.text("No eviction events.", "沒有回收事件。", "没有回收事件。")
                                        Layout.fillWidth: true
                                    }

                                    ListView {
                                        visible: evictEventModel.count > 0
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        model: evictEventModel
                                        spacing: root.theme.gapXs
                                        delegate: RowLayout {
                                            width: ListView.view.width
                                            spacing: root.theme.gapSm

                                            Common.HashPill {
                                                theme: root.theme
                                                hash: model.hash
                                                prefixLength: 12
                                            }
                                            Label {
                                                text: root.i18n.text("Evicted at", "回收時間", "回收时间") + ": " + model.evictedAt
                                                color: root.theme.textSecondary
                                                font.pixelSize: root.theme.fontData
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ── Health & Diagnostics: Integrity + Admin ─────────────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.theme.gapMd

                        Common.ShellPanel {
                            theme: root.theme
                            title: root.i18n.text("Integrity Check", "完整性檢查", "完整性检查")
                            subtitle: root.i18n.text("Checks whether the recorded source chain is intact.",
                                                     "檢查版本來源紀錄鏈是否完整。",
                                                     "检查版本来源记录链是否完整。")
                            Layout.fillWidth: true
                            Layout.preferredHeight: 292

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: root.theme.gapSm

                                Common.SeverityBanner {
                                    theme: root.theme
                                    severity: root.verifyCode === 1 ? "ok" : (root.verifyCode === 0 ? "warn" : "info")
                                    title: root.verifyState
                                    message: root.i18n.text("This check reviews the recorded source chain, not the contents of every design file.",
                                                            "此檢查核對版本來源紀錄鏈，不代表驗證所有設計檔內容。",
                                                            "此检查核对版本来源记录链，不代表验证所有设计文件内容。")
                                    compact: true
                                    Layout.fillWidth: true
                                }

                                Common.KeyValueGrid {
                                    theme: root.theme
                                    rows: root.integrityRows()
                                    monoValue: true
                                    keyWidth: 130
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        Common.ShellPanel {
                            theme: root.theme
                            title: root.i18n.text("Admin Overview", "管理總覽", "管理总览")
                            subtitle: root.i18n.text("Read-only status for paths, permissions, scale, backup, and record chain.",
                                                     "唯讀顯示路徑、權限、規模、備份與紀錄鏈狀態。",
                                                     "只读显示路径、权限、规模、备份与记录链状态。")
                            Layout.fillWidth: true
                            Layout.preferredHeight: 292

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: root.theme.gapSm

                                RowLayout {
                                    Layout.fillWidth: true
                                    Common.StatusDot {
                                        theme: root.theme
                                        state: root.adminLineageState.indexOf(root.i18n.text("intact", "完整", "完整")) >= 0 ? "ok" : "neutral"
                                        label: root.adminLineageState
                                        Layout.fillWidth: true
                                    }
                                    Common.ActionButton {
                                        objectName: "adminStatusButton"
                                        theme: root.theme
                                        text: root.i18n.text("Refresh", "重新整理", "刷新")
                                        kind: "secondary"
                                        onClicked: root.loadAdminStatus()
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: root.theme.gapMd

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Common.SectionHeader {
                                            theme: root.theme
                                            title: root.i18n.text("Paths", "路徑", "路径")
                                            compact: true
                                        }
                                        ListView {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            clip: true
                                            model: adminPathModel
                                            delegate: Common.StatusDot {
                                                width: ListView.view.width
                                                theme: root.theme
                                                state: model.exists && model.reachable ? "ok" : "danger"
                                                label: model.name + "  " +
                                                       root.i18n.text("Available space", "可用空間", "可用空间") +
                                                       ": " + root.formatBytes(model.availableBytes)
                                            }
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Common.SectionHeader {
                                            theme: root.theme
                                            title: root.i18n.text("Permissions", "權限", "权限")
                                            compact: true
                                        }
                                        ListView {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            clip: true
                                            model: adminPermissionModel
                                            delegate: Common.StatusDot {
                                                width: ListView.view.width
                                                theme: root.theme
                                                state: model.matches ? "ok" : "warn"
                                                label: model.name + "  " +
                                                       root.i18n.text("Mode", "模式", "模式") + ": " + model.mode +
                                                       "  " + root.i18n.text("Expected mode", "預期模式", "预期模式") + ": " + model.expectedMode +
                                                       "  " + root.i18n.text("Group", "群組", "群组") + ": " + model.group
                                            }
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Common.SectionHeader {
                                            theme: root.theme
                                            title: root.i18n.text("Scale / Backup", "規模 / 備份", "规模 / 备份")
                                            compact: true
                                        }
                                        Label {
                                            text: root.adminScaleSummary.length > 0 ? root.adminScaleSummary : "—"
                                            color: root.theme.textSecondary
                                            font.pixelSize: root.theme.fontData
                                            font.family: root.theme.monoFont
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: root.i18n.text("Latest backup", "最新備份", "最新备份") + ": " + root.adminBackupSummary
                                            color: root.theme.textSecondary
                                            font.pixelSize: root.theme.fontData
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: root.i18n.text("Access rules are displayed as reported. This panel does not modify settings.",
                                                                 "存取規則依系統回報顯示；此面板不修改設定。",
                                                                 "访问规则依系统回报显示；此面板不修改设置。")
                                            color: root.theme.textSecondary
                                            font.pixelSize: root.theme.fontCaption
                                            wrapMode: Text.Wrap
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ── Right column: Operations ─────────────────────────────────
                Common.ShellPanel {
                    theme: root.theme
                    title: root.i18n.text("Operations", "維護操作", "维护操作")
                    subtitle: root.i18n.text("Risky maintenance actions ask for confirmation before they run.",
                                             "高風險維護動作會先要求確認。",
                                             "高风险维护动作会先要求确认。")
                    elevated: false
                    Layout.preferredWidth: 340
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: root.theme.gapMd

                        // GC / Cleanup Preview ─────────────────────────────
                        Common.InspectorSection {
                            theme: root.theme
                            title: root.i18n.text("Cleanup Preview", "清理預覽", "清理预览")
                            subtitle: root.i18n.text("Preview cleanup candidates without removing data.",
                                                     "只預覽清理候選項目，不移除資料。",
                                                     "只预览清理候选项目，不移除数据。")
                            collapsible: false
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.previewReady ? 284 : 196

                            Common.ActionButton {
                                objectName: "gcPreviewButton"
                                theme: root.theme
                                text: root.i18n.text("GC preview", "GC 預覽", "GC 预览")
                                kind: "primary"
                                Layout.fillWidth: true
                                onClicked: root.previewReconcile()
                            }

                            Common.SeverityBanner {
                                visible: root.previewReady
                                theme: root.theme
                                severity: root.phantomCount > 0 ? "warn" : "ok"
                                title: root.i18n.text("Preview result", "預覽結果", "预览结果")
                                message: root.i18n.text("Unmatched object links", "未匹配物件連結", "未匹配对象链接") + ": " + root.phantomCount
                                compact: true
                                Layout.fillWidth: true
                            }

                            ListView {
                                visible: root.previewReady && root.phantomCount > 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: 70
                                clip: true
                                model: phantomRefModel
                                delegate: Label {
                                    width: ListView.view.width
                                    text: model.hash.slice(0, 12) + "...  " + model.linkPath
                                    color: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                    font.family: root.theme.monoFont
                                    elide: Text.ElideMiddle
                                }
                            }

                            Common.ActionButton {
                                objectName: "gcReconcileButton"
                                visible: root.previewReady
                                theme: root.theme
                                text: root.i18n.text("Confirm clear", "確認清除", "确认清除")
                                kind: "danger"
                                dangerous: true
                                Layout.fillWidth: true
                                onClicked: root.confirmGc(["gc", "--reconcile"])
                            }
                        }

                        // ─── Danger Zone ─────────────────────────────────────
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: root.theme.accentDanger
                            opacity: 0.6
                        }

                        Label {
                            text: root.i18n.text("⚠  Danger Zone", "⚠  危險區域", "⚠  危险区域")
                            color: root.theme.accentDanger
                            font.pixelSize: root.theme.fontBody
                            font.bold: true
                            Layout.fillWidth: true
                        }

                        Label {
                            text: root.i18n.text(
                                "These actions may permanently modify or remove cached data. Review Cleanup Preview results before proceeding.",
                                "這些動作可能會永久修改或移除快取資料。請先確認清理預覽結果再繼續。",
                                "这些操作可能会永久修改或移除缓存数据。请先确认清理预览结果再继续。")
                            color: root.theme.textSecondary
                            font.pixelSize: root.theme.fontCaption
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        // Cache Reclaim ────────────────────────────────────
                        Common.InspectorSection {
                            theme: root.theme
                            title: root.i18n.text("Cache Reclaim", "快取回收", "缓存回收")
                            subtitle: root.i18n.text("Reclaim objects according to configured watermarks.",
                                                     "依設定的水位回收物件。",
                                                     "按设置的水位回收对象。")
                            collapsible: false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 156

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: root.i18n.text("high %", "高水位 %", "高水位 %")
                                    color: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                }
                                SpinBox {
                                    id: highSpin
                                    from: 0
                                    to: 100
                                    value: 90
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: root.i18n.text("low %", "低水位 %", "低水位 %")
                                    color: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                }
                                SpinBox {
                                    id: lowSpin
                                    from: 0
                                    to: 100
                                    value: 70
                                    Layout.fillWidth: true
                                }
                            }

                            Common.ActionButton {
                                objectName: "gcReclaimButton"
                                theme: root.theme
                                text: root.i18n.text("Confirm Cache Reclaim", "確認快取回收", "确认缓存回收")
                                kind: "danger"
                                dangerous: true
                                Layout.fillWidth: true
                                onClicked: root.confirmGc([
                                    "gc", "--reclaim", "--high", String(highSpin.value / 100.0),
                                    "--low", String(lowSpin.value / 100.0)
                                ])
                            }
                        }

                        // Full GC ──────────────────────────────────────────
                        Common.InspectorSection {
                            theme: root.theme
                            title: root.i18n.text("Full GC", "完整 GC", "完整 GC")
                            subtitle: root.i18n.text("High-risk cleanup. Review preview result before running.",
                                                     "高風險清理。執行前請先確認預覽結果。",
                                                     "高风险清理。执行前请先确认预览结果。")
                            collapsible: false
                            Layout.fillWidth: true
                            Layout.preferredHeight: 118

                            Common.ActionButton {
                                objectName: "gcFullButton"
                                theme: root.theme
                                text: root.i18n.text("Confirm Full GC", "確認完整 GC", "确认完整 GC")
                                kind: "danger"
                                dangerous: true
                                Layout.fillWidth: true
                                onClicked: root.confirmGc(["gc"])
                            }
                        }

                        // Operation Evidence ───────────────────────────────
                        Common.InspectorSection {
                            theme: root.theme
                            title: root.i18n.text("Operation Evidence", "操作憑據", "操作凭据")
                            subtitle: root.i18n.text("Persistent result of the last maintenance operation.",
                                                     "上次維護操作的持久結果記錄。",
                                                     "上次维护操作的持久结果记录。")
                            collapsible: false
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Common.EmptyState {
                                visible: !root.lastOperationReady
                                theme: root.theme
                                title: root.i18n.text("No maintenance result yet.", "尚無維護結果。", "暂无维护结果。")
                                message: root.i18n.text("Run GC preview before cleanup actions.",
                                                        "執行清理前，請先跑 GC 預覽。",
                                                        "执行清理前，请先运行 GC 预览。")
                                Layout.fillWidth: true
                            }

                            ColumnLayout {
                                visible: root.lastOperationReady
                                Layout.fillWidth: true
                                spacing: root.theme.gapXs

                                Common.SeverityBanner {
                                    theme: root.theme
                                    severity: root.lastGcPreviewMode ? "info" : "warn"
                                    title: root.lastOperationStatus
                                    message: root.lastOperationSummary
                                    compact: true
                                    Layout.fillWidth: true
                                }

                                Common.KeyValueGrid {
                                    theme: root.theme
                                    rows: [
                                        { key: root.i18n.text("Mode", "模式", "模式"), value: root.lastOperationMode },
                                        { key: root.i18n.text("Status", "狀態", "状态"), value: root.lastOperationStatus },
                                        { key: root.i18n.text("Time", "時間", "时间"), value: root.lastOperationTime },
                                        { key: root.i18n.text("Actor", "角色", "角色"), value: root.i18n.text("PM", "PM", "PM") }
                                    ]
                                    keyWidth: 60
                                    Layout.fillWidth: true
                                }

                                Common.KeyValueGrid {
                                    theme: root.theme
                                    rows: root.lastOperationRows()
                                    keyWidth: 148
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
