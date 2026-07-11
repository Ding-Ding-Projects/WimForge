import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

Control {
    id: root

    property bool dark: Material.theme === Material.Dark
    property string text: ""
    property string tone: "neutral"
    property bool showDot: false
    property bool uppercase: true
    property bool compact: true

    implicitHeight: compact ? 24 : 28
    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    leftPadding: compact ? 9 : 11
    rightPadding: leftPadding
    topPadding: 0
    bottomPadding: 0
    focusPolicy: Qt.NoFocus
    Accessible.name: text

    contentItem: Row {
        spacing: root.showDot ? DesignTokens.spacing4 : 0

        Rectangle {
            visible: root.showDot
            width: 6
            height: 6
            radius: 3
            anchors.verticalCenter: parent.verticalCenter
            color: DesignTokens.toneStrong(root.tone, root.dark)
            Accessible.ignored: true
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: root.uppercase ? root.text.toUpperCase() : root.text
            color: DesignTokens.toneForeground(root.tone, root.dark)
            font.family: DesignTokens.fontBody
            font.pixelSize: root.compact ? 10 : 11
            font.weight: Font.Bold
            font.letterSpacing: root.uppercase ? 0.7 : 0
        }
    }

    background: Rectangle {
        radius: DesignTokens.radiusPill
        color: DesignTokens.toneContainer(root.tone, root.dark)
    }
}
