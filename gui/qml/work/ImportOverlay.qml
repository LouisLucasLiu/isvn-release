import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// Controller-owned Import surface. It renders only backend-provided safe
// labels/mappings and never receives an external absolute source path.
Item {
    id: root
    property var theme
    property var i18n
    property var controller
    property string activeVersion: ""
    readonly property string phase: controller ? controller.phase : "idle"
    anchors.fill: parent
    visible: controller && controller.phase !== "idle" &&
             (!controller.operationTarget || controller.operationTarget === activeVersion)

    Rectangle { anchors.fill: parent; color: Qt.rgba(0, 0, 0, 0.34) }

    Common.ShellPanel {
        anchors.centerIn: parent
        width: Math.min(820, parent.width - 64)
        height: Math.min(660, parent.height - 64)
        theme: root.theme
        ColumnLayout {
            anchors.fill: parent
            spacing: root.theme ? root.theme.gapMd : 12
            Common.SectionHeader {
                Layout.fillWidth: true; theme: root.theme
                title: root.phase === "success"
                       ? (root.i18n ? root.i18n.text("Import complete · Local workspace only", "匯入完成 · 僅本機工作區", "导入完成 · 仅本地工作区") : "Import complete · Local workspace only")
                       : root.phase === "confirm"
                         ? (root.i18n ? root.i18n.text("Confirm Import", "確認匯入", "确认导入") : "Confirm Import")
                       : root.phase === "recovery"
                           ? (root.i18n ? root.i18n.text("Recovery Required", "需要復原", "需要恢复") : "Recovery Required")
                           : root.phase === "manualReview"
                             ? (root.i18n ? root.i18n.text("Manual Review Required", "需人工檢視", "需人工检查") : "Manual Review Required")
                           : (root.i18n ? root.i18n.text("Import Preview", "匯入預覽", "导入预览") : "Import Preview")
                subtitle: !root.controller ? ""
                          : root.controller.destination.length > 0
                            ? root.controller.activeVersion + " / " + root.controller.destination
                            : root.controller.activeVersion
            }
            Common.SeverityBanner {
                Layout.fillWidth: true; theme: root.theme
                visible: root.phase === "failed" || root.phase === "recovery" || root.phase === "manualReview"
                severity: root.phase === "manualReview" ? "danger" : "warn"
                title: root.controller ? root.controller.reasonText(root.controller.errorCode) : ""
                message: root.phase === "recovery"
                         ? (root.i18n ? root.i18n.text("Verify or restore the interrupted local operation before trying Import again.",
                                                        "請先驗證或復原中斷的本機操作，再次嘗試匯入。",
                                                        "请先验证或恢复中断的本地操作，再次尝试导入。") : "")
                         : ""
            }
            Label {
                Layout.fillWidth: true
                visible: root.phase === "confirm"
                text: root.i18n ? root.i18n.text("Import writes only to the Active local workspace. No repository revision is created. " + (root.controller ? root.controller.selectedReplacementCount : 0) + " existing LOCAL files will be replaced. Imported files join the Checkin Batch only after success; Checkin still needs a later message and Confirm.",
                                                  "匯入只寫入使用中本機工作區，不會建立版本庫修訂。將取代 " + (root.controller ? root.controller.selectedReplacementCount : 0) + " 個既有 LOCAL 檔案。成功後檔案才加入簽入批次；簽入仍需要稍後輸入訊息並確認。",
                                                  "导入只写入使用中的本地工作区，不会创建版本库修订。将替换 " + (root.controller ? root.controller.selectedReplacementCount : 0) + " 个现有 LOCAL 文件。成功后文件才加入签入批次；签入仍需稍后输入信息并确认。")
                                : "Local workspace only"
                color: root.theme ? root.theme.textSecondary : "#aab8c2"; wrapMode: Text.Wrap
            }
            ListView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                visible: root.phase === "review" || root.phase === "confirm"
                model: root.controller ? root.controller.items : []
                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width; spacing: root.theme ? root.theme.gapSm : 8
                    CheckBox {
                        visible: modelData.replace_local_available === true
                        checked: root.controller && root.controller.replaceLocal[modelData.destination] === true
                        onToggled: if (root.controller) root.controller.setReplaceLocal(modelData.destination, checked)
                    }
                    Label { Layout.fillWidth: true; text: modelData.destination; elide: Text.ElideMiddle
                            font.family: root.theme ? root.theme.monoFont : "monospace"
                            color: root.theme ? root.theme.textPrimary : "#eef4f8" }
                    Label { text: modelData.reason_code === "OK" || !root.controller ? "" : root.controller.reasonText(modelData.reason_code)
                            color: root.theme ? root.theme.accentWarn : "#e8b84c"; font.pixelSize: 12 }
                }
            }
            Label {
                Layout.fillWidth: true; visible: root.phase === "success"
                text: root.controller && root.controller.batchIntegrated
                      ? (root.i18n ? root.i18n.text("No SVN revision was created. The exact imported paths were added to this target's Checkin Batch; review must be regenerated.",
                                                     "沒有建立 SVN 修訂。確切匯入路徑已加入此目標的簽入批次；必須重新檢視。",
                                                     "未创建 SVN 修订。确切导入路径已加入此目标的签入批次；必须重新查看。")
                                   : "No SVN revision was created.")
                      : (root.i18n ? root.i18n.text("No SVN revision was created. Work-status refresh is pending or failed, so Batch integration is unconfirmed. Refresh, then add the imported LOCAL paths manually if needed.",
                                                     "沒有建立 SVN 修訂。工作狀態重新整理尚未完成或失敗，因此簽入批次整合未確認。請重新整理，必要時手動加入匯入的 LOCAL 路徑。",
                                                     "未创建 SVN 修订。工作状态刷新尚未完成或失败，因此签入批次整合未确认。请刷新，必要时手动添加导入的 LOCAL 路径。")
                                   : "No SVN revision was created; Batch integration is pending.")
                wrapMode: Text.Wrap; color: root.theme ? root.theme.textSecondary : "#aab8c2"
            }
            RowLayout {
                Layout.fillWidth: true
                Common.ActionButton { theme: root.theme
                    text: root.i18n ? root.i18n.text("Cancel", "取消", "取消") : "Cancel"
                    visible: root.phase === "review" || root.phase === "confirm" ||
                             root.phase === "failed" || root.phase === "success" || root.phase === "manualReview"
                    onClicked: if (root.controller) root.controller.reset() }
                Item { Layout.fillWidth: true }
                Common.ActionButton { theme: root.theme; kind: "primary"
                    objectName: "importContinueButton"
                    visible: root.phase === "review"
                    enabled: root.controller && root.controller.canConfirm
                    text: root.i18n ? root.i18n.text("Continue", "繼續", "继续") : "Continue"
                    onClicked: root.controller.continueToConfirm() }
                Common.ActionButton { theme: root.theme; kind: "primary"
                    objectName: "importConfirmApplyButton"
                    visible: root.phase === "confirm"
                    enabled: root.controller && !root.controller.busy
                    text: root.i18n ? root.i18n.text("Import", "匯入", "导入") : "Import"
                    onClicked: root.controller.confirmApply() }
                Common.ActionButton { theme: root.theme; kind: "primary"
                    visible: root.phase === "recovery" && root.controller && root.controller.recoveryOperationId.length > 0
                    text: root.i18n ? root.i18n.text("Restore", "復原", "恢复") : "Restore"
                    onClicked: root.controller.restoreRecovery() }
            }
        }
    }
}
