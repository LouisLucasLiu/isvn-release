import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

ColumnLayout {
    id: root

    property QtObject theme
    property string title: ""
    property string subtitle: ""
    property string tip: ""
    property bool compact: false

    spacing: root.compact ? 0 : (root.theme ? root.theme.gapXs : 4)

    RowLayout {
        Layout.fillWidth: true
        spacing: root.theme ? root.theme.gapXs : 4

        Label {
            text: root.title
            visible: root.title.length > 0
            color: root.theme ? root.theme.textPrimary : "#1f2937"
            font.pixelSize: root.theme ? root.theme.fontSection : 15
            font.bold: true
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        InfoTip {
            visible: root.tip.length > 0
            theme: root.theme
            tip: root.tip
        }
    }

    Label {
        text: root.subtitle
        visible: root.subtitle.length > 0
        color: root.theme ? root.theme.textSecondary : "#59636e"
        font.pixelSize: root.theme ? root.theme.fontCaption : 11
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
}
