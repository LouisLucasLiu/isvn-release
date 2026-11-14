import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    property var theme
    property string title: ""
    property string message: ""
    property string actionText: ""
    signal actionRequested()

    spacing: root.theme ? root.theme.gapSm : 8

    Label {
        text: root.title
        visible: root.title.length > 0
        color: root.theme ? root.theme.textPrimary : "#1f2937"
        font.pixelSize: root.theme ? root.theme.fontSection : 15
        font.bold: true
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        Layout.fillWidth: true
    }

    Label {
        text: root.message
        visible: root.message.length > 0
        color: root.theme ? root.theme.textSecondary : "#59636e"
        font.pixelSize: root.theme ? root.theme.fontBody : 13
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        Layout.fillWidth: true
    }

    ActionButton {
        visible: root.actionText.length > 0
        text: root.actionText
        theme: root.theme
        kind: "secondary"
        Layout.alignment: Qt.AlignHCenter
        onClicked: root.actionRequested()
    }
}
