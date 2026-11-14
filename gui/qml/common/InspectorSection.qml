import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Rectangle {
    id: root

    property QtObject theme
    property string title: ""
    property string subtitle: ""
    property string tip: ""
    property bool expanded: true
    property bool collapsible: true
    default property alias content: body.data

    implicitHeight: sectionLayout.implicitHeight + (root.theme ? root.theme.gapMd : 12) * 2
    color: "transparent"
    border.color: root.theme ? root.theme.border : "#d0d7de"
    radius: root.theme ? root.theme.radius : 6

    ColumnLayout {
        id: sectionLayout
        anchors.fill: parent
        anchors.margins: root.theme ? root.theme.gapMd : 12
        spacing: root.theme ? root.theme.gapSm : 8

        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapXs : 4

            ToolButton {
                visible: root.collapsible
                text: root.expanded ? "−" : "+"
                implicitWidth: 24
                implicitHeight: 24
                palette.buttonText: root.theme ? root.theme.textPrimary : "#1f2937"
                onClicked: root.expanded = !root.expanded
            }

            SectionHeader {
                theme: root.theme
                title: root.title
                subtitle: root.subtitle
                tip: root.tip
                compact: true
                Layout.fillWidth: true
            }
        }

        ColumnLayout {
            id: body
            visible: !root.collapsible || root.expanded
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapSm : 8
        }
    }
}
