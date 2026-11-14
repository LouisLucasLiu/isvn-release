import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Rectangle {
    id: root

    property QtObject theme
    property string path: ""
    property bool selected: false
    property bool showStageDot: true
    property bool mono: true
    signal clicked(string path)

    implicitWidth: pillRow.implicitWidth + (root.theme ? root.theme.gapMd * 2 : 24)
    implicitHeight: 30
    radius: root.theme ? root.theme.radius : 6
    color: root.selected && root.theme ? root.theme.bgElevated : (root.theme ? root.theme.bgPanel : "#ffffff")
    border.color: root.selected && root.theme ? root.theme.accent : (root.theme ? root.theme.border : "#d0d7de")

    RowLayout {
        id: pillRow
        anchors.fill: parent
        anchors.leftMargin: root.theme ? root.theme.gapSm : 8
        anchors.rightMargin: root.theme ? root.theme.gapSm : 8
        spacing: root.theme ? root.theme.gapXs : 4

        Rectangle {
            visible: root.showStageDot
            Layout.preferredWidth: 8
            Layout.preferredHeight: 8
            radius: 4
            color: root.theme ? root.theme.stageColor(root.path) : "#3fb6c7"
        }

        Label {
            text: root.path
            color: root.theme ? root.theme.textPrimary : "#1f2937"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            font.family: root.mono && root.theme ? root.theme.monoFont : ""
            elide: Text.ElideMiddle
            verticalAlignment: Text.AlignVCenter
            ToolTip.text: root.path
            ToolTip.visible: hover.hovered && root.path.length > 0
            Layout.fillWidth: true

            HoverHandler { id: hover }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked(root.path)
    }
}
