import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// Project / Version navigation module for Workspace A.
// Display + signal only: it builds a structural phase/block/stage/version tree
// from `branches` data, manages local expansion/filter/selection-highlight state,
// and emits safe signals. It never runs backend commands and never infers
// provenance, ancestry, cache, hash, or retention.
Item {
    id: root

    property QtObject theme
    property QtObject i18n
    property var branches: []
    property string selectedVersion: ""
    property string projectPath: ""
    property string projectName: ""

    signal versionSelected(string versionPath)
    signal showInGraphRequested(string versionPath)
    signal compareRequested(string versionPath)
    signal refreshRequested()

    // Local UI state only.
    property string filterText: ""
    property var expandedState: ({})
    property var allGroupKeys: []
    property bool filtering: filterText.trim().length > 0
    property bool hasBranches: (branches ? branches.length : 0) > 0
    property int visibleVersionCount: 0

    function tr(en, tw, cn) { return root.i18n ? root.i18n.text(en, tw, cn) : tw }

    function effectiveName() {
        if (projectName.length > 0) return projectName
        var p = String(projectPath).replace(/\/+$/, "")
        var idx = p.lastIndexOf("/")
        return idx >= 0 ? p.substring(idx + 1) : p
    }

    function rowPath(branch) {
        return branch.path && String(branch.path).length > 0
               ? branch.path
               : [branch.phase, branch.block, branch.stage, branch.version].join("/")
    }

    function keyFor(type, phase, block, stage) {
        if (type === "phase") return "phase|" + phase
        if (type === "block") return "block|" + phase + "|" + block
        return "stage|" + phase + "|" + block + "|" + stage
    }

    function isExpanded(key) {
        if (filtering) return true
        return expandedState[key] !== false
    }

    function setExpanded(key, value) {
        var next = {}
        for (var k in expandedState) next[k] = expandedState[k]
        next[key] = value
        expandedState = next
        rebuild()
    }

    function toggle(key) { setExpanded(key, !isExpanded(key)) }

    function expandAll() {
        var next = {}
        for (var i = 0; i < allGroupKeys.length; ++i) next[allGroupKeys[i]] = true
        expandedState = next
        rebuild()
    }

    function collapseAll() {
        var next = {}
        for (var i = 0; i < allGroupKeys.length; ++i) next[allGroupKeys[i]] = false
        expandedState = next
        rebuild()
    }

    // Expand the ancestor groups of a version path so it becomes visible.
    function reveal(path) {
        var parts = String(path).split("/")
        if (parts.length < 4) return
        var next = {}
        for (var k in expandedState) next[k] = expandedState[k]
        next[keyFor("phase", parts[0], "", "")] = true
        next[keyFor("block", parts[0], parts[1], "")] = true
        next[keyFor("stage", parts[0], parts[1], parts[2])] = true
        expandedState = next
        rebuild()
    }

    function presentStages() {
        // Always show the full stable stage palette so the legend is consistent across
        // projects of all sizes. Project-specific stage names not in the stable set are
        // appended at the end.
        var stableSet = ["RTL", "SYN", "CTS", "ROUTE", "SIGNOFF", "OTHER"]
        var inStable = {}
        for (var i = 0; i < stableSet.length; ++i) inStable[stableSet[i]] = true
        var extras = [], seen = {}
        var list = branches || []
        for (var j = 0; j < list.length; ++j) {
            var s = list[j].stage || ""
            if (s.length > 0 && !inStable[s] && !seen[s]) { seen[s] = true; extras.push(s) }
        }
        return extras.length > 0 ? stableSet.concat(extras) : stableSet
    }

    function rebuild() {
        treeModel.clear()
        var needle = filterText.trim().toLowerCase()
        var phases = []
        var phaseMap = {}
        var groupKeys = []
        var versionCount = 0
        var list = branches || []
        for (var i = 0; i < list.length; ++i) {
            var branch = list[i]
            var path = rowPath(branch)
            var phase = branch.phase || ""
            var block = branch.block || ""
            var stage = branch.stage || ""
            var version = branch.version || ""
            if (needle.length > 0) {
                var hay = (path + " " + phase + " " + block + " " + stage + " " + version).toLowerCase()
                if (hay.indexOf(needle) < 0) continue
            }
            if (!phaseMap[phase]) {
                phaseMap[phase] = { label: phase, blocks: [], blockMap: {} }
                phases.push(phaseMap[phase])
            }
            var phaseNode = phaseMap[phase]
            if (!phaseNode.blockMap[block]) {
                phaseNode.blockMap[block] = { label: block, stages: [], stageMap: {} }
                phaseNode.blocks.push(phaseNode.blockMap[block])
            }
            var blockNode = phaseNode.blockMap[block]
            if (!blockNode.stageMap[stage]) {
                blockNode.stageMap[stage] = { label: stage, versions: [] }
                blockNode.stages.push(blockNode.stageMap[stage])
            }
            blockNode.stageMap[stage].versions.push({
                version: version, phase: phase, block: block, stage: stage, path: path,
                isHead: branch.is_head === true || branch.isHead === true,
                removed: branch.removed === true
            })
        }

        for (var p = 0; p < phases.length; ++p) {
            var pNode = phases[p]
            var pKey = keyFor("phase", pNode.label, "", "")
            groupKeys.push(pKey)
            var pExpanded = isExpanded(pKey)
            treeModel.append({ type: "phase", depth: 0, label: pNode.label, key: pKey,
                               expanded: pExpanded, stage: "", path: pNode.label, version: "",
                               removed: false, isHead: false, childCount: pNode.blocks.length })
            if (!pExpanded) continue
            for (var b = 0; b < pNode.blocks.length; ++b) {
                var bNode = pNode.blocks[b]
                var bKey = keyFor("block", pNode.label, bNode.label, "")
                groupKeys.push(bKey)
                var bExpanded = isExpanded(bKey)
                treeModel.append({ type: "block", depth: 1, label: bNode.label, key: bKey,
                                   expanded: bExpanded, stage: "",
                                   path: pNode.label + "/" + bNode.label, version: "",
                                   removed: false, isHead: false, childCount: bNode.stages.length })
                if (!bExpanded) continue
                for (var s = 0; s < bNode.stages.length; ++s) {
                    var sNode = bNode.stages[s]
                    var sKey = keyFor("stage", pNode.label, bNode.label, sNode.label)
                    groupKeys.push(sKey)
                    var sExpanded = isExpanded(sKey)
                    treeModel.append({ type: "stage", depth: 2, label: sNode.label, key: sKey,
                                       expanded: sExpanded, stage: sNode.label,
                                       path: pNode.label + "/" + bNode.label + "/" + sNode.label,
                                       version: "", removed: false, isHead: false,
                                       childCount: sNode.versions.length })
                    if (!sExpanded) continue
                    for (var v = 0; v < sNode.versions.length; ++v) {
                        var vn = sNode.versions[v]
                        treeModel.append({ type: "version", depth: 3, label: vn.version, key: vn.path,
                                           expanded: true, stage: vn.stage, path: vn.path,
                                           version: vn.version, removed: vn.removed,
                                           isHead: vn.isHead, childCount: 0 })
                        ++versionCount
                    }
                }
            }
        }
        allGroupKeys = groupKeys
        visibleVersionCount = versionCount
    }

    onBranchesChanged: rebuild()
    Component.onCompleted: rebuild()

    // When the selected version changes from outside (e.g. another panel), expand its
    // phase/block/stage ancestors and scroll it into view. Display-only: no checkout,
    // no backend command. While filtering, the filter result stays primary.
    onSelectedVersionChanged: {
        if (selectedVersion.length === 0 || filtering)
            return
        reveal(selectedVersion)
        for (var i = 0; i < treeModel.count; ++i) {
            var rowItem = treeModel.get(i)
            if (rowItem.type === "version" && rowItem.path === selectedVersion) {
                treeView.positionViewAtIndex(i, ListView.Contain)
                break
            }
        }
    }

    ListModel { id: treeModel }

    // Hidden helper for "Copy version path" / "Copy partial path".
    TextEdit { id: clip; visible: false; readOnly: true }
    function copyText(value) {
        clip.text = String(value || "")
        clip.selectAll()
        clip.copy()
        clip.deselect()
    }

    // Context-menu target row.
    property string menuPath: ""
    property string menuKey: ""
    property string menuLabel: ""

    Menu {
        id: versionMenu
        objectName: "versionMenuObj"
        MenuItem {
            text: root.menuLabel
            enabled: false
        }
        MenuSeparator {}
        MenuItem {
            text: root.tr("Select version", "選取版本", "选择版本")
            onTriggered: root.versionSelected(root.menuPath)
        }
        MenuItem {
            text: root.tr("Show in graph", "在圖中顯示", "在图中显示")
            onTriggered: root.showInGraphRequested(root.menuPath)
        }
        MenuItem {
            text: root.tr("Copy version path", "複製版本路徑", "复制版本路径")
            onTriggered: root.copyText(root.menuPath)
        }
        MenuItem {
            text: root.tr("Add to compare basket", "加入比對籃", "加入对比篮")
            onTriggered: root.compareRequested(root.menuPath)
        }
    }

    Menu {
        id: groupMenu
        objectName: "groupMenuObj"
        MenuItem {
            text: root.menuLabel
            enabled: false
        }
        MenuSeparator {}
        MenuItem {
            text: root.tr("Expand", "展開", "展开")
            onTriggered: root.setExpanded(root.menuKey, true)
        }
        MenuItem {
            text: root.tr("Collapse", "收合", "收起")
            onTriggered: root.setExpanded(root.menuKey, false)
        }
        MenuItem {
            text: root.tr("Collapse siblings", "收合相鄰群組", "收起相邻分组")
            onTriggered: root.collapseSiblings(root.menuKey)
        }
        MenuItem {
            text: root.tr("Copy partial path", "複製部分路徑", "复制部分路径")
            onTriggered: root.copyText(root.menuPath)
        }
    }

    // Collapse other group keys at the same prefix depth, keep this one expanded.
    function collapseSiblings(key) {
        var prefix = key.substring(0, key.lastIndexOf("|") + 1)
        var next = {}
        for (var i = 0; i < allGroupKeys.length; ++i) {
            var k = allGroupKeys[i]
            if (k.indexOf(prefix) === 0 && k.split("|").length === key.split("|").length)
                next[k] = (k === key)
        }
        for (var e in expandedState)
            if (next[e] === undefined) next[e] = expandedState[e]
        next[key] = true
        expandedState = next
        rebuild()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme.gapSm

        // ── PROJECT / PATH ──
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1

            Common.SectionHeader {
                Layout.fillWidth: true
                theme: root.theme
                title: root.tr("Project / Path", "專案 / 路徑", "项目 / 路径")
                compact: true
            }
            Label {
                visible: root.projectPath.length > 0
                text: root.effectiveName()
                color: root.theme.textPrimary
                font.pixelSize: root.theme.fontBody
                font.bold: true
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: root.projectPath.length > 0 ? root.projectPath
                      : root.tr("Project path unavailable", "專案路徑不可用", "项目路径不可用")
                color: root.theme.textSecondary
                font.family: root.theme.monoFont
                font.pixelSize: root.theme.fontData
                elide: Text.ElideMiddle
                Layout.fillWidth: true
                ToolTip.text: root.projectPath
                ToolTip.visible: pathHover.hovered && root.projectPath.length > 0
                HoverHandler { id: pathHover }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.theme.border }

        // ── Toolbar ──
        ColumnLayout {
            Layout.fillWidth: true
            spacing: root.theme.gapXs

            TextField {
                id: filterField
                Layout.fillWidth: true
                text: root.filterText
                placeholderText: root.tr("Filter versions...", "篩選版本...", "筛选版本...")
                color: root.theme.textPrimary
                placeholderTextColor: root.theme.textSecondary
                font.pixelSize: root.theme.fontBody
                onTextChanged: { root.filterText = text; root.rebuild() }
                background: Rectangle {
                    color: root.theme.bgElevated
                    border.color: root.theme.border
                    radius: root.theme.radius
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: root.theme.gapXs

                Common.ActionButton {
                    theme: root.theme
                    kind: "ghost"
                    text: root.tr("Expand all", "全部展開", "全部展开")
                    onClicked: root.expandAll()
                }
                Common.ActionButton {
                    theme: root.theme
                    kind: "ghost"
                    text: root.tr("Collapse all", "全部收合", "全部收起")
                    onClicked: root.collapseAll()
                }
                Item { Layout.fillWidth: true }
                Common.ActionButton {
                    theme: root.theme
                    kind: "ghost"
                    text: root.tr("Refresh", "重新整理", "刷新")
                    onClicked: root.refreshRequested()
                }
            }
        }

        // ── VERSION TREE ──
        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.tr("Version tree", "版本樹", "版本树")
            compact: true
        }

        Common.EmptyState {
            visible: !root.hasBranches
            Layout.fillWidth: true
            theme: root.theme
            title: root.tr("No versions available.", "目前沒有版本。", "当前没有版本。")
        }
        Common.EmptyState {
            visible: root.hasBranches && root.visibleVersionCount === 0
            Layout.fillWidth: true
            theme: root.theme
            title: root.tr("No matching versions.", "沒有符合的版本。", "没有符合的版本。")
        }

        ListView {
            id: treeView
            visible: root.visibleVersionCount > 0 || treeModel.count > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: treeModel
            spacing: 1
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                width: ListView.view.width
                height: 26

                Rectangle {
                    anchors.fill: parent
                    radius: root.theme.radius
                    color: model.type === "version" && root.selectedVersion === model.path
                           ? root.theme.bgElevated
                           : (rowHover.hovered ? root.theme.bgElevated : "transparent")
                    opacity: model.removed ? 0.5 : 1.0

                    // accent left border for the selected version leaf
                    Rectangle {
                        visible: model.type === "version" && root.selectedVersion === model.path
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        radius: 1
                        color: root.theme.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: root.theme.gapXs + model.depth * 14
                        anchors.rightMargin: root.theme.gapXs
                        spacing: root.theme.gapXs

                        Label {
                            text: model.type === "version" ? "" : (model.expanded ? "▾" : "▸")
                            color: root.theme.textSecondary
                            font.pixelSize: root.theme.fontData
                            horizontalAlignment: Text.AlignHCenter
                            Layout.preferredWidth: 12
                        }

                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 4
                            visible: model.type === "stage" || model.type === "version"
                            color: root.theme.stageColor(model.stage.length > 0
                                                         ? "/" + model.stage + "/" : model.path)
                        }

                        Label {
                            text: model.label
                            color: root.theme.textPrimary
                            font.pixelSize: model.type === "version" ? root.theme.fontData : root.theme.fontBody
                            font.family: model.type === "version" ? root.theme.monoFont : root.theme.sansFont
                            font.bold: model.type !== "version"
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }

                        // count badge for group rows
                        Label {
                            visible: model.type !== "version" && model.childCount > 0
                            text: model.childCount
                            color: root.theme.textSecondary
                            font.pixelSize: root.theme.fontCaption
                        }

                        // latest pill
                        Rectangle {
                            visible: model.type === "version" && model.isHead
                            Layout.preferredWidth: latestLabel.implicitWidth + 10
                            Layout.preferredHeight: 15
                            radius: 7
                            color: "transparent"
                            border.color: root.theme.accentOk
                            Label {
                                id: latestLabel
                                anchors.centerIn: parent
                                text: root.tr("latest", "最新", "最新")
                                color: root.theme.accentOk
                                font.pixelSize: root.theme.fontCaption
                            }
                        }

                        // removed badge
                        Label {
                            visible: model.type === "version" && model.removed
                            text: root.tr("removed", "已移除", "已移除")
                            color: root.theme.textSecondary
                            font.pixelSize: root.theme.fontCaption
                        }
                    }

                    HoverHandler { id: rowHover }

                    ToolTip.text: model.path
                    ToolTip.visible: rowHover.hovered && model.type === "version"

                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: {
                            if (model.type === "version")
                                root.versionSelected(model.path)
                            else
                                root.toggle(model.key)
                        }
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: function(point) {
                            root.menuPath = model.path
                            root.menuKey = model.key
                            root.menuLabel = model.type === "version" ? model.version : model.label
                            if (model.type === "version")
                                versionMenu.popup()
                            else
                                groupMenu.popup()
                        }
                    }
                }
            }
        }

        // ── STAGE LEGEND ──
        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.tr("Stage legend", "階段圖例", "阶段图例")
            compact: true
        }
        Common.StageLegend {
            Layout.fillWidth: true
            theme: root.theme
            stages: root.presentStages()
        }
    }
}
