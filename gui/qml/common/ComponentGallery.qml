import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml

Item {
    id: root

    property QtObject theme
    property QtObject i18n

    function text(en, zhTw, zhCn) {
        return root.i18n ? root.i18n.text(en, zhTw, zhCn) : en
    }

    ScrollView {
        id: galleryScroll
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: galleryScroll.availableWidth
            spacing: root.theme ? root.theme.gapLg : 16

            ShellPanel {
                theme: root.theme
                title: root.text("Common Component Gallery", "共用元件展示", "共用组件展示")
                subtitle: root.text("Visual-only samples. No backend calls are made.",
                                    "純視覺樣本，不會呼叫後端。",
                                    "纯视觉样本，不会调用后端。")
                elevated: true
                Layout.fillWidth: true
                Layout.preferredHeight: 310

                ColumnLayout {
                    anchors.fill: parent
                    spacing: root.theme ? root.theme.gapMd : 12

                    SectionHeader {
                        theme: root.theme
                        title: root.text("Status and actions", "狀態與動作", "状态与动作")
                        subtitle: root.text("Compact indicators for dense panels.",
                                            "適合高密度面板的小型指示。",
                                            "适合高密度面板的小型指示。")
                        tip: root.text("These samples only emit QML signals.",
                                       "這些樣本只發出 QML signal。",
                                       "这些样本只发出 QML signal。")
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.theme ? root.theme.gapMd : 12

                        StatusDot { theme: root.theme; state: "ok"; label: root.text("Ready", "就緒", "就绪") }
                        StatusDot { theme: root.theme; state: "warn"; label: root.text("Review", "需檢視", "需检查") }
                        StatusDot { theme: root.theme; state: "danger"; label: root.text("Blocked", "已阻擋", "已阻挡") }
                        RoleBadge { theme: root.theme; role: "PM" }
                        RoleBadge { theme: root.theme; role: "MEMBER" }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: root.theme ? root.theme.gapSm : 8

                        ActionButton { theme: root.theme; text: root.text("Primary", "主要", "主要"); kind: "primary" }
                        ActionButton { theme: root.theme; text: root.text("Secondary", "次要", "次要"); kind: "secondary" }
                        ActionButton { theme: root.theme; text: "GC"; kind: "danger"; dangerous: true }
                        ActionButton { theme: root.theme; text: root.text("Ghost", "透明", "透明"); kind: "ghost" }
                    }

                    EmptyState {
                        theme: root.theme
                        title: root.text("No version selected", "尚未選取版本", "尚未选取版本")
                        message: root.text("Choose a version to inspect its details.",
                                           "選取版本後可檢視資訊。",
                                           "选取版本后可检查信息。")
                        actionText: root.text("Refresh", "重新整理", "刷新")
                        Layout.fillWidth: true
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 290
                spacing: root.theme ? root.theme.gapLg : 16

                ShellPanel {
                    theme: root.theme
                    title: root.text("Inspector samples", "檢視器樣本", "检查器样本")
                    elevated: false
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: root.theme ? root.theme.gapMd : 12

                        InspectorSection {
                            theme: root.theme
                            title: root.text("Selected version", "目前版本", "当前版本")
                            subtitle: root.text("Dense metadata layout", "高密度資訊排列", "高密度信息排列")
                            tip: root.text("Collapse is local only.", "收合只影響本地顯示。", "折叠只影响本地显示。")
                            Layout.fillWidth: true
                            Layout.preferredHeight: 150

                            KeyValueGrid {
                                theme: root.theme
                                monoValue: true
                                rows: [
                                    { key: root.text("Version", "版本", "版本"), value: "Trail/Block1/SYN/20260613v1" },
                                    { key: root.text("Stage", "階段", "阶段"), value: "SYN" },
                                    { key: root.text("Actor", "Actor", "Actor"), value: "alice" }
                                ]
                                Layout.fillWidth: true
                            }
                        }

                        SeverityBanner {
                            theme: root.theme
                            severity: "warn"
                            title: root.text("Source check needs review", "來源核對需檢視", "来源核对需检查")
                            message: root.text("You decide whether the recorded source is correct.",
                                               "由你判斷記錄的來源是否正確。",
                                               "由你判断记录的来源是否正确。")
                            Layout.fillWidth: true
                        }
                    }
                }

                ShellPanel {
                    theme: root.theme
                    title: root.text("Path and file samples", "路徑與檔案樣本", "路径与文件样本")
                    elevated: false
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: root.theme ? root.theme.gapSm : 8

                        VersionPathPill { theme: root.theme; path: "Trail/Block1/RTL/20260613v1"; selected: true; Layout.fillWidth: true }
                        VersionPathPill { theme: root.theme; path: "P1_trial/top/400-APR/20260601v1"; Layout.fillWidth: true }
                        VersionPathPill { theme: root.theme; path: "P1_trial/top/450-ROUTE/20260601v1"; Layout.fillWidth: true }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: root.theme ? root.theme.gapSm : 8
                            FileStateBadge { theme: root.theme; i18n: root.i18n; linkState: "LINKED"; svnState: "normal" }
                            FileStateBadge { theme: root.theme; i18n: root.i18n; linkState: "LOCAL"; svnState: "modified" }
                            HashPill { theme: root.theme; hash: "a1b2c3d4e5f678901234567890abcdef"; copyable: true }
                            HashPill { theme: root.theme; hash: "9f8e7d6c5b4a32100123456789abcdef" }
                        }

                        StageLegend {
                            theme: root.theme
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            ShellPanel {
                theme: root.theme
                title: root.text("Graph samples", "關係圖樣本", "关系图样本")
                subtitle: root.text("Node clicks emit metadata-only signals.",
                                    "節點點擊只發出資訊用 signal。",
                                    "节点点击只发出信息用 signal。")
                elevated: true
                Layout.fillWidth: true
                Layout.preferredHeight: 260

                ColumnLayout {
                    anchors.fill: parent
                    spacing: root.theme ? root.theme.gapMd : 12

                    GraphToolbar {
                        theme: root.theme
                        i18n: root.i18n
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: root.theme ? root.theme.gapMd : 12

                        GraphNodeCard {
                            theme: root.theme
                            i18n: root.i18n
                            versionPath: "Trail/Block1/RTL/20260613v1"
                            versionLabel: "20260613v1"
                            stage: "RTL"
                            actor: "alice"
                            timestamp: "2026-06-13 09:00"
                            note: root.text("Root version", "根版本", "根版本")
                            current: true
                            selected: true
                        }

                        GraphNodeCard {
                            theme: root.theme
                            i18n: root.i18n
                            versionPath: "Trail/Block1/SYN/20260613v1"
                            versionLabel: "20260613v1"
                            stage: "SYN"
                            actor: "bob"
                            timestamp: "2026-06-14 11:20"
                            note: root.text("Derived version", "衍生版本", "衍生版本")
                        }

                        GraphNodeCard {
                            theme: root.theme
                            i18n: root.i18n
                            versionPath: "P1_trial/top/450-ROUTE/20260601v1"
                            versionLabel: "20260601v1"
                            stage: "ROUTE"
                            actor: "carol"
                            timestamp: "2026-06-15 15:30"
                            superseded: true
                        }
                    }
                }
            }
        }
    }
}
