import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// CheckinBatchOverlay renders the real Checkin controller state. It receives
// only backend-authoritative Preview/Apply/Recovery data and invokes only
// CheckinBatchController methods; it never derives eligibility, hashes, SVN
// arguments, or a fallback retry path.
Rectangle {
    id: root

    property var theme
    property var i18n
    property var controller
    property string activeVersion: ""

    visible: controller && controller.phase !== "idle" &&
             (!controller.batchTarget || controller.batchTarget === activeVersion)
    color: theme ? theme.workSurface0 : "#0a1823"

    function t(en, tw, cn) { return i18n ? i18n.text(en, tw, cn) : en }
    readonly property string code: controller
                                   ? (controller.manualReviewCode || controller.errorCode || "") : ""
    readonly property bool recoveryIncomplete: controller && controller.phase === "manualReview" &&
                                             controller.manualReviewCode === "RECOVERY_QUERY_INCOMPLETE"
    readonly property bool queryFailure: code === "SVN_QUERY_TIMEOUT" ||
                                      code === "SVN_OUTPUT_LIMIT_EXCEEDED" ||
                                      code === "SVN_QUERY_FAILED" ||
                                      code === "LOCAL_SCAN_LIMIT_EXCEEDED" ||
                                      code === "REMOTE_QUERY_LIMIT_EXCEEDED" ||
                                      code === "SELECTOR_LIMIT_EXCEEDED" ||
                                      code === "BATCH_ITEM_LIMIT_EXCEEDED"

    MouseArea { anchors.fill: parent } // blocked state never clicks through

    Common.ShellPanel {
        anchors.centerIn: parent
        width: Math.min(parent.width - 56, 840)
        height: Math.min(parent.height - 56, 640)
        theme: root.theme

        ColumnLayout {
            anchors.fill: parent
            spacing: root.theme ? root.theme.gapMd : 12

            Common.SectionHeader {
                Layout.fillWidth: true
                theme: root.theme
                title: root.controller && root.controller.phase === "confirm"
                       ? root.t("Confirm Checkin", "確認簽入", "确认签入")
                       : root.controller && root.controller.phase === "success"
                         ? root.t("Checkin complete", "簽入完成", "签入完成")
                         : root.controller && root.controller.phase === "outcomeUnknown"
                           ? root.t("Checkin outcome unknown", "簽入結果未知", "签入结果未知")
                           : root.controller && root.controller.phase === "manualReview"
                             ? root.t("Manual review required", "需人工檢視", "需人工检视")
                             : root.controller && root.controller.phase === "recovery"
                               ? root.t("Recovery required", "需要復原", "需要恢复")
                               : root.t("Checkin Batch Review", "簽入批次檢視", "签入批次查看")
                subtitle: root.controller ? root.controller.batchTarget : ""
            }

            Label {
                Layout.fillWidth: true
                visible: root.controller && root.controller.selectedPaths.length > 0
                text: root.controller
                      ? (root.t("Selected scope: ", "選取範圍：", "选择范围：") +
                         root.controller.selectedPaths.join(", ")) : ""
                elide: Text.ElideRight
                color: root.theme ? root.theme.textSecondary : "#aab8c2"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                visible: root.controller && (root.controller.phase === "previewing" ||
                                              root.controller.phase === "applying")
                text: root.t("Preparing authoritative backend evidence…", "正在建立權威後端證據…", "正在建立权威后端证据…")
                color: root.theme ? root.theme.textSecondary : "#aab8c2"
                font.pixelSize: root.theme ? root.theme.fontSection : 15
            }

            // Review is the concrete backend plan, never a recomputed QML list.
            ListView {
                objectName: "checkinReviewItems"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                visible: root.controller && (root.controller.phase === "review" ||
                                             root.controller.phase === "confirm")
                model: root.controller ? root.controller.concreteItems : []
                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width
                    Label {
                        Layout.fillWidth: true
                        text: modelData.path
                        elide: Text.ElideMiddle
                        font.family: root.theme ? root.theme.monoFont : "monospace"
                        color: root.theme ? root.theme.textPrimary : "#eef4f8"
                    }
                    Label {
                        text: modelData.reason_code === "OK" ? root.t("Ready", "就緒", "就绪")
                                                               : root.controller.reasonText(modelData.reason_code)
                        color: modelData.allowed === true
                               ? (root.theme ? root.theme.accentOk : "#3fb950")
                               : (root.theme ? root.theme.accentDanger : "#e5534b")
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                    }
                }
            }

            Common.SeverityBanner {
                Layout.fillWidth: true
                visible: root.controller && (root.controller.phase === "review" ||
                                              root.controller.phase === "confirm") &&
                         (!root.controller.canApply || root.controller.hasBlockingConflict)
                theme: root.theme
                severity: "danger"
                title: root.t("Checkin is blocked", "簽入已封鎖", "签入已阻止")
                message: root.controller && root.controller.hasBlockingConflict
                         ? root.controller.reasonText("CONFLICT_BLOCKED")
                         : root.controller && root.code.length > 0 ? root.controller.reasonText(root.code) : ""
            }

            TextArea {
                objectName: "checkinMessageArea"
                Layout.fillWidth: true
                Layout.preferredHeight: 70
                visible: root.controller && root.controller.phase === "confirm"
                text: root.controller ? root.controller.checkinMessage : ""
                placeholderText: root.t("Enter Checkin Message…", "輸入簽入訊息…", "输入签入信息…")
                onTextChanged: if (root.controller) root.controller.checkinMessage = text
                color: root.theme ? root.theme.textPrimary : "#eef4f8"
                background: Rectangle {
                    radius: root.theme ? root.theme.radius : 6
                    color: root.theme ? root.theme.workSurface2 : "#122633"
                    border.color: root.theme ? root.theme.border : "#263946"
                }
            }

            Common.SeverityBanner {
                Layout.fillWidth: true
                visible: root.controller && (root.controller.phase === "failed" ||
                                              root.controller.phase === "outcomeUnknown" ||
                                              root.controller.phase === "manualReview" ||
                                              root.controller.phase === "recovery" ||
                                              (root.controller.phase === "confirm" && root.code.length > 0))
                theme: root.theme
                severity: root.recoveryIncomplete || (root.controller && root.controller.phase === "manualReview")
                          ? "danger" : "warn"
                title: root.recoveryIncomplete
                       ? root.t("Recovery verification incomplete", "復原驗證尚未完成", "恢复验证尚未完成")
                       : root.queryFailure
                         ? root.t("Checkin Preview needs attention", "簽入預覽需要處理", "签入预览需要处理")
                         : ""
                message: root.controller ? root.controller.reasonText(root.code) : ""
            }

            // Backend discovery may return several interrupted journals.  A
            // user must select the exact target-pinned record; no list position
            // is ever treated as a recovery authority.
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 150)
                visible: root.controller && root.controller.phase === "recovery" &&
                         root.controller.recoveryData && root.controller.recoveryData.operations &&
                         root.controller.recoveryData.operations.length > 0
                clip: true
                model: root.controller && root.controller.recoveryData ? root.controller.recoveryData.operations : []
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

            Common.SeverityBanner {
                Layout.fillWidth: true
                visible: root.controller && root.controller.phase === "success"
                theme: root.theme
                severity: "info"
                title: root.t("Committed exactly once", "已精確簽入一次", "已精确签入一次")
                message: root.controller && root.controller.verifiedRevision !== null
                         ? (root.t("Repository revision r", "版本庫修訂 r", "版本库修订 r") +
                            root.controller.verifiedRevision)
                         : root.t("The backend verified the Checkin result.", "後端已驗證簽入結果。", "后端已验证签入结果。")
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.controller && root.controller.phase !== "previewing" &&
                         root.controller.phase !== "applying"

                Common.ActionButton {
                    theme: root.theme
                    visible: root.controller && (root.controller.phase === "review" ||
                                                 root.controller.phase === "confirm" ||
                                                 root.controller.phase === "failed" ||
                                                 root.controller.phase === "success")
                    text: root.t("Close", "關閉", "关闭")
                    onClicked: {
                        if (!root.controller) return
                        if (root.controller.phase === "success") root.controller.resetBatch()
                        else root.controller.dismissPresentation()
                    }
                }
                Item { Layout.fillWidth: true }
                Common.ActionButton {
                    objectName: "checkinReviewAgainButton"
                    theme: root.theme
                    visible: root.controller && root.controller.phase === "failed" &&
                             root.controller.selectedPaths.length > 0
                    enabled: root.controller && !root.recoveryIncomplete
                    text: root.t("Review again", "再次檢視", "再次查看")
                    onClicked: root.controller.addToBatch()
                }
                Common.ActionButton {
                    objectName: "checkinContinueButton"
                    theme: root.theme
                    kind: "primary"
                    visible: root.controller && root.controller.phase === "review"
                    enabled: root.controller && root.controller.canApply
                    text: root.t("Continue", "繼續", "继续")
                    onClicked: root.controller.continueToConfirm()
                }
                Common.ActionButton {
                    objectName: "checkinConfirmApplyButton"
                    theme: root.theme
                    kind: "primary"
                    visible: root.controller && root.controller.phase === "confirm"
                    enabled: root.controller && root.controller.canApply &&
                             root.controller.checkinMessage.replace(/^\s+|\s+$/g, "").length > 0
                    text: root.t("Checkin", "簽入", "签入")
                    onClicked: root.controller.confirmApply()
                }
                Common.ActionButton {
                    objectName: "checkinVerifyOutcomeButton"
                    theme: root.theme
                    kind: "primary"
                    visible: root.controller && root.controller.phase === "outcomeUnknown" &&
                             root.controller.recoveryOperationId.length > 0
                    text: root.t("Verify repository", "驗證版本庫", "验证版本库")
                    onClicked: root.controller.verifyOutcome()
                }
                Common.ActionButton {
                    objectName: "checkinRecoverButton"
                    theme: root.theme
                    kind: "primary"
                    visible: root.controller && root.controller.phase === "recovery"
                    enabled: root.controller && root.controller.recoveryOperationId.length > 0
                    text: root.t("Recover", "復原", "恢复")
                    onClicked: root.controller.recover(root.controller.recoveryOperationId)
                }
            }
        }
    }
}
