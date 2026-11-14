import QtQuick
import QtQuick.Controls

Label {
    id: root

    property var theme
    property string tip: ""

    text: "ⓘ"
    color: theme ? theme.textSecondary : "#596170"
    font.pixelSize: theme ? theme.fontBody : 13
    ToolTip.text: tip
    ToolTip.visible: hover.hovered && tip.length > 0
    ToolTip.delay: 350

    HoverHandler { id: hover }
}
