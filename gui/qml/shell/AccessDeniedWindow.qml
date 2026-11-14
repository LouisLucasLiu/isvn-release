import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

ApplicationWindow {
    id: root
    objectName: "accessDeniedWindow"
    width: 720
    height: 420
    minimumWidth: 520
    minimumHeight: 320
    visible: true
    title: "isvn"
    Common.Theme { id: theme }
    Common.I18n { id: i18n }
    Rectangle {
        anchors.fill: parent
        color: theme.workSurface0
        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(520, parent.width - 72)
            spacing: theme.gapMd
            Label { text: i18n.text("Access Denied", "拒絕存取", "拒绝访问"); color: theme.textPrimary; font.pixelSize: theme.fontSection + 8; font.bold: true }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18n.text("This user is not authorized for the current Project. No Project page or command was opened.",
                                 "目前使用者未獲此專案授權；未開啟任何專案頁面或指令。",
                                 "当前用户未获此项目授权；未打开任何项目页面或命令。")
                color: theme.textSecondary
            }
            Button { text: i18n.text("Exit", "離開", "退出"); onClicked: Qt.quit() }
        }
    }
    function reactivate() { root.show(); root.raise(); root.requestActivate() }
}
