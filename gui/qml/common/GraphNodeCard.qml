import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Rectangle {
    id: root

    property QtObject theme
    property QtObject i18n
    property string versionPath: ""
    property string versionLabel: ""
    property string subtitle: ""
    property string stage: ""
    property string actor: ""
    property string timestamp: ""
    property string note: ""
    property bool selected: false
    property bool removed: false
    property bool superseded: false
    property bool current: false
    property bool showStage: true
    property bool showNotes: true

    signal clicked(string versionPath)
    signal rightClicked(string versionPath)
    signal hovered(string versionPath)

    function text(en, zhTw, zhCn) {
        return root.i18n ? root.i18n.text(en, zhTw, zhCn) : en
    }

    implicitWidth: 220
    implicitHeight: 92
    radius: root.theme ? root.theme.radius : 6
    color: root.theme ? (root.current ? root.theme.bgElevated : root.theme.bgPanel) : "#ffffff"
    border.width: root.selected ? 2 : 1
    border.color: root.selected && root.theme ? root.theme.accent :
                  (root.current && root.theme ? root.theme.accentOk :
                   (root.theme ? root.theme.border : "#d0d7de"))
    opacity: root.removed || root.superseded ? 0.62 : 1.0

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.theme ? root.theme.gapSm : 8
        spacing: root.theme ? root.theme.gapXs : 4

        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapXs : 4

            Rectangle {
                visible: root.showStage
                Layout.preferredWidth: 9
                Layout.preferredHeight: 9
                radius: 5
                color: root.theme ? root.theme.stageColor(root.stage.length > 0 ? "/" + root.stage + "/" : root.versionPath) : "#3fb6c7"
            }

            Label {
                text: root.versionLabel.length > 0 ? root.versionLabel : root.versionPath
                color: root.theme ? root.theme.textPrimary : "#1f2937"
                font.pixelSize: root.theme ? root.theme.fontBody : 13
                font.bold: true
                elide: Text.ElideMiddle
                Layout.fillWidth: true
                ToolTip.text: root.versionPath
                ToolTip.visible: hover.hovered && root.versionPath.length > 0
            }
        }

        Label {
            text: root.subtitle
            visible: root.subtitle.length > 0
            color: root.theme ? root.theme.textSecondary : "#59636e"
            font.pixelSize: root.theme ? root.theme.fontCaption : 11
            font.family: root.theme ? root.theme.monoFont : "monospace"
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }

        Label {
            text: root.timestamp
            visible: root.timestamp.length > 0
            color: root.theme ? root.theme.textSecondary : "#59636e"
            font.pixelSize: root.theme ? root.theme.fontCaption : 11
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Label {
            text: root.actor
            visible: root.actor.length > 0
            color: root.theme ? root.theme.textSecondary : "#59636e"
            font.pixelSize: root.theme ? root.theme.fontCaption : 11
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapXs : 4

            StatusDot {
                visible: root.current
                theme: root.theme
                state: "ok"
                label: root.text("Current", "目前", "当前")
                Layout.fillWidth: true
            }

            StatusDot {
                visible: root.removed || root.superseded
                theme: root.theme
                state: root.removed ? "danger" : "warn"
                label: root.removed ? root.text("Removed", "已移除", "已移除") :
                                      root.text("Updated", "已更新", "已更新")
                Layout.fillWidth: true
            }
        }

        Label {
            text: root.note
            visible: root.showNotes && root.note.length > 0
            color: root.theme ? root.theme.textSecondary : "#59636e"
            font.pixelSize: root.theme ? root.theme.fontCaption : 11
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: if (hovered) root.hovered(root.versionPath)
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton)
                root.rightClicked(root.versionPath)
            else
                root.clicked(root.versionPath)
        }
    }
}
