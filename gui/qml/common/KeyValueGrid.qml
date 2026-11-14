import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

ColumnLayout {
    id: root

    property QtObject theme
    property var rows: []
    property bool monoValue: false
    property int keyWidth: 92

    spacing: root.theme ? root.theme.gapXs : 4

    Repeater {
        model: root.rows || []

        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapSm : 8

            Label {
                text: modelData.key || ""
                color: root.theme ? root.theme.textSecondary : "#59636e"
                font.pixelSize: root.theme ? root.theme.fontData : 12
                elide: Text.ElideRight
                Layout.preferredWidth: root.keyWidth
                Layout.maximumWidth: root.keyWidth
            }

            TextEdit {
                text: modelData.value === undefined || modelData.value === null ? "" : String(modelData.value)
                color: root.theme ? root.theme.textPrimary : "#1f2937"
                font.pixelSize: root.theme ? root.theme.fontData : 12
                font.family: root.monoValue && root.theme ? root.theme.monoFont : ""
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
                selectionColor: root.theme ? root.theme.accent : "#3FB6C7"
                ToolTip.text: text
                ToolTip.visible: hover.hovered && text.length > 0
                Layout.fillWidth: true

                HoverHandler { id: hover }
            }
        }
    }
}
