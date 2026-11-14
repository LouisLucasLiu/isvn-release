import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// ImportPreview — read-only preview of an external drop into the Active Working
// Version. Import performs NO repository write (the preview performs nothing at
// all). Collision policy:
//   absent        → allow after validation
//   managed/LINKED → HARD BLOCK (direct user to Make Editable on the original)
//   existing LOCAL → allow only with an explicit, default-OFF Replace Local
// A managed destination can never be turned into an allowed replacement by a
// GUI checkbox.
Common.ShellPanel {
    id: root

    property var theme
    property var i18n
    property var workState
    property var workModel

    function destLabel(state) {
        if (!root.i18n)
            return state
        if (state === "absent") return root.i18n.text("New", "新增", "新增")
        if (state === "localExisting") return root.i18n.text("Existing LOCAL", "既有 LOCAL", "既有 LOCAL")
        if (state === "managed") return root.i18n.text("Managed — blocked", "受管 — 已阻擋", "受管 — 已阻挡")
        return state
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapSm : 8

        RowLayout {
            Layout.fillWidth: true
            Common.ActionButton {
                theme: root.theme
                implicitHeight: 28
                text: root.i18n ? root.i18n.text("← Back to File Status", "← 返回檔案狀態", "← 返回文件状态") : "← Back to File Status"
                onClicked: root.workState.backToWork()
            }
            Item { Layout.fillWidth: true }
        }

        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.i18n ? root.i18n.text("Import to Working Version", "匯入至工作版本", "导入至工作版本") : "Import to Working Version"
            subtitle: root.i18n ? root.i18n.text("Review external files before they are copied into Work as LOCAL and added to the Checkin Batch. Preview is read-only.",
                                                 "在將外部檔案複製為 LOCAL 並加入簽入批次前先檢視。預覽為唯讀。",
                                                 "在将外部文件复制为 LOCAL 并加入签入批次前先查看。预览为只读。")
                                : "Review external files before they are copied into Work as LOCAL and added to the Checkin Batch. Preview is read-only."
        }

        Common.KeyValueGrid {
            Layout.fillWidth: true
            theme: root.theme
            monoValue: true
            keyWidth: 150
            rows: [
                { key: (root.i18n ? root.i18n.text("Drop target", "放置目標", "放置目标") : "Drop target"),
                  value: (root.i18n ? root.i18n.text("Active Version root", "使用中版本根目錄", "使用中版本根目录") : "Active Version root") },
                { key: (root.i18n ? root.i18n.text("Destination root", "目的根目錄", "目的根目录") : "Destination root"),
                  value: (root.workState && root.workState.activeVersion() ? root.workState.activeVersion().localPath + "/" : "") },
                { key: (root.i18n ? root.i18n.text("External source", "外部來源", "外部来源") : "External source"),
                  value: (root.workModel ? root.workModel.importSourceRoot : "") }
            ]
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.theme ? root.theme.rowHeight : 36
            color: root.theme ? root.theme.workSurface2 : "#122633"
            radius: root.theme ? root.theme.radius : 6
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                Label { Layout.preferredWidth: 280; text: root.i18n ? root.i18n.text("External Source", "外部來源", "外部来源") : "External Source"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.fillWidth: true; text: root.i18n ? root.i18n.text("Destination (relative)", "目的（相對）", "目的（相对）") : "Destination (relative)"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.preferredWidth: 130; text: root.i18n ? root.i18n.text("Destination state", "目的狀態", "目的状态") : "Destination state"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
                Label { Layout.preferredWidth: 120; text: root.i18n ? root.i18n.text("Replace Local", "取代 LOCAL", "替换 LOCAL") : "Replace Local"; color: root.theme ? root.theme.textSecondary : "#aab8c2"; font.pixelSize: root.theme ? root.theme.fontData : 12 }
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.workModel ? root.workModel.importItems : []
            boundsBehavior: Flickable.StopAtBounds
            delegate: Rectangle {
                id: importRow
                required property var modelData
                required property int index
                readonly property bool managed: modelData.destinationState === "managed"
                readonly property bool localExisting: modelData.destinationState === "localExisting"
                objectName: managed ? "importManagedRow" : (localExisting ? "importLocalExistingRow" : "importRow_" + index)

                width: ListView.view ? ListView.view.width : 0
                height: root.theme ? root.theme.rowHeight : 36
                color: managed
                       ? Qt.rgba((root.theme ? root.theme.accentDanger : "#e5534b").r,
                                 (root.theme ? root.theme.accentDanger : "#e5534b").g,
                                 (root.theme ? root.theme.accentDanger : "#e5534b").b, 0.07)
                       : (root.theme ? root.theme.workSurface0 : "#0a1823")
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: root.theme ? root.theme.border : "#263946" }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: root.theme ? root.theme.gapMd : 12
                    anchors.rightMargin: root.theme ? root.theme.gapMd : 12
                    spacing: root.theme ? root.theme.gapSm : 8

                    Label { Layout.preferredWidth: 280; text: modelData.sourcePath; color: root.theme ? root.theme.textPrimary : "#eef4f8"; font.pixelSize: root.theme ? root.theme.fontData : 12; font.family: root.theme ? root.theme.monoFont : "monospace"; elide: Text.ElideMiddle }
                    Label { Layout.fillWidth: true; text: modelData.destinationPath; color: root.theme ? root.theme.textPrimary : "#eef4f8"; font.pixelSize: root.theme ? root.theme.fontData : 12; font.family: root.theme ? root.theme.monoFont : "monospace"; elide: Text.ElideMiddle }
                    Label {
                        Layout.preferredWidth: 130
                        text: root.destLabel(modelData.destinationState)
                        color: parent.parent.managed ? (root.theme ? root.theme.accentDanger : "#e5534b")
                               : (parent.parent.localExisting ? (root.theme ? root.theme.accentWarn : "#e0a030")
                                                              : (root.theme ? root.theme.accentOk : "#3fb950"))
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                    }

                    Item {
                        Layout.preferredWidth: 120
                        Layout.fillHeight: true

                        // Managed destinations: HARD BLOCK. No checkbox exists, so
                        // the user can never turn a managed collision into an
                        // allowed replacement.
                        Label {
                            visible: importRow.managed
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.i18n ? root.i18n.text("Blocked", "已阻擋", "已阻挡") : "Blocked"
                            color: root.theme ? root.theme.accentDanger : "#e5534b"
                            font.pixelSize: root.theme ? root.theme.fontData : 12
                            font.bold: true
                        }

                        // Existing LOCAL: an explicit, default-OFF replacement.
                        CheckBox {
                            objectName: importRow.localExisting ? "replaceLocalCheckbox" : ""
                            visible: importRow.localExisting
                            anchors.verticalCenter: parent.verticalCenter
                            checked: root.workState ? root.workState.replaceLocalDefault : false
                        }

                        Label {
                            visible: !importRow.managed && !importRow.localExisting
                            anchors.verticalCenter: parent.verticalCenter
                            text: "—"
                            color: root.theme ? root.theme.textMuted : "#71818d"
                            font.pixelSize: root.theme ? root.theme.fontData : 12
                        }
                    }
                }
            }
        }

        Common.SeverityBanner {
            Layout.fillWidth: true
            theme: root.theme
            severity: "info"
            message: root.i18n ? root.i18n.text("Imported files become LOCAL and are added to the ephemeral Checkin Batch only after Apply succeeds. No repository revision is created by import.",
                                                "匯入的檔案成為 LOCAL，且僅在套用成功後才加入暫時性的簽入批次。匯入不會建立版本庫修訂。",
                                                "导入的文件成为 LOCAL，且仅在应用成功后才加入临时性的签入批次。导入不会创建版本库修订。")
                               : "Imported files become LOCAL and are added to the ephemeral Checkin Batch only after Apply succeeds. No repository revision is created by import."
        }

        RowLayout {
            Layout.fillWidth: true
            Common.ActionButton {
                theme: root.theme
                text: root.i18n ? root.i18n.text("Cancel", "取消", "取消") : "Cancel"
                onClicked: root.workState.backToWork()
            }
            Item { Layout.fillWidth: true }
            Common.ActionButton {
                objectName: "importApplyButton"
                theme: root.theme
                kind: "primary"
                text: root.i18n ? root.i18n.text("Import to Work & Add to Batch", "匯入工作並加入批次", "导入工作并加入批次") : "Import to Work & Add to Batch"
                onClicked: root.workState.backToWork()
            }
        }
    }
}
