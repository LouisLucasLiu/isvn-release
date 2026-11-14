import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common

// WorkHeader — Active Working Version identity + state chips + secondary
// actions. Header actions never compete visually with the workflow primary
// action. Create Derived Version is disclosed as a composite flow.
Rectangle {
    id: root

    property var theme
    property var i18n
    property var workState
    property var workModel
    property bool realBinding: false
    property bool canInspect: true
    property bool canCreateDerived: true

    signal inspectInExploreRequested()
    signal createDerivedRequested()

    implicitHeight: root.theme ? root.theme.workHeaderHeight : 104
    color: root.theme ? root.theme.workSurface0 : "#0a1823"

    function version() {
        return root.workState ? root.workState.activeVersion() : null
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.theme ? root.theme.gapXl : 24
        anchors.rightMargin: root.theme ? root.theme.gapXl : 24
        anchors.topMargin: root.theme ? root.theme.gapMd : 12
        anchors.bottomMargin: root.theme ? root.theme.gapMd : 12
        spacing: root.theme ? root.theme.gapMd : 12

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            Label {
                text: root.i18n ? root.i18n.text("ACTIVE WORKING VERSION", "使用中的工作版本", "使用中的工作版本")
                                : "ACTIVE WORKING VERSION"
                color: root.theme ? root.theme.textMuted : "#71818d"
                font.pixelSize: root.theme ? root.theme.fontData : 12
            }

            RowLayout {
                spacing: root.theme ? root.theme.gapMd : 12

                Label {
                    objectName: "activeVersionIdentity"
                    text: root.version()
                          ? root.version().phase + " / " + root.version().block + " / "
                            + root.version().stage + " / " + root.version().versionName
                          : (root.i18n ? root.i18n.text("No active version", "尚無使用中版本", "尚无使用中版本")
                                       : "No active version")
                    color: root.theme ? root.theme.textPrimary : "#eef4f8"
                    font.pixelSize: root.theme ? root.theme.fontTitle : 22
                    font.bold: true
                }

                Common.StatusDot {
                    visible: root.version() && root.version().changedCount > 0
                    theme: root.theme
                    state: "active"
                    label: root.version() ? (root.version().changedCount + " "
                            + (root.i18n ? root.i18n.text("changed", "已變更", "已变更") : "changed")) : ""
                }

                Common.StatusDot {
                    visible: root.version() && root.version().conflictCount > 0
                    theme: root.theme
                    state: "danger"
                    label: root.version() ? (root.version().conflictCount + " "
                            + (root.i18n ? root.i18n.text("conflict", "衝突", "冲突") : "conflict")) : ""
                }

                Common.StatusDot {
                    visible: root.version() && root.version().localCount > 0
                    theme: root.theme
                    state: "warn"
                    label: root.version() ? (root.version().localCount + " LOCAL") : ""
                }
            }

            Label {
                text: root.version() ? root.version().localPath : ""
                color: root.theme ? root.theme.textMuted : "#71818d"
                font.pixelSize: root.theme ? root.theme.fontData : 12
                font.family: root.theme ? root.theme.monoFont : "monospace"
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
        }

        Common.ActionButton {
            objectName: "headerInspectInExploreButton"
            theme: root.theme
            kind: "secondary"
            enabled: root.canInspect
            text: root.i18n ? root.i18n.text("Inspect in Explore", "在探索檢視", "在探索查看") : "Inspect in Explore"
            onClicked: root.inspectInExploreRequested()
        }

        Common.ActionButton {
            objectName: "createDerivedButton"
            theme: root.theme
            kind: "secondary"
            enabled: root.canCreateDerived
            text: root.i18n ? root.i18n.text("Create Derived Version", "建立衍生版本", "创建衍生版本") : "Create Derived Version"
            onClicked: root.createDerivedRequested()
        }
    }
}
