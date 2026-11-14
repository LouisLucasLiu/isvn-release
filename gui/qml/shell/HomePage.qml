import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// Bounded Home composition: it consumes already-authoritative Work/session
// snapshots.  It never starts a command or performs a broad aggregation.
Item {
    id: root
    objectName: "homePage"
    property var theme
    property var i18n
    property var readController
    property var sessionStore
    // Number of operations listed in the Activity Log.
    property int recentActivityCount: 0
    property var operationCoordinator
    signal continueWorkRequested()

    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: root.theme.gapLg
            anchors.margins: root.theme.gapLg

            Common.SectionHeader {
                Layout.fillWidth: true
                theme: root.theme
                title: root.i18n.text("Home", "首頁", "首页")
                subtitle: root.i18n.text("Bounded Project summary", "受限的專案摘要", "受限的项目摘要")
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 900 ? 2 : 1
                columnSpacing: root.theme.gapLg
                rowSpacing: root.theme.gapLg

                Repeater {
                    model: [
                        { "title": root.i18n.text("Active Working Version", "目前工作版本", "当前工作版本"),
                          "text": root.readController && root.readController.activeVersionId.length > 0
                                  ? root.readController.activeVersionId
                                  : root.i18n.text("No Active Version", "沒有啟用中的版本", "没有启用中的版本") },
                        { "title": root.i18n.text("Local checkouts", "本機 checkout", "本地 checkout"),
                          "text": root.readController ? String(root.readController.localVersions.length) : "—" },
                        { "title": root.i18n.text("Operation health", "操作健康狀態", "操作健康状态"),
                          "text": root.operationCoordinator && root.operationCoordinator.operationCount > 0
                                  ? root.i18n.text("Review Operation Center", "請檢視操作中心", "请查看操作中心")
                                  : root.i18n.text("No outstanding operation", "沒有未結操作", "没有未结操作") },
                        { "title": root.i18n.text("Recent activity", "近期活動", "近期活动"),
                          "text": String(root.recentActivityCount) }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 102
                        color: root.theme.workSurface1
                        border.color: root.theme.border
                        radius: root.theme.radius
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: root.theme.gapMd
                            Label { text: modelData.title; color: root.theme.textMuted; font.pixelSize: root.theme.fontData }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.text
                                color: root.theme.textPrimary
                                font.pixelSize: root.theme.fontBody
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }
            }

            Common.ActionButton {
                objectName: "homeContinueWork"
                theme: root.theme
                text: root.i18n.text("Continue Work", "繼續工作", "继续工作")
                onClicked: root.continueWorkRequested()
            }
        }
    }
}
