import QtQuick

QtObject {
    id: root

    property bool dark: true

    readonly property color bgBase: dark ? "#0E1116" : "#FAFBFC"
    readonly property color bgPanel: dark ? "#161B22" : "#FFFFFF"
    readonly property color bgElevated: dark ? "#1C232D" : "#F1F5F9"
    readonly property color border: dark ? "#2A3441" : "#D0D7DE"
    readonly property color textPrimary: dark ? "#E6EDF3" : "#1F2937"
    readonly property color textSecondary: dark ? "#8B98A5" : "#59636E"
    readonly property color accent: "#3FB6C7"
    readonly property color accentWarn: "#E0A030"
    readonly property color accentDanger: "#E5534B"
    readonly property color accentOk: "#3FB950"

    readonly property int radius: 6
    readonly property int gapXs: 4
    readonly property int gapSm: 8
    readonly property int gapMd: 12
    readonly property int gapLg: 16
    readonly property int gapXl: 24

    readonly property int fontTitle: 22
    readonly property int fontSection: 15
    readonly property int fontBody: 13
    readonly property int fontData: 12
    readonly property int fontCaption: 11

    readonly property string sansFont: "Inter, IBM Plex Sans, Noto Sans CJK TC, sans-serif"
    readonly property string monoFont: "JetBrains Mono, IBM Plex Mono, monospace"

    function stageColor(path) {
        var normalized = String(path || "").toUpperCase()
        if (normalized.indexOf("/RTL/") >= 0 || normalized === "RTL") return accent
        if (normalized.indexOf("/SYN/") >= 0 || normalized === "SYN") return accentWarn
        if (normalized.indexOf("/CTS/") >= 0 || normalized === "CTS") return "#A78BFA"
        if (normalized.indexOf("/ROUTE/") >= 0 || normalized === "ROUTE") return "#F472B6"
        if (normalized.indexOf("/SIGNOFF/") >= 0 || normalized === "SIGNOFF") return accentOk
        if (normalized.indexOf("/PNR/") >= 0 || normalized.indexOf("/P&R/") >= 0 || normalized === "PNR") return "#A78BFA"
        return accentOk
    }

// --- Work shell tokens --------------------------------------------------
    // These extend the dark/light token set for the Home/Work/Explore/
    // Maintenance shell. The legacy shell keeps using the tokens above.

    // Azure primary accent reserved for the Work focus / active selection /
    // single dominant primary action, kept distinct from the legacy teal accent.
    readonly property color workAccent: dark ? "#1687f8" : "#0f6fd6"
    readonly property color workAccentHover: dark ? "#2b96ff" : "#0d63c2"

    // Layered neutral surfaces for the dense Work layout.
    readonly property color workSurface0: dark ? "#0A1823" : "#F4F7FA"
    readonly property color workSurface1: dark ? "#0D1D29" : "#FFFFFF"
    readonly property color workSurface2: dark ? "#122633" : "#EDF2F6"
    readonly property color workSurfaceHover: dark ? "#172E3D" : "#E4EBF1"
    readonly property color workSelected: dark ? "#123C5F" : "#DCEBFB"
    readonly property color workBorderStrong: dark ? "#365061" : "#C2CCD6"
    readonly property color textMuted: dark ? "#71818D" : "#8A97A3"

    // Stage colors are independent of status colors.
    readonly property color stageRtl: "#A56DE2"
    readonly property color stageSyn: "#2F89FF"
    readonly property color stageCts: "#54C960"
    readonly property color stageRoute: "#F1A126"
    readonly property color stageSignoff: "#FF5B3A"

    // Geometry for the Work shell chrome.
    readonly property int topBarHeight: 56
    readonly property int workHeaderHeight: 104
    readonly property int bottomBarHeight: 30
    readonly property int rowHeight: 36
    readonly property int controlHeight: 36

    function workStageColor(stage) {
        var s = String(stage || "").toUpperCase()
        if (s === "RTL") return stageRtl
        if (s === "SYN" || s === "SYNTHESIS") return stageSyn
        if (s === "CTS") return stageCts
        if (s === "ROUTE") return stageRoute
        if (s === "SIGNOFF") return stageSignoff
        return accentOk
    }
}
