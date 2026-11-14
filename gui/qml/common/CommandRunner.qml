import QtQuick

QtObject {
    id: root
    property var i18n
    property var lastJson: null
    property string lastError: ""
    signal succeeded(var json)
    signal failed(string message)

    function runJson(args) {
        lastError = ""
        isvnCli.commandSucceeded.connect(onSucceeded)
        isvnCli.commandFailed.connect(onFailed)
        isvnCli.runJson(args)
    }

    function onSucceeded(json) {
        isvnCli.commandSucceeded.disconnect(onSucceeded)
        isvnCli.commandFailed.disconnect(onFailed)
        lastJson = json
        succeeded(json)
    }

    function onFailed(message) {
        isvnCli.commandSucceeded.disconnect(onSucceeded)
        isvnCli.commandFailed.disconnect(onFailed)
        lastError = humanize(message)
        failed(lastError)
    }

    function text(en, zhTw, zhCn) {
        return root.i18n ? root.i18n.text(en, zhTw, zhCn) : zhTw
    }

    function humanize(message) {
        var raw = String(message || "")
        var lower = raw.toLowerCase()
        if (lower.indexOf("not inside an isvn workspace") >= 0 ||
                lower.indexOf("run isvn member init first") >= 0 ||
                raw.indexOf("找不到 isvn 專案") >= 0) {
            return text("This folder is not part of an isvn workspace. Create or open a workspace first.",
                        "尚未加入工作區。請先建立或開啟你的工作區。",
                        "尚未加入工作区。请先建立或打开你的工作区。")
        }
        if (lower.indexOf("unknown command") >= 0) {
            return text("This action is not supported by the installed isvn command. Please update isvn.",
                        "目前安裝的 isvn 不支援這個動作，請先更新 isvn。",
                        "目前安装的 isvn 不支持这个动作，请先更新 isvn。")
        }
        if (lower.indexOf("missing project .isvn/config") >= 0) {
            return text("The project setup is incomplete. Ask the PM to initialize the project first.",
                        "專案設定尚未完成。請 PM 先完成專案初始化。",
                        "项目设置尚未完成。请 PM 先完成项目初始化。")
        }
        if (lower.indexOf("not under target working copy") >= 0 ||
                lower.indexOf("not under current working copy") >= 0) {
            return text("The selected file is outside this version's folder. Choose a file inside the version folder, or check in to copy it in automatically.",
                        "所選檔案不在這個版本的資料夾內。請選擇版本資料夾底下的檔案;若直接簽入，系統會自動把它複製進版本後再簽入。",
                        "所选文件不在这个版本的文件夹内。请选择版本文件夹下的文件;若直接签入，系统会自动把它复制进版本后再签入。")
        }
        if (lower.indexOf("permission denied") >= 0 || raw.indexOf("權限") >= 0) {
            return text("You do not have permission to complete this action in the current project.",
                        "你目前沒有權限在這個專案執行此動作。",
                        "你目前没有权限在这个项目执行此动作。")
        }
        var detail = raw.trim()
        if (detail.length > 0) {
            return text("Action failed: ", "動作失敗：", "动作失败：") + detail
        }
        return text("The action failed. Check your workspace state and try again.",
                    "動作失敗。請確認工作區狀態後再試一次。",
                    "动作失败。请确认工作区状态后再试一次。")
    }
}
