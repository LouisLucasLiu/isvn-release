import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// FileStatusWorkspace — folder/file hierarchy of the Active Working Version with
// multi-selection. Columns: selection, Path, State, Source (LINKED/LOCAL), Size.
// State color is a rendering of the explicit model `state` field; it is never
// inferred from color or filename. Blank-space external drop targets the Active
// Version root; a folder-row drop targets that folder. Drop opens the real
// backend Import Preview and copies nothing until explicit Apply.
Common.ShellPanel {
    id: root

    // `theme` comes from ShellPanel. Redeclaring it here would hide it from
    // ShellPanel, which then paints a fixed white surface under themed text.
    property var i18n
    property var workState
    property var workModel
    // The real Batch controller is the session-only authority for explicit
    // selection in the default Product Shell.  File Status owns only the
    // pointer/keyboard affordance; it never derives eligibility or performs a
    // filesystem/SVN action.
    property var checkinController: null

    signal importRequested(string destination, var urls)
    signal selectionRequested(var paths)

    readonly property var selectedPaths: checkinController && checkinController.selectedPaths
                                         ? checkinController.selectedPaths : []
    property string activeFilter: "All"
    readonly property var activeFiles: root.workModel && root.workModel.activeFiles
                                       ? root.workModel.activeFiles : []
    readonly property var filteredFiles: {
        var result = []
        for (var i = 0; i < activeFiles.length; ++i) {
            var item = activeFiles[i]
            if (!item) continue
            if (activeFilter === "Changed" && (item.state === "" || item.state === "clean")) continue
            if (activeFilter === "Editable" && item.state !== "editable") continue
            if (activeFilter === "LOCAL" && item.sourceKind !== "LOCAL") continue
            if (activeFilter === "Conflict" && item.state !== "conflict") continue
            result.push(item)
        }
        return result
    }
    readonly property int filteredCount: filteredFiles.length

    function isSelected(path) {
        for (var i = 0; i < selectedPaths.length; ++i)
            if (selectedPaths[i] === path) return true
        return false
    }

    function _commitSelection(paths) {
        // A signal keeps File Status presentation-only.  WorkPage forwards the
        // exact project-relative list to CheckinBatchController.setSelection().
        root.selectionRequested(paths)
    }

    // Shared by the DropAreas and default-shell interaction tests.  The method
    // merely forwards transient URLs to WorkPage; ImportController remains the
    // sole owner of Preview/Apply and stdin transport.
    function requestImport(destination, urls) {
        root.importRequested(destination || "", urls || [])
    }

    function toggleSelection(path, selected) {
        var next = []
        var seen = {}
        for (var i = 0; i < selectedPaths.length; ++i) {
            var existing = String(selectedPaths[i])
            if (existing.length > 0 && existing !== path && !seen[existing]) {
                seen[existing] = true
                next.push(existing)
            }
        }
        if (selected && path.length > 0) next.push(path)
        _commitSelection(next)
    }

    function setAllChanged(selected) {
        var next = []
        if (selected) {
            var files = root.activeFiles
            for (var i = 0; i < files.length; ++i) {
                var item = files[i]
                // This is an explicit bulk checkbox action over visible file
                // rows, not an all-eligible backend fallback.  The backend
                // still validates every selector during Batch Preview.
                if (item && item.kind === "file" && item.state !== "clean")
                    next.push(item.path)
            }
        }
        _commitSelection(next)
    }

    readonly property bool allChangedSelected: {
        var files = root.activeFiles
        var count = 0
        for (var i = 0; i < files.length; ++i) {
            var item = files[i]
            if (!item || item.kind !== "file" || item.state === "clean") continue
            ++count
            if (!root.isSelected(item.path)) return false
        }
        return count > 0
    }

    function stateColor(state) {
        if (!root.theme)
            return "#8b98a5"
        if (state === "modified")
            return root.theme.workAccent
        if (state === "editable")
            return root.theme.accentWarn
        if (state === "new")
            return root.theme.accentOk
        if (state === "conflict")
            return root.theme.accentDanger
        if (state === "missing" || state === "removed")
            return root.theme.accentDanger
        return root.theme.textSecondary
    }

    function stateLabel(state) {
        if (!root.i18n || state.length === 0)
            return state
        if (state === "clean") return root.i18n.text("Clean", "乾淨", "乾净")
        if (state === "modified") return root.i18n.text("Modified", "已修改", "已修改")
        if (state === "editable") return root.i18n.text("Editable", "可編輯", "可编辑")
        if (state === "new") return root.i18n.text("New", "新增", "新增")
        if (state === "conflict") return root.i18n.text("Conflict", "衝突", "冲突")
        if (state === "missing") return root.i18n.text("Missing", "遺失", "遗失")
        if (state === "removed") return root.i18n.text("Removed", "已移除", "已移除")
        return state
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapSm : 8

        RowLayout {
            Layout.fillWidth: true
            Common.SectionHeader {
                Layout.fillWidth: true
                theme: root.theme
                title: root.i18n ? root.i18n.text("File Status", "檔案狀態", "文件状态") : "File Status"
            }
        }

        // Filter is a presentation-only view over the current authoritative
        // WorkRead snapshot.  It never changes selection authority or emits a
        // filesystem/SVN query.
        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapXs : 4

            Repeater {
                model: ["All", "Changed", "Editable", "LOCAL", "Conflict"]
                delegate: Common.ActionButton {
                    required property string modelData
                    theme: root.theme
                    kind: root.activeFilter === modelData ? "primary" : "ghost"
                    implicitHeight: 28
                    text: modelData
                    onClicked: root.activeFilter = modelData
                }
            }

            Item { Layout.fillWidth: true }

            CheckBox {
                objectName: "selectAllChanged"
                enabled: root.checkinController !== null &&
                         !root.checkinController.mutationsBlocked
                checked: root.allChangedSelected
                text: root.i18n ? root.i18n.text("Select all changed", "選取所有變更", "选取所有变更")
                                : "Select all changed"
                onClicked: root.setAllChanged(checked)
                contentItem: Label {
                    text: parent.text
                    color: root.theme ? root.theme.textSecondary : "#aab8c2"
                    font.pixelSize: root.theme ? root.theme.fontData : 12
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: parent.indicator.width + 6
                }
            }
        }

        // Column header.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.theme ? root.theme.rowHeight : 36
            radius: root.theme ? root.theme.radius : 6
            color: root.theme ? root.theme.workSurface2 : "#122633"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                spacing: root.theme ? root.theme.gapSm : 8
                Label { Layout.preferredWidth: 26; text: "" }
                Label { Layout.fillWidth: true; text: root.i18n ? root.i18n.text("Path", "路徑", "路径") : "Path"
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.preferredWidth: 108; text: root.i18n ? root.i18n.text("State", "狀態", "状态") : "State"
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.preferredWidth: 78; text: root.i18n ? root.i18n.text("Source", "來源", "来源") : "Source"
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.preferredWidth: 72; horizontalAlignment: Text.AlignRight
                        text: root.i18n ? root.i18n.text("Size", "大小", "大小") : "Size"
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
            }
        }

        ListView {
            id: fileList
            objectName: "fileStatusList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.filteredFiles
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: fileRow
                // The required index property is declared explicitly, so no
                // bare `index` ReferenceError occurs in the delegate.
                required property var modelData
                required property int index

                readonly property bool isFolder: modelData.kind === "folder"

                width: fileList.width
                height: root.theme ? root.theme.rowHeight : 36
                color: root.isSelected(modelData.path)
                       ? Qt.rgba((root.theme ? root.theme.workAccent : "#1687f8").r,
                                 (root.theme ? root.theme.workAccent : "#1687f8").g,
                                 (root.theme ? root.theme.workAccent : "#1687f8").b, 0.10)
                       : (fileRow.index % 2 === 0
                          ? (root.theme ? root.theme.workSurface0 : "#0a1823")
                          : (root.theme ? root.theme.workSurface1 : "#0d1d29"))

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: root.theme ? root.theme.border : "#263946"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                    anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                    spacing: root.theme ? root.theme.gapSm : 8

                    CheckBox {
                        objectName: "fileSelectionCheckbox"
                        Layout.preferredWidth: 26
                        checked: root.isSelected(fileRow.modelData.path)
                        visible: !fileRow.isFolder
                        enabled: root.checkinController !== null &&
                                 !root.checkinController.mutationsBlocked
                        onClicked: root.toggleSelection(fileRow.modelData.path, checked)
                    }

                    Label {
                        Layout.fillWidth: true
                        text: (fileRow.isFolder ? "▾  " : "    ") + fileRow.modelData.path
                        color: root.theme ? root.theme.textPrimary : "#eef4f8"
                        font.pixelSize: root.theme ? root.theme.fontBody : 13
                        font.family: fileRow.isFolder ? (root.theme ? root.theme.sansFont : "sans-serif")
                                                      : (root.theme ? root.theme.monoFont : "monospace")
                        elide: Text.ElideMiddle
                    }

                    Label {
                        Layout.preferredWidth: 108
                        text: root.stateLabel(fileRow.modelData.state)
                        visible: fileRow.modelData.state.length > 0
                        color: root.stateColor(fileRow.modelData.state)
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                    }

                    Label {
                        Layout.preferredWidth: 78
                        text: fileRow.modelData.sourceKind
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                        font.family: root.theme ? root.theme.monoFont : "monospace"
                    }

                    Label {
                        Layout.preferredWidth: 72
                        horizontalAlignment: Text.AlignRight
                        text: root.workModel ? root.workModel.humanSize(fileRow.modelData.sizeBytes) : ""
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                        font.family: root.theme ? root.theme.monoFont : "monospace"
                    }
                }

                // A folder row is a concrete destination.  A file row consumes
                // the drop too, then asks the backend to reject it as
                // INVALID_DROP_TARGET; it must never fall through to the blank
                // Active-root DropArea and imply parent-file replacement.
                DropArea {
                    anchors.fill: parent
                    enabled: true
                    onDropped: function(drop) {
                        drop.acceptProposedAction()
                        root.requestImport(fileRow.modelData.path, drop.urls)
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.theme ? root.theme.rowHeight : 36
            radius: root.theme ? root.theme.radius : 6
            color: root.theme ? root.theme.workSurface2 : "#122633"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                Label {
                    text: root.filteredCount + " "
                          + (root.i18n ? root.i18n.text("items", "項目", "项目") : "items")
                    color: root.theme ? root.theme.textMuted : "#71818d"
                    font.pixelSize: root.theme ? root.theme.fontData : 12
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: root.i18n ? root.i18n.text("Drop into blank space → Active Version root",
                                                     "拖到空白處 → 使用中版本根目錄",
                                                     "拖到空白处 → 使用中版本根目录")
                                    : "Drop into blank space → Active Version root"
                    color: root.theme ? root.theme.textSecondary : "#aab8c2"
                    font.pixelSize: root.theme ? root.theme.fontData : 12
                }
            }
        }
    }

    // Blank-space external drop targets the Active Working Version root. It sits
    // beneath the row-level folder DropAreas so folder rows win.
    DropArea {
        anchors.fill: parent
        z: -1
        onEntered: dropHint.visible = true
        onExited: dropHint.visible = false
        onDropped: function(drop) {
            dropHint.visible = false
            drop.acceptProposedAction()
            root.requestImport("", drop.urls)
        }
    }

    Rectangle {
        id: dropHint
        visible: false
        anchors.fill: parent
        anchors.margins: 8
        radius: root.theme ? root.theme.radius : 6
        color: Qt.rgba((root.theme ? root.theme.workAccent : "#1687f8").r,
                       (root.theme ? root.theme.workAccent : "#1687f8").g,
                       (root.theme ? root.theme.workAccent : "#1687f8").b, 0.14)
        border.width: 2
        border.color: root.theme ? root.theme.workAccent : "#1687f8"

        Label {
            anchors.centerIn: parent
            text: root.i18n ? root.i18n.text("Drop to preview import (nothing is copied yet)",
                                             "放開以預覽匯入（尚未複製任何檔案）",
                                             "放开以预览导入（尚未复制任何文件）")
                            : "Drop to preview import (nothing is copied yet)"
            color: root.theme ? root.theme.textPrimary : "#eef4f8"
            font.pixelSize: 16
            font.bold: true
        }
    }
}
