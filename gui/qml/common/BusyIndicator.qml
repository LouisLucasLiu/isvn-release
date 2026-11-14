import QtQuick
import QtQuick.Controls

Item {
    id: root
    property bool running: false
    width: 24
    height: 24

    BusyIndicator {
        anchors.fill: parent
        running: root.running
        visible: root.running
    }
}
