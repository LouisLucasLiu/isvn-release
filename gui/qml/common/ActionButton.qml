import QtQuick
import QtQuick.Controls

Button {
    id: root

    property var theme
    property string kind: "secondary"
    property bool dangerous: false

    implicitHeight: 32
    leftPadding: root.theme ? root.theme.gapMd : 12
    rightPadding: root.theme ? root.theme.gapMd : 12

    function bgColor() {
        if (!root.theme)
            return "#f1f5f9"
        if (!root.enabled)
            return root.theme.bgElevated
        if (root.dangerous || root.kind === "danger")
            return root.theme.dark ? "#342126" : "#fff1f0"
        if (root.kind === "primary")
            return root.theme.accent
        if (root.kind === "ghost")
            return "transparent"
        return root.theme.bgElevated
    }

    function fgColor() {
        if (!root.theme)
            return "#1f2937"
        if (!root.enabled)
            return root.theme.textSecondary
        if (root.kind === "primary")
            return root.theme.dark ? root.theme.bgBase : "#ffffff"
        if (root.dangerous || root.kind === "danger")
            return root.theme.accentDanger
        return root.theme.textPrimary
    }

    background: Rectangle {
        color: root.bgColor()
        radius: root.theme ? root.theme.radius : 6
        border.color: !root.theme ? "#d0d7de" :
                      root.dangerous || root.kind === "danger" ? root.theme.accentDanger :
                      (root.kind === "primary" ? root.theme.accent : root.theme.border)
        opacity: root.enabled ? 1.0 : 0.55
    }

    contentItem: Label {
        text: root.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: root.fgColor()
        font.pixelSize: root.theme ? root.theme.fontBody : 13
        font.bold: root.kind === "primary"
        elide: Text.ElideRight
    }
}
