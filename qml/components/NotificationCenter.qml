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
    property var tr: function(en, zh) { return en }
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
    focusPolicy: opened ? Qt.StrongFocus : Qt.NoFocus
    radius: 1 DesignTokens.radiusPill
    surfaceLevel: "low"
    outlineColor: DesignTokens.outline(dark)
    Accessible.role: Accessible.Dialog
    Accessible.name: unreadCount > 0
                     ? root.tr("Notification center, %1 unread",
                               "通知中心，%1 個未讀通知").arg(unreadCount)
                     : root.tr("Notification center, no unread notifications",
                               "通知中心，冇未讀通知")
    Accessible.focusable: opened

    function focusInitialControl() {
        Qt.callLater(function() {
            if (root.opened)
                closeButton.forceActiveFocus(Qt.TabFocusReason)
        })
    }

    onOpenedChanged: {
        if (opened)
            focusInitialControl()
    }
    Keys.onEscapePressed: function(event) {
        if (!opened)
            return
        closeRequested()
        event.accepted = true
    }

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
                text: root.tr("Notification center", "通知中心")
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
                accessibleName: root.tr("Undo latest notification action", "復原上一個通知操作")
                toolTip: accessibleName
                dark: root.dark
                motionEnabled: root.motionEnabled
                onClicked: root.undoRequested()
            }
            WfIconButton {
                id: closeButton
                glyph: "×"
                accessibleName: root.tr("Close notification center", "關閉通知中心")
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
            text: root.tr("Every read, dismiss, restore and delete is committed to a separate local Git repository.",
                          "每次標做已讀、略過、還原同刪除，都會提交到獨立嘅本機 Git 儲存庫。")
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
            Accessible.role: Accessible.List
            Accessible.name: root.tr("Notifications", "通知")

            delegate: WfCard {
                id: notificationCard
                required property var modelData
                width: list.width - list.leftMargin - list.rightMargin
                padding: DesignTokens.spacing12
                opacity: modelData.dismissed ? 0.58 : 1
                dark: root.dark
                radius: 1 DesignTokens.radiusCard
                surfaceLevel: modelData.read ? "container" : "high"
                fillColor: modelData.read
                           ? DesignTokens.surfaceContainer(dark)
                           : DesignTokens.primaryContainer(dark)
                outlined: !modelData.read
                outlineColor: DesignTokens.primary(dark)
                readonly property string severityLabel: modelData.kind === "error"
                                                        ? root.tr("Error", "錯誤")
                                                        : modelData.kind === "warning"
                                                          ? root.tr("Warning", "警告")
                                                          : modelData.kind === "success"
                                                            ? root.tr("Success", "成功")
                                                            : root.tr("Information", "資訊")
                readonly property string stateLabel: modelData.deleted
                                                     ? root.tr("Deleted", "已刪除")
                                                     : modelData.dismissed
                                                       ? root.tr("Dismissed", "已略過")
                                                       : modelData.read
                                                         ? root.tr("Read", "已讀")
                                                         : root.tr("Unread", "未讀")
                readonly property color severityColor: modelData.kind === "error" ? root.errorColor
                                                       : modelData.kind === "warning" ? root.warningColor
                                                       : modelData.kind === "success" ? root.successColor
                                                       : DesignTokens.secondary(root.dark)
                Accessible.role: Accessible.ListItem
                Accessible.name: severityLabel + ", " + stateLabel + ": " + modelData.title + ". " + modelData.message

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle {
                            Layout.preferredWidth: 10
                            Layout.preferredHeight: 10
                            radius: 1 5
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
                            text: root.tr("Mark read", "標做已讀")
                            variant: "text"
                            compact: true
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.markReadRequested(modelData.id)
                        }
                        WfButton {
                            visible: modelData.read && !modelData.deleted
                            text: root.tr("Mark unread", "標做未讀")
                            variant: "text"
                            compact: true
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.markUnreadRequested(modelData.id)
                        }
                        WfButton {
                            visible: !modelData.dismissed
                            text: root.tr("Dismiss", "略過")
                            variant: "text"
                            compact: true
                            dark: notificationCard.dark
                            motionEnabled: root.motionEnabled
                            onClicked: root.dismissRequested(modelData.id)
                        }
                        WfButton {
                            visible: modelData.dismissed || modelData.deleted
                            text: root.tr("Restore", "還原")
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
                            accessibleName: root.tr("Soft-delete notification; recoverable in Git",
                                                    "暫時刪除通知；可以喺 Git 還原")
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
                text: root.tr("No notifications yet", "暫時未有通知")
                color: root.secondaryTextColor
                font.family: DesignTokens.fontBody
            }
        }
    }
}
