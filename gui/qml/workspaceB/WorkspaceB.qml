import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

Item {
    id: root
    objectName: "workspaceB"
    signal statusMessage(string message)

    // versionFilter = version the user explicitly chose here; shellVersion = version
    // already selected in the app shell (Workspace A). selectedVersionPath() prefers the
    // explicit choice and falls back to the shell version so users need not retype it.
    property string versionFilter: ""
    property string shellVersion: ""
    property string projectPath: ""
    property string loadedVersion: ""
    // Version each evidence request was issued for; a response for any other
    // version arrives after the selection changed and is discarded.
    property string svnLogRequestVersion: ""
    property string auditRequestVersion: ""
    property string reconcileRequestVersion: ""
    // Failure of the last evidence refresh for the selected version; shown in
    // the evidence area so a failed load is never read as "no evidence".
    property string evidenceError: ""
    property string reconcileChild: ""
    property string reconcileLineageParent: ""
    property string reconcileSvnHint: ""
// Source Check user-facing state model. reconcileConsistent holds the raw
    // backend "consistent" value (true/false/null); sourceCheckPhase tracks not_run/ran/error.
    property var reconcileConsistent: undefined
    property string sourceCheckPhase: "not_run"
    property int selectedProvenanceIndex: -1
    property string selectedGraphVersion: ""
    property bool refreshAllInProgress: false
    property int svnLogCount: svnLogModel.count
    property int lineageAuditCount: lineageAuditModel.count
    property int lineagePanoramaCount: lineagePanoramaModel.count
    property string lineageMode: "version"
    property string versionQueryType: "ancestor-chain"
    property string fileQueryType: "versions"
    property string versionQueryTarget: ""
    property string fileQueryPath: ""
    property string fileVersionContext: ""
    property int lineageMaxDepth: 20
    property string lineageAuthority: lineageMode === "version" ? "declared_provenance" : "declared_inventory"
    property bool lineageContentVerified: false
    property string lineageStatus: "empty"
    property string lineageStatusMessage: ""
    property string lineageSummary: ""
    property string lineageJsonSummary: ""
    property bool lineageTruncated: false
    property int lineageWarningCount: lineageWarningModel.count
    property int lineageResultCount: lineageGraphNodeModel.count
    property int selectedLineageNodeIndex: -1
    property string selectedLineageCanonical: selectedLineageNodeIndex >= 0 && selectedLineageNodeIndex < lineageGraphNodeModel.count ? lineageGraphNodeModel.get(selectedLineageNodeIndex).canonical : ""
    property int multiCandidateCount: 0
    property var i18n: ({ text: function(en, zhTw, zhCn) { return zhTw } })
    property var theme: ({ bgBase: "#0E1116", bgPanel: "#161B22", bgElevated: "#1C232D", border: "#2A3441", textPrimary: "#E6EDF3", textSecondary: "#8B98A5", accent: "#3FB6C7", accentWarn: "#E0A030", accentDanger: "#E5534B", accentOk: "#3FB950", radius: 6, gapXs: 4, gapSm: 8, gapMd: 12, gapLg: 16, fontSection: 15, fontBody: 13, fontData: 12, fontCaption: 11, monoFont: "monospace" })

    ListModel { id: svnLogModel }
    ListModel { id: lineageAuditModel }
    ListModel { id: lineagePanoramaModel }
    ListModel { id: versionPickModel }
    ListModel { id: lineageGraphNodeModel }
    ListModel { id: lineageGraphEdgeModel }
    ListModel { id: lineageWarningModel }

    property var versionQueryOptions: [
        { label: "Parent", value: "parent" },
        { label: "Ancestor Chain", value: "ancestor-chain" },
        { label: "Root", value: "root" },
        { label: "Pedigree", value: "pedigree" }
    ]
    property var fileQueryOptions: [
        { label: "Versions", value: "versions" },
        { label: "Parent Versions", value: "parent" },
        { label: "Root Versions", value: "root" }
    ]

    // Hidden field used only to place read-only text on the clipboard.
    TextEdit { id: evidenceClipboard; visible: false; readOnly: true }

    Common.CommandRunner {
        id: versionPickRunner
        i18n: root.i18n
        onSucceeded: function(json) { root.populateVersionPicker(json) }
        onFailed: function(message) { root.statusMessage(message) }
    }

    Common.CommandRunner {
        id: lineageQueryRunner
        i18n: root.i18n
        onSucceeded: function(json) { root.populateLineageQuery(json) }
        onFailed: function(message) {
            root.populateLineageUnavailable(message)
        }
    }

    Common.MetadataPopover {
        id: graphMetadataPopover
        i18n: root.i18n
        theme: root.theme
        showGetUpstream: false
    }

    Dialog {
        id: versionPickerDialog
        objectName: "versionPickerDialog"
        modal: true
        title: root.i18n.text("Choose version", "選擇版本", "选择版本")
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, root.width - 80)
        height: Math.min(460, root.height - 80)
        standardButtons: Dialog.Close
        // The list uses theme text colours, so the dialog surface must use the
        // theme too. Button text keeps the style default to stay readable on
        // the style's own button face.
        palette.window: root.theme.bgPanel
        palette.windowText: root.theme.textPrimary

        contentItem: ColumnLayout {
            spacing: root.theme.gapSm

            Label {
                text: root.i18n.text("Pick a version to review its evidence.",
                                     "選一個版本來檢視證據。", "选一个版本来查看证据。")
                color: root.theme.textSecondary
                font.pixelSize: root.theme.fontData
                Layout.fillWidth: true
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: versionPickModel
                spacing: 2
                delegate: ItemDelegate {
                    id: versionPickRow
                    width: ListView.view.width
                    highlighted: model.path === root.versionFilter
                    background: Rectangle {
                        color: versionPickRow.highlighted ? root.theme.workSelected
                             : (versionPickRow.hovered || versionPickRow.down) ? root.theme.workSurfaceHover
                             : "transparent"
                        border.width: versionPickRow.visualFocus ? 2 : 0
                        border.color: root.theme.workAccent
                        radius: 3
                    }
                    contentItem: Label {
                        text: model.path
                        color: root.theme.textPrimary
                        font.family: root.theme.monoFont
                        font.pixelSize: root.theme.fontData
                        elide: Text.ElideMiddle
                    }
                    onClicked: {
                        root.versionFilter = model.path
                        versionPickerDialog.close()
                    }
                }
            }

            Common.EmptyState {
                visible: versionPickModel.count === 0
                Layout.fillWidth: true
                theme: root.theme
                title: root.i18n.text("No versions found", "找不到版本", "找不到版本")
                message: root.i18n.text("Open this from a workspace that has versions.",
                                        "請從有版本的工作區開啟。", "请从有版本的工作区打开。")
            }
        }
    }

    Common.CommandRunner {
        id: svnLogRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            if (root.svnLogRequestVersion !== root.selectedVersionPath()) {
                root.refreshAllInProgress = false
                return
            }
            root.populateSvnLog(json)
            if (root.refreshAllInProgress)
                root.loadLineageAudit()
        }
        onFailed: function(message) {
            root.refreshAllInProgress = false
            if (root.svnLogRequestVersion === root.selectedVersionPath()) {
                root.evidenceError = message
                root.statusMessage(message)
            }
        }
    }

    Common.CommandRunner {
        id: lineageAuditRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            if (root.auditRequestVersion !== root.selectedVersionPath()) {
                root.refreshAllInProgress = false
                return
            }
            root.populateLineageAudit(json)
            if (root.refreshAllInProgress)
                root.runReconcileReport()
        }
        onFailed: function(message) {
            root.refreshAllInProgress = false
            if (root.auditRequestVersion === root.selectedVersionPath()) {
                root.evidenceError = message
                root.statusMessage(message)
            }
        }
    }

    Common.CommandRunner {
        id: reconcileRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.refreshAllInProgress = false
            if (root.reconcileRequestVersion !== root.selectedVersionPath())
                return
            root.populateReconcile(json)
        }
        onFailed: function(message) {
            // Do not surface raw backend stderr (may contain internal terms); show a
            // conservative, sanitized message instead.
            root.refreshAllInProgress = false
            if (root.reconcileRequestVersion !== root.selectedVersionPath())
                return
            root.sourceCheckPhase = "error"
            root.statusMessage(root.sourceCheckMessage())
        }
    }

    Common.ConfirmDialog {
        id: reconcileDecisionDialog
        i18n: root.i18n
        theme: root.theme
        title: root.i18n.text("You decide the correct source", "請你判斷正確來源", "请你判断正确来源")
        body: root.i18n.text("This check only reports possible mismatch. You decide what the correct source should be.",
                             "這裡只報告可能不一致；正確來源由你判斷。",
                             "这里只报告可能不一致；正确来源由你判断。")
        onConfirmed: root.statusMessage(root.i18n.text("No source record was changed in the GUI",
                                                       "GUI 未變更版本來源紀錄",
                                                       "GUI 未变更版本来源记录"))
    }

    function hasVersion() {
        return selectedVersionPath().length > 0
    }

    function selectedVersionPath() {
        return versionFilter.length > 0 ? versionFilter.trim() : shellVersion.trim()
    }

    function filterArgs(prefix) {
        var args = prefix.slice()
        var version = selectedVersionPath()
        if (version.length > 0)
            args.push(version)
        return args
    }

    function effectiveVersionTarget() {
        var typed = versionQueryTarget.trim()
        return typed.length > 0 ? typed : selectedVersionPath()
    }

    function effectiveFileVersionContext() {
        var typed = fileVersionContext.trim()
        return typed.length > 0 ? typed : selectedVersionPath()
    }

    function authorityLabel() {
        return lineageMode === "version"
               ? root.i18n.text("Declared provenance", "宣告來源", "声明来源")
               : root.i18n.text("Declared inventory", "宣告清單", "声明清单")
    }

    function contentBadgeLabel() {
        return root.i18n.text("Not content-verified", "未驗證內容", "未验证内容")
    }

    function queryTypeLabel(value) {
        var source = lineageMode === "version" ? versionQueryOptions : fileQueryOptions
        for (var i = 0; i < source.length; ++i) {
            if (source[i].value === value)
                return source[i].label
        }
        return value
    }

    function clearLineageQuery() {
        lineageGraphNodeModel.clear()
        lineageGraphEdgeModel.clear()
        lineageWarningModel.clear()
        selectedLineageNodeIndex = -1
        lineageStatus = "empty"
        lineageStatusMessage = root.i18n.text("Run a lineage query to populate the graph.",
                                              "執行查詢後會顯示關係圖。",
                                              "执行查询后会显示关系图。")
        lineageSummary = ""
        lineageJsonSummary = ""
        lineageTruncated = false
        multiCandidateCount = 0
    }

    function runLineageQuery() {
        if (lineageMode === "version") {
            var target = effectiveVersionTarget()
            if (target.length === 0) {
                lineageStatus = "warning"
                lineageStatusMessage = root.i18n.text("Select or enter a version first.",
                                                      "請先選擇或輸入版本。",
                                                      "请先选择或输入版本。")
                return
            }
            var args = ["provenance", "query", "--project", projectPath, "--version", target,
                        "--type", versionQueryType]
            if (versionQueryType === "ancestor-chain" || versionQueryType === "pedigree") {
                args.push("--max-depth")
                args.push(String(lineageMaxDepth))
            }
            lineageStatus = "loading"
            lineageQueryRunner.runJson(args)
            return
        }

        var path = fileQueryPath.trim()
        if (path.length === 0) {
            lineageStatus = "warning"
            lineageStatusMessage = root.i18n.text("Enter a file path first.",
                                                  "請先輸入檔案路徑。",
                                                  "请先输入文件路径。")
            return
        }
        var fileArgs = ["file", "query", "--project", projectPath, "--path", path,
                        "--type", fileQueryType]
        var versionContext = effectiveFileVersionContext()
        if (fileQueryType !== "versions" && versionContext.length > 0) {
            fileArgs.splice(4, 0, "--version", versionContext)
        }
        lineageStatus = "loading"
        lineageQueryRunner.runJson(fileArgs)
    }

    function populateLineageUnavailable(message) {
        lineageGraphNodeModel.clear()
        lineageGraphEdgeModel.clear()
        lineageWarningModel.clear()
        selectedLineageNodeIndex = -1
        lineageStatus = "not_available"
        lineageStatusMessage = root.i18n.text("Lineage query is not available in the installed command.",
                                              "目前安裝的指令尚未支援血緣查詢。",
                                              "目前安装的命令尚未支持血缘查询。")
        if (String(message || "").length > 0)
            lineageWarningModel.append({ code: "QUERY_NOT_AVAILABLE", message: lineageStatusMessage })
        lineageSummary = lineageStatusMessage
        lineageJsonSummary = ""
        lineageTruncated = false
        multiCandidateCount = 0
    }

    function versionDisplay(versionObject) {
        if (!versionObject)
            return ""
        if (versionObject.version && versionObject.stage)
            return versionObject.stage + "/" + versionObject.version
        if (versionObject.version)
            return versionObject.version
        if (versionObject.canonical)
            return String(versionObject.canonical).split("/").pop()
        return ""
    }

    function appendVersionNode(versionObject, relation, depth, emphasized) {
        if (!versionObject)
            return -1
        var canonical = versionObject.canonical || ""
        var versionLabel = versionDisplay(versionObject)
        var queryTarget = versionObject.version === "ROOT"
                          ? "ROOT"
                          : (versionObject.phase && versionObject.block && versionObject.stage && versionObject.version
                             ? [versionObject.phase, versionObject.block, versionObject.stage, versionObject.version].join("/")
                             : versionLabel)
        var existing = findGraphNode(canonical, versionLabel)
        if (existing >= 0)
            return existing
        lineageGraphNodeModel.append({
            nodeType: "version",
            canonical: canonical,
            label: versionLabel,
            queryTarget: queryTarget,
            phase: versionObject.phase || "",
            block: versionObject.block || "",
            stage: versionObject.stage || "",
            version: versionObject.version || versionLabel,
            relation: relation || versionObject.relation || "",
            depth: depth !== undefined && depth !== null ? String(depth) :
                   (versionObject.depth !== undefined ? String(versionObject.depth) :
                    (versionObject.depth_from_subject !== undefined ? String(versionObject.depth_from_subject) : "")),
            authority: lineageAuthority,
            contentVerified: "false",
            filePath: "",
            managed: "",
            resultCount: "",
            emphasized: emphasized === true
        })
        return lineageGraphNodeModel.count - 1
    }

    function appendFileNode(path, managed, count) {
        lineageGraphNodeModel.append({
            nodeType: "file",
            canonical: "",
            label: path,
            queryTarget: path,
            phase: "",
            block: "",
            stage: "",
            version: "",
            relation: "file",
            depth: "",
            authority: lineageAuthority,
            contentVerified: "false",
            filePath: path,
            managed: managed ? "true" : "false",
            resultCount: String(count),
            emphasized: true
        })
        return lineageGraphNodeModel.count - 1
    }

    function findGraphNode(canonical, label) {
        for (var i = 0; i < lineageGraphNodeModel.count; ++i) {
            var n = lineageGraphNodeModel.get(i)
            if (canonical.length > 0 && n.canonical === canonical)
                return i
            if (canonical.length === 0 && n.label === label)
                return i
        }
        return -1
    }

    function appendGraphEdge(fromIndex, toIndex, label) {
        if (fromIndex < 0 || toIndex < 0)
            return
        lineageGraphEdgeModel.append({
            fromIndex: fromIndex,
            toIndex: toIndex,
            label: label || ""
        })
    }

    function populateLineageQuery(json) {
        lineageGraphNodeModel.clear()
        lineageGraphEdgeModel.clear()
        lineageWarningModel.clear()
        selectedLineageNodeIndex = -1
        lineageAuthority = json.authority || (lineageMode === "version" ? "declared_provenance" : "declared_inventory")
        lineageContentVerified = json.content_verified === true
        lineageTruncated = json.truncated === true
        var warnings = json.warnings || []
        for (var i = 0; i < warnings.length; ++i) {
            lineageWarningModel.append({
                code: warnings[i].code || "",
                message: warnings[i].message || ""
            })
        }
        if (json.status === "error" || json.error) {
            lineageStatus = "error"
            lineageStatusMessage = (json.error && json.error.message) ? json.error.message :
                                   root.i18n.text("Query failed.", "查詢失敗。", "查询失败。")
            lineageSummary = lineageStatusMessage
            lineageJsonSummary = JSON.stringify(json, null, 2)
            return
        }
        if (lineageMode === "version")
            populateVersionLineage(json)
        else
            populateFileLineage(json)
        lineageJsonSummary = JSON.stringify(json, null, 2)
        if (lineageGraphNodeModel.count > 0)
            selectedLineageNodeIndex = 0
    }

    function populateVersionLineage(json) {
        var subject = json.subject || {}
        var chain = []
        if (json.pedigree && json.pedigree.chain)
            chain = json.pedigree.chain
        else if (json.ancestor_chain)
            chain = json.ancestor_chain.slice().reverse()
        else if (json.parent)
            chain = [json.parent]
        else if (json.root)
            chain = [json.root]
        var previous = -1
        for (var i = 0; i < chain.length; ++i) {
            var item = chain[i]
            var depth = item.depth !== undefined ? item.depth : item.depth_from_subject
            var idx = appendVersionNode(item, item.relation || "", depth, false)
            if (previous >= 0)
                appendGraphEdge(previous, idx, "declared")
            previous = idx
        }
        var subjectIdx = appendVersionNode(subject, "subject", 0, true)
        if (previous >= 0 && previous !== subjectIdx)
            appendGraphEdge(previous, subjectIdx, "subject")
        if (lineageGraphNodeModel.count === 1 && (!json.parent && !(json.ancestor_chain && json.ancestor_chain.length)))
            lineageStatusMessage = root.i18n.text("No declared lineage found.",
                                                  "找不到宣告血緣。",
                                                  "找不到声明血缘。")
        else
            lineageStatusMessage = root.i18n.text("Version lineage loaded.",
                                                  "版本血緣已載入。",
                                                  "版本血缘已载入。")
        lineageStatus = "ok"
        var labels = []
        for (var n = 0; n < lineageGraphNodeModel.count; ++n)
            labels.push(lineageGraphNodeModel.get(n).label)
        lineageSummary = "Subject: " + versionDisplay(subject) + "\n"
                + "Chain: " + labels.join(" -> ") + "\n"
                + "Authority: " + lineageAuthority + "\n"
                + "Content verified: false\n"
                + "Warnings: " + lineageWarningModel.count + "\n"
                + "Truncated: " + (lineageTruncated ? "true" : "false")
        multiCandidateCount = 0
    }

    function populateFileLineage(json) {
        var file = json.file || {}
        var path = file.path || fileQueryPath.trim()
        var managed = file.managed === true
        var versions = json.versions || []
        var subjects = json.subjects || []
        var parentVersions = json.parent_versions || []
        var rootVersions = json.root_versions || []
        var candidates = fileQueryType === "parent" ? parentVersions :
                         (fileQueryType === "root" ? rootVersions : versions)
        var fileIdx = appendFileNode(path, managed, candidates.length)
        if (fileQueryType === "versions") {
            for (var i = 0; i < versions.length; ++i) {
                var vIdx = appendVersionNode(versions[i], versions[i].role || "present_in",
                                             versions[i].depth, false)
                appendGraphEdge(fileIdx, vIdx, "present in")
            }
        } else {
            for (var s = 0; s < subjects.length; ++s) {
                var sIdx = appendVersionNode(subjects[s], "subject", subjects[s].depth, true)
                appendGraphEdge(fileIdx, sIdx, "file subject")
            }
            var relation = fileQueryType === "parent" ? "file_parent" : "file_root"
            for (var c = 0; c < candidates.length; ++c) {
                var cIdx = appendVersionNode(candidates[c], candidates[c].relation || relation,
                                             candidates[c].depth, false)
                appendGraphEdge(cIdx, fileIdx, relation)
            }
        }
        multiCandidateCount = candidates.length
        if (!managed) {
            lineageStatus = "warning"
            lineageStatusMessage = root.i18n.text("This file path is not managed by isvn.",
                                                  "此檔案路徑不在 isvn 管理內。",
                                                  "此文件路径不在 isvn 管理内。")
        } else if (candidates.length === 0) {
            lineageStatus = "warning"
            lineageStatusMessage = root.i18n.text("No matching versions found for this file.",
                                                  "找不到符合此檔案的版本。",
                                                  "找不到符合此文件的版本。")
        } else if (candidates.length > 1) {
            lineageStatus = "ok"
            lineageStatusMessage = root.i18n.text("This query returned multiple candidate versions.",
                                                  "此查詢回傳多個候選版本。",
                                                  "此查询返回多个候选版本。")
        } else {
            lineageStatus = "ok"
            lineageStatusMessage = root.i18n.text("File lineage loaded.",
                                                  "檔案血緣已載入。",
                                                  "文件血缘已载入。")
        }
        var names = []
        for (var n = 0; n < candidates.length; ++n)
            names.push(versionDisplay(candidates[n]))
        lineageSummary = "File: " + path + "\n"
                + "Result count: " + candidates.length + "\n"
                + "Candidates: " + (names.length > 0 ? names.join(", ") : "none") + "\n"
                + "Managed: " + (managed ? "true" : "false") + "\n"
                + "Authority: " + lineageAuthority + "\n"
                + "Content verified: false\n"
                + "Warnings: " + lineageWarningModel.count
    }

    function loadSvnLog() {
        svnLogRequestVersion = selectedVersionPath()
        svnLogRunner.runJson(filterArgs(["log"]))
    }

    function loadLineageAudit() {
        auditRequestVersion = selectedVersionPath()
        lineageAuditRunner.runJson(filterArgs(["lineage", "audit"]))
    }

    function runReconcileReport() {
        reconcileRequestVersion = selectedVersionPath()
        reconcileRunner.runJson(filterArgs(["reconcile"]))
    }

    // Single read-only evidence refresh: SVN commits -> provenance -> source check.
    function refreshAllReview() {
        if (!hasVersion()) {
            root.statusMessage(root.i18n.text("Select a version first", "請先選擇版本", "请先选择版本"))
            return
        }
        // One refresh at a time: the CLI bridge runs a single command, so a second
        // request would fail and cut the running refresh short.
        if (refreshAllInProgress)
            return
        evidenceError = ""
        loadedVersion = selectedVersionPath()
        refreshAllInProgress = true
        loadSvnLog()
    }

    // Evidence is loaded only by an explicit refresh.  When the selected version
    // changes, evidence shown for the previous version is cleared so it is never
    // presented as belonging to the new selection.
    function clearReviewEvidence() {
        loadedVersion = ""
        refreshAllInProgress = false
        evidenceError = ""
        svnLogModel.clear()
        lineageAuditModel.clear()
        lineagePanoramaModel.clear()
        selectedProvenanceIndex = -1
        selectedGraphVersion = ""
    }

    function reviewTargetChanged() {
        resetSourceCheck()
        if (loadedVersion !== selectedVersionPath())
            clearReviewEvidence()
    }

    function populateSvnLog(json) {
        svnLogModel.clear()
        var rows = json.entries || []
        for (var i = 0; i < rows.length; ++i) {
            svnLogModel.append({
                rev: rows[i].rev || rows[i].revision || "",
                author: rows[i].author || "",
                time: rows[i].time || "",
                message: rows[i].message || ""
            })
        }
        root.statusMessage(root.i18n.text("Commit history loaded", "Commit 歷史已載入", "Commit 历史已载入"))
    }

    function populateLineageAudit(json) {
        lineageAuditModel.clear()
        lineagePanoramaModel.clear()
        selectedProvenanceIndex = -1
        selectedGraphVersion = ""
        var rows = json.records || []
        for (var i = 0; i < rows.length; ++i) {
            lineageAuditModel.append({
                seq: rows[i].seq || 0,
                ts: rows[i].ts || "",
                actor: rows[i].actor || "",
                action: rows[i].action || "",
                child: rows[i].child || "",
                oldParent: rows[i].old_parent || "",
                newParent: rows[i].new_parent || "",
                reason: rows[i].reason || ""
            })
            root.addPanoramaNode(rows[i])
        }
        root.statusMessage(root.i18n.text("Provenance log loaded", "版本來源紀錄已載入", "版本来源记录已载入"))
    }

    function addPanoramaNode(row) {
        var child = row.child || ""
        if (child.length === 0)
            return
        for (var i = 0; i < lineagePanoramaModel.count; ++i) {
            if (lineagePanoramaModel.get(i).child === child) {
                lineagePanoramaModel.set(i, {
                    child: child,
                    parent: row.new_parent || "",
                    actor: row.actor || "",
                    ts: row.ts || "",
                    action: row.action || ""
                })
                return
            }
        }
        lineagePanoramaModel.append({
            child: child,
            parent: row.new_parent || "",
            actor: row.actor || "",
            ts: row.ts || "",
            action: row.action || ""
        })
    }

    function populateReconcile(json) {
        reconcileChild = json.child || ""
        reconcileLineageParent = json.lineage_parent || ""
        reconcileSvnHint = json.svn_copyfrom_hint || ""
        reconcileConsistent = json.consistent
        sourceCheckPhase = "ran"
        // Note: backend verify.detail is intentionally NOT surfaced; the Details
        // expander uses a QML-side humanized message keyed off the normalized state.
        root.statusMessage(root.i18n.text("Source check loaded", "來源核對已載入", "来源核对已载入"))
    }

    function openVersionPicker() {
        versionPickRunner.runJson(["branches"])
        versionPickerDialog.open()
    }

    function populateVersionPicker(json) {
        versionPickModel.clear()
        var rows = json.branches || []
        for (var i = 0; i < rows.length; ++i) {
            var path = rows[i].path
                       || [rows[i].phase, rows[i].block, rows[i].stage, rows[i].version].join("/")
            versionPickModel.append({ path: path })
        }
    }

    function versionStage(path) {
        var parts = String(path || "").split("/")
        return parts.length > 2 ? parts[2] : ""
    }

    function versionLeaf(path) {
        var parts = String(path || "").split("/")
        return parts.length > 0 ? parts[parts.length - 1] : ""
    }

    function sourceValue(value) {
        return value && value.length > 0 ? value : "ROOT"
    }

    // Clear any prior Source Check result so a newly-selected version shows NOT RUN.
    function resetSourceCheck() {
        sourceCheckPhase = "not_run"
        reconcileConsistent = undefined
        reconcileChild = ""
        reconcileLineageParent = ""
        reconcileSvnHint = ""
    }

    // Humanized, whitelist-only message for the Details expander. Never includes raw
    // backend detail or stderr.
    function sourceCheckMessage() {
        switch (normalizedSourceCheckState()) {
        case "pass": return root.i18n.text("Source evidence is consistent.",
                                           "來源證據一致。", "来源证据一致。")
        case "warning": return root.i18n.text("Source evidence needs review.",
                                              "來源證據需要檢視。", "来源证据需要查看。")
        case "review_needed": return root.i18n.text("Source evidence is incomplete or unavailable.",
                                                    "來源證據不完整或無法取得。", "来源证据不完整或无法获取。")
        case "error": return root.i18n.text("Source Check could not complete. Retry or review the log if the problem persists.",
                                            "來源核對未能完成。請重試;若問題持續，請檢視記錄。",
                                            "来源核对未能完成。请重试;若问题持续，请查看记录。")
        default: return ""
        }
    }

    // Normalize backend state into a user-facing state: pass | warning | review_needed
    // | not_run | error. Uses the read-only "consistent" field (true/false/null).
    // consistent=null means the backend could not perform the copy-from comparison.
    // When no SVN copy-from hint is available there is nothing to compare — report
    // NOT RUN with a clear reason rather than the ambiguous REVIEW NEEDED.
    function normalizedSourceCheckState() {
        if (sourceCheckPhase === "error")
            return "error"
        if (sourceCheckPhase !== "ran")
            return "not_run"
        if (reconcileConsistent === true)
            return "pass"
        if (reconcileConsistent === false)
            return "warning"
        // consistent === null: copy-from comparison was not performed.
        // Only use review_needed when a hint exists (something to compare against).
        if (reconcileSvnHint.length > 0)
            return "review_needed"
        return "not_run"
    }

    // Explains why the source check reports NOT RUN after having actually run.
    // Returns an empty string when the check was never run or when a result is available.
    function sourceCheckNotRunReason() {
        if (sourceCheckPhase !== "ran")
            return ""
        // Use loose inequality: null and undefined both indicate "no comparison result".
        // reconcileConsistent is null (explicit JSON null) or undefined (key absent).
        if (reconcileConsistent != null || reconcileSvnHint.length > 0)
            return ""
        if (reconcileLineageParent.length === 0)
            return root.i18n.text("Source comparison applies to derived versions only.",
                                  "來源核對僅適用於派生版本。",
                                  "来源核对仅适用于派生版本。")
        return root.i18n.text("No source reference recorded for this version.",
                              "此版本未記錄來源關係。",
                              "此版本未记录来源关系。")
    }

    function sourceCheckDotState() {
        var s = normalizedSourceCheckState()
        if (s === "pass") return "ok"
        if (s === "warning") return "warn"
        if (s === "error") return "danger"
        return "neutral"
    }

    function sourceCheckStateLabel() {
        switch (normalizedSourceCheckState()) {
        case "pass": return root.i18n.text("PASS", "PASS", "PASS")
        case "warning": return root.i18n.text("WARNING", "WARNING", "WARNING")
        case "review_needed": return root.i18n.text("REVIEW NEEDED", "REVIEW NEEDED", "REVIEW NEEDED")
        case "error": return root.i18n.text("ERROR", "ERROR", "ERROR")
        default: return root.i18n.text("NOT RUN", "NOT RUN", "NOT RUN")
        }
    }

    function sourceCheckMeaning() {
        switch (normalizedSourceCheckState()) {
        case "pass": return root.i18n.text("No source inconsistency was found for this version.",
                                           "未發現此版本的來源不一致。", "未发现此版本的来源不一致。")
        case "warning": return root.i18n.text("A possible source relationship needs review.",
                                              "有可能的來源關係需要檢視。", "有可能的来源关系需要查看。")
        case "review_needed": return root.i18n.text("Source evidence is incomplete or unavailable.",
                                                    "來源證據不完整或無法取得。", "来源证据不完整或无法获取。")
        case "error": return root.i18n.text("Source Check could not complete.",
                                            "來源核對未能完成。", "来源核对未能完成。")
        default: {
            var reason = root.sourceCheckNotRunReason()
            return reason.length > 0 ? reason :
                   root.i18n.text("Run Source Check to review source evidence for this version.",
                                  "執行來源核對以檢視此版本的來源證據。", "执行来源核对以查看此版本的来源证据。")
        }
        }
    }

    function sourceCheckRecommendation() {
        switch (normalizedSourceCheckState()) {
        case "pass": return root.i18n.text("You may use this evidence together with the SVN Commit History and Provenance Log.",
                                           "可搭配 SVN Commit 歷史與版本來源紀錄一起使用此證據。",
                                           "可搭配 SVN Commit 历史与版本来源记录一起使用此证据。")
        case "warning": return root.i18n.text("Review the Source hint and Provenance Log before relying on this version.",
                                              "在依賴此版本前，請檢視來源提示與版本來源紀錄。",
                                              "在依赖此版本前，请查看来源提示与版本来源记录。")
        case "review_needed": return root.i18n.text("Use SVN Commit History and Provenance Log to decide whether the source relationship is acceptable.",
                                                    "請用 SVN Commit 歷史與版本來源紀錄判斷來源關係是否可接受。",
                                                    "请用 SVN Commit 历史与版本来源记录判断来源关系是否可接受。")
        case "error": return root.i18n.text("Retry Source Check, or inspect the details if the problem persists.",
                                            "請重試來源核對；若問題持續，請檢視細節。",
                                            "请重试来源核对；若问题持续，请查看细节。")
        default: return root.i18n.text("Run Source Check after selecting a version.",
                                       "選擇版本後執行來源核對。", "选择版本后执行来源核对。")
        }
    }

    // Sanitized detail rows for the expander — no raw JSON / internal terms.
    function sourceCheckDetailRows() {
        var rows = [
            { key: root.i18n.text("Selected version", "目前版本", "当前版本"),
              value: reconcileChild.length > 0 ? reconcileChild : selectedVersionPath() },
            { key: root.i18n.text("Source hint", "來源提示", "来源提示"),
              value: reconcileSvnHint.length > 0 ? reconcileSvnHint
                     : root.i18n.text("unavailable", "無法取得", "无法获取") },
            { key: root.i18n.text("Source version", "來源版本", "来源版本"),
              value: sourceValue(reconcileLineageParent) },
            { key: root.i18n.text("Result", "結果", "结果"), value: sourceCheckStateLabel() }
        ]
        if (sourceCheckPhase !== "not_run" && sourceCheckMessage().length > 0)
            rows.push({ key: root.i18n.text("Message", "訊息", "消息"), value: sourceCheckMessage() })
        var notRunReason = root.sourceCheckNotRunReason()
        if (notRunReason.length > 0)
            rows.push({ key: root.i18n.text("Reason", "原因", "原因"), value: notRunReason })
        return rows
    }

    function selectedVersionRows() {
        var path = selectedVersionPath()
        return [
            { key: root.i18n.text("Path", "路徑", "路径"),
              value: path.length > 0 ? path : root.i18n.text("No version selected", "尚未選擇版本", "尚未选择版本") },
            { key: root.i18n.text("Stage", "階段", "阶段"), value: versionStage(path) },
            { key: root.i18n.text("Version", "Version", "Version"), value: versionLeaf(path) }
        ]
    }

    function provenanceSummaryRows() {
        if (lineageAuditModel.count === 0) {
            return [
                { key: root.i18n.text("Record count", "紀錄數", "记录数"), value: "0" },
                { key: root.i18n.text("Latest action", "最近動作", "最近动作"), value: root.i18n.text("None", "無", "无") }
            ]
        }
        var row = lineageAuditModel.get(lineageAuditModel.count - 1)
        return [
            { key: root.i18n.text("Record count", "紀錄數", "记录数"), value: String(lineageAuditModel.count) },
            { key: root.i18n.text("Latest action", "最近動作", "最近动作"), value: row.action },
            { key: root.i18n.text("Latest actor", "最近人員", "最近人员"), value: row.actor },
            { key: root.i18n.text("Latest time", "最近時間", "最近时间"), value: row.ts }
        ]
    }

    function graphNodeClicked(index) {
        if (index < 0 || index >= lineagePanoramaModel.count)
            return
        var row = lineagePanoramaModel.get(index)
        selectedGraphVersion = row.child
        graphMetadataPopover.versionPath = row.child
        graphMetadataPopover.parentPath = row.parent
        graphMetadataPopover.actor = row.actor
        graphMetadataPopover.timestamp = row.ts
        graphMetadataPopover.note = row.action
        graphMetadataPopover.status = root.i18n.text("Metadata only", "只顯示資訊", "只显示信息")
        graphMetadataPopover.open()
    }

    // ── Evidence copy (read-only, plain text) ────────────────────────────────
    function copyText(value) {
        evidenceClipboard.text = String(value || "")
        evidenceClipboard.selectAll()
        evidenceClipboard.copy()
        evidenceClipboard.deselect()
    }

    function commitSummaryText() {
        var lines = [root.i18n.text("SVN Commit History:", "SVN Commit 歷史：", "SVN Commit 历史：")]
        if (svnLogModel.count === 0)
            lines.push(root.i18n.text("- none loaded", "- 尚未載入", "- 尚未载入"))
        for (var i = 0; i < svnLogModel.count; ++i) {
            var r = svnLogModel.get(i)
            lines.push("- r" + r.rev + " | " + r.author + " | " + r.time + " | " + r.message)
        }
        return lines.join("\n")
    }

    function provenanceSummaryText() {
        var lines = [root.i18n.text("Provenance Log:", "版本來源紀錄：", "版本来源记录：")]
        if (lineageAuditModel.count === 0)
            lines.push(root.i18n.text("- none loaded", "- 尚未載入", "- 尚未载入"))
        for (var i = 0; i < lineageAuditModel.count; ++i) {
            var a = lineageAuditModel.get(i)
            lines.push("- #" + a.seq + " | " + a.action + " | " + a.actor + " | " + a.ts
                       + " | " + a.child + " | " + sourceValue(a.oldParent) + " -> " + sourceValue(a.newParent))
        }
        return lines.join("\n")
    }

    function sourceCheckSummaryText() {
        var hint = reconcileSvnHint.length > 0 ? reconcileSvnHint
                   : root.i18n.text("unavailable", "無法取得", "无法获取")
        return root.i18n.text("Source Check Summary", "來源核對摘要", "来源核对摘要") + "\n"
               + root.i18n.text("Version: ", "版本：", "版本：") + selectedVersionPath() + "\n"
               + root.i18n.text("Status: ", "狀態：", "状态：") + sourceCheckStateLabel() + "\n"
               + root.i18n.text("Meaning: ", "說明：", "说明：") + sourceCheckMeaning() + "\n"
               + root.i18n.text("Source hint: ", "來源提示：", "来源提示：") + hint + "\n"
               + root.i18n.text("Recommendation: ", "建議：", "建议：") + sourceCheckRecommendation() + "\n"
               + root.i18n.text("Review-only: No version data was changed.",
                                "僅供檢視:未變更任何版本資料。",
                                "仅供查看:未变更任何版本数据。")
    }

    function evidenceSummaryText() {
        return root.i18n.text("Version: ", "版本：", "版本：") + selectedVersionPath() + "\n\n"
               + commitSummaryText() + "\n\n"
               + provenanceSummaryText() + "\n\n"
               + sourceCheckSummaryText()
    }

    function copyAndReport(text, label) {
        copyText(text)
        root.statusMessage(root.i18n.text("Copied ", "已複製：", "已复制：") + label)
    }

    onVersionFilterChanged: reviewTargetChanged()
    onShellVersionChanged: reviewTargetChanged()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.theme.gapSm
        spacing: root.theme.gapSm

        // ── Header / toolbar ────────────────────────────────────────────────
        Common.ShellPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            theme: root.theme
            elevated: true

            RowLayout {
                anchors.fill: parent
                spacing: root.theme.gapMd

                Label {
                    text: root.i18n.text("Lineage & Evidence Explorer", "血緣與證據探索", "血缘与证据探索")
                    color: root.theme.textPrimary
                    font.pixelSize: root.theme.fontSection
                    font.bold: true
                    Layout.preferredWidth: 220
                    elide: Text.ElideRight
                }

                Common.VersionPathPill {
                    Layout.fillWidth: true
                    theme: root.theme
                    path: root.selectedVersionPath().length > 0
                          ? root.selectedVersionPath()
                          : root.i18n.text("No version selected", "尚未選擇版本", "尚未选择版本")
                    selected: root.selectedVersionPath().length > 0
                    showStageDot: root.selectedVersionPath().length > 0
                }

                Common.ActionButton {
                    objectName: "chooseVersionButton"
                    theme: root.theme
                    text: root.i18n.text("Choose version", "選擇版本", "选择版本")
                    kind: "secondary"
                    onClicked: root.openVersionPicker()
                }

                Common.ActionButton {
                    objectName: "refreshEvidenceButton"
                    theme: root.theme
                    text: root.i18n.text("Refresh evidence", "重新整理證據", "刷新证据")
                    kind: "primary"
                    enabled: root.hasVersion() && !root.refreshAllInProgress
                    onClicked: root.refreshAllReview()
                }
            }
        }

        // ── Lineage Explorer: declared provenance / inventory query surface ──
        Common.ShellPanel {
            objectName: "lineageExplorerPanel"
            Layout.fillWidth: true
            Layout.preferredHeight: 430
            theme: root.theme
            title: root.i18n.text("Workspace B — Lineage & Evidence Explorer",
                                  "Workspace B — 血緣與證據探索",
                                  "Workspace B — 血缘与证据探索")
            subtitle: root.i18n.text("Read-only declared provenance and declared inventory. Terminux checksum evidence remains separate.",
                                     "唯讀宣告來源與宣告清單；Terminux checksum 證據是另一個內容事實來源。",
                                     "只读声明来源与声明清单；Terminux checksum 证据是另一个内容事实来源。")

            ColumnLayout {
                anchors.fill: parent
                spacing: root.theme.gapSm

                RowLayout {
                    objectName: "lineageContextBar"
                    Layout.fillWidth: true
                    spacing: root.theme.gapSm

                    ComboBox {
                        objectName: "lineageModeSwitch"
                        Layout.preferredWidth: 170
                        model: [root.i18n.text("Version Lineage", "版本血緣", "版本血缘"),
                                root.i18n.text("File Lineage", "檔案血緣", "文件血缘")]
                        currentIndex: root.lineageMode === "version" ? 0 : 1
                        onActivated: function(index) {
                            root.lineageMode = index === 0 ? "version" : "file"
                            root.clearLineageQuery()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.i18n.text("Project: ", "Project: ", "Project: ") +
                              (root.projectPath.length > 0 ? root.projectPath : root.i18n.text("current workspace", "目前工作區", "当前工作区"))
                        color: root.theme.textSecondary
                        font.family: root.theme.monoFont
                        font.pixelSize: root.theme.fontData
                        elide: Text.ElideMiddle
                    }

                    Rectangle {
                        objectName: "declaredProvenanceBadge"
                        visible: root.lineageMode === "version"
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: 148
                        radius: root.theme.radius
                        color: root.theme.bgElevated
                        border.color: root.theme.accent
                        Label {
                            anchors.centerIn: parent
                            text: root.i18n.text("Declared provenance", "宣告來源", "声明来源")
                            color: root.theme.textPrimary
                            font.pixelSize: root.theme.fontCaption
                        }
                    }

                    Rectangle {
                        objectName: "declaredInventoryBadge"
                        visible: root.lineageMode === "file"
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: 132
                        radius: root.theme.radius
                        color: root.theme.bgElevated
                        border.color: root.theme.accent
                        Label {
                            anchors.centerIn: parent
                            text: root.i18n.text("Declared inventory", "宣告清單", "声明清单")
                            color: root.theme.textPrimary
                            font.pixelSize: root.theme.fontCaption
                        }
                    }

                    Rectangle {
                        objectName: "notContentVerifiedBadge"
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: 140
                        radius: root.theme.radius
                        color: root.theme.bgElevated
                        border.color: root.theme.accentWarn
                        Label {
                            anchors.centerIn: parent
                            text: root.contentBadgeLabel()
                            color: root.theme.textPrimary
                            font.pixelSize: root.theme.fontCaption
                        }
                    }

                    Common.ActionButton {
                        theme: root.theme
                        text: root.i18n.text("Use current", "使用目前版本", "使用当前版本")
                        kind: "secondary"
                        enabled: root.selectedVersionPath().length > 0
                        onClicked: {
                            root.versionQueryTarget = root.selectedVersionPath()
                            root.fileVersionContext = root.selectedVersionPath()
                        }
                    }
                    Common.ActionButton {
                        objectName: "copyLineageSummaryButton"
                        theme: root.theme
                        text: root.i18n.text("Copy summary", "複製摘要", "复制摘要")
                        kind: "secondary"
                        enabled: root.lineageSummary.length > 0
                        onClicked: root.copyAndReport(root.lineageSummary,
                                                      root.i18n.text("lineage summary", "血緣摘要", "血缘摘要"))
                    }
                    Common.ActionButton {
                        theme: root.theme
                        text: root.i18n.text("Clear", "清除", "清除")
                        kind: "ghost"
                        onClicked: root.clearLineageQuery()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: root.theme.gapSm

                    Common.ShellPanel {
                        objectName: "lineageQueryPanel"
                        Layout.preferredWidth: 250
                        Layout.fillHeight: true
                        theme: root.theme
                        title: root.i18n.text("Query Panel", "查詢面板", "查询面板")

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: root.theme.gapSm

                            Label {
                                Layout.fillWidth: true
                                text: root.lineageMode === "version"
                                      ? root.i18n.text("Version target", "目標版本", "目标版本")
                                      : root.i18n.text("File path", "檔案路徑", "文件路径")
                                color: root.theme.textSecondary
                                font.pixelSize: root.theme.fontCaption
                            }

                            TextField {
                                objectName: "versionLineageTargetInput"
                                visible: root.lineageMode === "version"
                                Layout.fillWidth: true
                                text: root.versionQueryTarget
                                placeholderText: root.selectedVersionPath().length > 0 ? root.selectedVersionPath() : "P1/TOP/SYN/v3"
                                onTextChanged: root.versionQueryTarget = text
                            }

                            TextField {
                                objectName: "fileLineagePathInput"
                                visible: root.lineageMode === "file"
                                Layout.fillWidth: true
                                text: root.fileQueryPath
                                placeholderText: "src/top.v"
                                onTextChanged: root.fileQueryPath = text
                            }

                            TextField {
                                objectName: "fileVersionContextInput"
                                visible: root.lineageMode === "file"
                                Layout.fillWidth: true
                                text: root.fileVersionContext
                                placeholderText: root.i18n.text("Optional version context", "可選版本 context", "可选版本 context")
                                onTextChanged: root.fileVersionContext = text
                            }

                            Label {
                                Layout.fillWidth: true
                                text: root.i18n.text("Query type", "查詢類型", "查询类型")
                                color: root.theme.textSecondary
                                font.pixelSize: root.theme.fontCaption
                            }

                            ComboBox {
                                objectName: "versionLineageQueryTypeCombo"
                                visible: root.lineageMode === "version"
                                Layout.fillWidth: true
                                model: root.versionQueryOptions
                                textRole: "label"
                                valueRole: "value"
                                onActivated: root.versionQueryType = currentValue
                                Component.onCompleted: currentIndex = 1
                            }

                            ComboBox {
                                objectName: "fileLineageQueryTypeCombo"
                                visible: root.lineageMode === "file"
                                Layout.fillWidth: true
                                model: root.fileQueryOptions
                                textRole: "label"
                                valueRole: "value"
                                onActivated: root.fileQueryType = currentValue
                            }

                            SpinBox {
                                objectName: "lineageMaxDepthInput"
                                visible: root.lineageMode === "version" &&
                                         (root.versionQueryType === "ancestor-chain" || root.versionQueryType === "pedigree")
                                Layout.fillWidth: true
                                from: 1
                                to: 100
                                value: root.lineageMaxDepth
                                onValueModified: root.lineageMaxDepth = value
                            }

                            Common.ActionButton {
                                objectName: "runLineageQueryButton"
                                Layout.fillWidth: true
                                theme: root.theme
                                text: root.lineageMode === "version"
                                      ? root.i18n.text("Run Lineage Query", "執行版本血緣查詢", "执行版本血缘查询")
                                      : root.i18n.text("Run File Query", "執行檔案血緣查詢", "执行文件血缘查询")
                                kind: "primary"
                                onClicked: root.runLineageQuery()
                            }

                            Common.SeverityBanner {
                                objectName: "lineageMissingCommandState"
                                visible: root.lineageStatus === "not_available"
                                Layout.fillWidth: true
                                theme: root.theme
                                severity: "warn"
                                compact: true
                                title: root.i18n.text("Not available", "尚未支援", "尚未支持")
                                message: root.lineageStatusMessage
                            }
                        }
                    }

                    Common.ShellPanel {
                        objectName: "lineageGraphCanvas"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        theme: root.theme
                        title: root.i18n.text("Lineage Graph Canvas", "血緣圖", "血缘图")
                        subtitle: root.lineageMode === "version"
                                  ? root.i18n.text("Declared version relationships, root to subject.", "宣告版本關係，由 root 指向 subject。", "声明版本关系，由 root 指向 subject。")
                                  : root.i18n.text("File-centered candidate graph; multiple candidate versions are preserved.", "以檔案為中心的候選圖；保留多個候選版本。", "以文件为中心的候选图；保留多个候选版本。")

                        Item {
                            anchors.fill: parent

                            Common.EmptyState {
                                visible: lineageGraphNodeModel.count === 0
                                anchors.centerIn: parent
                                width: Math.min(420, parent.width - root.theme.gapLg * 2)
                                theme: root.theme
                                title: root.lineageStatus === "loading"
                                       ? root.i18n.text("Loading lineage", "正在載入血緣", "正在载入血缘")
                                       : root.i18n.text("No graph loaded", "尚未載入圖", "尚未载入图")
                                message: root.lineageStatusMessage.length > 0 ? root.lineageStatusMessage :
                                         root.i18n.text("Run a query to draw declared relationships.",
                                                        "執行查詢以繪製宣告關係。",
                                                        "执行查询以绘制声明关系。")
                            }

                            Canvas {
                                id: lineageEdgeCanvas
                                anchors.fill: parent
                                visible: lineageGraphNodeModel.count > 0
                                onPaint: {
                                    var ctx = getContext("2d")
                                    ctx.reset()
                                    ctx.strokeStyle = root.theme.accent
                                    ctx.lineWidth = 1.5
                                    for (var e = 0; e < lineageGraphEdgeModel.count; ++e) {
                                        var edge = lineageGraphEdgeModel.get(e)
                                        var from = edge.fromIndex
                                        var to = edge.toIndex
                                        var fromX = nodeX(from) + 110
                                        var fromY = nodeY(from) + 40
                                        var toX = nodeX(to)
                                        var toY = nodeY(to) + 40
                                        ctx.beginPath()
                                        ctx.moveTo(fromX, fromY)
                                        ctx.lineTo(toX, toY)
                                        ctx.stroke()
                                        ctx.beginPath()
                                        ctx.moveTo(toX, toY)
                                        ctx.lineTo(toX - 7, toY - 4)
                                        ctx.lineTo(toX - 7, toY + 4)
                                        ctx.closePath()
                                        ctx.fillStyle = root.theme.accent
                                        ctx.fill()
                                    }
                                }
                                readonly property int nodeHeight: 76
                                // The graph area is shorter when the headers above it
                                // need more height, as they do with CJK text.  A card is
                                // moved up as far as needed to stay inside the area.
                                function fitY(preferred) {
                                    return Math.max(0, Math.min(preferred, height - nodeHeight - 4))
                                }
                                function nodeX(index) {
                                    if (root.lineageMode === "file")
                                        return index === 0 ? 28 : 230 + ((index - 1) % 3) * 150
                                    return 28 + index * 150
                                }
                                function nodeY(index) {
                                    if (root.lineageMode === "file")
                                        return index === 0 ? fitY(116)
                                                           : 40 + Math.floor((index - 1) / 3) * 92
                                    return fitY(110)
                                }
                                onHeightChanged: requestPaint()
                                Connections {
                                    target: lineageGraphNodeModel
                                    function onCountChanged() { lineageEdgeCanvas.requestPaint() }
                                }
                                Connections {
                                    target: lineageGraphEdgeModel
                                    function onCountChanged() { lineageEdgeCanvas.requestPaint() }
                                }
                            }

                            Repeater {
                                model: lineageGraphNodeModel
                                delegate: Rectangle {
                                    objectName: model.nodeType === "file" ? "lineageFileNode" : "lineageVersionNode"
                                    x: lineageEdgeCanvas.nodeX(index)
                                    y: lineageEdgeCanvas.nodeY(index)
                                    width: 122
                                    height: lineageEdgeCanvas.nodeHeight
                                    radius: root.theme.radius
                                    color: index === root.selectedLineageNodeIndex ? root.theme.bgPanel : root.theme.bgElevated
                                    border.color: model.emphasized ? root.theme.accentWarn :
                                                  (index === root.selectedLineageNodeIndex ? root.theme.accent : root.theme.border)

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: root.theme.gapXs
                                        spacing: 2
                                        Label {
                                            Layout.fillWidth: true
                                            text: model.label
                                            color: root.theme.textPrimary
                                            font.family: root.theme.monoFont
                                            font.pixelSize: root.theme.fontData
                                            font.bold: model.emphasized
                                            elide: Text.ElideMiddle
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: model.nodeType === "file" ? root.i18n.text("file", "檔案", "文件") : model.stage
                                            color: root.theme.textSecondary
                                            font.pixelSize: root.theme.fontCaption
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: model.relation + (model.depth.length > 0 ? " · d" + model.depth : "")
                                            color: root.theme.accent
                                            font.pixelSize: root.theme.fontCaption
                                            elide: Text.ElideRight
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: root.selectedLineageNodeIndex = index
                                    }
                                }
                            }
                        }
                    }

                    Common.ShellPanel {
                        objectName: "lineageNodeInspector"
                        Layout.preferredWidth: 270
                        Layout.fillHeight: true
                        theme: root.theme
                        title: root.i18n.text("Node Inspector", "節點檢視器", "节点检查器")

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: root.theme.gapSm

                            Common.EmptyState {
                                visible: root.selectedLineageNodeIndex < 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                theme: root.theme
                                title: root.i18n.text("No node selected", "尚未選擇節點", "尚未选择节点")
                                message: root.i18n.text("Click a graph node to inspect canonical identity and relation metadata.",
                                                        "點選圖中節點以檢視 canonical identity 與關係 metadata。",
                                                        "点击图中节点以查看 canonical identity 与关系 metadata。")
                            }

                            ColumnLayout {
                                visible: root.selectedLineageNodeIndex >= 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: root.theme.gapSm

                                Common.KeyValueGrid {
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    monoValue: true
                                    keyWidth: 92
                                    rows: root.selectedLineageNodeIndex >= 0 ? [
                                        { key: "canonical", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).canonical },
                                        { key: "phase", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).phase },
                                        { key: "block", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).block },
                                        { key: "stage", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).stage },
                                        { key: "version", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).version },
                                        { key: "relation", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).relation },
                                        { key: "depth", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).depth },
                                        { key: "authority", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).authority },
                                        { key: "content_verified", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).contentVerified },
                                        { key: "file path", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).filePath },
                                        { key: "managed", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).managed },
                                        { key: "result count", value: lineageGraphNodeModel.get(root.selectedLineageNodeIndex).resultCount }
                                    ] : []
                                }

                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    text: root.i18n.text("Copy canonical", "複製 canonical", "复制 canonical")
                                    kind: "secondary"
                                    enabled: root.selectedLineageCanonical.length > 0
                                    onClicked: root.copyAndReport(root.selectedLineageCanonical, "canonical")
                                }
                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    text: root.i18n.text("Use as query target", "作為查詢目標", "作为查询目标")
                                    kind: "secondary"
                                    onClicked: {
                                        var node = lineageGraphNodeModel.get(root.selectedLineageNodeIndex)
                                        if (node.nodeType === "version")
                                            root.versionQueryTarget = node.queryTarget || node.version
                                        else
                                            root.fileQueryPath = node.filePath
                                    }
                                }
                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    text: root.i18n.text("Open in Workspace A", "在工作區開啟", "在工作区打开")
                                    enabled: false
                                    kind: "secondary"
                                }
                            }
                        }
                    }
                }

                Common.ShellPanel {
                    objectName: "lineageResultSummaryStrip"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 72
                    theme: root.theme
                    elevated: true

                    RowLayout {
                        anchors.fill: parent
                        spacing: root.theme.gapSm

                        Common.StatusDot {
                            theme: root.theme
                            state: root.lineageStatus === "ok" ? "ok" :
                                   (root.lineageStatus === "error" ? "danger" :
                                    (root.lineageStatus === "warning" || root.lineageStatus === "not_available" ? "warn" : "neutral"))
                            label: root.lineageStatusMessage.length > 0 ? root.lineageStatusMessage :
                                   root.i18n.text("Ready for lineage query", "可執行血緣查詢", "可执行血缘查询")
                            Layout.preferredWidth: 300
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.i18n.text("Results: ", "結果數：", "结果数：") + root.lineageResultCount
                                  + "  ·  " + root.i18n.text("Warnings: ", "警告：", "警告：") + root.lineageWarningCount
                                  + "  ·  " + root.i18n.text("Truncated: ", "截斷：", "截断：") + (root.lineageTruncated ? "true" : "false")
                                  + "  ·  " + root.authorityLabel() + "  ·  " + root.contentBadgeLabel()
                            color: root.theme.textSecondary
                            font.pixelSize: root.theme.fontData
                            elide: Text.ElideRight
                        }

                        Common.ActionButton {
                            theme: root.theme
                            text: root.i18n.text("Copy JSON summary", "複製 JSON 摘要", "复制 JSON 摘要")
                            kind: "secondary"
                            enabled: root.lineageJsonSummary.length > 0
                            onClicked: root.copyAndReport(root.lineageJsonSummary, "JSON")
                        }
                    }
                }
            }
        }

        // ── Cold-start guided empty state (no version selected) ──────────────
        Common.ShellPanel {
            visible: !root.hasVersion()
            Layout.fillWidth: true
            Layout.fillHeight: true
            theme: root.theme

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(520, parent.width - root.theme.gapLg * 2)
                spacing: root.theme.gapMd

                Common.EmptyState {
                    Layout.fillWidth: true
                    theme: root.theme
                    title: root.i18n.text("Select a version to review evidence",
                                          "選擇版本以檢視證據", "选择版本以查看证据")
                    message: root.i18n.text("Select a version to review its commit history, provenance log, and source evidence.",
                                            "選擇版本以檢視它的 commit 歷史、版本來源紀錄與來源證據。",
                                            "选择版本以查看它的 commit 历史、版本来源记录与来源证据。")
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: root.theme.gapSm

                    Common.ActionButton {
                        theme: root.theme
                        text: root.i18n.text("Choose version", "選擇版本", "选择版本")
                        kind: "primary"
                        onClicked: root.openVersionPicker()
                    }
                    Common.ActionButton {
                        theme: root.theme
                        text: root.i18n.text("Refresh evidence", "重新整理證據", "刷新证据")
                        kind: "secondary"
                        enabled: root.hasVersion() && !root.refreshAllInProgress
                        onClicked: root.refreshAllReview()
                    }
                }
            }
        }

        Common.SeverityBanner {
            objectName: "evidenceStateBanner"
            Layout.fillWidth: true
            visible: root.hasVersion() && (root.refreshAllInProgress || root.evidenceError.length > 0)
            theme: root.theme
            compact: true
            severity: root.evidenceError.length > 0 ? "danger" : "info"
            title: root.evidenceError.length > 0
                   ? root.i18n.text("Evidence could not be loaded", "無法載入證據", "无法载入证据")
                   : root.i18n.text("Loading evidence…", "正在載入證據…", "正在载入证据…")
            message: root.evidenceError
        }

        // ── Main evidence area (version selected) ────────────────────────────
        RowLayout {
            visible: root.hasVersion()
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: root.theme.gapSm

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: root.theme.gapSm

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: root.theme.gapSm

                    Common.ShellPanel {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        theme: root.theme
                        title: root.i18n.text("SVN Commit History", "SVN Commit 歷史", "SVN Commit 历史")
                        subtitle: root.i18n.text("Repository commits for the selected version.",
                                                 "目前版本的 SVN commit 紀錄。",
                                                 "当前版本的 SVN commit 记录。")

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: root.theme.gapSm

                            Common.EmptyState {
                                visible: svnLogModel.count === 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                theme: root.theme
                                title: root.i18n.text("No commits to show yet.",
                                                     "尚無 commit 可顯示。",
                                                     "尚无 commit 可显示。")
                                message: root.i18n.text("Use Refresh evidence to load commit history.",
                                                       "使用「重新整理證據」載入 commit 歷史。",
                                                       "使用“刷新证据”载入 commit 历史。")
                            }

                            ListView {
                                visible: svnLogModel.count > 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: svnLogModel
                                spacing: root.theme.gapSm
                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: Math.max(74, logColumn.implicitHeight + root.theme.gapMd)
                                    radius: root.theme.radius
                                    color: root.theme.bgElevated
                                    border.color: root.theme.border

                                    ColumnLayout {
                                        id: logColumn
                                        anchors.fill: parent
                                        anchors.margins: root.theme.gapSm
                                        spacing: root.theme.gapXs

                                        Label {
                                            text: "r" + model.rev + " | " + model.author + " | " + model.time
                                            color: root.theme.textPrimary
                                            font.bold: true
                                            font.family: root.theme.monoFont
                                            font.pixelSize: root.theme.fontData
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        TextEdit {
                                            text: model.message
                                            color: root.theme.textSecondary
                                            font.pixelSize: root.theme.fontBody
                                            readOnly: true
                                            selectByMouse: true
                                            persistentSelection: true
                                            textFormat: TextEdit.PlainText
                                            wrapMode: TextEdit.Wrap
                                            selectionColor: root.theme.accent
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Common.ShellPanel {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        theme: root.theme
                        title: root.i18n.text("Provenance Log", "版本來源紀錄", "版本来源记录")
                        subtitle: root.i18n.text("User-recorded version source decisions.",
                                                 "使用者記錄的版本來源決策。",
                                                 "用户记录的版本来源决策。")

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: root.theme.gapSm

                            Common.EmptyState {
                                visible: lineageAuditModel.count === 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                theme: root.theme
                                title: root.i18n.text("No provenance records to show yet.",
                                                     "尚無版本來源紀錄可顯示。",
                                                     "尚无版本来源记录可显示。")
                                message: root.i18n.text("Provenance records are separate from SVN commit history.",
                                                       "版本來源紀錄與 SVN commit 歷史分開顯示。",
                                                       "版本来源记录与 SVN commit 历史分开显示。")
                            }

                            ListView {
                                visible: lineageAuditModel.count > 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: lineageAuditModel
                                spacing: root.theme.gapSm
                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: Math.max(122, auditColumn.implicitHeight + root.theme.gapMd)
                                    radius: root.theme.radius
                                    color: index === root.selectedProvenanceIndex ? root.theme.bgPanel : root.theme.bgElevated
                                    border.color: index === root.selectedProvenanceIndex ? root.theme.accent : root.theme.border

                                    ColumnLayout {
                                        id: auditColumn
                                        anchors.fill: parent
                                        anchors.margins: root.theme.gapSm
                                        spacing: root.theme.gapXs

                                        Label {
                                            text: "#" + model.seq + " | " + root.i18n.text("Action", "動作", "动作") + ": " +
                                                  model.action + " | " + root.i18n.text("Actor", "人員", "人员") + ": " + model.actor
                                            color: root.theme.textPrimary
                                            font.bold: true
                                            font.family: root.theme.monoFont
                                            font.pixelSize: root.theme.fontData
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: root.i18n.text("Time", "時間", "时间") + ": " + model.ts
                                            color: root.theme.textSecondary
                                            font.pixelSize: root.theme.fontData
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: root.i18n.text("Version", "Version", "Version") + ": " + model.child
                                            color: root.theme.textSecondary
                                            font.family: root.theme.monoFont
                                            font.pixelSize: root.theme.fontData
                                            elide: Text.ElideMiddle
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: root.i18n.text("Previous source", "前次來源", "前次来源") + ": " +
                                                  root.sourceValue(model.oldParent) + "  ->  " +
                                                  root.i18n.text("New source", "新來源", "新来源") + ": " +
                                                  root.sourceValue(model.newParent)
                                            color: root.theme.textSecondary
                                            font.family: root.theme.monoFont
                                            font.pixelSize: root.theme.fontData
                                            elide: Text.ElideMiddle
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: root.i18n.text("Reason", "原因", "原因") + ": " + model.reason
                                            visible: model.reason.length > 0
                                            color: root.theme.textSecondary
                                            font.pixelSize: root.theme.fontData
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: root.selectedProvenanceIndex = index
                                    }
                                }
                            }
                        }
                    }
                }

                // Version Relationships — a relationship card strip (not a drawn
                // node-link graph). Built from the Version graph metadata; read-only.
                Common.ShellPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 190
                    theme: root.theme
                    title: root.i18n.text("Version Relationships", "版本關係", "版本关系")
                    // The hint is part of the subtitle rather than a row of its own:
                    // the panel has a fixed height, and the cards need the room.
                    subtitle: root.i18n.text("Version graph metadata — recorded relationships only (read-only). Click a card for details only.",
                                             "版本關係 metadata：僅顯示已記錄的來源關係，不讀取檔案內容。點節點看資訊。",
                                             "版本关系 metadata：仅显示已记录的来源关系，不读取文件内容。点节点看信息。")

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: root.theme.gapSm

                        Common.EmptyState {
                            visible: lineagePanoramaModel.count === 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            theme: root.theme
                            title: root.i18n.text("No recorded relationships yet.",
                                                 "尚無已記錄的關係。",
                                                 "尚无已记录的关系。")
                            message: root.i18n.text("Use Refresh evidence to load recorded relationships.",
                                                   "使用「重新整理證據」載入已記錄的關係。",
                                                   "使用“刷新证据”载入已记录的关系。")
                        }

                        ListView {
                            visible: lineagePanoramaModel.count > 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            orientation: ListView.Horizontal
                            spacing: root.theme.gapSm
                            clip: true
                            model: lineagePanoramaModel
                            delegate: Common.GraphNodeCard {
                                width: 250
                                // As tall as the list, so every line of the card is shown.
                                height: ListView.view ? ListView.view.height : 104
                                theme: root.theme
                                i18n: root.i18n
                                versionPath: model.child
                                versionLabel: root.versionLeaf(model.child)
                                stage: root.versionStage(model.child)
                                actor: model.actor
                                timestamp: model.ts
                                note: root.i18n.text("Source version", "來源版本", "来源版本") + ": " + root.sourceValue(model.parent)
                                selected: root.selectedGraphVersion === model.child
                                current: model.child === root.selectedVersionPath()
                                onClicked: root.graphNodeClicked(index)
                            }
                        }
                    }
                }
            }

            // ── Right inspector: version, Source Check (contained), Evidence ──
            Common.ShellPanel {
                Layout.preferredWidth: 320
                Layout.minimumWidth: 280
                Layout.fillHeight: true
                theme: root.theme
                title: root.i18n.text("Inspector", "檢視器", "检查器")
                subtitle: root.i18n.text("Review summaries stay separate", "核對摘要分開呈現", "核对摘要分开呈现")

                ScrollView {
                    id: inspectorScroll
                    anchors.fill: parent
                    clip: true
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: inspectorScroll.availableWidth
                        spacing: root.theme.gapSm

                        Common.InspectorSection {
                            Layout.fillWidth: true
                            theme: root.theme
                            title: root.i18n.text("Selected version", "目前版本", "当前版本")
                            collapsible: false

                            Common.KeyValueGrid {
                                Layout.fillWidth: true
                                theme: root.theme
                                monoValue: true
                                keyWidth: 86
                                rows: root.selectedVersionRows()
                            }
                        }

// Source Check — review-only state surface.
                        Common.InspectorSection {
                            Layout.fillWidth: true
                            theme: root.theme
                            title: root.i18n.text("Source Check", "來源核對", "来源核对")
                            subtitle: root.i18n.text("Compares the selected version with available source evidence and gives a review recommendation. It does not change project data.",
                                                     "比對目前版本與可取得的來源證據，提供檢視建議;不會變更專案資料。",
                                                     "比对当前版本与可获取的来源证据，提供查看建议;不会变更项目数据。")
                            collapsible: false

                            // Status badge + short meaning sentence.
                            Common.StatusDot {
                                objectName: "sourceCheckStatusDot"
                                Layout.fillWidth: true
                                theme: root.theme
                                state: root.sourceCheckDotState()
                                label: root.sourceCheckStateLabel()
                            }

                            Label {
                                Layout.fillWidth: true
                                text: root.sourceCheckMeaning()
                                color: root.theme.textSecondary
                                font.pixelSize: root.theme.fontBody
                                wrapMode: Text.Wrap
                            }

                            Common.ActionButton {
                                objectName: "runReconcileButton"
                                Layout.fillWidth: true
                                theme: root.theme
                                text: root.i18n.text("Run Source Check", "執行來源核對", "执行来源核对")
                                kind: "secondary"
                                enabled: root.hasVersion() && !root.refreshAllInProgress
                                onClicked: root.runReconcileReport()
                            }

                            // Recommendation (what to review next).
                            Common.SeverityBanner {
                                Layout.fillWidth: true
                                theme: root.theme
                                severity: root.sourceCheckDotState() === "warn" ? "warn"
                                          : (root.sourceCheckDotState() === "danger" ? "danger" : "info")
                                compact: true
                                title: root.i18n.text("Recommendation", "建議", "建议")
                                message: root.sourceCheckRecommendation()
                            }

                            // Review-only assurance — always visible.
                            Common.SeverityBanner {
                                Layout.fillWidth: true
                                theme: root.theme
                                severity: "info"
                                compact: true
                                title: root.i18n.text("Review-only", "僅供檢視", "仅供查看")
                                message: root.i18n.text("Source Check is review-only. It does not apply, repair, checkout, or modify any version automatically.",
                                                       "來源核對僅供檢視。不會自動套用、修復、checkout 或修改任何版本。",
                                                       "来源核对仅供查看。不会自动套用、修复、checkout 或修改任何版本。")
                            }

                            // Safe details expander — sanitized fields only, no raw output.
                            Common.InspectorSection {
                                objectName: "sourceCheckDetails"
                                Layout.fillWidth: true
                                theme: root.theme
                                title: root.i18n.text("Details", "細節", "细节")
                                collapsible: true
                                expanded: false

                                Common.KeyValueGrid {
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    monoValue: true
                                    keyWidth: 104
                                    rows: root.sourceCheckDetailRows()
                                }
                            }

                            Common.ActionButton {
                                objectName: "copySourceCheckButton"
                                Layout.fillWidth: true
                                theme: root.theme
                                kind: "secondary"
                                text: root.i18n.text("Copy Source Check summary", "複製來源核對摘要", "复制来源核对摘要")
                                onClicked: root.copyAndReport(root.sourceCheckSummaryText(),
                                                              root.i18n.text("Source Check summary", "來源核對摘要", "来源核对摘要"))
                            }

                            // Review-only human step (never mutates; opens a guidance note).
                            Common.ActionButton {
                                objectName: "reconcileDecisionButton"
                                Layout.fillWidth: true
                                theme: root.theme
                                text: root.i18n.text("Review source decision", "檢視來源決策", "查看来源决策")
                                kind: "secondary"
                                enabled: root.reconcileChild.length > 0 || root.hasVersion()
                                onClicked: reconcileDecisionDialog.open()
                            }
                        }

                        Common.InspectorSection {
                            Layout.fillWidth: true
                            theme: root.theme
                            title: root.i18n.text("Provenance summary", "版本來源摘要", "版本来源摘要")
                            expanded: true

                            Common.KeyValueGrid {
                                Layout.fillWidth: true
                                theme: root.theme
                                monoValue: false
                                keyWidth: 104
                                rows: root.provenanceSummaryRows()
                            }
                        }

                        // Evidence — read-only copy/export actions.
                        Common.InspectorSection {
                            Layout.fillWidth: true
                            theme: root.theme
                            title: root.i18n.text("Evidence", "證據", "证据")
                            subtitle: root.i18n.text("Copy read-only summaries as plain text.",
                                                     "以純文字複製唯讀摘要。",
                                                     "以纯文本复制只读摘要。")
                            collapsible: false

                            Common.ActionButton {
                                Layout.fillWidth: true
                                theme: root.theme
                                kind: "secondary"
                                text: root.i18n.text("Copy selected version", "複製目前版本", "复制当前版本")
                                enabled: root.hasVersion()
                                onClicked: root.copyAndReport(root.selectedVersionPath(),
                                                              root.i18n.text("selected version", "目前版本", "当前版本"))
                            }
                            Common.ActionButton {
                                Layout.fillWidth: true
                                theme: root.theme
                                kind: "secondary"
                                text: root.i18n.text("Copy commit summary", "複製 commit 摘要", "复制 commit 摘要")
                                onClicked: root.copyAndReport(root.commitSummaryText(),
                                                              root.i18n.text("commit summary", "commit 摘要", "commit 摘要"))
                            }
                            Common.ActionButton {
                                Layout.fillWidth: true
                                theme: root.theme
                                kind: "secondary"
                                text: root.i18n.text("Copy provenance summary", "複製版本來源摘要", "复制版本来源摘要")
                                onClicked: root.copyAndReport(root.provenanceSummaryText(),
                                                              root.i18n.text("provenance summary", "版本來源摘要", "版本来源摘要"))
                            }
                            Common.ActionButton {
                                Layout.fillWidth: true
                                theme: root.theme
                                kind: "primary"
                                text: root.i18n.text("Copy all evidence", "複製全部證據", "复制全部证据")
                                enabled: root.hasVersion()
                                onClicked: root.copyAndReport(root.evidenceSummaryText(),
                                                              root.i18n.text("all evidence", "全部證據", "全部证据"))
                            }
                        }

                        Common.InspectorSection {
                            Layout.fillWidth: true
                            theme: root.theme
                            title: root.i18n.text("Review guidance", "核對提示", "核对提示")
                            collapsible: false

                            Common.SeverityBanner {
                                Layout.fillWidth: true
                                theme: root.theme
                                severity: "info"
                                title: root.i18n.text("Separate sources", "來源分開", "来源分开")
                                message: root.i18n.text("SVN commits and Provenance records are shown separately. Use Source Check to review consistency before deciding whether the recorded source is correct.",
                                                       "SVN commit 與版本來源紀錄分開顯示。請使用來源核對來檢查一致性，再判斷記錄的來源是否正確。",
                                                       "SVN commit 与版本来源记录分开显示。请使用来源核对来检查一致性，再判断记录的来源是否正确。")
                            }
                        }
                    }
                }
            }
        }
    }
}
