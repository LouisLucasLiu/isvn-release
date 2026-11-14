import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property var theme
    property string role: "UNKNOWN"

    implicitWidth: content.implicitWidth + (root.theme ? root.theme.gapMd * 2 : 24)
    implicitHeight: 28
    radius: root.theme ? root.theme.radius : 6
    color: root.theme ? root.theme.bgElevated : "#f1f5f9"
    border.color: root.theme ? root.theme.border : "#d0d7de"

    function normalizedRole() {
        if (root.role === "PM" || root.role === "MEMBER" || root.role === "DENIED")
            return root.role
        return "UNKNOWN"
    }

    function roleState() {
        if (root.normalizedRole() === "PM")
            return "ok"
        if (root.normalizedRole() === "MEMBER")
            return "active"
        if (root.normalizedRole() === "DENIED")
            return "danger"
        return "neutral"
    }

    RowLayout {
        id: content
        anchors.centerIn: parent
        spacing: root.theme ? root.theme.gapXs : 4

        StatusDot {
            theme: root.theme
            state: root.roleState()
            label: ""
            Layout.preferredWidth: 8
        }

        Label {
            text: root.normalizedRole()
            color: root.theme ? root.theme.textPrimary : "#1f2937"
            font.pixelSize: root.theme ? root.theme.fontData : 12
            font.bold: true
            font.family: root.theme ? root.theme.monoFont : ""
        }
    }
}
