import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// WorkOperationOverlay — the transactional operation surface. It renders the
// WorkActionController phase (confirm / applying / result / recovery /
// outcomeUnknown / create) for Make Editable, Revert, Discard Local, Remove from
// Version, and Create Derived Version. It consolidates the roles of the
// LocalActionConfirm / RemoveConfirm / OperationResult / RecoveryRequired /
// CreateDerivedFlow components into one bounded surface. It only calls
// WorkActionController methods; it performs no
// filesystem/SVN work and shows a single primary action per state.
Rectangle {
    id: root

    property var theme
    property var i18n
    property var controller
    property string activeVersion: ""

    visible: controller && controller.phase !== "idle" &&
             (!controller.operationTarget || controller.operationTarget === activeVersion)
    color: theme ? theme.workSurface0 : "#0a1823"

    function t(en, tw, cn) { return root.i18n ? root.i18n.text(en, tw, cn) : en }
    function isDestructive() {
        return root.controller && (root.controller.action === "revert" ||
                                   root.controller.action === "discard-local" ||
                                   root.controller.action === "remove")
    }
    function actionTitle() {
        if (!root.controller) return ""
        var a = root.controller.action
        if (a === "make-editable") return t("Make Editable", "設為可編輯", "设为可编辑")
        if (a === "revert") return t("Revert", "還原", "还原")
        if (a === "discard-local") return t("Discard Local", "捨棄本機", "舍弃本机")
        if (a === "remove") return t("Remove from Version", "從版本移除", "从版本移除")
        if (a === "create") return t("Create Derived Version", "建立衍生版本", "创建衍生版本")
        return a
    }

    MouseArea { anchors.fill: parent }  // swallow clicks to the panels behind

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.theme ? root.theme.gapLg : 16
        spacing: root.theme ? root.theme.gapMd : 12

        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.actionTitle()
            subtitle: root.controller ? (root.t("Step", "步驟", "步骤") + ": " + root.controller.phase) : ""
        }

        // ---- CONFIRM (local actions) ----
        Loader {
            Layout.fillWidth: true
            active: root.controller && root.controller.phase === "confirm" &&
                    root.controller.action !== "remove"
            visible: active
            sourceComponent: Component {
                ColumnLayout {
                    spacing: root.theme ? root.theme.gapMd : 12
                    Common.SeverityBanner {
                        Layout.fillWidth: true
                        theme: root.theme
                        severity: root.isDestructive() ? "warn" : "info"
                        title: root.isDestructive()
                               ? root.t("This discards local changes", "這會捨棄本機變更", "这会舍弃本机变更") : ""
                        message: root.controller && root.controller.action === "discard-local"
                                 ? root.t("Local-only deletion of NEW LOCAL content. No repository revision is created.",
                                          "僅刪除新的本機內容，不會建立版本庫修訂。", "仅删除新的本机内容，不会创建版本库修订。")
                                 : root.controller && root.controller.action === "revert"
                                   ? root.t("Restores the managed state and discards local edits. No repository revision is created.",
                                            "還原受管狀態並捨棄本機修改，不會建立版本庫修訂。", "还原受管状态并舍弃本机修改，不会创建版本库修订。")
                                   : root.t("Converts managed files to editable local copies. No repository revision is created.",
                                            "將受管檔案轉為可編輯的本機副本，不會建立版本庫修訂。", "将受管文件转为可编辑的本机副本，不会创建版本库修订。")
                    }
                    Common.SeverityBanner {
                        Layout.fillWidth: true
                        visible: root.controller && root.controller.errorCode.length > 0
                        theme: root.theme
                        severity: "danger"
                        message: root.controller ? root.controller.reasonText(root.controller.errorCode) : ""
                    }
                    Item { Layout.fillHeight: true }
                    RowLayout {
                        Layout.fillWidth: true
                        Common.ActionButton {
                            theme: root.theme
                            text: root.t("Cancel", "取消", "取消")
                            onClicked: root.controller.reset()
                        }
                        Item { Layout.fillWidth: true }
                        Common.ActionButton {
                            objectName: "opApplyButton"
                            theme: root.theme
                            kind: "primary"
                            dangerous: root.isDestructive()
                            text: root.t("Apply", "套用", "应用")
                            onClicked: root.controller.confirmLocalApply()
                        }
                    }
                }
            }
        }

        // ---- CONFIRM (remove) ----
        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: active
            active: root.controller && root.controller.phase === "confirm" &&
                    root.controller.action === "remove"
            visible: active
            sourceComponent: Component {
                ColumnLayout {
                    spacing: root.theme ? root.theme.gapMd : 12
                    Common.SeverityBanner {
                        Layout.fillWidth: true
                        theme: root.theme
                        severity: "warn"
                        title: root.t("Remove from Version", "從版本移除", "从版本移除")
                        message: root.t("One removal creates exactly one repository revision. Remove never enters the Checkin Batch.",
                                        "一次移除只會建立一個版本庫修訂。移除不會進入簽入批次。",
                                        "一次移除只会创建一个版本库修订。移除不会进入签入批次。")
                    }
                    Label {
                        text: root.t("Removal Message (required)", "移除訊息（必填）", "移除信息（必填）")
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                    }
                    TextArea {
                        objectName: "removeMessageArea"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 54
                        text: root.controller ? root.controller.removeMessage : ""
                        onTextChanged: if (root.controller) root.controller.removeMessage = text
                        color: root.theme ? root.theme.textPrimary : "#eef4f8"
                        background: Rectangle {
                            radius: root.theme ? root.theme.radius : 6
                            color: root.theme ? root.theme.workSurface2 : "#122633"
                            border.color: root.theme ? root.theme.border : "#263946"
                        }
                    }
                    // Modified-LOCAL discard acknowledgment (default-off, required).
                    CheckBox {
                        objectName: "discardAckCheckbox"
                        visible: root.controller && root.controller.previewData &&
                                 root.controller.previewData.local_discard_required
                        checked: root.controller ? root.controller.allowDiscardLocal : false
                        // Keep the acknowledgement controller-owned even when
                        // accessibility/keyboard activation changes `checked`
                        // without a pointer toggle signal.
                        onCheckedChanged: if (root.controller) root.controller.allowDiscardLocal = checked
                        contentItem: Label {
                            text: root.t("I acknowledge that uncommitted local edits will be discarded",
                                         "我確認未提交的本機修改將被捨棄", "我确认未提交的本机修改将被舍弃")
                            color: root.theme ? root.theme.accentDanger : "#e5534b"
                            font.pixelSize: root.theme ? root.theme.fontData : 12
                            leftPadding: parent.indicator.width + 6
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Common.SeverityBanner {
                        Layout.fillWidth: true
                        visible: root.controller && root.controller.errorCode.length > 0
                        theme: root.theme
                        severity: "danger"
                        message: root.controller ? root.controller.reasonText(root.controller.errorCode) : ""
                    }
                    Item { Layout.fillHeight: true }
                    RowLayout {
                        Layout.fillWidth: true
                        Common.ActionButton { theme: root.theme; text: root.t("Cancel", "取消", "取消")
                            onClicked: root.controller.reset() }
                        Item { Layout.fillWidth: true }
                        Common.ActionButton {
                            objectName: "removeApplyButton"
                            theme: root.theme; kind: "danger"; dangerous: true
                            enabled: root.controller &&
                                     root.controller.removeMessage.replace(/^\s+|\s+$/g, "").length > 0 &&
                                     (!root.controller.previewData ||
                                      !root.controller.previewData.local_discard_required ||
                                      root.controller.allowDiscardLocal)
                            text: root.t("Remove", "移除", "移除")
                            onClicked: root.controller.confirmRemoveApply()
                        }
                    }
                }
            }
        }

        // ---- APPLYING ----
        Label {
            Layout.alignment: Qt.AlignHCenter
            visible: root.controller && root.controller.phase === "applying"
            text: root.t("Working…", "處理中…", "处理中…")
            color: root.theme ? root.theme.textSecondary : "#aab8c2"
            font.pixelSize: root.theme ? root.theme.fontSection : 15
        }

        // ---- RESULT ----
        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: active
            active: root.controller && root.controller.phase === "result"
            visible: active
            sourceComponent: Component {
                ColumnLayout {
                    spacing: root.theme ? root.theme.gapMd : 12
                    property var rd: root.controller ? root.controller.resultData : null
                    property string st: rd && rd.status ? rd.status : ""
                    Common.SeverityBanner {
                        Layout.fillWidth: true
                        theme: root.theme
                        severity: (parent.st === "succeeded" || parent.st === "created_checked_out" ||
                                   parent.st === "verified_committed") ? "ok"
                                  : (parent.st === "checkout_failed" || parent.st === "verified_not_committed") ? "warn" : "danger"
                        title: parent.st === "succeeded" ? root.t("Completed", "已完成", "已完成")
                               : parent.st === "verified_committed" ? root.t("Verified — committed", "已驗證 — 已提交", "已验证 — 已提交")
                               : parent.st === "verified_not_committed" ? root.t("Verified — not committed, rolled back", "已驗證 — 未提交，已還原", "已验证 — 未提交，已还原")
                               : parent.st === "checkout_failed" ? root.t("Version created · Checkout failed", "版本已建立 · 檢出失敗", "版本已创建 · 检出失败")
                               : parent.st === "failed" ? root.t("Failed — rolled back", "失敗 — 已還原", "失败 — 已还原")
                               : parent.st
                        message: (parent.rd && parent.rd.revision)
                                 ? (root.t("Repository revision", "版本庫修訂", "版本库修订") + " r" + parent.rd.revision)
                                 : root.t("No repository revision was created.", "未建立版本庫修訂。", "未创建版本库修订。")
                    }
                    Item { Layout.fillHeight: true }
                    RowLayout {
                        Layout.fillWidth: true
                        // Retry Checkout for a create checkout failure.
                        Common.ActionButton {
                            objectName: "retryCheckoutButton"
                            visible: parent.parent.st === "checkout_failed"
                            theme: root.theme; kind: "primary"
                            text: root.t("Retry Checkout", "重試檢出", "重试检出")
                            onClicked: root.controller.retryCheckout()
                        }
                        Item { Layout.fillWidth: true }
                        Common.ActionButton {
                            objectName: "opDoneButton"
                            theme: root.theme; kind: "primary"
                            text: root.t("Done", "完成", "完成")
                            onClicked: root.controller.reset()
                        }
                    }
                }
            }
        }

        // ---- OUTCOME UNKNOWN ----
        Loader {
            Layout.fillWidth: true
            active: root.controller && root.controller.phase === "outcomeUnknown"
            visible: active
            sourceComponent: Component {
                ColumnLayout {
                    spacing: root.theme ? root.theme.gapMd : 12
                    Common.SeverityBanner {
                        Layout.fillWidth: true
                        theme: root.theme
                        severity: "warn"
                        title: root.t("Commit outcome unknown", "提交結果未知", "提交结果未知")
                        message: root.t("Verification is required before any retry. Automatic retry is disabled.",
                                        "重試前必須驗證。已停用自動重試。", "重试前必须验证。已停用自动重试。")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Common.ActionButton {
                            objectName: "verifyOutcomeButton"
                            theme: root.theme; kind: "primary"
                            text: root.t("Verify repository", "驗證版本庫", "验证版本库")
                            onClicked: root.controller.verifyOutcome()
                        }
                    }
                }
            }
        }

        // ---- MANUAL REVIEW REQUIRED (evidence mismatch / diverged / ambiguous) ----
        Loader {
            Layout.fillWidth: true
            active: root.controller && root.controller.phase === "manualReview"
            visible: active
            sourceComponent: Component {
                ColumnLayout {
                    spacing: root.theme ? root.theme.gapMd : 12
                    Common.SeverityBanner {
                        objectName: "manualReviewBanner"
                        Layout.fillWidth: true
                        theme: root.theme
                        severity: "danger"
                        title: root.t("Manual review required", "需人工檢視", "需人工检视")
                        message: root.controller
                                 ? root.controller.reasonText(root.controller.manualReviewCode)
                                 : ""
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.t("The operation is blocked and nothing was changed. No automatic retry or rollback is performed until a person reviews the evidence.",
                                     "此操作已被阻擋，未變更任何內容。在有人檢視證據前不會自動重試或還原。",
                                     "此操作已被阻挡，未变更任何内容。在有人检视证据前不会自动重试或还原。")
                        color: root.theme ? root.theme.textSecondary : "#aab8c2"
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Common.ActionButton {
                            theme: root.theme
                            text: root.t("Close", "關閉", "关闭")
                            onClicked: root.controller.reset()
                        }
                    }
                }
            }
        }

        // ---- RECOVERY REQUIRED ----
        Loader {
            Layout.fillWidth: true
            active: root.controller && root.controller.phase === "recovery"
            visible: active
            sourceComponent: Component {
                ColumnLayout {
                    spacing: root.theme ? root.theme.gapMd : 12
                    // Recovery is never implicitly bound to the first
                    // discovered journal record.  The user explicitly selects
                    // a backend-supplied operation ID and target Version.
                    property var operations: root.controller && root.controller.recoveryData &&
                                             root.controller.recoveryData.operations
                                             ? root.controller.recoveryData.operations : []
                    Common.SeverityBanner {
                        Layout.fillWidth: true
                        theme: root.theme
                        severity: "danger"
                        title: root.t("Recovery required", "需要復原", "需要复原")
                        message: root.t("An interrupted Work operation must be recovered before any mutation. No content is changed until you recover.",
                                        "有被中斷的工作操作必須先復原才能進行任何變更。在你復原前不會更動任何內容。",
                                        "有被中断的工作操作必须先复原才能进行任何变更。在你复原前不会更动任何内容。")
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 150)
                        visible: model.length > 0
                        clip: true
                        model: parent.operations
                        delegate: RowLayout {
                            required property var modelData
                            width: ListView.view.width
                            Label {
                                Layout.fillWidth: true
                                text: modelData.operation + " · " + modelData.target_version
                                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                                elide: Text.ElideMiddle
                            }
                            Common.ActionButton {
                                theme: root.theme
                                text: root.t("Select", "選擇", "选择")
                                onClicked: root.controller.presentRecovery(modelData)
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Common.ActionButton {
                            objectName: "recoverButton"
                            theme: root.theme; kind: "primary"
                            enabled: root.controller && root.controller.recoveryOperationId.length > 0
                            text: root.t("Recover now", "立即復原", "立即复原")
                            onClicked: root.controller.recover(root.controller.recoveryOperationId)
                        }
                    }
                }
            }
        }

        // ---- CREATE composite ----
        Loader {
            Layout.fillWidth: true
            active: root.controller && root.controller.phase === "create"
            visible: active
            sourceComponent: Component {
                OperationStepper {
                    theme: root.theme
                    steps: ["Create", "Checkout", "Set Active"]
                    currentStep: root.controller ? root.controller.createStep : 0
                    failedStep: root.controller ? root.controller.createFailedStep : -1
                }
            }
        }
    }
}
