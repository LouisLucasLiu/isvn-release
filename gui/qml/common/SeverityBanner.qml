import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Rectangle {
    id: root

    property QtObject theme
    property string severity: "info"
    property string title: ""
    property string message: ""
    property bool compact: false

    function accentColor() {
        if (!root.theme)
            return "#3fb6c7"
        if (root.severity === "ok")
            return root.theme.accentOk
        if (root.severity === "warn")
            return root.theme.accentWarn
        if (root.severity === "danger")
            return root.theme.accentDanger
        return root.theme.accent
    }

    implicitHeight: bannerLayout.implicitHeight + (root.compact ? 12 : 18)
    radius: root.theme ? root.theme.radius : 6
    color: root.theme ? root.theme.bgPanel : "#ffffff"
    border.color: root.theme ? root.theme.border : "#d0d7de"

    Rectangle {
        width: 3
        radius: 2
        color: root.accentColor()
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
    }

    ColumnLayout {
        id: bannerLayout
        anchors.fill: parent
        anchors.leftMargin: root.theme ? root.theme.gapMd : 12
        anchors.rightMargin: root.theme ? root.theme.gapSm : 8
        anchors.topMargin: root.compact ? 6 : 9
        anchors.bottomMargin: root.compact ? 6 : 9
        spacing: root.compact ? 0 : (root.theme ? root.theme.gapXs : 4)

        Label {
            text: root.title
            visible: root.title.length > 0
            color: root.theme ? root.theme.textPrimary : "#1f2937"
            font.pixelSize: root.theme ? root.theme.fontBody : 13
            font.bold: true
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Label {
            text: root.message
            visible: root.message.length > 0
            color: root.theme ? root.theme.textSecondary : "#59636e"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}
