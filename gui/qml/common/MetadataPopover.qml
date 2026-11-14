import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    property string versionPath: ""
    property string parentPath: ""
    property string actor: ""
    property string timestamp: ""
    property string note: ""
    property string status: ""
    property bool showGetUpstream: true
    property var i18n
    property var theme
    signal checkoutAncestorRequested(string versionPath)

    modal: true
    focus: true
    width: 360
    padding: 14
    background: Rectangle {
        color: root.theme ? root.theme.bgElevated : "#ffffff"
        border.color: root.theme ? root.theme.border : "#d9dee8"
        radius: root.theme ? root.theme.radius : 6
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        VersionPathPill {
            theme: root.theme
            path: root.versionPath
            selected: true
            Layout.fillWidth: true
        }
        KeyValueGrid {
            theme: root.theme
            monoValue: true
            keyWidth: 80
            rows: [
                { key: root.i18n ? root.i18n.text("Version", "Version", "Version") : "Version",
                  value: root.versionPath },
                { key: root.i18n ? root.i18n.text("Parent", "Parent（上一版）", "Parent（上一版）") : "Parent",
                  value: root.parentPath.length === 0 ? "root" : root.parentPath },
                { key: root.i18n ? root.i18n.text("Created by", "建立者", "创建者") : "Created by",
                  value: root.actor },
                { key: root.i18n ? root.i18n.text("Created at", "建立時間", "创建时间") : "Created at",
                  value: root.timestamp },
                { key: root.i18n ? root.i18n.text("Status", "狀態", "状态") : "Status",
                  value: root.status }
            ]
            Layout.fillWidth: true
        }
        Label {
            text: root.note
            visible: root.note.length > 0
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            color: root.theme ? root.theme.textPrimary : "#1f2937"
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button {
                visible: root.showGetUpstream
                text: root.i18n ? root.i18n.text("Get upstream", "取上游版本", "取上游版本") : "取上游版本"
                onClicked: root.checkoutAncestorRequested(root.versionPath)
            }
            Button {
                text: root.i18n ? root.i18n.text("Close", "關閉", "关闭") : "關閉"
                onClicked: root.close()
            }
        }
    }
}
