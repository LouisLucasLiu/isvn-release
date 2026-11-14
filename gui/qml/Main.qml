import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "common" as Common
import "workspaceA" as WorkspaceA
import "workspaceB" as WorkspaceB
import "workspaceC" as WorkspaceC

ApplicationWindow {
    id: root
    width: 1440
    height: 860
    minimumWidth: 1120
    minimumHeight: 720
    visible: true
    title: "isvn"

    property int activeWorkspace: 0
    property string currentVersion: ""
    property string statusText: ""

    Common.I18n {
        id: i18n
        objectName: "i18n"
    }
    Common.Theme {
        id: theme
        objectName: "theme"
    }

    ListModel { id: activityLog }

    function logActivity(message) {
        if (!message || message.length === 0)
            return
        activityLog.insert(0, {
            time: Qt.formatTime(new Date(), "hh:mm:ss"),
            text: message,
            isError: /fail|error|not |無法|錯誤|失敗|cannot|denied/i.test(message)
        })
        while (activityLog.count > 200)
            activityLog.remove(activityLog.count - 1)
    }

    function copyAllActivityLog() {
        var lines = []
        for (var i = 0; i < activityLog.count; i++) {
            var entry = activityLog.get(i)
            lines.push(entry.time + "  " + entry.text)
        }
        activityLogClipboard.text = lines.join("\n")
        activityLogClipboard.selectAll()
        activityLogClipboard.copy()
        activityLogClipboard.deselect()
    }

    onStatusTextChanged: logActivity(statusText)

    Component.onCompleted: roleResolver.refresh(".")

    // Wire resolved role to the C++ backend so IsvnCli can block mutating
    // maintenance commands for non-PM roles at the dispatch layer.
    Binding {
        target: isvnCli
        property: "maintenanceRole"
        value: roleResolver.roleName
    }

    function displayVersion() {
        return currentVersion.length > 0 ? currentVersion :
               i18n.text("No version selected", "尚未選擇版本", "尚未选择版本")
    }

    function roleState() {
        if (roleResolver.roleName === "PM")
            return "ok"
        if (roleResolver.roleName === "MEMBER")
            return "active"
        if (roleResolver.roleName === "DENIED")
            return "danger"
        return "neutral"
    }

    function serviceState() {
        if (isvnCli.running)
            return "active"
        return roleResolver.allowed ? "ok" : "danger"
    }

    function serviceLabel() {
        if (isvnCli.running)
            return i18n.text("Busy", "執行中", "执行中")
        return roleResolver.allowed ?
                    i18n.text("Ready", "就緒", "就绪") :
                    i18n.text("Error", "錯誤", "错误")
    }

    function visibleStatusText() {
        if (isvnCli.running)
            return i18n.text("Working on the requested action", "正在處理指定動作", "正在处理指定动作")
        if (statusText.length > 0)
            return statusText
        if (!roleResolver.allowed)
            return i18n.text("Open an isvn project or workspace to continue.",
                             "請在 isvn 專案或工作區中開啟。",
                             "请在 isvn 项目或工作区中打开。")
        return i18n.text("Ready", "就緒", "就绪")
    }

    color: theme.bgBase
    palette.window: theme.bgBase
    palette.windowText: theme.textPrimary
    palette.base: theme.bgElevated
    palette.alternateBase: theme.bgPanel
    palette.text: theme.textPrimary
    palette.button: theme.bgElevated
    palette.buttonText: theme.textPrimary
    palette.highlight: theme.accent
    palette.highlightedText: theme.bgBase
    palette.placeholderText: theme.textSecondary

    // Top command bar: product identity, workspace tabs (the only global
    // workspace switcher), project path, backend status, role, language, theme.
    header: ToolBar {
        height: 56
        background: Rectangle {
            color: theme.bgPanel
            border.width: 0
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.border
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: theme.gapLg
            anchors.rightMargin: theme.gapLg
            spacing: theme.gapMd

            ColumnLayout {
                Layout.preferredWidth: 152
                spacing: 0
                Label {
                    text: "isvn"
                    color: theme.textPrimary
                    font.pixelSize: theme.fontSection + 3
                    font.bold: true
                }
                Label {
                    text: i18n.text("IC Design Version Console",
                                    "IC 設計版本控制台",
                                    "IC 设计版本控制台")
                    color: theme.textSecondary
                    font.pixelSize: theme.fontCaption
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            TabBar {
                id: tabs
                enabled: roleResolver.allowed
                Layout.preferredWidth: 420
                Layout.alignment: Qt.AlignVCenter
                currentIndex: root.activeWorkspace
                onCurrentIndexChanged: root.activeWorkspace = currentIndex
                background: Rectangle { color: "transparent" }

                TabButton {
                    objectName: "workspaceATab"
                    text: i18n.text("Workspace", "工作區", "工作区")
                    contentItem: Label {
                        text: parent.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: parent.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: theme.fontBody
                        font.bold: parent.checked
                    }
                    background: Rectangle {
                        color: parent.checked ? theme.bgElevated : "transparent"
                        border.color: parent.checked ? theme.accent : theme.border
                        radius: theme.radius
                    }
                }
                TabButton {
                    objectName: "workspaceBTab"
                    text: i18n.text("History", "歷史", "历史")
                    contentItem: Label {
                        text: parent.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: parent.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: theme.fontBody
                        font.bold: parent.checked
                    }
                    background: Rectangle {
                        color: parent.checked ? theme.bgElevated : "transparent"
                        border.color: parent.checked ? theme.accent : theme.border
                        radius: theme.radius
                    }
                }
                TabButton {
                    objectName: "workspaceCTab"
                    text: roleResolver.roleName === "PM" ?
                          i18n.text("Maintenance", "維護", "维护") :
                          i18n.text("Maintenance (PM)", "維護 (PM)", "维护 (PM)")
                    enabled: roleResolver.roleName === "PM"
                    contentItem: Label {
                        text: parent.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: !parent.enabled ? theme.textSecondary :
                               (parent.checked ? theme.textPrimary : theme.textSecondary)
                        opacity: parent.enabled ? 1.0 : 0.45
                        font.pixelSize: theme.fontBody
                        font.bold: parent.checked
                    }
                    background: Rectangle {
                        color: parent.checked ? theme.bgElevated : "transparent"
                        border.color: parent.checked ? theme.accent : theme.border
                        radius: theme.radius
                        opacity: parent.enabled ? 1.0 : 0.55
                    }
                    ToolTip.visible: !enabled && hovered
                    ToolTip.text: i18n.text("PM only", "僅 PM 可用", "仅 PM 可用")
                }
            }

            // Project path: visible but not dominant; takes the flexible middle space.
            Label {
                text: roleResolver.projectDir.length > 0 ? roleResolver.projectDir :
                      i18n.text("Project not opened", "尚未開啟專案", "尚未打开项目")
                color: theme.textSecondary
                font.family: theme.monoFont
                font.pixelSize: theme.fontData
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }

            Common.StatusDot {
                theme: theme
                state: root.serviceState()
                label: root.serviceLabel()
                Layout.preferredWidth: 86
            }

            Common.RoleBadge {
                theme: theme
                role: roleResolver.roleName
            }

            ComboBox {
                objectName: "languageSelector"
                Layout.preferredWidth: 132
                model: i18n.languageOptions
                textRole: "label"
                valueRole: "code"
                currentIndex: i18n.languageOptions.findIndex(function(option) {
                    return option.code === i18n.language
                })
                onActivated: function(index) {
                    i18n.language = i18n.languageOptions[index].code
                }
            }

            Common.ActionButton {
                objectName: "themeToggle"
                theme: theme
                kind: "ghost"
                text: theme.dark ? i18n.text("Light", "淺色", "浅色") :
                                   i18n.text("Dark", "深色", "深色")
                onClicked: theme.dark = !theme.dark
            }
        }
    }

    // Single global bottom status bar: status dot + last action, selected
    // version, and the activity log toggle.
    footer: ToolBar {
        height: 32
        background: Rectangle {
            color: theme.bgPanel
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: theme.border
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: theme.gapLg
            anchors.rightMargin: theme.gapLg
            spacing: theme.gapMd

            Common.StatusDot {
                theme: theme
                state: root.serviceState()
                label: root.visibleStatusText()
                Layout.fillWidth: true
            }

            Label {
                text: root.displayVersion()
                color: theme.textSecondary
                font.pixelSize: theme.fontData
                font.family: theme.monoFont
                elide: Text.ElideMiddle
                Layout.preferredWidth: 320
            }

// Compact build identity: click to open the About popup.
            Label {
                objectName: "buildIdentityLabel"
                text: (buildInfo ? buildInfo.program + " · " + buildInfo.commit : "isvn-gui")
                color: theme.textSecondary
                font.pixelSize: theme.fontCaption
                font.family: theme.monoFont
                ToolTip.text: buildInfo
                    ? buildInfo.program + " " + buildInfo.version + "\ncommit: " + buildInfo.commit
                      + "\nbuild: " + buildInfo.buildTime + " (" + buildInfo.buildType + ")\nqt: " + buildInfo.qt
                    : ""
                ToolTip.visible: idHover.hovered
                HoverHandler { id: idHover }
                TapHandler { onTapped: aboutPopup.open() }
            }

            Common.ActionButton {
                objectName: "activityLogToggle"
                theme: theme
                kind: "ghost"
                text: i18n.text("Log", "記錄", "记录")
                onClicked: logPopup.opened ? logPopup.close() : logPopup.open()
            }

            Common.BusyIndicator {
                running: isvnCli.running
            }
        }
    }

// About / build identity popup — small, non-intrusive.
    Popup {
        id: aboutPopup
        objectName: "aboutPopup"
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: 360
        padding: theme.gapMd
        background: Rectangle {
            color: theme.bgElevated
            border.color: theme.border
            radius: theme.radius
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: theme.gapSm
            Label {
                text: i18n.text("About", "關於", "关于")
                color: theme.textPrimary
                font.pixelSize: theme.fontSection
                font.bold: true
            }
            Label {
                text: "isvn — " + i18n.text("IC Design Version Console", "IC 設計版本控制台", "IC 设计版本控制台")
                color: theme.textSecondary
                font.pixelSize: theme.fontData
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Common.KeyValueGrid {
                Layout.fillWidth: true
                theme: theme
                monoValue: true
                keyWidth: 96
                rows: buildInfo ? [
                    { key: i18n.text("Program", "程式", "程序"), value: buildInfo.program },
                    { key: i18n.text("Version", "版本", "版本"), value: buildInfo.version },
                    { key: i18n.text("Commit", "Commit", "Commit"), value: buildInfo.commit },
                    { key: i18n.text("Build time", "建置時間", "构建时间"), value: buildInfo.buildTime },
                    { key: i18n.text("Build type", "建置類型", "构建类型"), value: buildInfo.buildType },
                    { key: "Qt", value: buildInfo.qt }
                ] : []
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Common.ActionButton {
                    theme: theme
                    kind: "ghost"
                    text: i18n.text("Close", "關閉", "关闭")
                    onClicked: aboutPopup.close()
                }
            }
        }
    }

    Popup {
        id: logPopup
        objectName: "activityLogPopup"
        width: Math.min(680, root.width - theme.gapLg * 2)
        height: Math.min(320, root.height - 160)
        x: (parent ? parent.width : root.width) - width - theme.gapLg
        y: (parent ? parent.height : root.height) - height - theme.gapLg
        modal: false
        focus: true
        padding: theme.gapMd

        background: Rectangle {
            color: theme.bgPanel
            border.color: theme.border
            radius: theme.radius
        }

        contentItem: ColumnLayout {
            spacing: theme.gapSm

            RowLayout {
                Layout.fillWidth: true
                spacing: theme.gapSm

                Label {
                    text: i18n.text("Activity log", "操作記錄", "操作记录")
                    color: theme.textPrimary
                    font.pixelSize: theme.fontSection
                    font.bold: true
                    Layout.fillWidth: true
                }
                Common.ActionButton {
                    theme: theme
                    kind: "ghost"
                    enabled: activityLog.count > 0
                    text: i18n.text("Copy all", "複製全部", "复制全部")
                    onClicked: root.copyAllActivityLog()
                }
                Common.ActionButton {
                    theme: theme
                    kind: "ghost"
                    text: i18n.text("Clear", "清除", "清除")
                    onClicked: activityLog.clear()
                }
                Common.ActionButton {
                    theme: theme
                    kind: "ghost"
                    text: i18n.text("Close", "關閉", "关闭")
                    onClicked: logPopup.close()
                }
            }

            // Hidden helper used to push the whole log to the system clipboard.
            TextEdit {
                id: activityLogClipboard
                visible: false
                readOnly: true
            }

            Common.EmptyState {
                visible: activityLog.count === 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                theme: theme
                title: i18n.text("No activity yet", "尚無操作記錄", "尚无操作记录")
                message: i18n.text("Command results and errors will appear here.",
                                   "指令結果與錯誤會顯示在這裡。",
                                   "指令结果与错误会显示在这里。")
            }

            ListView {
                visible: activityLog.count > 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: activityLog
                spacing: 2
                delegate: RowLayout {
                    width: ListView.view.width
                    spacing: theme.gapSm

                    TextEdit {
                        text: model.time
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        textFormat: TextEdit.PlainText
                        color: theme.textSecondary
                        selectionColor: theme.accent
                        font.family: theme.monoFont
                        font.pixelSize: theme.fontData
                        Layout.preferredWidth: 64
                        Layout.alignment: Qt.AlignTop
                    }
                    TextEdit {
                        text: model.text
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        textFormat: TextEdit.PlainText
                        color: model.isError ? theme.accentDanger : theme.textPrimary
                        selectionColor: theme.accent
                        font.pixelSize: theme.fontData
                        wrapMode: TextEdit.WrapAnywhere
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: theme.gapMd
        spacing: theme.gapMd

        Rectangle {
            visible: !roleResolver.allowed
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: theme.bgElevated
            border.color: theme.accentDanger
            radius: theme.radius

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: theme.gapMd
                anchors.rightMargin: theme.gapMd
                spacing: theme.gapSm

                Common.StatusDot {
                    theme: theme
                    state: "danger"
                    label: i18n.text("This folder is not available as an isvn project or workspace.",
                                     "目前位置不是可用的 isvn 專案或工作區。",
                                     "当前位置不是可用的 isvn 项目或工作区。")
                    Layout.fillWidth: true
                }
            }
        }

        // No global left navigation rail and no global right inspector: the top
        // tabs are the only workspace switcher, and each workspace owns its own
        // full-width layout and inspectors.
        StackLayout {
            enabled: roleResolver.allowed
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.activeWorkspace

            WorkspaceA.WorkspaceA {
                i18n: i18n
                theme: theme
                projectPath: roleResolver.projectDir
                onVersionFocused: function(version) {
                    root.currentVersion = version
                    root.statusText = i18n.text("Selected version: ", "目前版本：", "当前版本：") + version
                }
                onOperationStatus: function(message) {
                    root.statusText = message
                }
            }

            WorkspaceB.WorkspaceB {
                i18n: i18n
                theme: theme
                projectPath: roleResolver.projectDir
                shellVersion: root.currentVersion
                onStatusMessage: function(message) {
                    root.statusText = message
                }
            }

            WorkspaceC.WorkspaceC {
                objectName: "workspaceC"
                i18n: i18n
                theme: theme
                pmMode: roleResolver.roleName === "PM"
                onStatusMessage: function(message) {
                    root.statusText = message
                }
            }
        }
    }
}
