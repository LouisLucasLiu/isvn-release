import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common
import "../work" as Work

// ProductShell — the default source shell.  Main.qml remains the
// complete runtime rollback root selected only by ISVN_GUI_SHELL=legacy.
Item {
    id: root
    objectName: "productShell"

    // Theme / i18n are created internally so the shell is self-contained for the
    // preview host and tests, but remain the same reusable primitives the legacy
    // shell uses.
    property alias theme: themeObject
    property alias i18n: i18nObject
    property alias workState: workStateObject
    property alias workModel: workModelObject
    property alias readController: readControllerObject
    property string currentPage: "Work"

    // Real-read mode binds the Work normal state to the live CLI read contract
    // via WorkReadController. When false, the typed mock model
    // drives the screen (preview/state QA only).
    property bool readMode: false
    property var isvnCli: null
    property var sessionStore: null
    property var roleResolver: null
    property string projectPath: ""
    // Build identity of this GUI binary and the workspace it was started in.
    // Both are supplied by the application entry point; a host without them
    // (preview, tests) says so instead of showing a made-up identity.
    property var buildInfo: null
    property string workspaceRoot: ""

    readonly property var dataSource: root.readMode ? readControllerObject : workModelObject
    // A fixed, safe shell identity independent of the currently visible page.
    readonly property var shellIdentity: ({
        "projectName": readControllerObject.projectName,
        "workspacePath": "",
        "role": roleResolver ? roleResolver.roleName : "UNKNOWN",
        "buildIdentity": root.buildInfo && root.buildInfo.buildIdentity
                         ? root.buildInfo.buildIdentity : "isvn-gui (build identity unavailable)",
        "backendState": isvnCli && isvnCli.running ? "busy" : "ready"
    })

    Common.Theme { id: themeObject; objectName: "workTheme" }
    Common.I18n { id: i18nObject; objectName: "workI18n" }

    Work.WorkMockModel { id: workModelObject; objectName: "workModel" }
    Work.WorkReadController {
        id: readControllerObject
        objectName: "workReadController"
        isvnCli: root.isvnCli
        sessionStore: root.sessionStore
        i18n: i18nObject
        projectPath: root.projectPath
        operationCoordinator: operationCoordinatorObject
    }
    Work.WorkUiState {
        id: workStateObject
        objectName: "workState"
        model: root.dataSource
    }
    // The target-keyed coordinator lives above individual Work page instances.
    // session-only and target-keyed, so a result for A survives a navigation
    // to B without being rebound to the visible Active Version.
    Work.OperationCoordinator {
        id: operationCoordinatorObject
        objectName: "workOperationCoordinator"
        isvnCli: root.isvnCli
        activeVersion: readControllerObject.activeVersionId
    }
    property alias operationCoordinator: operationCoordinatorObject

    // Transactional Work action orchestration (read mode only).
    Work.WorkActionController {
        id: actionControllerObject
        objectName: "workActionController"
        isvnCli: root.isvnCli
        sessionStore: root.sessionStore
        i18n: i18nObject
        readController: readControllerObject
        operationCoordinator: operationCoordinatorObject
        activeVersion: readControllerObject.activeVersionId
    }
    property alias actionController: actionControllerObject

    // Atomic Checkin Batch orchestration (read mode only). Bound to the
    // same single-flight IsvnCli; emits only checkin-batch / work-recovery.
    Work.CheckinBatchController {
        id: checkinControllerObject
        objectName: "checkinBatchController"
        isvnCli: root.isvnCli
        sessionStore: root.sessionStore
        i18n: i18nObject
        readController: readControllerObject
        operationCoordinator: operationCoordinatorObject
        activeVersion: readControllerObject.activeVersionId
    }
    property alias checkinController: checkinControllerObject
    Work.ImportController {
        id: importControllerObject
        objectName: "importController"
        isvnCli: root.isvnCli
        sessionStore: root.sessionStore
        i18n: i18nObject
        readController: readControllerObject
        checkinController: checkinControllerObject
        operationCoordinator: operationCoordinatorObject
        activeVersion: readControllerObject.activeVersionId
    }
    property alias importController: importControllerObject
    // Explore's explicit checkout of an existing Version (read mode only).
    ExploreCheckoutController {
        id: exploreCheckoutObject
        objectName: "exploreCheckoutController"
        operationCoordinator: operationCoordinatorObject
        readController: readControllerObject
        i18n: i18nObject
        workspaceRoot: root.workspaceRoot
    }
    property alias exploreCheckoutController: exploreCheckoutObject
    // Development-only WorkPreviewHost may render a controller-owned Checkin
    // error without enabling the production read bridge.
    property bool previewMode: false
    // QML Popups live in the ApplicationWindow overlay and are intentionally
    // outside a named shell item's grabToImage surface.  The development-only
    // host may request an in-tree mirror to render the very same target-keyed
    // Operation Center model; production continues to use the Popup below.
    property bool operationCenterInlineForPreview: false
    property bool operationCenterInlineVisible: false

    // In real-read mode the Active Version comes from the controller.  Work
    // actions use only the target-pinned controller bridge; QML never executes
    // SVN or filesystem mutation itself.
    Binding {
        target: workStateObject; property: "activeVersionId"
        value: readControllerObject.activeVersionId; when: root.readMode
    }
    Binding {
        target: workStateObject; property: "readOnlyBinding"
        value: true; when: root.readMode
    }
    Binding {
        target: workStateObject; property: "stateId"
        value: workStateObject.stateNormal; when: root.readMode
    }

    Component.onCompleted: {
        refreshActivity()
        if (root.readMode) readControllerObject.refresh()
    }

    Binding {
        target: isvnCli
        property: "maintenanceRole"
        value: roleResolver ? roleResolver.roleName : ""
        when: isvnCli !== null
    }

    // ---- Activity Log -------------------------------------------------------
    //
    // What this user changed in this project, newest first, read from the
    // session store.  Only operations that change data are listed; anything
    // else that an older build may have stored is left out.
    property var activityRecords: []

    function activityKind(operationType) {
        var kind = String(operationType || "")
        if (kind.indexOf("work-") === 0) kind = kind.substring(5)
        if (kind === "create") kind = "create-derived"
        // Operation names as the backend's recovery records report them.
        if (kind === "remove-batch") kind = "remove"
        var known = ["make-editable", "revert", "discard-local", "remove", "create-derived",
                     "recovery", "checkin-batch", "import", "explore-checkout"]
        return known.indexOf(kind) >= 0 ? kind : ""
    }

    function refreshActivity() {
        var listed = []
        var all = root.sessionStore ? root.sessionStore.recentActivity() : []
        for (var i = 0; i < all.length; ++i)
            if (activityKind(all[i].operationType).length > 0) listed.push(all[i])
        activityRecords = listed
    }

    function activityOperationText(kind) {
        if (kind === "make-editable") return i18nObject.text("Make Editable", "設為可編輯", "设为可编辑")
        if (kind === "revert") return i18nObject.text("Revert", "還原", "还原")
        if (kind === "discard-local") return i18nObject.text("Discard Local", "捨棄本機", "舍弃本机")
        if (kind === "remove") return i18nObject.text("Remove from Version", "從版本移除", "从版本移除")
        if (kind === "create-derived") return i18nObject.text("Create Derived Version", "建立衍生版本", "创建衍生版本")
        if (kind === "recovery") return i18nObject.text("Recovery", "復原", "恢复")
        if (kind === "checkin-batch") return i18nObject.text("Checkin", "簽入", "签入")
        if (kind === "import") return i18nObject.text("Import", "匯入", "导入")
        if (kind === "explore-checkout") return i18nObject.text("Checkout", "Checkout", "Checkout")
        return kind
    }

    function activityStatusText(status) {
        var value = String(status || "")
        if (value === "succeeded" || value === "success" || value === "imported" ||
                value === "created_checked_out")
            return i18nObject.text("done", "完成", "完成")
        if (value === "committed" || value === "verified_committed")
            return i18nObject.text("committed", "已簽入", "已签入")
        if (value === "failed" || value === "create_failed" || value === "verified_not_committed")
            return i18nObject.text("failed", "失敗", "失败")
        if (value === "checkout_failed")
            return i18nObject.text("created, checkout failed", "已建立，Checkout 失敗", "已创建，Checkout 失败")
        if (value === "rolled_back" || value === "restored" || value === "recovered")
            return i18nObject.text("restored", "已復原", "已恢复")
        if (value === "unknown" || value === "outcome_unknown")
            return i18nObject.text("outcome unknown", "結果未知", "结果未知")
        if (value === "manualReview" || value === "manual_review_required")
            return i18nObject.text("manual review needed", "需要人工檢視", "需要人工检查")
        if (value === "recovery" || value === "recovery_required")
            return i18nObject.text("recovery needed", "需要復原", "需要恢复")
        if (value === "shutdown_requested")
            return i18nObject.text("stopped on exit", "離開時停止", "退出时停止")
        return value
    }

    // "<operation> · <n items> · <status> · r<revision>"; a part without a value is left out.
    function activityTitle(record) {
        var parts = [activityOperationText(activityKind(record.operationType))]
        var count = record.itemCount !== undefined ? parseInt(record.itemCount, 10) : NaN
        if (!isNaN(count) && count > 0)
            parts.push(i18nObject.text(count === 1 ? "1 item" : count + " items",
                                       count + " 個項目", count + " 个项目"))
        var status = activityStatusText(record.status)
        if (status.length > 0) parts.push(status)
        if (record.revision !== undefined && String(record.revision).length > 0)
            parts.push("r" + record.revision)
        return parts.join(" · ")
    }

    function activityDetail(record) {
        var parts = []
        if (record.target !== undefined && String(record.target).length > 0)
            parts.push(String(record.target))
        if (record.endTime !== undefined && String(record.endTime).length > 0)
            parts.push(String(record.endTime))
        return parts.join("   ")
    }

    onSessionStoreChanged: refreshActivity()
    Connections {
        target: root.sessionStore
        ignoreUnknownSignals: true
        function onActivityChanged() { root.refreshActivity() }
    }

    // Activity Log details are opened from the status bar; a bounded popup, not
    // a global inspector.
    function openActivityLog() {
        refreshActivity()
        activityPopup.open()
    }

    function openOperationCenter() {
        if (operationCenterInlineForPreview) {
            operationCenterInlineVisible = true
            return
        }
        operationCenterPopup.open()
    }

    function openTargetOperation(record) {
        if (!record || !record.targetVersion) return false
        // Explicit user navigation only.  It cannot retarget a running
        // operation or apply an A result to the currently visible B.
        currentPage = "Work"
        if (!readControllerObject.selectActive(record.targetVersion)) return false
        Qt.callLater(function() {
            if (record.state !== "recovery" && record.state !== "manualReview") return
            var detail = record.result || ({})
            if (record.operationKind === "checkin-batch")
                checkinControllerObject.presentRecovery(detail)
            else if (record.operationKind === "import")
                importControllerObject.presentRecovery(detail)
            else if (String(record.operationKind).indexOf("work-") === 0 ||
                     record.operationKind === "remove-batch")
                actionControllerObject.presentRecovery(detail)
        })
        return true
    }

    function toggleLanguage() {
        i18nObject.language = i18nObject.language === "en" ? "zh_TW"
                            : i18nObject.language === "zh_TW" ? "zh_CN" : "en"
    }

    Rectangle {
        anchors.fill: parent
        color: themeObject.workSurface0
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TopCommandBar {
            objectName: "shellTopBar"
            Layout.fillWidth: true
            Layout.preferredHeight: themeObject.topBarHeight
            theme: themeObject
            i18n: i18nObject
            identity: root.shellIdentity
            currentPage: root.currentPage
            onNavRequested: function(page) { root.currentPage = page }
            onLanguageToggleRequested: root.toggleLanguage()
            onThemeToggleRequested: themeObject.dark = !themeObject.dark
        }

        // Page instances stay alive under this persistent shell, so F1 target
        // routing, drafts and running executors never depend on page lifetime.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Work.WorkPage {
                objectName: "workPage"
                anchors.fill: parent
                visible: root.currentPage === "Work"
                theme: themeObject
                i18n: i18nObject
                workState: workStateObject
                workModel: root.dataSource
                operationCoordinator: operationCoordinatorObject
                readController: root.readMode ? readControllerObject : null
                actionController: root.readMode ? actionControllerObject : null
                checkinController: (root.readMode || root.previewMode) ? checkinControllerObject : null
                importController: (root.readMode || root.previewMode) ? importControllerObject : null
                onInspectInExploreRequested: function(versionId) {
                    explorePageObject.inspectedVersion = versionId
                    root.currentPage = "Explore"
                }
            }

            HomePage {
                objectName: "homePageLoader"
                anchors.fill: parent
                visible: root.currentPage === "Home"
                theme: themeObject; i18n: i18nObject
                readController: readControllerObject
                sessionStore: root.sessionStore
                recentActivityCount: root.activityRecords.length
                operationCoordinator: operationCoordinatorObject
                onContinueWorkRequested: root.currentPage = "Work"
            }

            ExplorePage {
                id: explorePageObject
                objectName: "explorePageLoader"
                anchors.fill: parent
                visible: root.currentPage === "Explore"
                theme: themeObject; i18n: i18nObject
                readController: readControllerObject
                checkoutController: root.readMode ? exploreCheckoutObject : null
                projectPath: root.projectPath
                onOpenWorkRequested: root.currentPage = "Work"
            }

            MaintenancePage {
                objectName: "maintenancePageLoader"
                anchors.fill: parent
                visible: root.currentPage === "Maintenance"
                theme: themeObject; i18n: i18nObject
                role: root.roleResolver ? root.roleResolver.roleName : "DENIED"
                operationCoordinator: operationCoordinatorObject
            }
        }

        BottomStatusBar {
            objectName: "shellBottomBar"
            Layout.fillWidth: true
            Layout.preferredHeight: themeObject.bottomBarHeight
            theme: themeObject
            i18n: i18nObject
            workState: workStateObject
            identity: root.shellIdentity
            operationCoordinator: operationCoordinatorObject
            readController: root.readMode ? readControllerObject : null
            onActivityLogRequested: root.openActivityLog()
            onOperationCenterRequested: root.openOperationCenter()
        }
    }

    Popup {
        id: operationCenterPopup
        objectName: "operationCenterPopup"
        modal: false
        focus: true
        width: Math.min(620, root.width - 80)
        height: Math.min(420, root.height - 120)
        anchors.centerIn: Overlay.overlay

        background: Rectangle {
            color: themeObject.workSurface1
            border.color: themeObject.border
            radius: themeObject.radius
        }
        contentItem: ColumnLayout {
            spacing: themeObject.gapSm
            Common.SectionHeader {
                Layout.fillWidth: true
                theme: themeObject
                title: i18nObject.text("Operation Center", "操作中心", "操作中心")
                subtitle: i18nObject.text("Target-pinned background operations", "目標固定的背景操作", "目标固定的后台操作")
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: themeObject.gapSm
                model: operationCoordinatorObject.allOperations()
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: 58
                    radius: themeObject.radius
                    color: themeObject.workSurface0
                    border.color: themeObject.border
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: themeObject.gapSm
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label {
                                text: modelData.targetVersion + " · " + modelData.operationKind
                                color: themeObject.textPrimary
                                font.pixelSize: themeObject.fontData
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Label {
                                text: modelData.state + (modelData.errorCode ? " · " + modelData.errorCode : "")
                                color: themeObject.textSecondary
                                font.pixelSize: themeObject.fontCaption
                            }
                        }
                        Common.ActionButton {
                            theme: themeObject
                            text: i18nObject.text("Open", "開啟", "打开")
                            onClicked: {
                                // Explicit user navigation only; completion never changes Active.
                                root.openTargetOperation(modelData)
                                operationCenterPopup.close()
                            }
                        }
                    }
                }
            }
            Common.ActionButton {
                Layout.alignment: Qt.AlignRight
                theme: themeObject
                text: i18nObject.text("Close", "關閉", "关闭")
                onClicked: operationCenterPopup.close()
            }
        }
    }

    // Development capture mirror only. It shares the operation registry data
    // and target/state fields with the user-facing Popup above, allowing
    // deterministic render QA without using window.contentItem as a capture
    // target. It is permanently false in production ProductShell use.
    Rectangle {
        id: operationCenterInlinePreview
        objectName: "operationCenterInlinePreview"
        visible: root.operationCenterInlineForPreview && root.operationCenterInlineVisible
        z: 100
        width: Math.min(500, root.width - 72)
        height: Math.min(330, root.height - 110)
        anchors.right: parent.right
        anchors.rightMargin: themeObject.gapLg
        anchors.bottom: parent.bottom
        anchors.bottomMargin: themeObject.bottomBarHeight + themeObject.gapLg
        color: themeObject.workSurface1
        border.color: themeObject.workAccent
        radius: themeObject.radius

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: themeObject.gapMd
            spacing: themeObject.gapSm
            Common.SectionHeader {
                Layout.fillWidth: true
                theme: themeObject
                title: i18nObject.text("Operation Center", "操作中心", "操作中心")
                subtitle: i18nObject.text("Target-pinned operations", "目標固定的操作", "目标固定的操作")
            }
            Repeater {
                model: operationCoordinatorObject.allOperations()
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 54
                    radius: themeObject.radius
                    color: themeObject.workSurface0
                    border.color: themeObject.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: themeObject.gapSm
                        Label {
                            Layout.fillWidth: true
                            text: modelData.targetVersion + " · " + modelData.operationKind
                            color: themeObject.textPrimary
                            elide: Text.ElideMiddle
                            font.pixelSize: themeObject.fontData
                        }
                        Label {
                            text: modelData.state + (modelData.errorCode ? " · " + modelData.errorCode : "")
                            color: themeObject.textSecondary
                            font.pixelSize: themeObject.fontCaption
                        }
                    }
                }
            }
            Item { Layout.fillHeight: true }
            Common.ActionButton {
                Layout.alignment: Qt.AlignRight
                theme: themeObject
                text: i18nObject.text("Close", "關閉", "关闭")
                onClicked: root.operationCenterInlineVisible = false
            }
        }
    }

    Popup {
        id: activityPopup
        objectName: "activityLogPopup"
        modal: true
        focus: true
        width: Math.min(560, root.width - 80)
        height: Math.min(360, root.height - 120)
        anchors.centerIn: Overlay.overlay

        background: Rectangle {
            color: themeObject.workSurface1
            border.color: themeObject.border
            radius: themeObject.radius
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: themeObject.gapMd
            spacing: themeObject.gapSm

            Common.SectionHeader {
                theme: themeObject
                title: i18nObject.text("Activity Log", "活動紀錄", "活动记录")
                subtitle: i18nObject.text(
                    "Your recent operations in this project, newest first.",
                    "你在此專案最近執行的操作，最新的在最上面。",
                    "你在此项目最近执行的操作，最新的在最上面。")
            }

            // Without a session store (state preview) there are no records; the
            // preview's own status line is shown instead.
            Label {
                objectName: "activityLogEmptyText"
                visible: root.activityRecords.length === 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                text: root.sessionStore
                      ? i18nObject.text("No operations recorded yet.", "尚無操作紀錄。", "尚无操作记录。")
                      : workStateObject.lastAction
                color: themeObject.textSecondary
                font.pixelSize: themeObject.fontBody
                wrapMode: Text.Wrap
                verticalAlignment: Text.AlignTop
            }

            ListView {
                objectName: "activityLogList"
                visible: root.activityRecords.length > 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: themeObject.gapSm
                boundsBehavior: Flickable.StopAtBounds
                model: root.activityRecords
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: ColumnLayout {
                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    spacing: 1
                    Label {
                        Layout.fillWidth: true
                        text: root.activityTitle(modelData)
                        color: themeObject.textPrimary
                        font.pixelSize: themeObject.fontBody
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.activityDetail(modelData)
                        color: themeObject.textSecondary
                        font.pixelSize: themeObject.fontData
                        elide: Text.ElideMiddle
                    }
                }
            }

            Common.ActionButton {
                Layout.alignment: Qt.AlignRight
                theme: themeObject
                text: i18nObject.text("Close", "關閉", "关闭")
                onClicked: activityPopup.close()
            }
        }
    }
}
