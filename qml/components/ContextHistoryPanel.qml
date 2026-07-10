pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Non-modal mini history manager. Instantiate once near the application
// overlay, then call openAt(pointerX, pointerY, contextTitle, events) from any
// right-click handler. It deliberately uses Popup rather than Dialog, so an
// open history surface never blocks image servicing or keyboard navigation.
Popup {
    id: root

    property var events: []
    property string contextTitle: qsTr("This item")
    property string contextKey: ""
    property string elementId: ""
    property string branchName: "main"
    property int maximumVisibleActions: 20

    signal undoRequested(string eventId)
    signal redoRequested(string eventId)
    signal restoreRequested(string eventId)
    signal bookmarkRequested(string eventId, string name)
    signal branchRequested(string eventId, string name)

    function openAt(pointerX, pointerY, title, recentEvents) {
        contextTitle = title || qsTr("This item")
        events = recentEvents || []
        const availableWidth = parent ? parent.width : 1280
        const availableHeight = parent ? parent.height : 800
        x = Math.max(8, Math.min(pointerX, availableWidth - width - 8))
        y = Math.max(8, Math.min(pointerY, availableHeight - height - 8))
        open()
    }

    modal: false
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(420, parent ? parent.width - 16 : 420)
    height: Math.min(570, parent ? parent.height - 16 : 570)
    padding: 0
    z: 1200

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: 150; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; to: 0; duration: 90 }
            NumberAnimation { property: "scale"; to: 0.98; duration: 90 }
        }
    }

    background: Rectangle {
        radius: 22
        color: root.Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
        border.width: 1
        border.color: root.Material.theme === Material.Dark ? "#49454F" : "#CAC4D0"
        layer.enabled: true
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 8
            Layout.topMargin: 12
            Layout.bottomMargin: 10
            spacing: 8

            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: 12
                color: root.Material.theme === Material.Dark ? "#4A4458" : "#EADDFF"
                Label {
                    anchors.centerIn: parent
                    text: "↶"
                    font.pixelSize: 21
                    color: root.Material.theme === Material.Dark ? "#EADDFF" : "#21005D"
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    Layout.fillWidth: true
                    text: qsTr("History here")
                    font.pixelSize: 19
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: root.contextTitle + "  ·  " + root.branchName
                    font.pixelSize: 11
                    color: root.Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                    elide: Text.ElideMiddle
                }
            }
            ToolButton {
                text: "×"
                Accessible.name: qsTr("Close contextual history")
                onClicked: root.close()
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: root.Material.theme === Material.Dark ? "#49454F" : "#E7E0EC"
        }

        ListView {
            id: actionList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.events
            spacing: 7
            topMargin: 10
            bottomMargin: 10
            leftMargin: 10
            rightMargin: 10

            delegate: Pane {
                id: actionCard
                required property var modelData
                width: actionList.width - actionList.leftMargin - actionList.rightMargin
                padding: 11

                readonly property bool toggleable: modelData.type === "action"
                                                   || modelData.type === "compensation"
                readonly property bool effective: modelData.effective === undefined
                                                  ? true : modelData.effective
                readonly property string eventIcon: {
                    if (modelData.icon === "undo") return "↶"
                    if (modelData.icon === "redo") return "↷"
                    if (modelData.icon === "bookmark") return "★"
                    if (modelData.icon === "fork_right") return "⑂"
                    if (modelData.destructive) return "⚠"
                    return "✦"
                }

                background: Rectangle {
                    radius: 15
                    color: actionCard.effective
                           ? (actionCard.Material.theme === Material.Dark ? "#2B292F" : "#F7F2FA")
                           : (actionCard.Material.theme === Material.Dark ? "#252329" : "#F3EEF5")
                    border.width: actionCard.modelData.destructive ? 1 : 0
                    border.color: "#BA1A1A"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: actionCard.eventIcon
                            font.pixelSize: 18
                            color: actionCard.modelData.destructive ? "#BA1A1A" : actionCard.Material.accent
                            Accessible.name: actionCard.modelData.icon || qsTr("Action")
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                Layout.fillWidth: true
                                text: actionCard.modelData.title || qsTr("Recorded action")
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                opacity: actionCard.effective ? 1 : 0.62
                            }
                            Label {
                                Layout.fillWidth: true
                                text: actionCard.modelData.diffSummary || actionCard.modelData.description || qsTr("No diff summary")
                                font.pixelSize: 11
                                color: actionCard.Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                                elide: Text.ElideRight
                            }
                        }
                        Label {
                            text: "#" + (actionCard.modelData.sequence || "")
                            font.pixelSize: 10
                            color: actionCard.Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Button {
                            visible: actionCard.toggleable && actionCard.effective
                            icon.name: "edit-undo"
                            text: qsTr("Undo")
                            flat: true
                            onClicked: root.undoRequested(actionCard.modelData.id)
                        }
                        Button {
                            visible: actionCard.toggleable && !actionCard.effective
                            icon.name: "edit-redo"
                            text: qsTr("Redo")
                            flat: true
                            onClicked: root.redoRequested(actionCard.modelData.id)
                        }
                        Button {
                            visible: actionCard.modelData.type === "action"
                            text: qsTr("Restore here")
                            flat: true
                            onClicked: root.restoreRequested(actionCard.modelData.id)
                        }
                        Item { Layout.fillWidth: true }
                        ToolButton {
                            text: "★"
                            Accessible.name: qsTr("Bookmark this action")
                            onClicked: {
                                inlineEditor.mode = "bookmark"
                                inlineEditor.eventId = actionCard.modelData.id
                                inlineName.text = ""
                                inlineEditor.visible = true
                                inlineName.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                        }
                        ToolButton {
                            text: "⑂"
                            Accessible.name: qsTr("Branch from this action")
                            onClicked: {
                                inlineEditor.mode = "branch"
                                inlineEditor.eventId = actionCard.modelData.id
                                inlineName.text = ""
                                inlineEditor.visible = true
                                inlineName.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: actionList.count === 0
                width: Math.min(280, actionList.width - 32)
                text: qsTr("Nothing changed here yet. Future actions will appear without interrupting your work.")
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                color: root.Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
            }
        }

        Pane {
            id: inlineEditor
            property string mode: "bookmark"
            property string eventId: ""
            visible: false
            Layout.fillWidth: true
            padding: 10

            background: Rectangle {
                color: inlineEditor.Material.theme === Material.Dark ? "#332D41" : "#F1EAFE"
                border.width: 1
                border.color: inlineEditor.Material.accent
            }

            RowLayout {
                anchors.fill: parent
                Label { text: inlineEditor.mode === "branch" ? "⑂" : "★" }
                TextField {
                    id: inlineName
                    Layout.fillWidth: true
                    placeholderText: inlineEditor.mode === "branch"
                                     ? qsTr("New branch name") : qsTr("Bookmark name")
                    maximumLength: 80
                    onAccepted: saveInlineButton.clicked()
                }
                Button {
                    id: saveInlineButton
                    text: qsTr("Save")
                    enabled: inlineName.text.trim().length > 0
                    onClicked: {
                        if (inlineEditor.mode === "branch")
                            root.branchRequested(inlineEditor.eventId, inlineName.text.trim())
                        else
                            root.bookmarkRequested(inlineEditor.eventId, inlineName.text.trim())
                        inlineEditor.visible = false
                    }
                }
                ToolButton { text: "×"; onClicked: inlineEditor.visible = false }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            Layout.bottomMargin: 10
            text: qsTr("Undo is recorded as a new action, so undoing an undo safely redoes it.")
            wrapMode: Text.Wrap
            font.pixelSize: 10
            color: root.Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
        }
    }
}
