import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    property string path: ""
    property string linkState: ""
    property string svnState: ""
    property var theme

    implicitHeight: 34
    radius: 4
    color: theme ? theme.bgPanel : "#ffffff"
    border.color: theme ? theme.border : "#e0e4ea"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 8

        Label {
            text: root.svnState
            Layout.preferredWidth: 72
            Layout.maximumWidth: 72
            elide: Text.ElideRight
            color: root.theme ? root.theme.textSecondary : "#596170"
            font.family: root.theme ? root.theme.monoFont : ""
        }
        Label {
            text: root.path
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            color: root.theme ? root.theme.textPrimary : "#1f2937"
            font.family: root.theme ? root.theme.monoFont : ""
        }

        FileStateBadge {
            theme: root.theme
            linkState: root.linkState
            svnState: ""
        }
    }
}
