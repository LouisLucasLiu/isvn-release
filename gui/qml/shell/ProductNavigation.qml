import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// ProductNavigation — the Product Shell primary navigation.
// Home, Work, Explore, and Maintenance share the same persistent shell.
RowLayout {
    id: root
    objectName: "productNavigation"

    property var theme
    property var i18n
    property string currentPage: "Work"

    signal navRequested(string page)

    // Stable page keys; visible labels are localized reactively.
    readonly property var pages: ["Home", "Work", "Explore", "Maintenance"]

    spacing: root.theme ? root.theme.gapXs : 4

    function pageLabel(key) {
        if (!root.i18n)
            return key
        if (key === "Home")
            return root.i18n.text("Home", "首頁", "首页")
        if (key === "Work")
            return root.i18n.text("Work", "工作", "工作")
        if (key === "Explore")
            return root.i18n.text("Explore", "探索", "探索")
        if (key === "Maintenance")
            return root.i18n.text("Maintenance", "維運", "维运")
        return key
    }

    Repeater {
        model: root.pages

        delegate: Button {
            id: navButton
            required property string modelData
            objectName: "nav_" + modelData

            readonly property bool current: root.currentPage === modelData

            implicitHeight: 40
            leftPadding: root.theme ? root.theme.gapMd : 12
            rightPadding: root.theme ? root.theme.gapMd : 12
            hoverEnabled: true
            Accessible.name: root.pageLabel(modelData)

            onClicked: root.navRequested(modelData)

            contentItem: Label {
                text: root.pageLabel(navButton.modelData)
                color: navButton.current
                       ? (root.theme ? (root.theme.dark ? "#ffffff" : root.theme.textPrimary) : "#ffffff")
                       : (root.theme ? root.theme.textSecondary : "#59636e")
                font.pixelSize: root.theme ? root.theme.fontBody + 1 : 14
                font.bold: navButton.current
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: root.theme ? root.theme.radius : 6
                color: navButton.current
                       ? (root.theme ? root.theme.workAccent : "#1687f8")
                       : (navButton.hovered
                          ? (root.theme ? root.theme.workSurfaceHover : "#172e3d")
                          : "transparent")
                border.color: navButton.activeFocus
                              ? (root.theme ? root.theme.workAccentHover : "#2b96ff")
                              : "transparent"
                border.width: navButton.activeFocus ? 2 : 0
            }
        }
    }
}
