import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    property var theme
    property string state: "neutral"
    property string label: ""

    spacing: root.theme ? root.theme.gapXs : 4
    implicitHeight: Math.max(dot.implicitHeight, textLabel.implicitHeight)

    function stateColor() {
        if (!root.theme)
            return "#59636e"
        if (root.state === "ok")
            return root.theme.accentOk
        if (root.state === "warn")
            return root.theme.accentWarn
        if (root.state === "danger")
            return root.theme.accentDanger
        if (root.state === "active")
            return root.theme.accent
        return root.theme.textSecondary
    }

    Rectangle {
        id: dot
        Layout.preferredWidth: 8
        Layout.preferredHeight: 8
        radius: 4
        color: root.stateColor()
    }

    Label {
        id: textLabel
        text: root.label
        visible: root.label.length > 0
        color: root.theme ? root.theme.textSecondary : "#59636e"
        font.pixelSize: root.theme ? root.theme.fontBody : 13
        elide: Text.ElideRight
        Layout.fillWidth: true
    }
}
