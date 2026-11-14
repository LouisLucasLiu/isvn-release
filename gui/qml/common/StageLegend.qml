import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Flow {
    id: root

    property QtObject theme
    property var stages: ["RTL", "SYN", "CTS", "ROUTE", "SIGNOFF", "OTHER"]

    spacing: root.theme ? root.theme.gapSm : 8

    Repeater {
        model: root.stages || []

        Row {
            spacing: root.theme ? root.theme.gapXs : 4

            Rectangle {
                width: 8
                height: 8
                radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: root.theme ? root.theme.stageColor("/" + String(modelData) + "/") : "#3fb6c7"
            }

            Label {
                text: String(modelData)
                color: root.theme ? root.theme.textSecondary : "#59636e"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }
        }
    }
}
