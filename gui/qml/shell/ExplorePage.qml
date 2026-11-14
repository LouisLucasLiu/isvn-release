import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common
import "../workspaceB" as LegacyExplore

// Explore has its own inspected Version.  It never aliases Work Active state.
//
// Selecting a Version here only inspects it.  Checkout and Set Active are two
// separate, explicit actions: Checkout creates the local working copy for the
// inspected Version alone, and Set Active is what opens it in Work.
Item {
    id: root
    objectName: "explorePage"
    property var theme
    property var i18n
    property var readController
    property var checkoutController
    // Project directory for the embedded body's `--project` queries.
    property string projectPath: ""
    property string inspectedVersion: ""
    // Version for which Set Active was refused because it has no local working copy.
    property string activationRefusedFor: ""
    signal openWorkRequested()

    readonly property bool inspectedIsLocal: {
        var versions = root.readController ? root.readController.localVersions : []
        for (var i = 0; versions && i < versions.length; ++i)
            if (versions[i].id === root.inspectedVersion) return true
        return false
    }
    readonly property bool inspectedWellFormed: root.checkoutController
                                                ? root.checkoutController.wellFormed(root.inspectedVersion)
                                                : false
    readonly property string checkoutPhase: root.checkoutController ? root.checkoutController.phase : "idle"
    readonly property bool checkoutBusy: root.checkoutController ? root.checkoutController.busy : false

    // The inspected Version and the embedded body's own selection are one
    // value: a change on either side is copied to the other, so the header
    // field, the body and the Checkout target can never disagree.
    onInspectedVersionChanged: {
        root.activationRefusedFor = ""
        if (legacyExplore.versionFilter !== root.inspectedVersion)
            legacyExplore.versionFilter = root.inspectedVersion
        if (root.checkoutController)
            root.checkoutController.dismiss()
    }

    // Substitutes %1 and %2 in one pass, so inserted text is never re-read as
    // a placeholder or a replacement pattern.
    function fill(template, first, second) {
        return template.replace(/%([12])/g, function(match, index) {
            return index === "1" ? String(first) : String(second)
        })
    }

    readonly property bool activationRefused: root.checkoutPhase === "idle" && !root.inspectedIsLocal &&
                                              root.inspectedVersion.length > 0 &&
                                              root.activationRefusedFor === root.inspectedVersion

    function statusSeverity() {
        if (root.checkoutPhase === "failed") return "danger"
        if (root.checkoutPhase === "done") return "ok"
        if (root.activationRefused) return "warn"
        return "info"
    }

    function statusText() {
        var controller = root.checkoutController
        if (!controller) return ""
        if (controller.phase === "validating")
            return root.fill(root.i18n.text("Checking %1 before checkout…", "正在確認 %1…", "正在确认 %1…"),
                             controller.targetVersion, "")
        if (controller.phase === "running")
            return root.fill(root.i18n.text("Checking out %1…", "正在 checkout %1…", "正在 checkout %1…"),
                             controller.targetVersion, "")
        if (controller.phase === "done")
            return root.fill(root.i18n.text("Checked out %1 to %2. The Active version is unchanged; use Set Active and Open Work to open it.",
                                            "已將 %1 checkout 到 %2。啟用中的版本沒有改變；要開啟它請按「設為啟用並開啟工作」。",
                                            "已将 %1 checkout 到 %2。启用中的版本没有改变；要打开它请按“设为启用并打开工作”。"),
                             controller.targetVersion, controller.resultPath)
        if (controller.phase === "failed")
            return root.fill(root.i18n.text("Checkout of %1 did not complete. %2",
                                            "%1 的 checkout 沒有完成。%2", "%1 的 checkout 没有完成。%2"),
                             controller.targetVersion, controller.reasonText(controller.errorCode))
        if (root.inspectedVersion.length === 0) return ""
        if (root.activationRefused)
            return root.i18n.text("Set Active needs a local working copy. Checkout this version first.",
                                  "要設為啟用，必須先有本機 working copy。請先 Checkout 這個版本。",
                                  "要设为启用，必须先有本地 working copy。请先 Checkout 这个版本。")
        if (!root.inspectedWellFormed)
            return controller.reasonText("INVALID_VERSION")
        if (root.inspectedIsLocal)
            return root.i18n.text("This version is checked out in this workspace.",
                                  "這個版本已在此工作區中。", "这个版本已在此工作区中。")
        return root.i18n.text("This version is not in this workspace. Checkout creates its local working copy only; it does not change the Active version.",
                              "這個版本不在此工作區中。Checkout 只會建立它的本機 working copy，不會改變啟用中的版本。",
                              "这个版本不在此工作区中。Checkout 只会建立它的本地 working copy，不会改变启用中的版本。")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 58
            color: root.theme.workSurface1
            border.color: root.theme.border
            RowLayout {
                anchors.fill: parent
                anchors.margins: root.theme.gapSm
                Label { text: root.i18n.text("Inspected Version", "檢視中的版本", "查看中的版本"); color: root.theme.textMuted }
                TextField {
                    id: inspectField
                    objectName: "exploreInspectedVersion"
                    Layout.fillWidth: true
                    text: root.inspectedVersion
                    placeholderText: root.i18n.text("Project-relative Version", "專案相對版本", "项目相对版本")
                    // The typed text is the inspected Version at every keystroke,
                    // so an action never targets a value other than the one shown.
                    onTextEdited: root.inspectedVersion = text.trim()
                    onEditingFinished: root.inspectedVersion = text.trim()
                }
                Common.ActionButton {
                    objectName: "exploreCheckout"
                    theme: root.theme
                    kind: "primary"
                    visible: !!root.checkoutController && root.inspectedVersion.length > 0 && !root.inspectedIsLocal
                    enabled: root.inspectedWellFormed && !root.checkoutBusy && root.checkoutPhase !== "confirm"
                    text: root.i18n.text("Checkout…", "Checkout…", "Checkout…")
                    onClicked: root.checkoutController.request(root.inspectedVersion)
                }
                Common.ActionButton {
                    objectName: "exploreSetActiveOpenWork"
                    theme: root.theme
                    enabled: root.inspectedVersion.length > 0 && !!root.readController && !root.checkoutBusy
                    text: root.i18n.text("Set Active and Open Work", "設為啟用並開啟工作", "设为启用并打开工作")
                    onClicked: root.readController.revalidateAndSelect(root.inspectedVersion)
                }
            }
        }
        Common.SeverityBanner {
            objectName: "exploreCheckoutStatus"
            Layout.fillWidth: true
            Layout.margins: root.theme.gapSm
            visible: !!root.checkoutController && message.length > 0
            theme: root.theme
            compact: true
            severity: root.statusSeverity()
            message: root.statusText()
        }
        LegacyExplore.WorkspaceB {
            id: legacyExplore
            objectName: "embeddedExploreBody"
            Layout.fillWidth: true
            Layout.fillHeight: true
            i18n: root.i18n
            theme: root.theme
            projectPath: root.projectPath
            shellVersion: ""
            onStatusMessage: function(message) { }
            onVersionFilterChanged: {
                if (root.inspectedVersion !== legacyExplore.versionFilter)
                    root.inspectedVersion = legacyExplore.versionFilter
            }
        }
    }

    Common.ConfirmDialog {
        id: checkoutConfirm
        objectName: "exploreCheckoutConfirm"
        anchors.centerIn: Overlay.overlay
        i18n: root.i18n
        theme: root.theme
        title: root.i18n.text("Checkout version", "Checkout 版本", "Checkout 版本")
        confirmLabel: root.i18n.text("Checkout", "Checkout", "Checkout")
        body: root.checkoutController
              ? root.fill(root.i18n.text("Check out this version into your workspace?\n\nVersion: %1\nLocal folder: %2\n\nOnly this version is checked out. The Active version does not change; use Set Active and Open Work afterwards to work in it.",
                                         "要將這個版本 checkout 到你的工作區嗎？\n\n版本：%1\n本機資料夾：%2\n\n只會 checkout 這一個版本。啟用中的版本不會改變；之後要在這個版本工作，請再按「設為啟用並開啟工作」。",
                                         "要将这个版本 checkout 到你的工作区吗？\n\n版本：%1\n本地文件夹：%2\n\n只会 checkout 这一个版本。启用中的版本不会改变；之后要在这个版本工作，请再按“设为启用并打开工作”。"),
                          root.checkoutController.targetVersion,
                          root.checkoutController.expectedPath(root.checkoutController.targetVersion))
              : ""
        confirmAction: function() { root.checkoutController.confirm() }
        // Closing the dialog any other way than Checkout abandons the request.
        onClosed: if (root.checkoutController) root.checkoutController.cancel()
        // Themed buttons: the two choices stay legible in both themes.
        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.margins: root.theme.gapSm
                spacing: root.theme.gapSm
                Item { Layout.fillWidth: true }
                Common.ActionButton {
                    objectName: "exploreCheckoutCancelButton"
                    theme: root.theme
                    text: root.i18n.text("Cancel", "取消", "取消")
                    onClicked: checkoutConfirm.cancel()
                }
                Common.ActionButton {
                    objectName: "exploreCheckoutConfirmButton"
                    theme: root.theme
                    kind: "primary"
                    text: root.i18n.text("Checkout", "Checkout", "Checkout")
                    onClicked: checkoutConfirm.confirm()
                }
            }
        }
    }

    Connections {
        target: root.checkoutController ? root.checkoutController : null
        function onPhaseChanged() {
            if (root.checkoutController.phase === "confirm") checkoutConfirm.open()
            else if (checkoutConfirm.visible) checkoutConfirm.close()
        }
    }
    Connections {
        target: root.readController
        ignoreUnknownSignals: true
        function onExplicitActiveResolved(ok, version) {
            if (version !== root.inspectedVersion) return
            if (ok) {
                // The checkout note says Active is unchanged; that is over now.
                if (root.checkoutController) root.checkoutController.dismiss()
                root.openWorkRequested()
            } else {
                root.activationRefusedFor = version
            }
        }
    }
}
