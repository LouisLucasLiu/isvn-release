import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Rectangle {
    id: root

    property QtObject theme
    property QtObject i18n
    property string hash: ""
    property int prefixLength: 12
    property bool copyable: false

    function displayHash() {
        if (root.hash.length === 0)
            return "—"
        if (root.hash.length <= root.prefixLength)
            return root.hash
        return root.hash.slice(0, root.prefixLength) + "…"
    }

    implicitWidth: hashRow.implicitWidth + (root.theme ? root.theme.gapMd * 2 : 24)
    implicitHeight: 28
    radius: root.theme ? root.theme.radius : 6
    color: root.theme ? root.theme.bgElevated : "#f1f5f9"
    border.color: root.theme ? root.theme.border : "#d0d7de"

    RowLayout {
        id: hashRow
        anchors.centerIn: parent
        spacing: root.theme ? root.theme.gapXs : 4

        Label {
            text: root.displayHash()
            color: root.theme ? root.theme.textPrimary : "#1f2937"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            font.family: root.theme ? root.theme.monoFont : ""
            ToolTip.text: root.hash
            ToolTip.visible: hover.hovered && root.hash.length > 0

            HoverHandler { id: hover }
        }

        Label {
            visible: root.copyable
            text: root.i18n ? root.i18n.text("copy", "複製", "复制") : "copy"
            color: root.theme ? root.theme.textSecondary : "#59636e"
            font.pixelSize: root.theme ? root.theme.fontCaption : 11
        }
    }
}
