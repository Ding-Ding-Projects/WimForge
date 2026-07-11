import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: root
    property string message: ""
    property string tone: "info"
    property string actionText: ""
    property bool motionEnabled: true
    readonly property bool darkTheme: Material.theme === Material.Dark
    readonly property string toneLabel: tone === "error" ? qsTr("Error")
                                                : tone === "warning" ? qsTr("Warning")
                                                : tone === "success" ? qsTr("Success")
                                                : qsTr("Information")
    readonly property color toneBackground: DesignTokens.toneContainer(tone, darkTheme)
    readonly property color toneForeground: DesignTokens.toneForeground(tone, darkTheme)
    readonly property color toneBorder: DesignTokens.toneStrong(tone, darkTheme)
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
    implicitHeight: 60
    z: 1000
    Accessible.name: toneLabel + ": " + message

    Rectangle {
        anchors.fill: parent
        radius: DesignTokens.radiusCard
        color: root.toneBackground
        border.color: root.toneBorder

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: DesignTokens.spacing16
            anchors.rightMargin: DesignTokens.spacing8
            spacing: DesignTokens.spacing12
            Label {
                text: root.toneLabel
                color: root.toneForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                font.weight: Font.Bold
            }
            Label {
                text: root.message
                color: root.toneForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            WfButton {
                visible: root.actionText.length > 0
                text: root.actionText
                variant: "text"
                compact: true
                dark: root.darkTheme
                motionEnabled: root.motionEnabled
                Accessible.name: root.actionText
                onClicked: { root.actionTriggered(); root.visible = false }
            }
            WfIconButton {
                glyph: "×"
                accessibleName: qsTr("Dismiss notification")
                toolTip: accessibleName
                buttonSize: 36
                dark: root.darkTheme
                motionEnabled: root.motionEnabled
                onClicked: root.visible = false
            }
        }
    }

    Timer {
        id: hideTimer
        interval: 5200
        onTriggered: root.visible = false
    }

    Behavior on opacity {
        NumberAnimation {
            duration: DesignTokens.motionDuration(180, root.motionEnabled)
        }
    }
}
