import QtQuick
import QtQuick.Controls

Dialog {
    id: root
    property var i18n
    property var theme
    property string body: ""
    property string confirmLabel: ""
    property var confirmAction: function() {}
    signal confirmed()

    modal: true
    // Explicit width avoids the Fusion Dialog implicitWidth binding loop that
    // arises from sizing to a wrapped content Label.
    implicitWidth: 460
    title: root.i18n ? root.i18n.text("Confirm", "確認", "确认") : "確認"
    palette.window: theme ? theme.bgPanel : "#ffffff"
    palette.windowText: theme ? theme.textPrimary : "#1f2937"
    palette.buttonText: theme ? theme.textPrimary : "#1f2937"

    function confirm() {
        root.confirmAction()
        root.confirmed()
        root.close()
    }

    function cancel() {
        root.close()
    }

    Label {
        text: root.body
        width: 420
        wrapMode: Text.Wrap
        color: root.theme ? root.theme.textPrimary : "#1f2937"
        font.pixelSize: root.theme ? root.theme.fontBody : 13
    }

    // Themed buttons: the style's default button keeps a light face in the dark
    // theme, which leaves the light label text unreadable.
    footer: Row {
        spacing: root.theme ? root.theme.gapSm : 8
        padding: root.theme ? root.theme.gapSm : 8
        topPadding: 0
        ActionButton {
            objectName: "confirmDialogCancelButton"
            theme: root.theme
            text: root.i18n ? root.i18n.text("Cancel", "取消", "取消") : "Cancel"
            onClicked: root.cancel()
        }

        ActionButton {
            objectName: "confirmDialogOkButton"
            theme: root.theme
            kind: "primary"
            text: root.confirmLabel.length > 0 ? root.confirmLabel :
                  (root.i18n ? root.i18n.text("OK", "確定", "确定") : "OK")
            onClicked: root.confirm()
        }
    }
}
