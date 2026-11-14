import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// OutcomeUnknown — the client lost the response after the SVN commit may have
// occurred. The UI claims NEITHER success nor failure and MUST NOT automatically
// retry. The only route forward is Verify (check repository history + operation
// identity). A blind retry risks duplicate or contradictory history.
Common.ShellPanel {
    id: root

    property var theme
    property var i18n
    property var workState
    property var workModel

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapMd : 12

        OperationStepper {
            Layout.fillWidth: true
            theme: root.theme
            steps: ["Prepare", "Review", "Confirm", "Result"]
            currentStep: 3
        }

        Item { Layout.fillHeight: true }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(720, root.width - 60)
            Layout.preferredHeight: card.implicitHeight + (root.theme ? root.theme.gapXl * 2 : 48)
            radius: root.theme ? root.theme.radius : 6
            color: root.theme ? root.theme.workSurface1 : "#0d1d29"
            border.color: root.theme ? root.theme.accentWarn : "#e0a030"

            ColumnLayout {
                id: card
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: root.theme ? root.theme.gapXl : 24
                anchors.rightMargin: root.theme ? root.theme.gapXl : 24
                spacing: root.theme ? root.theme.gapMd : 12

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "⚠"
                    color: root.theme ? root.theme.accentWarn : "#e0a030"
                    font.pixelSize: 40
                }

                Label {
                    objectName: "outcomeUnknownTitle"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: root.i18n ? root.i18n.text("Commit outcome unknown — verification required",
                                                     "提交結果未知 — 需要驗證", "提交结果未知 — 需要验证")
                                    : "Commit outcome unknown — verification required"
                    color: root.theme ? root.theme.textPrimary : "#eef4f8"
                    font.pixelSize: root.theme ? root.theme.fontTitle : 22
                    font.bold: true
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: root.i18n ? root.i18n.text("The response was lost after the commit may have occurred. Automatic retry is disabled. Verify repository history and the expected operation identity before any retry.",
                                                     "在提交可能已發生後回應遺失。已停用自動重試。重試前請先驗證版本庫歷史與預期的操作識別。",
                                                     "在提交可能已发生后响应丢失。已停用自动重试。重试前请先验证版本库历史与预期的操作标识。")
                                    : "The response was lost after the commit may have occurred. Automatic retry is disabled. Verify repository history and the expected operation identity before any retry."
                    color: root.theme ? root.theme.textSecondary : "#aab8c2"
                    font.pixelSize: root.theme ? root.theme.fontBody : 13
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: root.theme ? root.theme.gapSm : 8
                    // Verify is the ONLY forward action; there is deliberately no
                    // direct Retry button here.
                    Common.ActionButton {
                        objectName: "verifyOutcomeButton"
                        theme: root.theme
                        kind: "primary"
                        text: root.i18n ? root.i18n.text("Verify repository history", "驗證版本庫歷史", "验证版本库历史") : "Verify repository history"
                        onClicked: root.workState.verifyOutcome()
                    }
                    Common.ActionButton {
                        theme: root.theme
                        text: root.i18n ? root.i18n.text("Back to Work", "返回工作", "返回工作") : "Back to Work"
                        onClicked: root.workState.backToWork()
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
