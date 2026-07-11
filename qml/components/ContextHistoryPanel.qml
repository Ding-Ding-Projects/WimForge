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
    property bool motionEnabled: true
    property bool dark: Material.theme === Material.Dark
    readonly property color secondaryTextColor: DesignTokens.onSurfaceVariant(dark)
    readonly property color errorColor: DesignTokens.error(dark)

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
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: DesignTokens.motionDuration(120, root.motionEnabled)
            }
            NumberAnimation {
                property: "scale"
                from: 0.96
                to: 1
                duration: DesignTokens.motionDuration(DesignTokens.motionShort,
                                                      root.motionEnabled)
                easing.type: Easing.OutCubic
            }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                to: 0
                duration: DesignTokens.motionDuration(90, root.motionEnabled)
            }
            NumberAnimation {
                property: "scale"
                to: 0.98
                duration: DesignTokens.motionDuration(90, root.motionEnabled)
            }
        }
    }

    background: Rectangle {
        radius: DesignTokens.radiusCard
        color: DesignTokens.surfaceLow(root.dark)
        border.width: 1
        border.color: DesignTokens.outline(root.dark)
        layer.enabled: true
    }

    contentItem: ColumnLayout {
        Accessible.name: qsTr("Contextual history for %1").arg(root.contextTitle)
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: DesignTokens.spacing16
            Layout.rightMargin: DesignTokens.spacing8
            Layout.topMargin: DesignTokens.spacing12
            Layout.bottomMargin: DesignTokens.spacing8
            spacing: DesignTokens.spacing8

            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: DesignTokens.radiusControl
                color: DesignTokens.primaryContainer(root.dark)
                Label {
                    anchors.centerIn: parent
                    text: "↶"
                    color: DesignTokens.onPrimaryContainer(root.dark)
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 20
                    Accessible.ignored: true
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    Layout.fillWidth: true
                    text: qsTr("History here")
                    color: DesignTokens.onSurface(root.dark)
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: root.contextTitle + "  ·  " + root.branchName
                    font.family: DesignTokens.fontMono
                    font.pixelSize: 11
                    color: root.secondaryTextColor
                    elide: Text.ElideMiddle
                }
            }
            WfIconButton {
                glyph: "×"
                accessibleName: qsTr("Close contextual history")
                toolTip: accessibleName
                dark: root.dark
                motionEnabled: root.motionEnabled
                onClicked: root.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: DesignTokens.outlineVariant(root.dark)
        }

        ListView {
            id: actionList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.events
            spacing: DesignTokens.spacing8
            topMargin: DesignTokens.spacing8
            bottomMargin: DesignTokens.spacing8
            leftMargin: DesignTokens.spacing8
            rightMargin: DesignTokens.spacing8

            delegate: WfCard {
                id: actionCard
                required property var modelData
                width: actionList.width - actionList.leftMargin - actionList.rightMargin
                padding: DesignTokens.spacing12
                dark: root.dark
                radius: DesignTokens.radiusCard
                surfaceLevel: effective ? "container" : "low"
                outlined: modelData.destructive
                outlineColor: root.errorColor

                readonly property bool toggleable: modelData.type === "action"
                                                   || modelData.type === "compensation"
                readonly property bool effective: modelData.effective === undefined
                                                  ? true : modelData.effective
                readonly property string stateLabel: effective ? qsTr("Applied") : qsTr("Undone")
                readonly property string eventIcon: {
                    if (modelData.icon === "undo") return "↶"
                    if (modelData.icon === "redo") return "↷"
                    if (modelData.icon === "bookmark") return "★"
                    if (modelData.icon === "fork_right") return "⑂"
                    if (modelData.destructive) return "⚠"
                    return "✦"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: actionCard.eventIcon
                            color: actionCard.modelData.destructive
                                   ? root.errorColor : DesignTokens.primary(root.dark)
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 18
                            Accessible.ignored: true
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                Layout.fillWidth: true
                                text: actionCard.modelData.title || qsTr("Recorded action")
                                color: DesignTokens.onSurface(root.dark)
                                font.family: DesignTokens.fontBody
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                opacity: actionCard.effective ? 1 : 0.62
                            }
                            Label {
                                Layout.fillWidth: true
                                text: actionCard.modelData.diffSummary || actionCard.modelData.description || qsTr("No diff summary")
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                color: root.secondaryTextColor
                                elide: Text.ElideRight
                            }
                        }
                        Label {
                            text: "#" + (actionCard.modelData.sequence || "")
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 10
                            color: root.secondaryTextColor
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        WfStatusChip {
                            text: actionCard.stateLabel
                            tone: actionCard.effective ? "success" : "neutral"
                            dark: root.dark
                        }
                        WfStatusChip {
                            visible: actionCard.modelData.destructive
                            text: qsTr("Destructive")
                            tone: "error"
                            dark: root.dark
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        WfButton {
                            visible: actionCard.toggleable && actionCard.effective
                            glyph: "↶"
                            text: qsTr("Undo")
                            variant: "text"
                            compact: true
                            dark: root.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.undoRequested(actionCard.modelData.id)
                        }
                        WfButton {
                            visible: actionCard.toggleable && !actionCard.effective
                            glyph: "↷"
                            text: qsTr("Redo")
                            variant: "text"
                            compact: true
                            dark: root.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.redoRequested(actionCard.modelData.id)
                        }
                        WfButton {
                            visible: actionCard.modelData.type === "action"
                            text: qsTr("Restore here")
                            variant: "tonal"
                            compact: true
                            dark: root.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.restoreRequested(actionCard.modelData.id)
                        }
                        Item { Layout.fillWidth: true }
                        WfIconButton {
                            glyph: "★"
                            accessibleName: qsTr("Bookmark this action")
                            toolTip: accessibleName
                            buttonSize: 34
                            dark: root.dark
                            motionEnabled: root.motionEnabled
                            onClicked: {
                                inlineEditor.mode = "bookmark"
                                inlineEditor.eventId = actionCard.modelData.id
                                inlineName.text = ""
                                inlineEditor.visible = true
                                inlineName.forceInputFocus()
                            }
                        }
                        WfIconButton {
                            glyph: "⑂"
                            accessibleName: qsTr("Branch from this action")
                            toolTip: accessibleName
                            buttonSize: 34
                            dark: root.dark
                            motionEnabled: root.motionEnabled
                            onClicked: {
                                inlineEditor.mode = "branch"
                                inlineEditor.eventId = actionCard.modelData.id
                                inlineName.text = ""
                                inlineEditor.visible = true
                                inlineName.forceInputFocus()
                            }
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
                color: root.secondaryTextColor
                font.family: DesignTokens.fontBody
            }
        }

        WfCard {
            id: inlineEditor
            property string mode: "bookmark"
            property string eventId: ""
            visible: false
            Layout.fillWidth: true
            padding: DesignTokens.spacing8
            dark: root.dark
            radius: DesignTokens.radiusControl
            fillColor: DesignTokens.primaryContainer(root.dark)
            outlineColor: DesignTokens.primary(root.dark)

            RowLayout {
                anchors.fill: parent
                Label { text: inlineEditor.mode === "branch" ? "⑂" : "★"; Accessible.ignored: true }
                WfField {
                    id: inlineName
                    Layout.fillWidth: true
                    placeholderText: inlineEditor.mode === "branch"
                                     ? qsTr("New branch name") : qsTr("Bookmark name")
                    maximumLength: 80
                    onAccepted: saveInlineButton.clicked()
                    dark: root.dark
                    motionEnabled: root.motionEnabled
                }
                WfButton {
                    id: saveInlineButton
                    text: qsTr("Save")
                    variant: "filled"
                    compact: true
                    dark: root.dark
                    motionEnabled: root.motionEnabled
                    enabled: inlineName.text.trim().length > 0
                    onClicked: {
                        if (inlineEditor.mode === "branch")
                            root.branchRequested(inlineEditor.eventId, inlineName.text.trim())
                        else
                            root.bookmarkRequested(inlineEditor.eventId, inlineName.text.trim())
                        inlineEditor.visible = false
                    }
                }
                WfIconButton {
                    glyph: "×"
                    accessibleName: qsTr("Cancel naming")
                    toolTip: accessibleName
                    buttonSize: 34
                    dark: root.dark
                    motionEnabled: root.motionEnabled
                    onClicked: inlineEditor.visible = false
                }
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
            font.family: DesignTokens.fontBody
            font.pixelSize: 10
            color: root.secondaryTextColor
        }
    }
}
