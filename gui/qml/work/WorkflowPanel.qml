import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// WorkflowPanel — contextual actions plus the target-keyed ephemeral Checkin
// Batch summary. In the default ProductShell binding, every mutation is
// delegated to the Work controllers; the fallback is used by the preview only.
//
// Remove is INDEPENDENT from the Checkin Batch: it is a separate high-risk
// action with its own confirm/message, and it is never shown
// as a Batch item. Create Derived Version is a composite Create -> Checkout ->
// Set Active flow.
Common.ShellPanel {
    id: root

    // `theme` comes from ShellPanel. Redeclaring it here would hide it from
    // ShellPanel, which then paints a fixed white surface under themed text.
    property var i18n
    property var workState
    property var workModel
    // Real read-mode action state. WorkUiState remains only for the isolated
    // preview path; it must not make the default Product Shell look
    // actionable while dispatching mock state.
    property var checkinController: null
    property var actionController: null
    property var operationCoordinator: null
    property string activeVersion: ""

    readonly property bool realBinding: checkinController !== null
    readonly property var selectedPaths: realBinding && checkinController.selectedPaths
                                       ? checkinController.selectedPaths : []
    readonly property int selectedCount: selectedPaths.length
    readonly property bool hasSelection: selectedCount > 0
    readonly property bool targetNeedsRecovery: operationCoordinator && activeVersion.length > 0
                                               ? operationCoordinator.needsRecovery(activeVersion) : false
    // A controller instance owns one visible local/checkin flow, so its own
    // unfinished flow keeps its buttons disabled.  An operation that another
    // flow is running on this Version does not: only an interrupted operation
    // that still has to be recovered holds the Version back.
    readonly property bool actionBlocked: targetNeedsRecovery ||
                                         (actionController && actionController.mutationsBlocked) ||
                                         (checkinController && checkinController.mutationsBlocked)
    readonly property var batchItems: {
        if (realBinding) {
            var items = checkinController.concreteItems || []
            if (items.length > 0) return items
            var pending = []
            for (var i = 0; i < selectedPaths.length; ++i)
                pending.push({ "path": selectedPaths[i], "reason_code": "" })
            return pending
        }
        return root.workModel ? root.workModel.batchEntries : []
    }
    readonly property int batchCount: realBinding
                                      ? ((checkinController.concreteItems || []).length)
                                      : (workState ? workState.batchCount : 0)
    readonly property string batchTarget: realBinding
                                          ? (checkinController.batchTarget || activeVersion)
                                          : (workState && workState.activeVersion()
                                             ? workState.activeVersion().repositoryPath : "")

    signal makeEditableRequested(var paths)
    signal discardLocalRequested(var paths)
    signal addToBatchRequested()
    signal revertRequested(var paths)
    signal removeRequested(var paths)
    signal createDerivedRequested()
    signal inspectInExploreRequested()
    signal reviewCheckinRequested()

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapSm : 8

        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.i18n ? root.i18n.text("Workflow", "工作流程", "工作流程") : "Workflow"
        }

        // Kept only for the standalone mock preview. The default
        // Product Shell always supplies the real controllers above.
        Common.SeverityBanner {
            Layout.fillWidth: true
            visible: !root.realBinding && root.workState && root.workState.readOnlyBinding
            theme: root.theme
            severity: "info"
            message: root.i18n ? root.i18n.text("Available after controller binding — actions do not execute yet.",
                                                  "控制器綁定後可用 — 動作尚未執行。",
                                                  "控制器绑定后可用 — 动作尚未执行。")
                               : "Available after controller binding — actions do not execute yet."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapMd : 12
            Label {
                objectName: "workflowSelectedPathsSummary"
                text: root.i18n
                      ? root.i18n.text("Selected paths · " + root.selectedCount,
                                       "已選路徑 · " + root.selectedCount,
                                       "已选路径 · " + root.selectedCount)
                      : "Selected paths · " + root.selectedCount
                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                font.pixelSize: root.theme ? root.theme.fontBody : 13
            }
            Item { Layout.fillWidth: true }
        }

        // Contextual actions. Make Editable / Add to Checkin Batch / Revert are
        // batch-oriented; Remove is intentionally separated below.
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: root.theme ? root.theme.gapSm : 8
            rowSpacing: root.theme ? root.theme.gapSm : 8

            Common.ActionButton {
                objectName: "makeEditableButton"
                Layout.fillWidth: true; theme: root.theme
                enabled: root.realBinding ? (root.hasSelection && !root.actionBlocked)
                                          : !(root.workState && root.workState.readOnlyBinding)
                text: root.i18n ? root.i18n.text("Make Editable", "設為可編輯", "设为可编辑") : "Make Editable"
                onClicked: root.makeEditableRequested(root.selectedPaths)
            }
            Common.ActionButton {
                objectName: "addToBatchButton"
                Layout.fillWidth: true; theme: root.theme
                enabled: root.realBinding ? (root.hasSelection && !root.actionBlocked)
                                          : !(root.workState && root.workState.readOnlyBinding)
                text: root.i18n ? root.i18n.text("Add to Checkin Batch", "加入簽入批次", "加入签入批次") : "Add to Checkin Batch"
                onClicked: root.addToBatchRequested()
            }
            Common.ActionButton {
                objectName: "revertButton"
                Layout.fillWidth: true; theme: root.theme
                enabled: root.realBinding ? (root.hasSelection && !root.actionBlocked)
                                          : !(root.workState && root.workState.readOnlyBinding)
                text: root.i18n ? root.i18n.text("Revert", "還原", "还原") : "Revert"
                onClicked: root.revertRequested(root.selectedPaths)
            }
            Common.ActionButton {
                objectName: "inspectInExploreButton"
                Layout.fillWidth: true; theme: root.theme
                text: root.i18n ? root.i18n.text("Inspect in Explore", "在探索檢視", "在探索查看") : "Inspect in Explore"
                onClicked: root.inspectInExploreRequested()
            }

            Common.ActionButton {
                objectName: "discardLocalButton"
                Layout.fillWidth: true; theme: root.theme
                dangerous: true
                enabled: root.realBinding ? (root.hasSelection && !root.actionBlocked)
                                          : !(root.workState && root.workState.readOnlyBinding)
                text: root.i18n ? root.i18n.text("Discard Local", "捨棄本機", "舍弃本机") : "Discard Local"
                onClicked: root.discardLocalRequested(root.selectedPaths)
            }
        }

        Common.ActionButton {
            objectName: "createDerivedVersionButton"
            Layout.fillWidth: true
            theme: root.theme
            enabled: root.realBinding ? (root.activeVersion.length > 0 && !root.actionBlocked)
                                      : !(root.workState && root.workState.readOnlyBinding)
            text: (root.i18n ? root.i18n.text("Create Derived Version", "建立衍生版本", "创建衍生版本") : "Create Derived Version")
                  + "  —  Create → Checkout → Set Active"
            onClicked: root.createDerivedRequested()
        }

        // Remove — independent high-risk action, NOT part of the Checkin Batch.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: removeCol.implicitHeight + (root.theme ? root.theme.gapMd * 2 : 24)
            radius: root.theme ? root.theme.radius : 6
            color: Qt.rgba((root.theme ? root.theme.accentDanger : "#e5534b").r,
                           (root.theme ? root.theme.accentDanger : "#e5534b").g,
                           (root.theme ? root.theme.accentDanger : "#e5534b").b, 0.06)
            border.color: Qt.rgba((root.theme ? root.theme.accentDanger : "#e5534b").r,
                                  (root.theme ? root.theme.accentDanger : "#e5534b").g,
                                  (root.theme ? root.theme.accentDanger : "#e5534b").b, 0.4)

            ColumnLayout {
                id: removeCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                spacing: root.theme ? root.theme.gapXs : 4

                Label {
                    text: root.i18n ? root.i18n.text("Remove is a separate atomic operation — it never enters the Checkin Batch.",
                                                     "移除是獨立的原子操作 — 不會進入簽入批次。",
                                                     "移除是独立的原子操作 — 不会进入签入批次。")
                                    : "Remove is a separate atomic operation — it never enters the Checkin Batch."
                    color: root.theme ? root.theme.textSecondary : "#aab8c2"
                    font.pixelSize: root.theme ? root.theme.fontCaption : 11
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Common.ActionButton {
                    objectName: "removeButton"
                    theme: root.theme
                    kind: "danger"
                    dangerous: true
                    enabled: root.realBinding ? (root.hasSelection && !root.actionBlocked)
                                              : !(root.workState && root.workState.readOnlyBinding)
                    text: root.i18n ? root.i18n.text("Remove", "移除", "移除") : "Remove"
                    onClicked: root.removeRequested(root.selectedPaths)
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.theme ? root.theme.border : "#263946" }

        // Checkin Batch summary (ephemeral, bound to the Active target).
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: root.i18n ? root.i18n.text("Checkin Batch", "簽入批次", "签入批次") : "Checkin Batch"
                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                font.pixelSize: root.theme ? root.theme.fontSection : 15
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Label {
                text: root.batchCount + " "
                      + (root.i18n ? root.i18n.text("items", "項目", "项目") : "items")
                color: root.theme ? root.theme.textSecondary : "#aab8c2"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }
        }

        Label {
            Layout.fillWidth: true
            text: (root.i18n ? root.i18n.text("Target", "目標", "目标") : "Target") + "  "
                  + root.batchTarget
            color: root.theme ? root.theme.textSecondary : "#aab8c2"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            font.family: root.theme ? root.theme.monoFont : "monospace"
            elide: Text.ElideMiddle
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.batchItems
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                required property var modelData
                width: ListView.view ? ListView.view.width : 0
                height: root.theme ? root.theme.rowHeight : 36
                color: "transparent"
                RowLayout {
                    anchors.fill: parent
                    spacing: root.theme ? root.theme.gapSm : 8
                    Label {
                        Layout.fillWidth: true
                        text: modelData.path !== undefined ? modelData.path : modelData.destinationPath
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                        font.family: root.theme ? root.theme.monoFont : "monospace"
                        elide: Text.ElideMiddle
                    }
                    Label {
                        text: modelData.reason_code === "CONFLICT_BLOCKED" || modelData.readiness === "conflict"
                              ? (root.i18n ? root.i18n.text("Conflict", "衝突", "冲突") : "Conflict")
                              : (root.i18n ? root.i18n.text("Ready", "就緒", "就绪") : "Ready")
                        color: modelData.reason_code === "CONFLICT_BLOCKED" || modelData.readiness === "conflict"
                               ? (root.theme ? root.theme.accentDanger : "#e5534b")
                               : (root.theme ? root.theme.accentOk : "#3fb950")
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.i18n ? root.i18n.text("Checkin Message", "簽入訊息", "签入信息") : "Checkin Message"
            color: root.theme ? root.theme.textSecondary : "#aab8c2"
            font.pixelSize: root.theme ? root.theme.fontData : 12
        }

        TextArea {
            id: messageArea
            objectName: "workflowMessageArea"
            Layout.fillWidth: true
            Layout.preferredHeight: 54
            placeholderText: root.i18n ? root.i18n.text("Enter checkin message…", "輸入簽入訊息…", "输入签入信息…")
                                       : "Enter checkin message…"
            color: root.theme ? root.theme.textPrimary : "#eef4f8"
            placeholderTextColor: root.theme ? root.theme.textMuted : "#71818d"
            font.pixelSize: root.theme ? root.theme.fontBody : 13
            wrapMode: TextEdit.Wrap
            text: root.realBinding ? root.checkinController.checkinMessage
                                   : (root.workState ? root.workState.checkinMessage : "")
            onTextChanged: {
                if (root.realBinding) root.checkinController.checkinMessage = text
                else if (root.workState) root.workState.checkinMessage = text
            }
            background: Rectangle {
                radius: root.theme ? root.theme.radius : 6
                color: root.theme ? root.theme.workSurface2 : "#122633"
                border.color: root.theme ? root.theme.border : "#263946"
            }
        }

        Common.ActionButton {
            objectName: "reviewCheckinButton"
            Layout.fillWidth: true
            theme: root.theme
            kind: "primary"
            enabled: root.realBinding ? (root.hasSelection && !root.actionBlocked)
                                      : (root.workState && root.workState.batchCount > 0 &&
                                         !root.workState.readOnlyBinding)
            text: root.i18n ? root.i18n.text("Review Checkin", "檢視簽入", "查看签入") : "Review Checkin"
            onClicked: root.reviewCheckinRequested()
        }
    }
}
