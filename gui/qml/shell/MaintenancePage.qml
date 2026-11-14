import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../common" as Common
import "../workspaceC" as LegacyMaintenance

// The legacy functional Maintenance body is embedded only once.  Members see
// only the explicitly read-only diagnostic surface; PM retains the proven body.
Item {
    id: root
    objectName: "maintenancePage"
    property var theme
    property var i18n
    property string role: "DENIED"
    property var operationCoordinator

    LegacyMaintenance.WorkspaceC {
        objectName: "embeddedMaintenanceBody"
        anchors.fill: parent
        visible: root.role === "PM"
        pmMode: true
        i18n: root.i18n
        theme: root.theme
        onStatusMessage: function(message) { }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(620, parent.width - 80)
        visible: root.role === "MEMBER"
        spacing: root.theme.gapMd
        Common.SectionHeader {
            Layout.fillWidth: true
            theme: root.theme
            title: root.i18n.text("Maintenance diagnostics", "維運診斷", "维护诊断")
            subtitle: root.i18n.text("Read-only for MEMBER", "MEMBER 僅可唯讀", "MEMBER 仅可只读")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.i18n.text("Integrity and status diagnostics remain available. Cleanup, reclaim, repair and every mutation are denied by the backend.",
                                 "完整性與狀態診斷仍可使用；清理、回收、修復及所有變更皆由後端拒絕。",
                                 "完整性与状态诊断仍可使用；清理、回收、修复及所有变更皆由后端拒绝。")
            color: root.theme.textSecondary
        }
        Common.ActionButton {
            objectName: "memberIntegrityVerify"
            theme: root.theme
            text: root.i18n.text("Verify integrity", "驗證完整性", "验证完整性")
            onClicked: {
                if (root.operationCoordinator)
                    root.operationCoordinator.beginRead("maintenance-integrity", "project", ["lineage", "verify"])
            }
        }
        Label {
            text: root.i18n.text("GC, purge, reclaim, repair and administration require PM authorization.",
                                 "GC、清除、回收、修復與管理需 PM 授權。",
                                 "GC、清除、回收、修复与管理需 PM 授权。")
            color: root.theme.textMuted
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}
