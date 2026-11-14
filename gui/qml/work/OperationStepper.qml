import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// OperationStepper — a small step indicator for multi-step operation flows.
// Used both by Checkin (Prepare/Review/Confirm/Result) and by the composite
// Create -> Checkout -> Set Active flow. Steps and their state are passed in;
// the component owns no workflow logic.
Item {
    id: root

    property var theme
    property var steps: ["Prepare", "Review", "Confirm", "Result"]
    property int currentStep: 1
    property int failedStep: -1

    implicitHeight: 42

    RowLayout {
        anchors.fill: parent
        spacing: root.theme ? root.theme.gapSm : 8

        Repeater {
            model: root.steps

            delegate: RowLayout {
                required property string modelData
                required property int index
                Layout.fillWidth: true
                spacing: root.theme ? root.theme.gapSm : 8

                readonly property bool done: index < root.currentStep && index !== root.failedStep
                readonly property bool failed: index === root.failedStep
                readonly property bool active: index === root.currentStep && index !== root.failedStep

                Rectangle {
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    radius: 13
                    color: parent.failed ? (root.theme ? root.theme.accentDanger : "#ff4d43")
                           : parent.active ? (root.theme ? root.theme.workAccent : "#1687f8")
                           : parent.done ? (root.theme ? root.theme.workSelected : "#123c5f")
                           : "transparent"
                    border.width: 1
                    border.color: parent.failed ? (root.theme ? root.theme.accentDanger : "#ff4d43")
                                  : (index <= root.currentStep
                                     ? (root.theme ? root.theme.workAccent : "#1687f8")
                                     : (root.theme ? root.theme.workBorderStrong : "#365061"))

                    Label {
                        anchors.centerIn: parent
                        text: parent.parent.failed ? "✕" : parent.parent.done ? "✓" : String(index + 1)
                        color: (index <= root.currentStep || parent.parent.failed) ? "#ffffff"
                               : (root.theme ? root.theme.textMuted : "#71818d")
                        font.pixelSize: root.theme ? root.theme.fontData : 12
                    }
                }

                Label {
                    text: modelData
                    color: parent.failed ? (root.theme ? root.theme.accentDanger : "#ff4d43")
                           : (index === root.currentStep
                              ? (root.theme ? root.theme.textPrimary : "#eef4f8")
                              : (root.theme ? root.theme.textMuted : "#71818d"))
                    font.pixelSize: root.theme ? root.theme.fontBody : 13
                }

                Rectangle {
                    visible: index < root.steps.length - 1
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: index < root.currentStep
                           ? (root.theme ? root.theme.workAccent : "#1687f8")
                           : (root.theme ? root.theme.workBorderStrong : "#365061")
                }
            }
        }
    }
}
