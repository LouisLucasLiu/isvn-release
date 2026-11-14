import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// BottomStatusBar — safe, bounded status fields only. It never shows raw
// command lines or absolute external paths.
Rectangle {
    id: root
    objectName: "bottomStatusBar"

    property var theme
    property var i18n
    property var workState
    property var identity
    property var operationCoordinator
    property var readController

    signal activityLogRequested()
    signal operationCenterRequested()

    implicitHeight: root.theme ? root.theme.bottomBarHeight : 30
    color: root.theme ? root.theme.workSurface0 : "#0a1823"

    function statusState() {
        if (root.readController && root.readController.state === "error") return "danger"
        if (root.readController && root.readController.state === "loading") return "warn"
        if (root.readController) return "ok"
        if (!root.workState)
            return "ok"
        if (root.workState.statusKind === "warn")
            return "warn"
        if (root.workState.statusKind === "error")
            return "danger"
        return "ok"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: root.theme ? root.theme.border : "#263946"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.theme ? root.theme.gapLg : 16
        anchors.rightMargin: root.theme ? root.theme.gapLg : 16
        spacing: root.theme ? root.theme.gapSm : 8

        Common.StatusDot {
            theme: root.theme
            state: root.statusState()
            label: ""
            Layout.preferredWidth: 8
        }

        Label {
            objectName: "statusSummary"
            text: root.readController
                  ? (root.readController.state === "loading"
                     ? (root.i18n ? root.i18n.text("Refreshing Work data", "正在重新整理工作資料", "正在刷新工作数据") : "Refreshing Work data")
                     : root.readController.state === "error"
                       ? (root.i18n ? root.i18n.text("Work data needs Refresh", "工作資料需要重新整理", "工作数据需要刷新") : "Work data needs Refresh")
                       : (root.i18n ? root.i18n.text("Ready", "就緒", "就绪") : "Ready"))
                  : (root.workState ? root.workState.statusText : "")
            color: root.theme ? root.theme.textPrimary : "#eef4f8"
            font.pixelSize: root.theme ? root.theme.fontData : 12
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: root.theme ? root.theme.border : "#263946"
        }

        Label {
            readonly property var last: root.operationCoordinator && root.operationCoordinator.allOperations().length > 0
                                        ? root.operationCoordinator.allOperations()[root.operationCoordinator.allOperations().length - 1]
                                        : null
            text: (root.i18n ? root.i18n.text("Last action", "上次動作", "上次动作") : "Last action")
                  + ":  " + (root.readController
                               ? (last ? (last.operationKind + " · " + last.state) : "—")
                               : (root.workState ? root.workState.lastAction : ""))
            color: root.theme ? root.theme.textSecondary : "#aab8c2"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            elide: Text.ElideRight
            Layout.maximumWidth: 320
        }

        // Target project-relative version/path (never an absolute external path).
        Label {
            objectName: "targetVersionField"
            text: root.readController && root.readController.activeVersionId.length > 0
                  ? (root.i18n ? root.i18n.text("Target", "目標", "目标") : "Target")
                    + "  " + root.readController.activeVersionId
                  : root.workState && root.workState.activeVersion()
                  ? (root.i18n ? root.i18n.text("Target", "目標", "目标") : "Target")
                    + "  " + root.workState.activeVersion().repositoryPath
                  : ""
            color: root.theme ? root.theme.textMuted : "#71818d"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            font.family: root.theme ? root.theme.monoFont : "monospace"
            elide: Text.ElideMiddle
            Layout.maximumWidth: 340
        }

        Item { Layout.fillWidth: true }

        Label {
            objectName: "operationCenterSummary"
            visible: root.operationCoordinator &&
                     (root.operationCoordinator.activeMutationCount > 0 ||
                      root.operationCoordinator.activeReadCount > 0)
            text: root.i18n
                  ? root.i18n.text("Operations: " + root.operationCoordinator.activeMutationCount
                                   + " mutation / " + root.operationCoordinator.activeReadCount + " read",
                                   "操作：" + root.operationCoordinator.activeMutationCount
                                   + " 個變更／" + root.operationCoordinator.activeReadCount + " 個讀取",
                                   "操作：" + root.operationCoordinator.activeMutationCount
                                   + " 个变更／" + root.operationCoordinator.activeReadCount + " 个读取")
                  : "Operations"
            color: root.theme ? root.theme.workAccent : "#1687f8"
            font.pixelSize: root.theme ? root.theme.fontData : 12
        }

        Label {
            text: root.i18n ? root.i18n.text("Storage OK", "儲存正常", "存储正常") : "Storage OK"
            color: root.theme ? root.theme.textMuted : "#71818d"
            font.pixelSize: root.theme ? root.theme.fontData : 12
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: root.theme ? root.theme.border : "#263946"
        }

        Button {
            objectName: "operationCenterButton"
            visible: root.operationCoordinator && root.operationCoordinator.operationCount > 0
            flat: true
            implicitHeight: 22
            hoverEnabled: true
            contentItem: Label {
                text: root.i18n ? root.i18n.text("Operation Center", "操作中心", "操作中心") : "Operation Center"
                color: root.theme ? root.theme.workAccent : "#1687f8"
                font.pixelSize: root.theme ? root.theme.fontData : 12
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: "transparent" }
            onClicked: root.operationCenterRequested()
        }

        Rectangle {
            visible: root.operationCoordinator && root.operationCoordinator.operationCount > 0
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: root.theme ? root.theme.border : "#263946"
        }

        // Activity Log entry point (opens details; not a global inspector).
        Button {
            objectName: "activityLogButton"
            flat: true
            implicitHeight: 22
            hoverEnabled: true
            contentItem: Label {
                text: root.i18n ? root.i18n.text("Activity Log", "活動紀錄", "活动记录") : "Activity Log"
                color: root.theme ? root.theme.workAccent : "#1687f8"
                font.pixelSize: root.theme ? root.theme.fontData : 12
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: "transparent" }
            onClicked: root.activityLogRequested()
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: root.theme ? root.theme.border : "#263946"
        }

        Label {
            text: root.identity ? root.identity.buildIdentity : ""
            color: root.theme ? root.theme.textMuted : "#71818d"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            font.family: root.theme ? root.theme.monoFont : "monospace"
        }
    }
}
