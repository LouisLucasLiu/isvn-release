import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// TopCommandBar — fixed identity + navigation + role/language/theme/backend.
// No control here executes a backend command.
Rectangle {
    id: root
    objectName: "topCommandBar"

    property var theme
    property var i18n
    property var identity
    property string currentPage: "Work"

    signal navRequested(string page)
    signal languageToggleRequested()
    signal themeToggleRequested()

    implicitHeight: root.theme ? root.theme.topBarHeight : 56
    color: root.theme ? root.theme.workSurface0 : "#0a1823"

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: root.theme ? root.theme.border : "#263946"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.theme ? root.theme.gapLg : 16
        anchors.rightMargin: root.theme ? root.theme.gapLg : 16
        spacing: root.theme ? root.theme.gapLg : 16

        Label {
            text: "isvn"
            color: root.theme ? root.theme.textPrimary : "#eef4f8"
            font.pixelSize: 22
            font.bold: true
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 24
            color: root.theme ? root.theme.border : "#263946"
        }

        // Fixed project identity — non-editable, no selector/switcher.
        RowLayout {
            spacing: root.theme ? root.theme.gapXs : 4
            Label {
                text: root.i18n ? root.i18n.text("Project", "專案", "项目") : "Project"
                color: root.theme ? root.theme.textMuted : "#71818d"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }
            Label {
                objectName: "fixedProjectName"
                text: root.identity ? root.identity.projectName : ""
                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                font.pixelSize: root.theme ? root.theme.fontBody : 13
            }
        }

        RowLayout {
            spacing: root.theme ? root.theme.gapXs : 4
            Label {
                text: root.i18n ? root.i18n.text("Workspace", "工作區", "工作区") : "Workspace"
                color: root.theme ? root.theme.textMuted : "#71818d"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }
            Label {
                objectName: "fixedWorkspacePath"
                text: root.identity ? root.identity.workspacePath : ""
                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                font.pixelSize: root.theme ? root.theme.fontBody : 13
                font.family: root.theme ? root.theme.monoFont : "monospace"
                elide: Text.ElideMiddle
                Layout.maximumWidth: 220
            }
        }

        Item { Layout.fillWidth: true }

        ProductNavigation {
            id: navigation
            theme: root.theme
            i18n: root.i18n
            currentPage: root.currentPage
            onNavRequested: function(page) { root.navRequested(page) }
        }

        Item { Layout.fillWidth: true }

        Common.RoleBadge {
            theme: root.theme
            role: root.identity ? root.identity.role : "UNKNOWN"
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 24
            color: root.theme ? root.theme.border : "#263946"
        }

        // Language control (front-end only).
        Button {
            objectName: "languageControl"
            flat: true
            implicitHeight: 28
            hoverEnabled: true
            contentItem: Label {
                text: root.i18n ? (root.i18n.language === "en" ? "English"
                                   : root.i18n.language === "zh_CN" ? "简体中文" : "繁體中文")
                                : "繁體中文"
                color: root.theme ? root.theme.textSecondary : "#aab8c2"
                font.pixelSize: root.theme ? root.theme.fontData : 12
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: "transparent" }
            onClicked: root.languageToggleRequested()
        }

        // Theme control (front-end only).
        Button {
            objectName: "themeControl"
            flat: true
            implicitHeight: 28
            hoverEnabled: true
            Accessible.name: "Toggle theme"
            contentItem: Label {
                text: (root.theme && !root.theme.dark) ? "☀" : "☾"
                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                font.pixelSize: 18
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: "transparent" }
            onClicked: root.themeToggleRequested()
        }

        // Backend/build status display; this bar does not initiate commands.
        RowLayout {
            spacing: root.theme ? root.theme.gapXs : 4
            Common.StatusDot {
                theme: root.theme
                state: "ok"
                label: ""
                Layout.preferredWidth: 8
            }
            Label {
                text: root.i18n ? root.i18n.text("Backend Ready", "後端就緒", "后端就绪") : "Backend Ready"
                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }
        }
    }
}
