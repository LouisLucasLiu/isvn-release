import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "../common" as Common

Item {
    id: root
    objectName: "workspaceA"
    property bool dagMode: false
    property string selectedVersion: ""
    property string selectedFile: ""
    property string fileFilter: ""
    property string fileFilterMode: "all"
    property string fileMenuPath: ""
    property string fileMenuSvn: ""
    property string fileMenuLink: ""
    property string selectedWorkingCopy: ""
    property string statusMessage: ""
    property bool refreshFocusAfterBranches: false
    property bool refreshAncestryAfterStatus: false
    property var branchesData: []
    property string projectPath: ""
    property int graphSelectedIndex: -1
    property bool graphShowStage: true
    property bool graphShowNotes: true
    property bool ancestryLoading: false
    property string ancestryError: ""
    property int graphMenuIndex: -1
    property string graphMenuPath: ""
    property bool actionsBusy: false
    property var i18n: ({ text: function(en, zhTw, zhCn) { return zhTw } })
    property var theme: ({ bgBase: "#0E1116", bgPanel: "#161B22", bgElevated: "#1C232D", border: "#2A3441", textPrimary: "#E6EDF3", textSecondary: "#8B98A5", accent: "#3FB6C7", accentWarn: "#E0A030", accentDanger: "#E5534B", accentOk: "#3FB950", radius: 6, gapSm: 8, gapMd: 12, gapLg: 16, gapXl: 24, fontTitle: 22, fontSection: 15, fontBody: 13, fontData: 12, monoFont: "monospace", stageColor: function(path) { return "#3FB6C7" } })
    signal versionFocused(string version)
    signal operationStatus(string message)
    onStatusMessageChanged: if (statusMessage.length > 0) operationStatus(statusMessage)

    ListModel { id: branchesModel }
    ListModel { id: filesModel }
    ListModel { id: filesViewModel }
    ListModel { id: ancestryModel }
    ListModel { id: compareBasket }
    ListModel { id: compareResultModel }
    ListModel { id: createParentModel }
    ListModel { id: renewStatusModel }

    Common.CommandRunner {
        id: branchesRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.populateBranches(json)
            if (root.refreshFocusAfterBranches) {
                root.refreshFocusAfterBranches = false
                // Keep a graph that is already on screen current; never open one.
                root.refreshFocusedStatus(ancestryModel.count > 0)
            }
        }
        onFailed: function(message) { root.statusMessage = message }
    }
    Common.CommandRunner {
        id: statusRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.populateStatus(json)
            root.refreshPendingAncestry()
        }
        onFailed: function(message) {
            // Status failed (e.g. version not checked out): drop stale checked-out state so
            // file actions stay disabled until a real working copy is confirmed.
            root.selectedWorkingCopy = ""
            filesModel.clear()
            filesViewModel.clear()
            root.statusMessage = message
            root.refreshPendingAncestry()
        }
    }
    Common.CommandRunner {
        id: ancestryRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            root.ancestryLoading = false
            // A graph requested for a previously selected version is discarded.
            if (json && json.child && json.child !== root.selectedVersion)
                return
            root.ancestryError = ""
            root.populateAncestry(json)
        }
        onFailed: function(message) {
            root.ancestryLoading = false
            root.ancestryError = message
            root.statusMessage = message
        }
    }
    Common.CommandRunner {
        id: actionRunner
        i18n: root.i18n
        onSucceeded: function(json) {
            if (json && json.result === "committed")
                root.statusMessage = (json.copied
                    ? root.i18n.text("Copied into version and checked in r", "已複製進版本並簽入 r", "已复制进版本并签入 r")
                    : root.i18n.text("Checked in r", "已簽入 r", "已签入 r")) + json.revision
            else if (json && json.result === "unchanged")
                root.statusMessage = root.i18n.text("No changes to check in; nothing was committed: ",
                                                    "內容沒有變動，沒有簽入新的修訂：",
                                                    "内容没有变动，没有签入新的修订：") + (json.file || "")
            else if (json && json.result === "local")
                root.statusMessage = root.i18n.text("Opened for editing: ", "已開啟編輯：", "已打开编辑：") + (json.file || "")
            else if (json && json.result === "checked_out")
                root.statusMessage = root.i18n.text("Checked out: ", "已取出：", "已取出：") + (json.version || "")
            else if (json && json.result === "created")
                root.statusMessage = root.i18n.text("Created: ", "已建立：", "已创建：") + (json.version || "")
            else if (json && json.result === "removed")
                root.statusMessage = root.i18n.text("Removed file ", "已移除檔案 ", "已移除文件 ") + (json.file || "")
                    + " (r" + (json.revision || "") + ")"
            else if (json && json.result === "renewed")
                root.statusMessage = root.i18n.text("Renewed contents (", "已續期內容（", "已续期内容（")
                    + (json.renewed_count || 0) + root.i18n.text(" files) of ", " 個檔）：", " 个文件）：") + (json.version || "")
            else if (json && json.result === "deleted") {
                root.statusMessage = root.i18n.text("Deleted version ", "已刪除版本：", "已删除版本：") + (json.version || "")
                    + " (" + root.i18n.text("freed ", "釋放 ", "释放 ") + (json.bytes_freed || 0) + " bytes)"
                if (json.version === root.selectedVersion)
                    root.selectedVersion = ""
            }
            else
                root.statusMessage = root.i18n.text("Command completed", "指令完成", "指令完成")
            root.refreshAll()
        }
        onFailed: function(message) { root.statusMessage = message }
    }
    Common.CommandRunner {
        id: renewStatusRunner
        i18n: root.i18n
        onSucceeded: function(json) { root.populateRenewStatus(json) }
        onFailed: function(message) { root.statusMessage = message }
    }

    Common.MetadataPopover {
        id: metadataPopover
        i18n: root.i18n
        theme: root.theme
        showGetUpstream: false
    }

    // Hidden helper to copy a version path to the system clipboard.
    TextEdit { id: graphClipboard; visible: false; readOnly: true }
    function copyGraphPath(value) {
        graphClipboard.text = String(value || "")
        graphClipboard.selectAll()
        graphClipboard.copy()
        graphClipboard.deselect()
    }

    // Keep the inspector's busy state in sync with the shared CLI bridge so action
    // buttons disable while a command is running.
    Connections {
        target: isvnCli
        function onRunningChanged() { root.actionsBusy = isvnCli.running }
    }

    function copyVersionFields() {
        var s = splitVersion(selectedVersion)
        copyGraphPath([s.phase, s.block, s.stage, s.version].join("\t"))
        statusMessage = root.i18n.text("Copied phase/block/stage/version", "已複製 phase/block/stage/version", "已复制 phase/block/stage/version")
    }

    function removeCompareVersion(index) {
        if (index >= 0 && index < compareBasket.count)
            compareBasket.remove(index)
    }

    function clearCompareBasket() {
        compareBasket.clear()
        compareResultModel.clear()
    }

    function scrollInspectorToBottom() {
        if (inspectorScroll.contentItem)
            inspectorScroll.contentItem.contentY =
                Math.max(0, inspectorScroll.contentItem.contentHeight - inspectorScroll.height)
    }

    function retentionLinkedCount() {
        var n = 0
        for (var i = 0; i < renewStatusModel.count; ++i)
            if (renewStatusModel.get(i).state === "LINKED") n += 1
        return n
    }

    function retentionLocalCount() {
        return renewStatusModel.count - retentionLinkedCount()
    }

    // Graph node context menu — safe, metadata/display actions only (no checkout/
    // Get upstream/edit/checkin/destructive operations).
    Menu {
        id: graphNodeMenu
        objectName: "graphNodeMenu"
        MenuItem {
            text: root.versionLeaf(root.graphMenuPath)
            enabled: false
        }
        MenuSeparator {}
        MenuItem {
            text: root.i18n.text("Select in graph", "在圖中選取", "在图中选取")
            onTriggered: if (root.graphMenuIndex >= 0) root.selectGraphNode(root.graphMenuIndex, false)
        }
        MenuItem {
            text: root.i18n.text("Center this node", "置中此節點", "居中此节点")
            onTriggered: {
                if (root.graphMenuIndex >= 0) root.graphSelectedIndex = root.graphMenuIndex
                graphPanel.centerSelected()
            }
        }
        MenuItem {
            text: root.i18n.text("Show metadata", "顯示資訊", "显示信息")
            onTriggered: if (root.graphMenuIndex >= 0) root.showMetadata(root.graphMenuIndex)
        }
        MenuItem {
            text: root.i18n.text("Copy version path", "複製版本路徑", "复制版本路径")
            onTriggered: root.copyGraphPath(root.graphMenuPath)
        }
        MenuItem {
            text: root.i18n.text("Add to compare basket", "加入比對籃", "加入对比篮")
            onTriggered: if (root.graphMenuPath.length > 0) root.addCompareVersion(root.graphMenuPath)
        }
    }

    // File-row context menu — safe, file-level actions only (no version-level/destructive
    // actions; those live in the right Version Inspector).
    Menu {
        id: fileRowMenu
        objectName: "fileRowMenu"
        MenuItem {
            text: root.fileMenuPath
            enabled: false
        }
        MenuSeparator {}
        MenuItem {
            text: root.i18n.text("Show file details", "顯示檔案資訊", "显示文件信息")
            onTriggered: fileDetailPopover.open()
        }
        MenuItem {
            text: root.i18n.text("Copy file path", "複製檔案路徑", "复制文件路径")
            onTriggered: { root.copyGraphPath(root.fileMenuPath); root.statusMessage = root.i18n.text("Copied file path: ", "已複製檔案路徑：", "已复制文件路径：") + root.fileMenuPath }
        }
        MenuItem {
            text: root.i18n.text("Copy version path", "複製版本路徑", "复制版本路径")
            onTriggered: root.copyGraphPath(root.selectedVersion)
        }
        MenuItem {
            text: root.i18n.text("Filter by directory", "依目錄篩選", "按目录筛选")
            enabled: root.fileDirectory(root.fileMenuPath).length > 0
            onTriggered: { root.fileFilter = root.fileDirectory(root.fileMenuPath); root.rebuildFilesView() }
        }
        MenuItem {
            text: root.i18n.text("Filter by state", "依狀態篩選", "按状态筛选")
            onTriggered: {
                root.fileFilterMode = root.fileIsChanged(root.fileMenuSvn) ? "changed"
                                      : (root.fileMenuLink === "LOCAL" ? "local" : "linked")
                root.rebuildFilesView()
            }
        }
        MenuSeparator {}
        // File workflow — gated on the version being checked out / editable here.
        MenuItem {
            text: root.i18n.text("Edit this file", "編輯此檔案", "编辑此文件")
            enabled: root.selectedWorkingCopy.length > 0 && !root.actionsBusy
            onTriggered: root.editFile(root.fileMenuPath)
            ToolTip.visible: hovered && !enabled
            ToolTip.text: root.actionsBusy
                          ? root.i18n.text("Command is running", "指令執行中", "指令执行中")
                          : root.i18n.text("Checkout this version first", "請先 checkout 此版本", "请先 checkout 此版本")
        }
        // Checkin shown only when the file has a checkin-worthy change (clean files hidden).
        MenuItem {
            visible: root.fileIsChanged(root.fileMenuSvn)
            text: root.i18n.text("Checkin this file", "checkin 此檔案", "checkin 此文件")
            enabled: root.selectedWorkingCopy.length > 0 && !root.actionsBusy
            onTriggered: root.checkinFile(root.fileMenuPath)
            ToolTip.visible: hovered && !enabled
            ToolTip.text: root.actionsBusy
                          ? root.i18n.text("Command is running", "指令執行中", "指令执行中")
                          : root.i18n.text("Checkout this version first", "請先 checkout 此版本", "请先 checkout 此版本")
        }
        MenuSeparator {}
        // Danger — single-file removal (uses isvn remove --from; never version-level delete).
        MenuItem {
            text: root.i18n.text("Remove this file from current version", "從目前版本移除此檔案", "从当前版本移除此文件")
            enabled: root.selectedWorkingCopy.length > 0 && !root.actionsBusy
            onTriggered: removeFileConfirm.open()
            ToolTip.visible: hovered && !enabled
            ToolTip.text: root.actionsBusy
                          ? root.i18n.text("Command is running", "指令執行中", "指令执行中")
                          : root.i18n.text("Checkout this version first", "請先 checkout 此版本", "请先 checkout 此版本")
        }
    }

    // Confirm dialog for single-file remove (file-level, not version-level delete).
    Common.ConfirmDialog {
        id: removeFileConfirm
        objectName: "removeFileConfirmDialog"
        i18n: root.i18n
        theme: root.theme
        title: root.i18n.text("Remove this file from current version", "從目前版本移除此檔案", "从当前版本移除此文件")
        body: root.i18n.text("Action: Remove file from current version\nTarget version: ", "動作：從目前版本移除檔案\n目標版本：", "动作：从当前版本移除文件\n目标版本：")
              + root.selectedVersion
              + root.i18n.text("\nFile: ", "\n檔案：", "\n文件：") + root.fileMenuPath
              + root.i18n.text("\nEffect: remove only this file from the version working copy and commit. The version and its other files are kept.",
                               "\n效果：只從版本工作副本移除這個檔案並提交;版本與其他檔案保留。",
                               "\n效果：只从版本工作副本移除这个文件并提交;版本与其他文件保留。")
        onConfirmed: root.removeFile(root.fileMenuPath)
    }

    // File detail popover (logical info only; no pool object path / raw link target).
    Popup {
        id: fileDetailPopover
        objectName: "fileDetailPopover"
        modal: true
        focus: true
        width: 360
        anchors.centerIn: Overlay.overlay
        padding: root.theme.gapMd
        background: Rectangle {
            color: root.theme.bgElevated
            border.color: root.theme.border
            radius: root.theme.radius
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: root.theme.gapSm
            Common.VersionPathPill {
                Layout.fillWidth: true
                theme: root.theme
                path: root.fileMenuPath
                showStageDot: false
                selected: true
            }
            Common.KeyValueGrid {
                Layout.fillWidth: true
                theme: root.theme
                keyWidth: 96
                rows: [
                    { key: root.i18n.text("File", "檔案", "文件"), value: root.fileMenuPath },
                    { key: root.i18n.text("SVN", "SVN", "SVN"), value: root.fileMenuSvn.length > 0 ? root.fileMenuSvn : "normal" },
                    { key: root.i18n.text("Link", "Link", "Link"), value: root.fileMenuLink.length > 0 ? root.fileMenuLink : root.i18n.text("unknown", "未知", "未知") },
                    { key: root.i18n.text("Version", "Version", "Version"), value: root.selectedVersion }
                ]
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Common.ActionButton {
                    theme: root.theme
                    kind: "ghost"
                    text: root.i18n.text("Copy file path", "複製檔案路徑", "复制文件路径")
                    onClicked: root.copyGraphPath(root.fileMenuPath)
                }
                Common.ActionButton {
                    theme: root.theme
                    kind: "ghost"
                    text: root.i18n.text("Close", "關閉", "关闭")
                    onClicked: fileDetailPopover.close()
                }
            }
        }
    }

    Common.ConfirmDialog {
        id: checkoutAncestorConfirm
        objectName: "getUpstreamConfirmDialog"
        i18n: root.i18n
        theme: root.theme
        title: root.i18n.text("Get upstream", "取得上游版本", "获取上游版本")
        body: root.i18n.text("Action: Get upstream\nTarget version: ", "動作：取得上游版本\n目標版本：", "动作：获取上游版本\n目标版本：")
              + root.selectedVersion
              + root.i18n.text("\nEffect: fetch content from the recorded upstream/parent source into this version. Runs only after you confirm.",
                               "\n效果：從記錄的上一版來源取得內容到這個版本；只有你確認後才會執行。",
                               "\n效果：从记录的上一版来源获取内容到这个版本；只有你确认后才会执行。")
        onConfirmed: root.runCheckoutAncestor()
    }

    Common.ConfirmDialog {
        id: createChildConfirm
        objectName: "createChildConfirmDialog"
        i18n: root.i18n
        theme: root.theme
        title: root.i18n.text("New derived version", "新衍生版本", "新衍生版本")
        body: root.i18n.text("Action: New derived version\nParent version: ", "動作：新衍生版本\nParent 版本：", "动作：新衍生版本\nParent 版本：")
              + root.selectedVersion
              + root.i18n.text("\nNew version: ", "\n新版本：", "\n新版本：") + childVersion.text
              + root.i18n.text("\nEffect: create a new version derived from this selected version.",
                               "\n效果：從這個選定版本建立一個新的衍生版本。",
                               "\n效果：从这个选定版本创建一个新的衍生版本。")
        onConfirmed: root.runCreateChild()
    }

    Common.ConfirmDialog {
        id: deleteVersionConfirm
        objectName: "deleteVersionConfirmDialog"
        i18n: root.i18n
        theme: root.theme
        title: root.i18n.text("Delete version and contents", "刪除版本與內容", "删除版本与内容")
        body: root.i18n.text("Action: Delete version and contents\nTarget version: ", "動作：刪除版本與內容\n目標版本：", "动作：删除版本与内容\n目标版本：")
              + root.selectedVersion
              + root.i18n.text("\nEffect: permanently delete this version and remove its contents from your workspace. Pool data referenced only by this version is freed. A version with derived children cannot be deleted.",
                               "\n效果：永久刪除這個版本並從你的工作區移除其內容；只被這個版本參照的快取資料會被釋放；有衍生子版本的版本無法刪除。",
                               "\n效果：永久删除这个版本并从你的工作区移除其内容；只被这个版本引用的缓存数据会被释放；有衍生子版本的版本无法删除。")
        onConfirmed: root.runDeleteVersion()
    }

    FileDialog {
        id: fileActionDialog
        objectName: "fileActionDialog"
        property string fileOp: ""
        fileMode: FileDialog.OpenFile
        acceptLabel: root.i18n.text("Select", "選擇", "选择")
        rejectLabel: root.i18n.text("Cancel", "取消", "取消")
        title: fileOp === "edit"
               ? root.i18n.text("Select a file to edit", "選擇要編輯的檔案", "选择要编辑的文件")
               : root.i18n.text("Select a file to check in", "選擇要簽入的檔案", "选择要签入的文件")
        currentFolder: root.selectedWorkingCopy.length > 0
                       ? "file://" + root.selectedWorkingCopy : ""
        onAccepted: {
            root.selectedFile = root.urlToPath(selectedFile)
            if (fileActionDialog.fileOp === "edit") {
                root.statusMessage = root.i18n.text("Editing ", "編輯中：", "编辑中：") + root.selectedFile
                actionRunner.runJson(["edit", root.selectedFile, "--in", root.selectedVersion])
            } else {
                var wc = root.selectedWorkingCopy
                var inside = wc.length > 0 && (root.selectedFile === wc
                                               || root.selectedFile.indexOf(wc + "/") === 0)
                root.statusMessage = inside
                    ? root.i18n.text("Checking in ", "簽入中：", "签入中：") + root.selectedFile
                    : root.i18n.text("Copying into version then checking in: ",
                                     "將複製進版本後簽入：",
                                     "将复制进版本后签入：") + root.selectedFile
                actionRunner.runJson(["checkin", root.selectedFile, "--to", root.selectedVersion])
            }
        }
    }

    Component.onCompleted: refreshAll()

    function refreshAll() {
        refreshFocusAfterBranches = true
        branchesRunner.runJson(["branches"])
    }

    function refreshFocusedStatus(alsoAncestry) {
        refreshAncestryAfterStatus = alsoAncestry
        if (selectedVersion.length > 0)
            statusRunner.runJson(["status", selectedVersion])
        else
            root.refreshPendingAncestry()
    }

    function refreshPendingAncestry() {
        if (!refreshAncestryAfterStatus)
            return
        refreshAncestryAfterStatus = false
        if (selectedVersion.length > 0) {
            root.ancestryLoading = true
            root.ancestryError = ""
            ancestryRunner.runJson(["ancestry", selectedVersion])
        }
    }

    function refreshGraph() {
        if (selectedVersion.length === 0) {
            statusMessage = root.i18n.text("Select a version first", "請先選取目前版本", "请先选取当前版本")
            return
        }
        root.ancestryLoading = true
        root.ancestryError = ""
        ancestryRunner.runJson(["ancestry", selectedVersion])
    }

    function populateBranches(json) {
        var previousParent = selectedCreateParentRaw()
        root.branchesData = json.branches || []
        branchesModel.clear()
        createParentModel.clear()
        createParentModel.append({
            label: root.i18n.text("Use selected version as parent", "以目前版本為 parent", "以当前版本为 parent"),
            value: "__FOCUS__"
        })
        createParentModel.append({
            label: "ROOT",
            value: "ROOT"
        })
        var restoreIndex = previousParent === "ROOT" ? 1 : 0
        var rows = json.branches || []
        for (var i = 0; i < rows.length; ++i) {
            var row = rows[i]
            var path = row.path || [row.phase, row.block, row.stage, row.version].join("/")
            branchesModel.append({
                path: path,
                phase: row.phase || "",
                block: row.block || "",
                stage: row.stage || "",
                version: row.version || "",
                isHead: row.is_head === true,
                removed: row.removed === true
            })
            createParentModel.append({
                label: path,
                value: path
            })
            if (previousParent === path)
                restoreIndex = createParentModel.count - 1
        }
        createParentCombo.currentIndex = restoreIndex
    }

    function populateStatus(json) {
        filesModel.clear()
        selectedWorkingCopy = json.working_copy || ""
        var version = json.version || ""
        if (version.length > 0 && selectedVersion.length === 0) {
            selectedVersion = version
            versionFocused(version)
        }
        var rows = json.files || []
        for (var i = 0; i < rows.length; ++i) {
            filesModel.append({
                path: rows[i].path || "",
                linkState: rows[i].link_state || "",
                svnState: rows[i].svn_state || ""
            })
        }
        rebuildFilesView()
    }

    function fileIsChanged(svnState) {
        return svnState.length > 0 && svnState !== "normal"
    }

    function fileMatchesFilter(row) {
        if (fileFilterMode === "changed" && !fileIsChanged(row.svnState)) return false
        if (fileFilterMode === "linked" && row.linkState !== "LINKED") return false
        if (fileFilterMode === "local" && row.linkState !== "LOCAL") return false
        var needle = fileFilter.trim().toLowerCase()
        if (needle.length > 0 && row.path.toLowerCase().indexOf(needle) < 0) return false
        return true
    }

    function rebuildFilesView() {
        filesViewModel.clear()
        for (var i = 0; i < filesModel.count; ++i) {
            var row = filesModel.get(i)
            if (fileMatchesFilter(row))
                filesViewModel.append({ path: row.path, linkState: row.linkState, svnState: row.svnState })
        }
    }

    function fileCount(kind) {
        var n = 0
        for (var i = 0; i < filesModel.count; ++i) {
            var row = filesModel.get(i)
            if (kind === "total") n += 1
            else if (kind === "linked" && row.linkState === "LINKED") n += 1
            else if (kind === "local" && row.linkState === "LOCAL") n += 1
            else if (kind === "changed" && fileIsChanged(row.svnState)) n += 1
        }
        return n
    }

    function fileAbsPath(logical) {
        return selectedWorkingCopy.length > 0 ? selectedWorkingCopy + "/" + logical : logical
    }

    function fileDirectory(logical) {
        var idx = String(logical).lastIndexOf("/")
        return idx > 0 ? logical.substring(0, idx) : ""
    }

    // Explicit, file-scoped edit/checkin (same backend flow as the Safe Actions buttons).
    function editFile(logical) {
        if (selectedVersion.length === 0 || selectedWorkingCopy.length === 0) return
        root.selectedFile = fileAbsPath(logical)
        root.statusMessage = root.i18n.text("Editing ", "編輯中：", "编辑中：") + root.selectedFile
        actionRunner.runJson(["edit", root.selectedFile, "--in", root.selectedVersion])
    }
    function checkinFile(logical) {
        if (selectedVersion.length === 0 || selectedWorkingCopy.length === 0) return
        root.selectedFile = fileAbsPath(logical)
        root.statusMessage = root.i18n.text("Checking in ", "簽入中：", "签入中：") + root.selectedFile
        actionRunner.runJson(["checkin", root.selectedFile, "--to", root.selectedVersion])
    }
    // Single-file remove (file-level): isvn remove <abs> --from <version>. Not version delete.
    function removeFile(logical) {
        if (selectedVersion.length === 0 || selectedWorkingCopy.length === 0) return
        root.selectedFile = fileAbsPath(logical)
        root.statusMessage = root.i18n.text("Removing ", "移除中：", "移除中：") + root.selectedFile
        actionRunner.runJson(["remove", root.selectedFile, "--from", root.selectedVersion])
    }

    function populateAncestry(json) {
        ancestryModel.clear()
        var rows = json.chain || []
        for (var i = 0; i < rows.length; ++i) {
            var row = rows[i]
            ancestryModel.append({
                path: row.version || row.path || "",
                parent: row.parent || "",
                actor: row.actor || "",
                time: row.created_at || row.time || "",
                note: row.note || "",
                removed: row.removed === true,
                superseded: row.superseded === true
            })
        }
        graphSelectedIndex = graphIndexForPath(selectedVersion)
        if (typeof graphPanel !== "undefined" && graphPanel !== null)
            Qt.callLater(graphPanel.fitGraph)
    }

    function populateRenewStatus(json) {
        renewStatusModel.clear()
        var rows = json.files || []
        for (var i = 0; i < rows.length; ++i) {
            renewStatusModel.append({
                file: rows[i].file || "",
                state: rows[i].state || "",
                hash: rows[i].hash || "",
                lastAccess: rows[i].last_access || "",
                refcount: rows[i].refcount === null || rows[i].refcount === undefined
                          ? "" : String(rows[i].refcount)
            })
        }
        statusMessage = root.i18n.text("Retention status loaded for ", "保留狀態已載入：", "保留状态已载入：") + (json.version || selectedVersion)
    }

    function loadRenewStatus() {
        if (selectedVersion.length === 0) {
            statusMessage = root.i18n.text("Select a version first", "請先選取目前版本", "请先选取当前版本")
            return
        }
        renewStatusRunner.runJson(["renew-status", selectedVersion])
    }

    function runRenewVersion() {
        if (selectedVersion.length === 0) {
            statusMessage = root.i18n.text("Select a version first", "請先選取目前版本", "请先选取当前版本")
            return
        }
        statusMessage = root.i18n.text("Renewing all contents of ", "續期所有內容中：", "续期所有内容中：") + selectedVersion
        actionRunner.runJson(["renew", "--in", selectedVersion])
    }

    function runDeleteVersion() {
        if (selectedVersion.length === 0) {
            statusMessage = root.i18n.text("Select a version first", "請先選取目前版本", "请先选取当前版本")
            return
        }
        statusMessage = root.i18n.text("Deleting ", "刪除中：", "删除中：") + selectedVersion
        actionRunner.runJson(["delete", selectedVersion])
    }

    function focusVersion(version) {
        selectedVersion = version
        // Reset stale per-version file view so a filter from the previous version does not
        // hide the new version's files, and dismiss stale overlays. View mode (dagMode),
        // the project-tree filter, and the compare basket are intentionally preserved.
        fileFilter = ""
        fileFilterMode = "all"
        selectedFile = ""
        // Clear stale checked-out state until the new version's status confirms it; this
        // prevents file actions from running against a previous version's working copy.
        selectedWorkingCopy = ""
        filesModel.clear()
        filesViewModel.clear()
        fileDetailPopover.close()
        metadataPopover.close()
        // The version graph is shown only on an explicit Refresh graph; drop the
        // previous version's graph so it is not read as this version's.
        ancestryModel.clear()
        ancestryLoading = false
        ancestryError = ""
        versionFocused(version)
        refreshFocusedStatus(false)
    }

    function addCompareVersion(version) {
        if (version.length === 0) return
        for (var i = 0; i < compareBasket.count; ++i) {
            if (compareBasket.get(i).path === version) return
        }
        if (compareBasket.count >= 2) compareBasket.remove(0)
        compareBasket.append({ path: version })
    }

    function runComparison() {
        compareResultModel.clear()
        if (compareBasket.count !== 2) {
            statusMessage = root.i18n.text("Explicitly select two versions first", "請先明確選兩個版本", "请先明确选两个版本")
            return
        }
        var left = splitVersion(compareBasket.get(0).path)
        var right = splitVersion(compareBasket.get(1).path)
        var fields = ["phase", "block", "stage", "version"]
        for (var i = 0; i < fields.length; ++i) {
            var field = fields[i]
            compareResultModel.append({
                field: field,
                left: left[field],
                right: right[field],
                changed: left[field] !== right[field]
            })
        }
        statusMessage = root.i18n.text("Compared details for two user-selected versions; no relationship was inferred automatically",
                                       "已比較兩個使用者明選版本的詳細資訊；未自動推斷關係",
                                       "已比较两个用户明选版本的详细信息；未自动推断关系")
    }

    function splitVersion(path) {
        var parts = path.split("/")
        return {
            phase: parts.length > 0 ? parts[0] : "",
            block: parts.length > 1 ? parts[1] : "",
            stage: parts.length > 2 ? parts[2] : "",
            version: parts.length > 3 ? parts[3] : ""
        }
    }

    function versionLeaf(path) {
        var parts = String(path || "").split("/")
        return parts.length > 0 ? parts[parts.length - 1] : ""
    }

    function versionStage(path) {
        var parts = String(path || "").split("/")
        return parts.length > 2 ? parts[2] : ""
    }

    function graphNodeSubtitle(path) {
        var s = splitVersion(path)
        return [s.phase, s.block, s.stage].filter(function(p) { return p.length > 0 }).join(" · ")
    }

    function selectedBranch() {
        for (var i = 0; i < branchesModel.count; ++i) {
            var row = branchesModel.get(i)
            if (row.path === selectedVersion)
                return row
        }
        return null
    }

    function selectedBranchRows() {
        var row = selectedBranch()
        if (!row) {
            return [
                { key: root.i18n.text("Path", "路徑", "路径"), value: selectedVersion },
                { key: root.i18n.text("Stage", "階段", "阶段"), value: versionStage(selectedVersion) }
            ]
        }
        return [
            { key: root.i18n.text("Phase", "Phase", "Phase"), value: row.phase },
            { key: root.i18n.text("Block", "Block", "Block"), value: row.block },
            { key: root.i18n.text("Stage", "階段", "阶段"), value: row.stage },
            { key: root.i18n.text("Version", "Version", "Version"), value: row.version },
            { key: root.i18n.text("Full path", "完整路徑", "完整路径"), value: row.path },
            { key: root.i18n.text("Status", "狀態", "状态"),
              value: row.removed ? root.i18n.text("Removed", "已移除", "已移除") :
                     (row.isHead ? root.i18n.text("Latest", "最新", "最新") :
                                   root.i18n.text("Available", "可用", "可用")) }
        ]
    }

    function ancestryRows(index) {
        if (index < 0 || index >= ancestryModel.count)
            return []
        var row = ancestryModel.get(index)
        return [
            { key: root.i18n.text("Parent", "Parent（上一版）", "Parent（上一版）"),
              value: row.parent.length > 0 ? row.parent : "ROOT" },
            { key: root.i18n.text("Created by", "建立者", "创建者"), value: row.actor },
            { key: root.i18n.text("Created at", "建立時間", "创建时间"), value: row.time },
            { key: root.i18n.text("Note", "備註", "备注"), value: row.note }
        ]
    }

    function ancestryStatus(index) {
        if (index < 0 || index >= ancestryModel.count)
            return root.i18n.text("Unknown", "未知", "未知")
        var row = ancestryModel.get(index)
        if (row.removed)
            return root.i18n.text("Removed", "已移除", "已移除")
        if (row.superseded)
            return root.i18n.text("Updated", "已更新", "已更新")
        if (row.path === selectedVersion)
            return root.i18n.text("Current", "目前", "当前")
        return root.i18n.text("Available", "可用", "可用")
    }

    function selectedGraphRows() {
        if (graphSelectedIndex < 0 || graphSelectedIndex >= ancestryModel.count)
            return [
                { key: root.i18n.text("Version", "Version", "Version"), value: selectedVersion },
                { key: root.i18n.text("Status", "狀態", "状态"),
                  value: selectedVersion.length > 0
                         ? root.i18n.text("No graph data", "沒有版本關係資料", "没有版本关系资料")
                         : root.i18n.text("No version selected", "尚未選擇版本", "尚未选择版本") }
            ]
        var row = ancestryModel.get(graphSelectedIndex)
        return [
            { key: root.i18n.text("Version", "Version", "Version"), value: row.path },
            { key: root.i18n.text("Parent", "Parent（上一版）", "Parent（上一版）"),
              value: row.parent.length > 0 ? row.parent : "ROOT" },
            { key: root.i18n.text("Created by", "建立者", "创建者"), value: row.actor },
            { key: root.i18n.text("Created at", "建立時間", "创建时间"), value: row.time },
            { key: root.i18n.text("Note", "備註", "备注"), value: row.note },
            { key: root.i18n.text("Status", "狀態", "状态"), value: ancestryStatus(graphSelectedIndex) }
        ]
    }

    function graphIndexForPath(path) {
        for (var i = 0; i < ancestryModel.count; ++i) {
            if (ancestryModel.get(i).path === path)
                return i
        }
        return -1
    }

    function graphParentIndex(index) {
        if (index < 0 || index >= ancestryModel.count)
            return -1
        var parent = ancestryModel.get(index).parent
        return graphIndexForPath(parent)
    }

    function graphDepth(index) {
        var depth = 0
        var cursor = index
        var guard = 0
        while (cursor >= 0 && guard < ancestryModel.count + 1) {
            var parent = graphParentIndex(cursor)
            if (parent < 0)
                break
            depth += 1
            cursor = parent
            guard += 1
        }
        return depth
    }

    function graphSiblingCount(parentPath) {
        var count = 0
        for (var i = 0; i < ancestryModel.count; ++i) {
            if ((ancestryModel.get(i).parent || "") === (parentPath || ""))
                count += 1
        }
        return Math.max(1, count)
    }

    function graphSiblingIndex(index) {
        if (index < 0 || index >= ancestryModel.count)
            return 0
        var parentPath = ancestryModel.get(index).parent || ""
        var sibling = 0
        for (var i = 0; i < index; ++i) {
            if ((ancestryModel.get(i).parent || "") === parentPath)
                sibling += 1
        }
        return sibling
    }

    // Horizontal/diagonal layout: a linear child->parent chain reads left-to-right
    // (root on the left, newest on the right) with a gentle downward slope; siblings
    // of the same parent fan out vertically. This keeps nodes readable rather than
    // stacking them into a tall vertical column.
    readonly property int graphDepthSpacing: 300
    readonly property int graphSiblingSpacing: 150
    readonly property int graphDiagonalStep: 24
    readonly property int graphNodeW: 204
    readonly property int graphNodeHalfH: 46

    function graphNodeX(index) {
        if (index < 0)
            return 60
        return 60 + graphDepth(index) * graphDepthSpacing
    }

    function graphNodeY(index) {
        if (index < 0)
            return 110
        var parentPath = ancestryModel.get(index).parent || ""
        var siblings = graphSiblingCount(parentPath)
        var sibling = graphSiblingIndex(index)
        var base = 110 + graphDepth(index) * graphDiagonalStep
        return base + (sibling - (siblings - 1) / 2.0) * graphSiblingSpacing
    }

    function graphContentWidth() {
        var maxX = 600
        for (var i = 0; i < ancestryModel.count; ++i)
            maxX = Math.max(maxX, graphNodeX(i) + graphNodeW + 80)
        return maxX
    }

    function graphContentHeight() {
        var maxY = 360
        var minY = 0
        for (var i = 0; i < ancestryModel.count; ++i) {
            maxY = Math.max(maxY, graphNodeY(i) + 100 + 60)
            minY = Math.min(minY, graphNodeY(i))
        }
        return maxY - Math.min(0, minY)
    }

    function selectGraphNode(index, openPopover) {
        if (index < 0 || index >= ancestryModel.count)
            return
        graphSelectedIndex = index
        if (openPopover)
            showMetadata(index)
    }

    function urlToPath(fileUrl) {
        var text = decodeURIComponent(String(fileUrl))
        return text.replace(/^file:\/\//, "")
    }

    function shortHash(hash) {
        var text = String(hash || "")
        return text.length > 12 ? text.substring(0, 12) : text
    }

    function prepareCheckoutAncestor() {
        if (selectedVersion.length === 0) {
            statusMessage = root.i18n.text("Select a version first", "請先選取目前版本", "请先选取当前版本")
            return
        }
        ancestryRunner.runJson(["ancestry", selectedVersion])
        checkoutAncestorConfirm.open()
    }

    function runCheckoutAncestor() {
        var args = ["checkout-ancestor", selectedVersion, "--depth", String(depthSpin.value), "--yes"]
        actionRunner.runJson(args)
    }

    function selectedCreateParentRaw() {
        if (createParentCombo.currentIndex < 0 ||
                createParentCombo.currentIndex >= createParentModel.count) {
            return "__FOCUS__"
        }
        return createParentModel.get(createParentCombo.currentIndex).value || "__FOCUS__"
    }

    function selectedCreateParent() {
        var parent = selectedCreateParentRaw()
        if (parent === "__FOCUS__")
            return selectedVersion
        return parent
    }

    function createParentReady() {
        return selectedCreateParent().length > 0
    }

    function runCreateChild() {
        var newVersion = childVersion.text.trim()
        var parent = selectedCreateParent()
        if (newVersion.length === 0) {
            statusMessage = root.i18n.text("Enter a new version", "請輸入新 version", "请输入新 version")
            return
        }
        if (parent.length === 0) {
            statusMessage = root.i18n.text("Select a version, ROOT, or another explicit parent first",
                                           "請先選目前版本，或明確選 ROOT/其他 parent",
                                           "请先选当前版本，或明确选 ROOT/其他 parent")
            return
        }
        actionRunner.runJson(["create", newVersion, "--parent", parent])
    }

    function prepareCreateChild() {
        var newVersion = childVersion.text.trim()
        var parent = selectedCreateParent()
        if (newVersion.length === 0) {
            statusMessage = root.i18n.text("Enter a new version", "請輸入新 version", "请输入新 version")
            return
        }
        if (parent.length === 0) {
            statusMessage = root.i18n.text("Select a version, ROOT, or another explicit parent first",
                                           "請先選目前版本，或明確選 ROOT/其他 parent",
                                           "请先选当前版本，或明确选 ROOT/其他 parent")
            return
        }
        createChildConfirm.open()
    }

    function showMetadata(index) {
        if (index < 0 || index >= ancestryModel.count) return
        var row = ancestryModel.get(index)
        graphSelectedIndex = index
        metadataPopover.versionPath = row.path
        metadataPopover.parentPath = row.parent
        metadataPopover.actor = row.actor
        metadataPopover.timestamp = row.time
        metadataPopover.note = row.note
        metadataPopover.status = ancestryStatus(index)
        metadataPopover.open()
    }

    Item {
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            color: root.theme.bgBase
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: root.theme.gapSm
            spacing: root.theme.gapSm

            // ── Compact workspace header ──
            Common.ShellPanel {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                theme: root.theme
                elevated: true

                RowLayout {
                    anchors.fill: parent
                    spacing: root.theme.gapMd

                    Label {
                        text: root.i18n.text("Workspace", "工作區", "工作区")
                        color: root.theme.textPrimary
                        font.pixelSize: root.theme.fontSection
                        font.bold: true
                        Layout.preferredWidth: 92
                        elide: Text.ElideRight
                    }

                    Common.VersionPathPill {
                        Layout.fillWidth: true
                        path: root.selectedVersion.length > 0
                              ? root.selectedVersion
                              : root.i18n.text("No version selected", "尚未選擇版本", "尚未选择版本")
                        selected: root.selectedVersion.length > 0
                        showStageDot: root.selectedVersion.length > 0
                        theme: root.theme
                        onClicked: if (root.selectedVersion.length > 0) root.focusVersion(root.selectedVersion)
                    }

                    Common.StatusDot {
                        theme: root.theme
                        state: root.selectedVersion.length > 0 ? "active" : "neutral"
                        label: root.selectedVersion.length > 0
                               ? root.i18n.text("Selected", "已選取", "已选择")
                               : root.i18n.text("Idle", "待命", "待命")
                        Layout.preferredWidth: 104
                    }

                    Common.ActionButton {
                        text: root.i18n.text("Refresh", "刷新", "刷新")
                        theme: root.theme
                        kind: "secondary"
                        onClicked: root.refreshAll()
                    }
                }
            }

            // ── Body: project tree | version graph (+ files) | inspector ──
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: root.theme.gapSm

// LEFT: project / version tree module
                Common.ShellPanel {
                    Layout.preferredWidth: 256
                    Layout.minimumWidth: 240
                    Layout.maximumWidth: 270
                    Layout.fillHeight: true
                    theme: root.theme
                    elevated: false

                    ProjectVersionTree {
                        objectName: "projectVersionTree"
                        anchors.fill: parent
                        theme: root.theme
                        i18n: root.i18n
                        branches: root.branchesData
                        selectedVersion: root.selectedVersion
                        projectPath: root.projectPath
                        onVersionSelected: function(versionPath) { root.focusVersion(versionPath) }
                        onShowInGraphRequested: function(versionPath) { root.focusVersion(versionPath) }
                        onCompareRequested: function(versionPath) { root.addCompareVersion(versionPath) }
                        onRefreshRequested: root.refreshAll()
                    }
                }

                // CENTER: version graph (dominant) + current-version files (secondary)
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: root.theme.gapSm

                    // ── local graph toolbar (view mode + navigation + refresh) ──
                    Common.ShellPanel {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 44
                        theme: root.theme
                        elevated: true

                        RowLayout {
                            anchors.fill: parent
                            spacing: root.theme.gapSm

                            Label {
                                text: root.i18n.text("Version graph", "版本關係圖", "版本关系图")
                                color: root.theme.textPrimary
                                font.pixelSize: root.theme.fontSection
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            // segmented view-mode control (local; replaces the old global shell switch)
                            Rectangle {
                                Layout.preferredHeight: 28
                                Layout.preferredWidth: viewModeRow.implicitWidth + 4
                                radius: root.theme.radius
                                color: root.theme.bgElevated
                                border.color: root.theme.border
                                RowLayout {
                                    id: viewModeRow
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    spacing: 2
                                    Common.ActionButton {
                                        theme: root.theme
                                        kind: !root.dagMode ? "primary" : "ghost"
                                        text: root.i18n.text("Graph + files", "圖譜＋檔案", "图谱＋文件")
                                        onClicked: root.dagMode = false
                                    }
                                    Common.ActionButton {
                                        theme: root.theme
                                        kind: root.dagMode ? "primary" : "ghost"
                                        text: root.i18n.text("Graph only", "只看圖譜", "只看图谱")
                                        onClicked: root.dagMode = true
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Common.ActionButton {
                                theme: root.theme
                                kind: "secondary"
                                text: root.i18n.text("Fit", "符合視窗", "适合窗口")
                                enabled: ancestryModel.count > 0
                                onClicked: graphPanel.fitGraph()
                            }
                            Common.ActionButton {
                                theme: root.theme
                                kind: "secondary"
                                text: root.i18n.text("Center selected", "置中選取", "居中选取")
                                enabled: ancestryModel.count > 0
                                onClicked: graphPanel.centerSelected()
                            }
                            Common.ActionButton {
                                theme: root.theme
                                kind: "ghost"
                                text: root.i18n.text("Reset", "重設", "重置")
                                onClicked: graphPanel.resetGraph()
                            }

                            // zoom cluster
                            Common.ActionButton {
                                theme: root.theme
                                kind: "ghost"
                                text: "−"
                                Layout.preferredWidth: 30
                                onClicked: graphPanel.zoomBy(0.87)
                            }
                            Label {
                                text: Math.round(graphPanel.graphScale * 100) + "%"
                                color: root.theme.textSecondary
                                font.family: root.theme.monoFont
                                font.pixelSize: root.theme.fontData
                                horizontalAlignment: Text.AlignHCenter
                                Layout.preferredWidth: 44
                            }
                            Common.ActionButton {
                                theme: root.theme
                                kind: "ghost"
                                text: "＋"
                                Layout.preferredWidth: 30
                                onClicked: graphPanel.zoomBy(1.15)
                            }

                            Common.ActionButton {
                                theme: root.theme
                                kind: "ghost"
                                text: root.i18n.text("Labels", "標籤", "标签")
                                onClicked: root.graphShowNotes = !root.graphShowNotes
                            }
                            Common.ActionButton {
                                theme: root.theme
                                kind: "secondary"
                                text: root.i18n.text("Refresh graph", "重新整理圖譜", "刷新图谱")
                                enabled: root.selectedVersion.length > 0
                                onClicked: root.refreshGraph()
                            }
                        }
                    }

                    // dominant graph canvas
                    Common.ShellPanel {
                        id: graphPanel
                        objectName: "graphPanel"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        theme: root.theme
                        property real graphScale: 1.0
                        property real minScale: 0.4
                        property real maxScale: 2.5

                        function zoomBy(factor) {
                            graphScale = Math.max(minScale, Math.min(maxScale, graphScale * factor))
                        }

                        function resetGraph() {
                            graphScale = 1.0
                            Qt.callLater(centerSelected)
                        }

                        function clampContentX(v) {
                            return Math.max(0, Math.min(v, graphFlick.contentWidth - graphFlick.width))
                        }
                        function clampContentY(v) {
                            return Math.max(0, Math.min(v, graphFlick.contentHeight - graphFlick.height))
                        }

                        // Center the active node (graph-selected, else current version) in the
                        // visible core at the current zoom.
                        function centerSelected() {
                            var idx = root.graphSelectedIndex
                            if (idx < 0)
                                idx = root.graphIndexForPath(root.selectedVersion)
                            if (idx < 0)
                                return
                            var cx = (root.graphNodeX(idx) + root.graphNodeW / 2) * graphScale - graphFlick.width / 2
                            var cy = (root.graphNodeY(idx) + root.graphNodeHalfH) * graphScale - graphFlick.height / 2
                            graphFlick.contentX = clampContentX(cx)
                            graphFlick.contentY = clampContentY(cy)
                        }

                        // Readable fit: scale so the graph fits, but never below a per-mode
                        // floor (allow panning when the graph is larger than the viewport) and
                        // never above 1.0. Then center the selected node.
                        function fitGraph() {
                            var floor = root.dagMode ? 0.70 : 0.62
                            var sx = graphFlick.width / Math.max(1, root.graphContentWidth())
                            var sy = graphFlick.height / Math.max(1, root.graphContentHeight())
                            var fit = Math.min(sx, sy) * 0.94
                            graphScale = Math.max(floor, Math.min(1.0, fit))
                            Qt.callLater(centerSelected)
                        }

                        Flickable {
                            id: graphFlick
                            anchors.fill: parent
                            clip: true
                            contentWidth: graphContent.width * graphPanel.graphScale
                            contentHeight: graphContent.height * graphPanel.graphScale
                            boundsBehavior: Flickable.StopAtBounds
                            interactive: true

                            Rectangle {
                                id: graphContent
                                width: Math.max(graphFlick.width, root.graphContentWidth())
                                height: Math.max(graphFlick.height, root.graphContentHeight())
                                color: "transparent"
                                scale: graphPanel.graphScale
                                transformOrigin: Item.TopLeft

                                Canvas {
                                    id: edgeCanvas
                                    anchors.fill: parent
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.reset()
                                        ctx.clearRect(0, 0, width, height)
                                        for (var i = 0; i < ancestryModel.count; ++i) {
                                            var parentIndex = root.graphParentIndex(i)
                                            if (parentIndex < 0)
                                                continue
                                            var parentRow = ancestryModel.get(parentIndex)
                                            var childRow = ancestryModel.get(i)
                                            var selectedEdge = i === root.graphSelectedIndex ||
                                                               parentIndex === root.graphSelectedIndex ||
                                                               childRow.path === root.selectedVersion
                                            var mutedEdge = parentRow.removed || parentRow.superseded ||
                                                            childRow.removed || childRow.superseded
                                            // Horizontal flow: parent sits left of child, so
                                            // connect parent right-edge to child left-edge.
                                            var x1 = root.graphNodeX(parentIndex) + root.graphNodeW
                                            var y1 = root.graphNodeY(parentIndex) + root.graphNodeHalfH
                                            var x2 = root.graphNodeX(i)
                                            var y2 = root.graphNodeY(i) + root.graphNodeHalfH
                                            var dx = Math.max(40, (x2 - x1) * 0.45)
                                            ctx.strokeStyle = selectedEdge ? root.theme.accent :
                                                              (mutedEdge ? root.theme.textSecondary : root.theme.border)
                                            ctx.globalAlpha = mutedEdge ? 0.45 : (selectedEdge ? 0.95 : 0.7)
                                            ctx.lineWidth = selectedEdge ? 3 : 2
                                            ctx.beginPath()
                                            ctx.moveTo(x1, y1)
                                            ctx.bezierCurveTo(x1 + dx, y1, x2 - dx, y2, x2, y2)
                                            ctx.stroke()
                                        }
                                        ctx.globalAlpha = 1.0
                                    }
                                    Connections {
                                        target: ancestryModel
                                        function onCountChanged() { edgeCanvas.requestPaint() }
                                    }
                                    Connections {
                                        target: root
                                        function onGraphSelectedIndexChanged() { edgeCanvas.requestPaint() }
                                        function onSelectedVersionChanged() { edgeCanvas.requestPaint() }
                                    }
                                }

                                Repeater {
                                    model: ancestryModel
                                    Common.GraphNodeCard {
                                        x: root.graphNodeX(index)
                                        y: root.graphNodeY(index)
                                        width: 204
                                        height: index === root.graphSelectedIndex ? 100 : 92
                                        theme: root.theme
                                        i18n: root.i18n
                                        versionPath: model.path
                                        versionLabel: root.versionLeaf(model.path)
                                        subtitle: root.graphNodeSubtitle(model.path)
                                        stage: root.versionStage(model.path)
                                        actor: model.actor
                                        timestamp: model.time
                                        note: model.note
                                        showStage: root.graphShowStage
                                        showNotes: root.graphShowNotes
                                        selected: index === root.graphSelectedIndex
                                        current: model.path === root.selectedVersion
                                        removed: model.removed
                                        superseded: model.superseded
                                        onClicked: root.selectGraphNode(index, true)
                                        onRightClicked: function(versionPath) {
                                            root.graphMenuIndex = index
                                            root.graphMenuPath = versionPath
                                            graphNodeMenu.popup()
                                        }
                                    }
                                }

                                // empty / loading / error states
                                Common.EmptyState {
                                    anchors.centerIn: parent
                                    visible: ancestryModel.count === 0 && !root.ancestryLoading && root.ancestryError.length === 0
                                    theme: root.theme
                                    title: root.selectedVersion.length > 0
                                           ? root.i18n.text("No version graph available", "沒有可用的版本關係圖", "没有可用的版本关系图")
                                           : root.i18n.text("No version selected", "尚未選擇版本", "尚未选择版本")
                                    message: root.selectedVersion.length > 0
                                             ? root.i18n.text("Use Refresh graph to show this version's derivation chain.",
                                                              "按「重新整理圖譜」顯示這個版本的衍生關係。",
                                                              "按“刷新图谱”显示这个版本的衍生关系。")
                                             : root.i18n.text("Select a version, then use Refresh graph.",
                                                              "請先選擇版本，再按「重新整理圖譜」。",
                                                              "请先选择版本，再按“刷新图谱”。")
                                }

                                ColumnLayout {
                                    anchors.centerIn: parent
                                    spacing: root.theme.gapSm
                                    visible: root.ancestryLoading
                                    Common.BusyIndicator { running: root.ancestryLoading; Layout.alignment: Qt.AlignHCenter }
                                    Label {
                                        text: root.i18n.text("Loading version graph...", "正在載入版本關係圖…", "正在加载版本关系图…")
                                        color: root.theme.textSecondary
                                        font.pixelSize: root.theme.fontBody
                                        Layout.alignment: Qt.AlignHCenter
                                    }
                                }

                                ColumnLayout {
                                    anchors.centerIn: parent
                                    width: 360
                                    spacing: root.theme.gapSm
                                    visible: root.ancestryError.length > 0 && !root.ancestryLoading
                                    Label {
                                        text: root.i18n.text("Unable to load version graph", "無法載入版本關係圖", "无法加载版本关系图")
                                        color: root.theme.accentDanger
                                        font.pixelSize: root.theme.fontSection
                                        font.bold: true
                                        Layout.alignment: Qt.AlignHCenter
                                    }
                                    Label {
                                        text: root.ancestryError
                                        color: root.theme.textSecondary
                                        font.pixelSize: root.theme.fontData
                                        wrapMode: Text.WrapAnywhere
                                        horizontalAlignment: Text.AlignHCenter
                                        Layout.fillWidth: true
                                    }
                                    Common.ActionButton {
                                        theme: root.theme
                                        kind: "secondary"
                                        text: root.i18n.text("Refresh graph", "重新整理圖譜", "刷新图谱")
                                        Layout.alignment: Qt.AlignHCenter
                                        onClicked: root.refreshGraph()
                                    }
                                }
                            }
                        }

                        WheelHandler {
                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            onWheel: function(event) {
                                graphPanel.zoomBy(event.angleDelta.y > 0 ? 1.1 : 0.9)
                                event.accepted = true
                            }
                        }
                    }

// ── secondary Current Version Files panel ──
                    Common.ShellPanel {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 244
                        visible: !root.dagMode
                        theme: root.theme

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: root.theme.gapSm

                            // toolbar: title + version pill + counts
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: root.theme.gapSm

                                Label {
                                    text: root.i18n.text("Current Version Files", "目前版本檔案", "当前版本文件")
                                    color: root.theme.textPrimary
                                    font.pixelSize: root.theme.fontSection
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Common.VersionPathPill {
                                    visible: root.selectedVersion.length > 0
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    path: root.selectedVersion
                                    showStageDot: true
                                    selected: false
                                }
                                Label {
                                    visible: root.selectedVersion.length > 0 && filesModel.count > 0
                                    text: root.i18n.text("Total ", "總數 ", "总数 ") + root.fileCount("total")
                                          + " · " + root.i18n.text("Linked ", "LINKED ", "LINKED ") + root.fileCount("linked")
                                          + " · " + root.i18n.text("Local ", "LOCAL ", "LOCAL ") + root.fileCount("local")
                                          + " · " + root.i18n.text("Changed ", "變更 ", "变更 ") + root.fileCount("changed")
                                    color: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                }
                            }

                            // filter row: filter box + display-only state filters + refresh
                            RowLayout {
                                visible: root.selectedVersion.length > 0 && filesModel.count > 0
                                Layout.fillWidth: true
                                spacing: root.theme.gapXs

                                TextField {
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 220
                                    text: root.fileFilter
                                    placeholderText: root.i18n.text("Filter files", "篩選檔案", "筛选文件")
                                    color: root.theme.textPrimary
                                    placeholderTextColor: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                    onTextChanged: { root.fileFilter = text; root.rebuildFilesView() }
                                    background: Rectangle {
                                        color: root.theme.bgElevated
                                        border.color: root.theme.border
                                        radius: root.theme.radius
                                    }
                                }
                                Common.ActionButton {
                                    theme: root.theme
                                    kind: root.fileFilterMode === "all" ? "primary" : "ghost"
                                    text: root.i18n.text("All", "全部", "全部")
                                    onClicked: { root.fileFilterMode = "all"; root.rebuildFilesView() }
                                }
                                Common.ActionButton {
                                    theme: root.theme
                                    kind: root.fileFilterMode === "changed" ? "primary" : "ghost"
                                    text: root.i18n.text("Changed", "變更", "变更")
                                    onClicked: { root.fileFilterMode = "changed"; root.rebuildFilesView() }
                                }
                                Common.ActionButton {
                                    theme: root.theme
                                    kind: root.fileFilterMode === "linked" ? "primary" : "ghost"
                                    text: "LINKED"
                                    onClicked: { root.fileFilterMode = "linked"; root.rebuildFilesView() }
                                }
                                Common.ActionButton {
                                    theme: root.theme
                                    kind: root.fileFilterMode === "local" ? "primary" : "ghost"
                                    text: "LOCAL"
                                    onClicked: { root.fileFilterMode = "local"; root.rebuildFilesView() }
                                }
                                Item { Layout.fillWidth: true }
                                Common.ActionButton {
                                    theme: root.theme
                                    kind: "secondary"
                                    text: root.i18n.text("Refresh files", "重新整理檔案", "刷新文件")
                                    enabled: root.selectedVersion.length > 0 && !root.actionsBusy
                                    onClicked: statusRunner.runJson(["status", root.selectedVersion])
                                }
                            }

                            // column header
                            RowLayout {
                                visible: root.selectedVersion.length > 0 && filesViewModel.count > 0
                                Layout.fillWidth: true
                                spacing: root.theme.gapSm

                                Label {
                                    text: root.i18n.text("File", "檔案", "文件")
                                    color: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: root.i18n.text("SVN", "SVN", "SVN") + " · " + root.i18n.text("Link", "Link", "Link")
                                    color: root.theme.textSecondary
                                    font.pixelSize: root.theme.fontData
                                    Layout.preferredWidth: 150
                                }
                            }

                            Common.EmptyState {
                                visible: root.selectedVersion.length === 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                theme: root.theme
                                title: root.i18n.text("No version selected", "尚未選擇版本", "尚未选择版本")
                                message: root.i18n.text("Select a version to inspect its files.",
                                                       "請選擇版本以查看檔案。",
                                                       "请选择版本以查看文件。")
                            }

                            Common.EmptyState {
                                visible: root.selectedVersion.length > 0 && filesModel.count === 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                theme: root.theme
                                title: root.i18n.text("No files in this version", "此版本沒有檔案", "此版本没有文件")
                                message: root.i18n.text("Refresh status if this looks stale.",
                                                       "如果資訊看起來過舊，請刷新狀態。",
                                                       "如果信息看起来过旧，请刷新状态。")
                                actionText: root.i18n.text("Refresh", "刷新", "刷新")
                                onActionRequested: statusRunner.runJson(["status", root.selectedVersion])
                            }

                            Common.EmptyState {
                                visible: root.selectedVersion.length > 0 && filesModel.count > 0 && filesViewModel.count === 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                theme: root.theme
                                title: root.i18n.text("No matching files", "沒有符合的檔案", "没有匹配的文件")
                                message: root.i18n.text("Adjust the filter to see files.",
                                                       "調整篩選條件以顯示檔案。",
                                                       "调整筛选条件以显示文件。")
                                actionText: root.i18n.text("Show all", "顯示全部", "显示全部")
                                onActionRequested: { root.fileFilter = ""; root.fileFilterMode = "all"; root.rebuildFilesView() }
                            }

                            ListView {
                                visible: root.selectedVersion.length > 0 && filesViewModel.count > 0
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: filesViewModel
                                spacing: 1
                                ScrollBar.vertical: ScrollBar {}
                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: 30
                                    color: root.selectedFile === model.path ? root.theme.bgElevated
                                           : (rowHover.hovered ? root.theme.bgElevated : "transparent")
                                    border.color: root.selectedFile === model.path ? root.theme.accent : "transparent"
                                    radius: root.theme.radius

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: root.theme.gapSm
                                        anchors.rightMargin: root.theme.gapSm
                                        spacing: root.theme.gapSm

                                        TextEdit {
                                            text: model.path
                                            color: root.theme.textPrimary
                                            font.family: root.theme.monoFont
                                            font.pixelSize: root.theme.fontData
                                            readOnly: true
                                            selectByMouse: true
                                            persistentSelection: true
                                            textFormat: TextEdit.PlainText
                                            wrapMode: TextEdit.NoWrap
                                            selectionColor: root.theme.accent
                                            clip: true
                                            Layout.fillWidth: true
                                        }

                                        Common.FileStateBadge {
                                            theme: root.theme
                                            i18n: root.i18n
                                            linkState: model.linkState
                                            svnState: model.svnState
                                            Layout.preferredWidth: 150
                                        }
                                    }

                                    HoverHandler { id: rowHover }
                                    TapHandler {
                                        acceptedButtons: Qt.LeftButton
                                        onTapped: root.selectedFile = model.path
                                    }
                                    TapHandler {
                                        acceptedButtons: Qt.RightButton
                                        onTapped: {
                                            root.selectedFile = model.path
                                            root.fileMenuPath = model.path
                                            root.fileMenuSvn = model.svnState
                                            root.fileMenuLink = model.linkState
                                            fileRowMenu.popup()
                                        }
                                    }
                                }
                            }

                        }
                    }
                }

// RIGHT: version inspector / action drawer
                Common.ShellPanel {
                    Layout.preferredWidth: 288
                    Layout.minimumWidth: 260
                    Layout.maximumWidth: 300
                    Layout.fillHeight: true
                    theme: root.theme
                    title: root.i18n.text("Version Inspector", "版本檢視", "版本查看")
                    subtitle: root.i18n.text("Metadata and explicit actions", "資訊與明確操作", "信息与明确操作")

                    ScrollView {
                        id: inspectorScroll
                        anchors.fill: parent
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: inspectorScroll.availableWidth
                            spacing: root.theme.gapSm

                            // ── Header: which version am I looking at ──
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: root.theme.gapXs

                                Common.VersionPathPill {
                                    visible: root.selectedVersion.length > 0
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    path: root.selectedVersion
                                    selected: true
                                    showStageDot: true
                                }

                                Common.StatusDot {
                                    visible: root.selectedVersion.length > 0
                                    theme: root.theme
                                    state: root.actionsBusy ? "active"
                                           : (root.selectedVersion.length > 0 ? "ok" : "neutral")
                                    label: root.actionsBusy
                                           ? root.i18n.text("Working...", "處理中…", "处理中…")
                                           : root.i18n.text("Stage: ", "階段：", "阶段：") + root.versionStage(root.selectedVersion)
                                    Layout.fillWidth: true
                                }

                                Common.EmptyState {
                                    visible: root.selectedVersion.length === 0
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    title: root.i18n.text("Select a version", "請選取版本", "请选择版本")
                                    message: root.i18n.text("Select a version from the project tree.",
                                                           "請從專案版本樹選取版本。",
                                                           "请从项目版本树选择版本。")
                                }
                            }

                            Common.InspectorSection {
                                visible: root.selectedVersion.length > 0
                                Layout.fillWidth: true
                                theme: root.theme
                                title: root.i18n.text("Metadata", "元資訊", "元信息")
                                collapsible: false

                                Common.KeyValueGrid {
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    monoValue: true
                                    keyWidth: 78
                                    rows: root.selectedBranchRows()
                                }
                            }

                            Common.InspectorSection {
                                visible: root.selectedVersion.length > 0
                                Layout.fillWidth: true
                                theme: root.theme
                                title: root.i18n.text("Safe Actions", "安全操作", "安全操作")
                                collapsible: false

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.theme.gapSm

                                    Common.ActionButton {
                                        Layout.fillWidth: true
                                        text: root.i18n.text("checkout", "checkout", "checkout")
                                        theme: root.theme
                                        kind: "primary"
                                        enabled: root.selectedVersion.length > 0 && !root.actionsBusy
                                        onClicked: actionRunner.runJson(["checkout", root.selectedVersion])
                                    }
                                    Common.ActionButton {
                                        Layout.fillWidth: true
                                        text: root.i18n.text("Edit", "編輯", "编辑")
                                        theme: root.theme
                                        kind: "secondary"
                                        enabled: root.selectedVersion.length > 0 && root.selectedWorkingCopy.length > 0 && !root.actionsBusy
                                        onClicked: {
                                            fileActionDialog.fileOp = "edit"
                                            fileActionDialog.open()
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.theme.gapSm

                                    Common.ActionButton {
                                        Layout.fillWidth: true
                                        text: root.i18n.text("checkin", "checkin", "checkin")
                                        theme: root.theme
                                        kind: "secondary"
                                        enabled: root.selectedVersion.length > 0 && root.selectedWorkingCopy.length > 0 && !root.actionsBusy
                                        onClicked: {
                                            fileActionDialog.fileOp = "checkin"
                                            fileActionDialog.open()
                                        }
                                    }
                                    Common.ActionButton {
                                        Layout.fillWidth: true
                                        text: root.i18n.text("Refresh", "刷新", "刷新")
                                        theme: root.theme
                                        kind: "secondary"
                                        enabled: root.selectedVersion.length > 0 && !root.actionsBusy
                                        onClicked: statusRunner.runJson(["status", root.selectedVersion])
                                    }
                                }

                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Copy path", "複製路徑", "复制路径")
                                    theme: root.theme
                                    kind: "ghost"
                                    enabled: root.selectedVersion.length > 0
                                    onClicked: {
                                        root.copyGraphPath(root.selectedVersion)
                                        root.statusMessage = root.i18n.text("Copied version path", "已複製版本路徑", "已复制版本路径")
                                    }
                                }
                            }

                            Common.InspectorSection {
                                visible: root.selectedVersion.length > 0
                                Layout.fillWidth: true
                                theme: root.theme
                                title: root.i18n.text("Version Actions", "版本操作", "版本操作")
                                tip: root.i18n.text("Get upstream and create actions run only after explicit confirmation.",
                                                    "取上游與建立版本都只會在明確確認後執行。",
                                                    "取上游与创建版本都只会在明确确认后执行。")
                                collapsible: false

                                TextField {
                                    id: childVersion
                                    objectName: "childVersionField"
                                    Layout.fillWidth: true
                                    placeholderText: root.i18n.text("phase/block/stage/version",
                                                                    "phase/block/stage/version",
                                                                    "phase/block/stage/version")
                                    color: root.theme.textPrimary
                                    placeholderTextColor: root.theme.textSecondary
                                    font.family: root.theme.monoFont
                                    background: Rectangle {
                                        color: root.theme.bgElevated
                                        border.color: root.theme.border
                                        radius: root.theme.radius
                                    }
                                }

                                ComboBox {
                                    id: createParentCombo
                                    objectName: "createParentCombo"
                                    Layout.fillWidth: true
                                    model: createParentModel
                                    textRole: "label"
                                    function displayLabel(value, label) {
                                        return value === "__FOCUS__"
                                            ? root.i18n.text("Use selected version as parent", "以目前版本為 parent", "以当前版本为 parent")
                                            : label
                                    }
                                    displayText: currentIndex >= 0 && currentIndex < createParentModel.count
                                        ? displayLabel(createParentModel.get(currentIndex).value,
                                                       createParentModel.get(currentIndex).label)
                                        : ""
                                    delegate: ItemDelegate {
                                        width: createParentCombo.width
                                        text: createParentCombo.displayLabel(model.value, model.label)
                                        highlighted: createParentCombo.highlightedIndex === index
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.theme.gapSm

                                    Common.ActionButton {
                                        Layout.fillWidth: true
                                        text: root.i18n.text("New derived version", "新衍生版本", "新衍生版本")
                                        theme: root.theme
                                        kind: "primary"
                                        enabled: childVersion.text.trim().length > 0 && root.createParentReady() && !root.actionsBusy
                                        onClicked: root.prepareCreateChild()
                                    }

                                    SpinBox {
                                        id: depthSpin
                                        from: 1
                                        to: 16
                                        value: 1
                                        Layout.preferredWidth: 78
                                        ToolTip.text: root.i18n.text("Depth", "層數", "层数")
                                    }
                                }

                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Get upstream", "取得上游版本", "获取上游版本")
                                    theme: root.theme
                                    kind: "secondary"
                                    enabled: root.selectedVersion.length > 0 && !root.actionsBusy
                                    onClicked: root.prepareCheckoutAncestor()
                                }

                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Delete version and contents", "刪除版本與內容", "删除版本与内容")
                                    theme: root.theme
                                    kind: "danger"
                                    dangerous: true
                                    enabled: root.selectedVersion.length > 0 && !root.actionsBusy
                                    onClicked: deleteVersionConfirm.open()
                                }
                            }

                            Common.InspectorSection {
                                visible: root.selectedVersion.length > 0
                                Layout.fillWidth: true
                                theme: root.theme
                                title: root.i18n.text("Retention status", "保留狀態", "保留状态")
                                tip: root.i18n.text("Shows whether files in this version are retained to avoid space cleanup.",
                                                    "顯示這個版本的檔案是否被保留、避免被空間回收清掉。",
                                                    "显示这个版本的文件是否被保留、避免被空间回收清掉。")
                                expanded: false

                                // Compact storage-only summary (retained vs local file counts).
                                Common.KeyValueGrid {
                                    visible: renewStatusModel.count > 0
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    keyWidth: 96
                                    rows: [
                                        { key: root.i18n.text("Retained files", "已保留檔案", "已保留文件"), value: root.retentionLinkedCount() },
                                        { key: root.i18n.text("Local files", "本機檔案", "本机文件"), value: root.retentionLocalCount() },
                                        { key: root.i18n.text("Total files", "檔案總數", "文件总数"), value: renewStatusModel.count }
                                    ]
                                }

                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Refresh", "刷新", "刷新")
                                    theme: root.theme
                                    kind: "secondary"
                                    enabled: root.selectedVersion.length > 0
                                    onClicked: root.loadRenewStatus()
                                }

                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Renew all contents", "續期所有內容", "续期所有内容")
                                    theme: root.theme
                                    kind: "secondary"
                                    enabled: root.selectedVersion.length > 0
                                    onClicked: root.runRenewVersion()
                                }

                                Common.EmptyState {
                                    visible: renewStatusModel.count === 0
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    title: root.i18n.text("No retention status loaded", "尚未載入保留狀態", "尚未载入保留状态")
                                    message: root.i18n.text("Refresh to inspect only the selected version.",
                                                           "刷新後只檢視目前版本。",
                                                           "刷新后只查看当前版本。")
                                }

                                Repeater {
                                    model: renewStatusModel

                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: retentionRow.implicitHeight + root.theme.gapSm * 2
                                        color: root.theme.bgElevated
                                        border.color: root.theme.border
                                        radius: root.theme.radius

                                        ColumnLayout {
                                            id: retentionRow
                                            anchors.fill: parent
                                            anchors.margins: root.theme.gapSm
                                            spacing: root.theme.gapXs

                                            TextEdit {
                                                text: model.file
                                                color: root.theme.textPrimary
                                                font.family: root.theme.monoFont
                                                font.pixelSize: root.theme.fontData
                                                readOnly: true
                                                selectByMouse: true
                                                persistentSelection: true
                                                textFormat: TextEdit.PlainText
                                                wrapMode: TextEdit.WrapAnywhere
                                                selectionColor: root.theme.accent
                                                Layout.fillWidth: true
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: root.theme.gapSm

                                                Common.StatusDot {
                                                    theme: root.theme
                                                    state: model.state === "LINKED" ? "ok" : "warn"
                                                    label: model.state === "LINKED"
                                                           ? root.i18n.text("retained", "已保留", "已保留")
                                                           : root.i18n.text("local", "本機", "本机")
                                                    Layout.fillWidth: true
                                                }
                                                Label {
                                                    text: root.shortHash(model.hash)
                                                    visible: model.hash.length > 0
                                                    color: root.theme.textSecondary
                                                    font.family: root.theme.monoFont
                                                    font.pixelSize: root.theme.fontData
                                                    elide: Text.ElideRight
                                                    Layout.preferredWidth: 92
                                                }
                                            }

                                            Common.KeyValueGrid {
                                                Layout.fillWidth: true
                                                theme: root.theme
                                                monoValue: false
                                                keyWidth: 74
                                                rows: [
                                                    { key: root.i18n.text("Last used", "最近使用", "最近使用"),
                                                      value: model.lastAccess.length > 0
                                                             ? model.lastAccess
                                                             : root.i18n.text("Not recorded", "未記錄", "未记录") },
                                                    { key: root.i18n.text("Count", "數量", "数量"),
                                                      value: model.refcount }
                                                ]
                                            }
                                        }
                                    }
                                }
                            }

                            // ── Compare basket: safe review state, not a repo operation ──
                            Common.InspectorSection {
                                visible: compareBasket.count > 0
                                Layout.fillWidth: true
                                theme: root.theme
                                title: root.i18n.text("Compare Basket", "比對籃", "比对篮")
                                collapsible: false

                                Repeater {
                                    model: compareBasket
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: root.theme.gapXs

                                        Common.VersionPathPill {
                                            Layout.fillWidth: true
                                            theme: root.theme
                                            path: (index + 1) + ". " + model.path
                                            showStageDot: false
                                            selected: false
                                        }
                                        Common.ActionButton {
                                            theme: root.theme
                                            kind: "ghost"
                                            text: root.i18n.text("Remove", "移除", "移除")
                                            onClicked: root.removeCompareVersion(index)
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.theme.gapSm

                                    Common.ActionButton {
                                        Layout.fillWidth: true
                                        text: root.i18n.text("Compare details", "比較詳細資訊", "比较详细信息")
                                        theme: root.theme
                                        kind: "secondary"
                                        enabled: compareBasket.count === 2
                                        onClicked: root.runComparison()
                                    }
                                    Common.ActionButton {
                                        text: root.i18n.text("Clear basket", "清空比對籃", "清空比对篮")
                                        theme: root.theme
                                        kind: "ghost"
                                        onClicked: root.clearCompareBasket()
                                    }
                                }

                                Repeater {
                                    model: compareResultModel
                                    Common.StatusDot {
                                        Layout.fillWidth: true
                                        theme: root.theme
                                        state: model.changed ? "warn" : "ok"
                                        label: model.field + ": " + model.left + " / " + model.right
                                    }
                                }
                            }

                            // ── More: low-risk secondary items only ──
                            Common.InspectorSection {
                                visible: root.selectedVersion.length > 0
                                Layout.fillWidth: true
                                theme: root.theme
                                title: root.i18n.text("More", "更多", "更多")
                                expanded: false

                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Copy version path", "複製版本路徑", "复制版本路径")
                                    theme: root.theme
                                    kind: "ghost"
                                    onClicked: {
                                        root.copyGraphPath(root.selectedVersion)
                                        root.statusMessage = root.i18n.text("Copied version path", "已複製版本路徑", "已复制版本路径")
                                    }
                                }
                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Copy phase/block/stage/version", "複製 phase/block/stage/version", "复制 phase/block/stage/version")
                                    theme: root.theme
                                    kind: "ghost"
                                    onClicked: root.copyVersionFields()
                                }
                                Common.ActionButton {
                                    Layout.fillWidth: true
                                    text: root.i18n.text("Refresh all", "全部重新整理", "全部刷新")
                                    theme: root.theme
                                    kind: "ghost"
                                    onClicked: root.refreshAll()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
