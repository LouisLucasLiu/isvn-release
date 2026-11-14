import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// LocalVersionNavigator — locally checked-out working versions ONLY, with the
// real Phase -> Block -> Stage -> Version hierarchy. It never shows a
// repository-wide branch as if it were local. Selecting a version requests an
// Active Working Version switch through WorkUiState (which enforces the
// batch-bound switch guard); it never checks out or fetches content.
Common.ShellPanel {
    id: root

    // `theme` comes from ShellPanel. Redeclaring it here would hide it from
    // ShellPanel, which then paints a fixed white surface under themed text.
    property var i18n
    property var workState
    property var workModel
    property var operationCoordinator

    signal versionActivateRequested(string versionId)

    ColumnLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapSm : 8

        Common.SectionHeader {
            theme: root.theme
            Layout.fillWidth: true
            title: root.i18n ? root.i18n.text("Local Working Versions", "本機工作版本", "本机工作版本")
                             : "Local Working Versions"
            subtitle: root.i18n ? root.i18n.text("Checked-out versions in this workspace",
                                                 "此工作區已檢出的版本", "此工作区已检出的版本")
                                : "Checked-out versions in this workspace"
        }

        ListView {
            id: list
            objectName: "localVersionList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 1
            model: root.workModel ? root.workModel.localVersions : []
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index

                readonly property bool active: root.workState
                                               && root.workState.activeVersionId === modelData.id
                // Progress display only; it never disables the row.
                readonly property bool operationRunning: root.operationCoordinator
                                                        && root.operationCoordinator.hasRunningOperation(modelData.id)
                readonly property bool recoveryPending: root.operationCoordinator
                                                       && root.operationCoordinator.needsRecovery(modelData.id)

                width: ListView.view ? ListView.view.width : 0
                height: 84
                radius: root.theme ? root.theme.radius : 6
                color: row.active ? (root.theme ? root.theme.workSelected : "#123c5f")
                       : hover.hovered ? (root.theme ? root.theme.workSurfaceHover : "#172e3d")
                       : (root.theme ? root.theme.workSurface1 : "#0d1d29")
                border.color: row.active ? (root.theme ? root.theme.workAccent : "#1687f8")
                              : (root.theme ? root.theme.border : "#263946")

                Rectangle {
                    visible: row.active
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 2
                    width: 3
                    radius: 1.5
                    color: root.theme ? root.theme.workAccent : "#1687f8"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: root.theme ? root.theme.gapLg : 16
                    anchors.rightMargin: root.theme ? root.theme.gapSm : 8
                    anchors.topMargin: root.theme ? root.theme.gapSm : 8
                    anchors.bottomMargin: root.theme ? root.theme.gapSm : 8
                    spacing: root.theme ? root.theme.gapSm : 8

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 4
                        Layout.preferredWidth: 10
                        Layout.preferredHeight: 10
                        radius: 5
                        color: root.theme ? root.theme.workStageColor(row.modelData.stage) : "#2f89ff"
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: row.modelData.versionName
                            color: root.theme ? root.theme.textPrimary : "#eef4f8"
                            font.pixelSize: 15
                            font.bold: row.active
                        }
                        Label {
                            Layout.fillWidth: true
                            // Phase / Block / Stage hierarchy line.
                            text: row.modelData.phase + " / " + row.modelData.block + " / " + row.modelData.stage
                            color: root.theme ? root.theme.textSecondary : "#aab8c2"
                            font.pixelSize: root.theme ? root.theme.fontData : 12
                            elide: Text.ElideMiddle
                        }
                        Label {
                            text: row.modelData.lastUsedAt
                            color: root.theme ? root.theme.textMuted : "#71818d"
                            font.pixelSize: root.theme ? root.theme.fontCaption : 11
                        }
                    }

                    ColumnLayout {
                        spacing: 3
                        Layout.alignment: Qt.AlignVCenter

                        Common.StatusDot {
                            objectName: "versionOperationState"
                            visible: row.operationRunning || row.recoveryPending
                            theme: root.theme
                            state: row.recoveryPending ? "warn" : "active"
                            label: row.recoveryPending
                                   ? (root.i18n ? root.i18n.text("recovery needed", "需要復原", "需要恢复")
                                                : "recovery needed")
                                   : (root.i18n ? root.i18n.text("operation running", "操作進行中", "操作进行中")
                                                : "operation running")
                        }
                        Common.StatusDot {
                            visible: row.modelData.conflictCount > 0
                            theme: root.theme
                            state: "danger"
                            label: row.modelData.conflictCount + " "
                                   + (root.i18n ? root.i18n.text("conflict", "衝突", "冲突") : "conflict")
                        }
                        Common.StatusDot {
                            visible: row.modelData.changedCount > 0
                            theme: root.theme
                            state: "active"
                            label: row.modelData.changedCount + " "
                                   + (root.i18n ? root.i18n.text("changed", "已變更", "已变更") : "changed")
                        }
                        Common.StatusDot {
                            visible: row.modelData.conflictCount === 0 && row.modelData.changedCount === 0
                            theme: root.theme
                            state: "ok"
                            label: root.i18n ? root.i18n.text("clean", "乾淨", "乾净") : "clean"
                        }
                        Common.StatusDot {
                            visible: row.modelData.localCount > 0
                            theme: root.theme
                            state: "warn"
                            label: row.modelData.localCount + " LOCAL"
                        }
                    }
                }

                HoverHandler { id: hover }
                TapHandler {
                    onTapped: {
                        if (!row.active)
                            root.versionActivateRequested(row.modelData.id)
                    }
                }
            }
        }
    }
}
