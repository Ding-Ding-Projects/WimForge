import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root
    property bool opened: false
    property var entries: []
    property int unreadCount: 0
    signal closeRequested()
    signal markReadRequested(string id)
    signal markUnreadRequested(string id)
    signal dismissRequested(string id)
    signal deleteRequested(string id)
    signal restoreRequested(string id)
    signal undoRequested()

    visible: x < (parent ? parent.width : 9999)
    x: opened ? parent.width - width - 16 : parent.width + 12
    y: 76
    width: Math.min(430, parent.width - 32)
    height: parent.height - 92
    z: 900
    padding: 0

    Behavior on x { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }

    background: Rectangle {
        radius: 24
        color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
        border.width: 1
        border.color: Material.theme === Material.Dark ? "#49454F" : "#CAC4D0"
        layer.enabled: true
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 18
            Label {
                text: qsTr("Notification center")
                font.pixelSize: 21
                font.weight: Font.Bold
            }
            Rectangle {
                visible: root.unreadCount > 0
                implicitWidth: Math.max(26, countText.implicitWidth + 12)
                implicitHeight: 24
                radius: 12
                color: Material.accent
                Label { id: countText; anchors.centerIn: parent; text: root.unreadCount; color: "white"; font.bold: true }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: "↶"
                onClicked: root.undoRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Undo latest notification action")
            }
            ToolButton { text: "×"; onClicked: root.closeRequested() }
        }

        Label {
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.bottomMargin: 12
            Layout.fillWidth: true
            text: qsTr("Every read, dismiss, restore and delete is committed to a separate local Git repository.")
            wrapMode: Text.Wrap
            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
            font.pixelSize: 12
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC"
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            topMargin: 12
            bottomMargin: 14
            leftMargin: 12
            rightMargin: 12
            model: root.entries

            delegate: Pane {
                required property var modelData
                width: list.width - list.leftMargin - list.rightMargin
                padding: 14
                opacity: modelData.dismissed ? 0.58 : 1

                background: Rectangle {
                    radius: 16
                    color: modelData.read
                           ? (Material.theme === Material.Dark ? "#2B292F" : "#F7F2FA")
                           : (Material.theme === Material.Dark ? "#332D41" : "#F1EAFE")
                    border.color: modelData.read ? "transparent" : Material.accent
                    border.width: modelData.read ? 0 : 1
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 7
                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle {
                            width: 10; height: 10; radius: 5
                            color: modelData.kind === "error" ? "#BA1A1A"
                                 : modelData.kind === "warning" ? "#F9A825"
                                 : modelData.kind === "success" ? "#2E7D32"
                                 : Material.accent
                        }
                        Label {
                            text: modelData.title
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: modelData.timestamp
                            font.pixelSize: 11
                            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                        }
                    }
                    Label {
                        text: modelData.message
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        color: Material.theme === Material.Dark ? "#E6E0E9" : "#49454F"
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            visible: !modelData.read
                            text: qsTr("Mark read")
                            flat: true
                            onClicked: root.markReadRequested(modelData.id)
                        }
                        Button {
                            visible: modelData.read && !modelData.deleted
                            text: qsTr("Mark unread")
                            flat: true
                            onClicked: root.markUnreadRequested(modelData.id)
                        }
                        Button {
                            visible: !modelData.dismissed
                            text: qsTr("Dismiss")
                            flat: true
                            onClicked: root.dismissRequested(modelData.id)
                        }
                        Button {
                            visible: modelData.dismissed || modelData.deleted
                            text: qsTr("Restore")
                            flat: true
                            onClicked: root.restoreRequested(modelData.id)
                        }
                        Item { Layout.fillWidth: true }
                        ToolButton {
                            visible: !modelData.deleted
                            text: "⌫"
                            onClicked: root.deleteRequested(modelData.id)
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Soft-delete (recoverable in Git)")
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: qsTr("No notifications yet")
                color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
            }
        }
    }
}
