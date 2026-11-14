import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

RowLayout {
    id: root

    property QtObject theme
    property QtObject i18n
    property string linkState: ""
    property string svnState: ""

    spacing: root.theme ? root.theme.gapXs : 4

    function normalizedLink() {
        if (root.linkState === "LINKED")
            return "LINKED"
        if (root.linkState === "LOCAL")
            return "LOCAL"
        return "unknown"
    }

    function linkLabel() {
        if (normalizedLink() === "LINKED")
            return root.i18n ? root.i18n.text("retained", "已保留", "已保留") : "retained"
        if (normalizedLink() === "LOCAL")
            return root.i18n ? root.i18n.text("local", "本機", "本机") : "local"
        return root.i18n ? root.i18n.text("unknown", "未知", "未知") : "unknown"
    }

    function linkStateName() {
        if (normalizedLink() === "LINKED")
            return "ok"
        if (normalizedLink() === "LOCAL")
            return "warn"
        return "neutral"
    }

    function svnStateLabel() {
        // Map raw SVN working-copy status codes to user-facing labels.
        // "obstructed" is the expected state for LINKED pool-symlink files and means retained.
        if (root.svnState === "obstructed")
            return root.i18n ? root.i18n.text("retained", "已保留", "已保留") : "retained"
        if (root.svnState === "normal")
            return root.i18n ? root.i18n.text("clean", "乾淨", "乾净") : "clean"
        if (root.svnState === "M")
            return root.i18n ? root.i18n.text("modified", "已修改", "已修改") : "modified"
        if (root.svnState === "?")
            return root.i18n ? root.i18n.text("untracked", "未追蹤", "未追踪") : "untracked"
        if (root.svnState === "!")
            return root.i18n ? root.i18n.text("missing", "遺失", "遗失") : "missing"
        if (root.svnState === "D")
            return root.i18n ? root.i18n.text("deleted", "已刪除", "已删除") : "deleted"
        return root.svnState.length > 0 ? root.svnState :
               (root.i18n ? root.i18n.text("unknown", "未知", "未知") : "unknown")
    }

    Label {
        text: root.svnStateLabel()
        color: (root.svnState === "normal" || root.svnState === "obstructed")
               ? (root.theme ? root.theme.textSecondary : "#59636e")
               : (root.theme ? root.theme.accentWarn : "#8a5a00")
        font.pixelSize: root.theme ? root.theme.fontData : 12
        font.family: root.theme ? root.theme.monoFont : ""
        elide: Text.ElideRight
        Layout.maximumWidth: 72
    }

    StatusDot {
        theme: root.theme
        state: root.linkStateName()
        label: root.linkLabel()
        Layout.preferredWidth: 84
    }
}
