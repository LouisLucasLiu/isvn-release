import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    property string versionPath: ""
    property bool selected: false
    property var theme
    signal focusRequested(string versionPath)

    implicitWidth: 220
    implicitHeight: 36
    color: "transparent"
    border.color: "transparent"

    VersionPathPill {
        anchors.fill: parent
        theme: root.theme
        path: root.versionPath
        selected: root.selected
        showStageDot: true
        onClicked: function(path) { root.focusRequested(path) }
    }
}
