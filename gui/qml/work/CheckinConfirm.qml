import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// CheckinConfirm — dedicated, non-modal final step. Read-only final mapping and
// a REQUIRED non-empty message. Confirm maps to exactly one Batch Checkin SVN
// revision. The backend revalidates authorization and target state immediately
// before execution; this component performs no backend call.
Common.ShellPanel {
    id: root

    property var theme
    property var i18n
    property var workState
    property var workModel

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapSm : 8

        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapLg : 16
            Common.ActionButton {
                theme: root.theme
                implicitHeight: 28
                text: root.i18n ? root.i18n.text("← Back to Review", "← 返回檢視", "← 返回查看") : "← Back to Review"
                onClicked: root.workState.beginReview()
            }
            OperationStepper {
                Layout.fillWidth: true
                theme: root.theme
                steps: ["Prepare", "Review", "Confirm", "Result"]
                currentStep: 2
            }
        }

        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.i18n ? root.i18n.text("Confirm Checkin", "確認簽入", "确认签入") : "Confirm Checkin"
            subtitle: root.i18n ? root.i18n.text("One batch, one repository revision. Final verification before writing to the existing version.",
                                                 "一個批次、一個版本庫修訂。寫入既有版本前的最終確認。",
                                                 "一个批次、一个版本库修订。写入既有版本前的最终确认。")
                                : "One batch, one repository revision. Final verification before writing to the existing version."
        }

        Common.KeyValueGrid {
            Layout.fillWidth: true
            theme: root.theme
            keyWidth: 150
            rows: [
                { key: (root.i18n ? root.i18n.text("Repository target", "版本庫目標", "版本库目标") : "Repository target"),
                  value: (root.workState && root.workState.activeVersion() ? root.workState.activeVersion().repositoryPath : "") },
                { key: (root.i18n ? root.i18n.text("Batch size", "批次大小", "批次大小") : "Batch size"),
                  value: (root.workState ? root.workState.batchCount : 0) + " "
                         + (root.i18n ? root.i18n.text("items → one revision", "個項目 → 一個修訂", "个项目 → 一个修订") : "items → one revision") },
                { key: (root.i18n ? root.i18n.text("Effect", "效果", "效果") : "Effect"),
                  value: (root.i18n ? root.i18n.text("Updates the existing version. No new version is created.",
                                                     "更新既有版本，不會建立新版本。", "更新既有版本，不会创建新版本。")
                                    : "Updates the existing version. No new version is created.") }
            ]
        }

        Label {
            Layout.fillWidth: true
            text: root.i18n ? root.i18n.text("Checkin Message (required)", "簽入訊息（必填）", "签入信息（必填）")
                            : "Checkin Message (required)"
            color: root.theme ? root.theme.textSecondary : "#aab8c2"
            font.pixelSize: root.theme ? root.theme.fontData : 12
        }

        TextArea {
            id: confirmMessage
            objectName: "confirmMessageArea"
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            text: root.workState ? root.workState.checkinMessage : ""
            onTextChanged: if (root.workState) root.workState.checkinMessage = text
            placeholderText: root.i18n ? root.i18n.text("A non-empty message is required.", "必須填寫非空白訊息。", "必须填写非空白信息。")
                                       : "A non-empty message is required."
            color: root.theme ? root.theme.textPrimary : "#eef4f8"
            placeholderTextColor: root.theme ? root.theme.textMuted : "#71818d"
            font.pixelSize: root.theme ? root.theme.fontBody : 13
            wrapMode: TextEdit.Wrap
            background: Rectangle {
                radius: root.theme ? root.theme.radius : 6
                color: root.theme ? root.theme.workSurface2 : "#122633"
                border.color: (root.workState && root.workState.messageInvalid)
                              ? (root.theme ? root.theme.accentDanger : "#e5534b")
                              : (root.theme ? root.theme.border : "#263946")
            }
        }

        Common.SeverityBanner {
            Layout.fillWidth: true
            visible: root.workState && root.workState.messageInvalid
            theme: root.theme
            severity: "danger"
            message: root.i18n ? root.i18n.text("A non-empty checkin message is required.", "必須填寫非空白的簽入訊息。", "必须填写非空白的签入信息。")
                               : "A non-empty checkin message is required."
        }

        Common.SeverityBanner {
            Layout.fillWidth: true
            theme: root.theme
            severity: "info"
            message: root.i18n ? root.i18n.text("Authorization and target state are revalidated immediately before execution.",
                                                "授權與目標狀態會在執行前立即重新驗證。", "授权与目标状态会在执行前立即重新验证。")
                               : "Authorization and target state are revalidated immediately before execution."
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            Common.ActionButton {
                theme: root.theme
                text: root.i18n ? root.i18n.text("Back to Review", "返回檢視", "返回查看") : "Back to Review"
                onClicked: root.workState.beginReview()
            }
            Item { Layout.fillWidth: true }
            Common.ActionButton {
                objectName: "confirmCheckinButton"
                theme: root.theme
                kind: "primary"
                text: root.i18n ? root.i18n.text("Confirm Checkin", "確認簽入", "确认签入") : "Confirm Checkin"
                onClicked: root.workState.confirmCheckin()
            }
        }
    }
}
