pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

WfCard {
    id: root
    property bool opened: false
    property var entries: []
    property int unreadCount: 0
    property bool motionEnabled: true
    readonly property color secondaryTextColor: DesignTokens.onSurfaceVariant(dark)
    readonly property color errorColor: DesignTokens.error(dark)
    readonly property color warningColor: DesignTokens.tertiary(dark)
    readonly property color successColor: DesignTokens.success(dark)
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
    radius: DesignTokens.radiusPill
    surfaceLevel: "low"
    outlineColor: DesignTokens.outline(dark)
    Accessible.name: unreadCount > 0
                     ? qsTr("Notification center, %1 unread").arg(unreadCount)
                     : qsTr("Notification center, no unread notifications")

    Behavior on x {
        NumberAnimation {
            duration: DesignTokens.motionDuration(DesignTokens.motionMedium,
                                                  root.motionEnabled)
            easing.type: Easing.OutCubic
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: DesignTokens.spacing16
            Label {
                text: qsTr("Notification center")
                color: DesignTokens.onSurface(root.dark)
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            WfStatusChip {
                visible: root.unreadCount > 0
                text: String(root.unreadCount)
                tone: "primary"
                uppercase: false
                dark: root.dark
            }
            Item { Layout.fillWidth: true }
            WfIconButton {
                glyph: "↶"
                accessibleName: qsTr("Undo latest notification action")
                toolTip: accessibleName
                dark: root.dark
                motionEnabled: root.motionEnabled
                onClicked: root.undoRequested()
            }
            WfIconButton {
                glyph: "×"
                accessibleName: qsTr("Close notification center")
                toolTip: accessibleName
                dark: root.dark
                motionEnabled: root.motionEnabled
                onClicked: root.closeRequested()
            }
        }

        Label {
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.bottomMargin: 12
            Layout.fillWidth: true
            text: qsTr("Every read, dismiss, restore and delete is committed to a separate local Git repository.")
            wrapMode: Text.Wrap
            color: root.secondaryTextColor
            font.family: DesignTokens.fontBody
            font.pixelSize: 12
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: DesignTokens.outlineVariant(root.dark)
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: DesignTokens.spacing8
            topMargin: DesignTokens.spacing12
            bottomMargin: DesignTokens.spacing12
            leftMargin: DesignTokens.spacing12
            rightMargin: DesignTokens.spacing12
            model: root.entries

            delegate: WfCard {
                id: notificationCard
                required property var modelData
                width: list.width - list.leftMargin - list.rightMargin
                padding: DesignTokens.spacing12
                opacity: modelData.dismissed ? 0.58 : 1
                dark: root.dark
                radius: DesignTokens.radiusCard
                surfaceLevel: modelData.read ? "container" : "high"
                fillColor: modelData.read
                           ? DesignTokens.surfaceContainer(dark)
                           : DesignTokens.primaryContainer(dark)
                outlined: !modelData.read
                outlineColor: DesignTokens.primary(dark)
                readonly property string severityLabel: modelData.kind === "error" ? qsTr("Error")
                                                       : modelData.kind === "warning" ? qsTr("Warning")
                                                       : modelData.kind === "success" ? qsTr("Success")
                                                       : qsTr("Information")
                readonly property string stateLabel: modelData.deleted ? qsTr("Deleted")
                                                    : modelData.dismissed ? qsTr("Dismissed")
                                                    : modelData.read ? qsTr("Read") : qsTr("Unread")
                readonly property color severityColor: modelData.kind === "error" ? root.errorColor
                                                       : modelData.kind === "warning" ? root.warningColor
                                                       : modelData.kind === "success" ? root.successColor
                                                       : DesignTokens.secondary(root.dark)
                Accessible.name: severityLabel + ", " + stateLabel + ": " + modelData.title + ". " + modelData.message

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle {
                            Layout.preferredWidth: 10
                            Layout.preferredHeight: 10
                            radius: 5
                            color: notificationCard.severityColor
                            Accessible.ignored: true
                        }
                        Label {
                            text: modelData.title
                            color: DesignTokens.onSurface(notificationCard.dark)
                            font.family: DesignTokens.fontBody
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: modelData.timestamp
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 11
                            color: root.secondaryTextColor
                        }
                    }
                    Label {
                        text: modelData.message
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        color: DesignTokens.onSurfaceVariant(notificationCard.dark)
                        font.family: DesignTokens.fontBody
                    }
                    WfStatusChip {
                        text: notificationCard.severityLabel + "  ·  " + notificationCard.stateLabel
                        tone: modelData.kind === "warning" ? "warning"
                              : modelData.kind === "success" ? "success"
                              : modelData.kind === "error" ? "error" : "info"
                        uppercase: false
                        dark: notificationCard.dark
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        WfButton {
                            visible: !modelData.read
                            text: qsTr("Mark read")
                            variant: "text"
                            compact: true
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.markReadRequested(modelData.id)
                        }
                        WfButton {
                            visible: modelData.read && !modelData.deleted
                            text: qsTr("Mark unread")
                            variant: "text"
                            compact: true
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.markUnreadRequested(modelData.id)
                        }
                        WfButton {
                            visible: !modelData.dismissed
                            text: qsTr("Dismiss")
                            variant: "text"
                            compact: true
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.dismissRequested(modelData.id)
                        }
                        WfButton {
                            visible: modelData.dismissed || modelData.deleted
                            text: qsTr("Restore")
                            variant: "tonal"
                            compact: true
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.restoreRequested(modelData.id)
                        }
                        Item { Layout.fillWidth: true }
                        WfIconButton {
                            visible: !modelData.deleted
                            glyph: "⌫"
                            variant: "destructive"
                            accessibleName: qsTr("Soft-delete notification; recoverable in Git")
                            toolTip: accessibleName
                            buttonSize: 34
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.deleteRequested(modelData.id)
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: qsTr("No notifications yet")
                color: root.secondaryTextColor
                font.family: DesignTokens.fontBody
            }
        }
    }
}
