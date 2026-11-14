import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Rectangle {
    id: root

    property QtObject theme
    property QtObject i18n
    property bool showTags: false
    property bool showStage: true
    property bool showNotes: true

    signal zoomInRequested()
    signal zoomOutRequested()
    signal fitRequested()
    signal resetRequested()

    function text(en, zhTw, zhCn) {
        return root.i18n ? root.i18n.text(en, zhTw, zhCn) : en
    }

    implicitHeight: 38
    implicitWidth: toolbarRow.implicitWidth + (root.theme ? root.theme.gapMd * 2 : 24)
    radius: root.theme ? root.theme.radius : 6
    color: root.theme ? root.theme.bgPanel : "#ffffff"
    border.color: root.theme ? root.theme.border : "#d0d7de"

    RowLayout {
        id: toolbarRow
        anchors.fill: parent
        anchors.leftMargin: root.theme ? root.theme.gapSm : 8
        anchors.rightMargin: root.theme ? root.theme.gapSm : 8
        spacing: root.theme ? root.theme.gapXs : 4

        ActionButton {
            theme: root.theme
            text: "−"
            kind: "ghost"
            Layout.preferredWidth: 32
            onClicked: root.zoomOutRequested()
        }
        ActionButton {
            theme: root.theme
            text: "+"
            kind: "ghost"
            Layout.preferredWidth: 32
            onClicked: root.zoomInRequested()
        }
        ActionButton {
            theme: root.theme
            text: root.text("Fit to view", "符合視窗", "适合窗口")
            kind: "secondary"
            onClicked: root.fitRequested()
        }
        ActionButton {
            theme: root.theme
            text: root.text("Reset view", "重設視圖", "重设视图")
            kind: "secondary"
            onClicked: root.resetRequested()
        }

        CheckBox {
            text: root.text("Show stage", "顯示階段", "显示阶段")
            checked: root.showStage
            onToggled: {
                root.showStage = checked
            }
        }
        CheckBox {
            text: root.text("Show notes", "顯示備註", "显示备注")
            checked: root.showNotes
            onToggled: {
                root.showNotes = checked
            }
        }
        CheckBox {
            text: root.text("Tags", "標籤", "标签")
            checked: root.showTags
            onToggled: {
                root.showTags = checked
            }
        }
    }
}
