import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root
    property string eyebrow: ""
    property string value: ""
    property string detail: ""
    property color accent: Material.accent
    property string glyph: "●"

    implicitWidth: 230
    implicitHeight: 142
    padding: 18

    background: Rectangle {
        radius: 18
        color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
        border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC"
        border.width: 1
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: root.eyebrow
                color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                width: 30; height: 30; radius: 10
                color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16)
                Label { anchors.centerIn: parent; text: root.glyph; color: root.accent; font.pixelSize: 14 }
            }
        }
        Label {
            text: root.value
            font.pixelSize: 26
            font.weight: Font.Bold
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            text: root.detail
            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
            font.pixelSize: 12
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }
}
