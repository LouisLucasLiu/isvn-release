import QtQuick

QtObject {
    id: root

    property string language: "zh_TW"
    readonly property var languageOptions: [
        { code: "en", label: "English" },
        { code: "zh_TW", label: "繁體中文" },
        { code: "zh_CN", label: "简体中文" }
    ]

    function text(en, zhTw, zhCn) {
        if (root.language === "en")
            return en
        if (root.language === "zh_CN")
            return zhCn && zhCn.length > 0 ? zhCn : zhTw
        return zhTw
    }
}
