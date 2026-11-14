import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property var theme
    property string title: ""
    property string subtitle: ""
    property bool elevated: false
    default property alias content: contentArea.data

    color: root.theme ? (root.elevated ? root.theme.bgElevated : root.theme.bgPanel) : "#ffffff"
    border.color: root.theme ? root.theme.border : "#d0d7de"
    radius: root.theme ? root.theme.radius : 6
    clip: true

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.theme ? root.theme.gapMd : 12
        spacing: root.theme ? root.theme.gapSm : 8

        ColumnLayout {
            visible: root.title.length > 0 || root.subtitle.length > 0
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapXs : 4

            Label {
                visible: root.title.length > 0
                text: root.title
                color: root.theme ? root.theme.textPrimary : "#1f2937"
                font.pixelSize: root.theme ? root.theme.fontSection : 15
                font.bold: true
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Label {
                visible: root.subtitle.length > 0
                text: root.subtitle
                color: root.theme ? root.theme.textSecondary : "#59636e"
                font.pixelSize: root.theme ? root.theme.fontCaption : 11
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }

        Item {
            id: contentArea
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
