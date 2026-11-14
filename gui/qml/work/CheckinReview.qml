import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// CheckinReview — verify target, path mappings, and conflicts before Confirm.
// An unresolved conflict blocks the WHOLE batch and disables progression. There
// is no Overwrite/Force/Skip/Continue-Anyway action; a LOCAL source can never
// overwrite a managed destination.
Common.ShellPanel {
    id: root

    property var theme
    property var i18n
    property var workState
    property var workModel

    readonly property bool blocked: root.workState && root.workState.reviewHasBlockingConflict

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapSm : 8

        RowLayout {
            Layout.fillWidth: true
            spacing: root.theme ? root.theme.gapLg : 16
            Common.ActionButton {
                theme: root.theme
                implicitHeight: 28
                text: root.i18n ? root.i18n.text("← Back to Work", "← 返回工作", "← 返回工作") : "← Back to Work"
                onClicked: root.workState.backToWork()
            }
            OperationStepper {
                Layout.fillWidth: true
                theme: root.theme
                steps: ["Prepare", "Review", "Confirm", "Result"]
                currentStep: 1
            }
        }

        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.i18n ? root.i18n.text("Review Checkin", "檢視簽入", "查看签入") : "Review Checkin"
            subtitle: root.i18n ? root.i18n.text("Verify target, paths, and conflicts before confirmation.",
                                                 "在確認前檢查目標、路徑與衝突。", "在确认前检查目标、路径与冲突。")
                                : "Verify target, paths, and conflicts before confirmation."
        }

        Common.SeverityBanner {
            objectName: "reviewConflictBanner"
            Layout.fillWidth: true
            visible: root.blocked
            theme: root.theme
            severity: "danger"
            message: root.i18n ? root.i18n.text("1 conflict must be resolved before checkin. The whole batch is blocked.",
                                                "簽入前必須解決 1 個衝突，整個批次已被阻擋。",
                                                "签入前必须解决 1 个冲突，整个批次已被阻挡。")
                               : "1 conflict must be resolved before checkin. The whole batch is blocked."
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.blocked
            Item { Layout.fillWidth: true }
            Common.ActionButton {
                objectName: "resolveConflictButton"
                theme: root.theme
                text: root.i18n ? root.i18n.text("Resolve conflict (change destination)",
                                                 "解決衝突（變更目的路徑）", "解决冲突（变更目的路径）")
                                : "Resolve conflict (change destination)"
                onClicked: root.workState.resolveConflict()
            }
        }

        Common.SeverityBanner {
            Layout.fillWidth: true
            visible: !root.blocked
            theme: root.theme
            severity: "ok"
            message: root.i18n ? root.i18n.text("All mappings are ready for confirmation.",
                                                "所有對應皆已就緒可供確認。", "所有对应皆已就绪可供确认。")
                               : "All mappings are ready for confirmation."
        }

        // Target summary — this updates an EXISTING version, never creates one.
        Common.KeyValueGrid {
            Layout.fillWidth: true
            theme: root.theme
            keyWidth: 150
            rows: [
                { key: (root.i18n ? root.i18n.text("Repository target", "版本庫目標", "版本库目标") : "Repository target"),
                  value: (root.workState && root.workState.activeVersion() ? root.workState.activeVersion().repositoryPath : "") },
                { key: (root.i18n ? root.i18n.text("Effect", "效果", "效果") : "Effect"),
                  value: (root.i18n ? root.i18n.text("Updates the existing version. No new version is created.",
                                                     "更新既有版本，不會建立新版本。", "更新既有版本，不会创建新版本。")
                                    : "Updates the existing version. No new version is created.") }
            ]
        }

        // Source -> destination mapping table.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.theme ? root.theme.rowHeight : 36
            color: root.theme ? root.theme.workSurface2 : "#122633"
            radius: root.theme ? root.theme.radius : 6
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                Label { Layout.preferredWidth: 220; text: root.i18n ? root.i18n.text("Source", "來源", "来源") : "Source"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.fillWidth: true; text: root.i18n ? root.i18n.text("Destination", "目的", "目的") : "Destination"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.preferredWidth: 90; text: root.i18n ? root.i18n.text("Change", "變更", "变更") : "Change"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.preferredWidth: 150; text: root.i18n ? root.i18n.text("Status", "狀態", "状态") : "Status"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.workModel ? root.workModel.batchEntries : []
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                required property var modelData
                readonly property bool isConflict: modelData.readiness === "conflict" && root.blocked
                width: ListView.view ? ListView.view.width : 0
                height: root.theme ? root.theme.rowHeight : 36
                color: isConflict
                       ? Qt.rgba((root.theme ? root.theme.accentDanger : "#e5534b").r,
                                 (root.theme ? root.theme.accentDanger : "#e5534b").g,
                                 (root.theme ? root.theme.accentDanger : "#e5534b").b, 0.06)
                       : (root.theme ? root.theme.workSurface0 : "#0a1823")
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: root.theme ? root.theme.border : "#263946" }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                    anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                    spacing: root.theme ? root.theme.gapSm : 8
                    Label { Layout.preferredWidth: 220; text: modelData.sourcePath; color: root.theme ? root.theme.textPrimary : "#eef4f8"; font.pixelSize: root.theme ? root.theme.fontData : 12; font.family: root.theme ? root.theme.monoFont : "monospace"; elide: Text.ElideMiddle }
                    Label { Layout.fillWidth: true; text: modelData.destinationPath; color: root.theme ? root.theme.textPrimary : "#eef4f8"; font.pixelSize: root.theme ? root.theme.fontData : 12; font.family: root.theme ? root.theme.monoFont : "monospace"; elide: Text.ElideMiddle }
                    Label { Layout.preferredWidth: 90; text: modelData.changeKind; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                    Label { Layout.preferredWidth: 150
                            text: parent.parent.isConflict ? (root.i18n ? root.i18n.text("Destination exists", "目的已存在", "目的已存在") : "Destination exists")
                                                           : (root.i18n ? root.i18n.text("Ready", "就緒", "就绪") : "Ready")
                            color: parent.parent.isConflict ? (root.theme ? root.theme.accentDanger : "#e5534b") : (root.theme ? root.theme.accentOk : "#3fb950")
                            font.pixelSize: root.theme ? root.theme.fontData : 12 }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Common.ActionButton {
                theme: root.theme
                text: root.i18n ? root.i18n.text("Back to Batch", "返回批次", "返回批次") : "Back to Batch"
                onClicked: root.workState.backToWork()
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: root.blocked
                text: root.i18n ? root.i18n.text("Resolve 1 conflict to continue", "解決 1 個衝突以繼續", "解决 1 个冲突以继续")
                                : "Resolve 1 conflict to continue"
                color: root.theme ? root.theme.accentDanger : "#e5534b"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }
            Common.ActionButton {
                objectName: "continueToConfirmButton"
                theme: root.theme
                kind: "primary"
                enabled: !root.blocked
                text: root.i18n ? root.i18n.text("Continue to Confirm", "繼續確認", "继续确认") : "Continue to Confirm"
                onClicked: root.workState.continueToConfirm()
            }
        }
    }
}
