import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// CheckinResult — the truthful repository outcome for a Batch Checkin. It renders
// exactly two outcomes:
//   Success — one batch, ONE shared SVN revision (never per-item revisions).
//   Failed  — no repository commit; the batch is retained for correction.
// There is no partial-batch outcome: a partly committed batch is not a valid
// Batch Checkin outcome. (Outcome Unknown is a separate view.)
Common.ShellPanel {
    id: root

    property var theme
    property var i18n
    property var workState
    property var workModel

    readonly property bool success: root.workState
                                    && root.workState.stateId === root.workState.stateCheckinSuccess

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapMd : 12

        OperationStepper {
            Layout.fillWidth: true
            theme: root.theme
            steps: ["Prepare", "Review", "Confirm", "Result"]
            currentStep: 3
            failedStep: root.success ? -1 : 3
        }

        Item { Layout.fillHeight: true }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(720, root.width - 60)
            Layout.preferredHeight: card.implicitHeight + (root.theme ? root.theme.gapXl * 2 : 48)
            radius: root.theme ? root.theme.radius : 6
            color: root.theme ? root.theme.workSurface1 : "#0d1d29"
            border.color: root.success ? (root.theme ? root.theme.accentOk : "#3fb950")
                                       : (root.theme ? root.theme.accentDanger : "#e5534b")

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
                    text: root.success ? "✓" : "✕"
                    color: root.success ? (root.theme ? root.theme.accentOk : "#3fb950")
                                        : (root.theme ? root.theme.accentDanger : "#e5534b")
                    font.pixelSize: 40
                }

                Label {
                    objectName: "checkinResultTitle"
                    Layout.alignment: Qt.AlignHCenter
                    text: root.success
                          ? (root.i18n ? root.i18n.text("Checkin succeeded", "簽入成功", "签入成功") : "Checkin succeeded")
                          : (root.i18n ? root.i18n.text("Checkin failed", "簽入失敗", "签入失败") : "Checkin failed")
                    color: root.theme ? root.theme.textPrimary : "#eef4f8"
                    font.pixelSize: root.theme ? root.theme.fontTitle : 22
                    font.bold: true
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: root.success
                          ? (root.i18n ? root.i18n.text("The whole batch was committed at one repository revision.",
                                                        "整個批次以單一版本庫修訂完成提交。", "整个批次以单一版本库修订完成提交。")
                                       : "The whole batch was committed at one repository revision.")
                          : (root.i18n ? root.i18n.text("No repository commit occurred. The batch is retained for correction and re-review.",
                                                        "未發生版本庫提交。批次已保留以供修正並重新檢視。", "未发生版本库提交。批次已保留以供修正并重新查看。")
                                       : "No repository commit occurred. The batch is retained for correction and re-review.")
                    color: root.theme ? root.theme.textSecondary : "#aab8c2"
                    font.pixelSize: root.theme ? root.theme.fontBody : 13
                    wrapMode: Text.Wrap
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.theme ? root.theme.border : "#263946" }

                Common.KeyValueGrid {
                    Layout.fillWidth: true
                    theme: root.theme
                    monoValue: true
                    keyWidth: 150
                    rows: root.success
                          ? [
                              { key: (root.i18n ? root.i18n.text("Operation ID", "操作 ID", "操作 ID") : "Operation ID"), value: "CHK-20260714-000001" },
                              { key: (root.i18n ? root.i18n.text("Revision (whole batch)", "修訂（整批）", "修订（整批）") : "Revision (whole batch)"), value: root.workState ? root.workState.successRevision : "" },
                              { key: (root.i18n ? root.i18n.text("Target", "目標", "目标") : "Target"), value: (root.workState && root.workState.activeVersion() ? root.workState.activeVersion().repositoryPath : "") }
                            ]
                          : [
                              { key: (root.i18n ? root.i18n.text("Operation ID", "操作 ID", "操作 ID") : "Operation ID"), value: "CHK-20260714-000002" },
                              { key: (root.i18n ? root.i18n.text("Revision", "修訂", "修订") : "Revision"), value: "—" },
                              { key: (root.i18n ? root.i18n.text("Batch", "批次", "批次") : "Batch"), value: (root.i18n ? root.i18n.text("retained", "已保留", "已保留") : "retained") }
                            ]
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: root.theme ? root.theme.gapSm : 8
                    Common.ActionButton {
                        visible: !root.success
                        theme: root.theme
                        text: root.i18n ? root.i18n.text("Back to Review", "返回檢視", "返回查看") : "Back to Review"
                        onClicked: root.workState.beginReview()
                    }
                    Common.ActionButton {
                        theme: root.theme
                        kind: "primary"
                        text: root.i18n ? root.i18n.text("Back to Work", "返回工作", "返回工作") : "Back to Work"
                        onClicked: root.workState.backToWork()
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
