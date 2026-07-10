import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property string message: ""
    property string tone: "info"
    property string actionText: ""
    signal actionTriggered()

    function show(text, kind, action) {
        message = text
        tone = kind || "info"
        actionText = action || ""
        visible = true
        hideTimer.restart()
    }

    visible: false
    implicitWidth: Math.min(620, parent ? parent.width - 48 : 620)
    implicitHeight: 58
    z: 1000

    Rectangle {
        anchors.fill: parent
        radius: 15
        color: root.tone === "error" ? "#601410"
             : root.tone === "warning" ? "#4A2800"
             : root.tone === "success" ? "#12351F"
             : "#322F35"
        border.color: root.tone === "error" ? "#FFB4AB"
                    : root.tone === "warning" ? "#FFB95C"
                    : root.tone === "success" ? "#8BD7A6"
                    : "#938F99"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 10
            spacing: 12
            Label {
                text: root.tone === "error" ? "!" : root.tone === "success" ? "✓" : "i"
                color: "white"
                font.pixelSize: 17
                font.weight: Font.Bold
            }
            Label {
                text: root.message
                color: "white"
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            Button {
                visible: root.actionText.length > 0
                text: root.actionText
                flat: true
                Material.foreground: "#D0BCFF"
                onClicked: { root.actionTriggered(); root.visible = false }
            }
            ToolButton {
                text: "×"
                Material.foreground: "white"
                onClicked: root.visible = false
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Dismiss")
            }
        }
    }

    Timer {
        id: hideTimer
        interval: 5200
        onTriggered: root.visible = false
    }

    Behavior on opacity { NumberAnimation { duration: 180 } }
}
