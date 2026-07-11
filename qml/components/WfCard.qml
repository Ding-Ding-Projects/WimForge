import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

Pane {
    id: root

    property bool dark: Material.theme === Material.Dark
    property string surfaceLevel: "lowest"
    property bool outlined: true
    property real radius: DesignTokens.radiusCard
    property bool motionEnabled: true
    property color fillColor: DesignTokens.surfaceForLevel(surfaceLevel, dark)
    property color outlineColor: DesignTokens.outlineVariant(dark)

    padding: DesignTokens.spacing16
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: root.radius
        color: root.fillColor
        border.width: root.outlined ? 1 : 0
        border.color: root.outlineColor

        Behavior on color {
            ColorAnimation {
                duration: DesignTokens.motionDuration(DesignTokens.motionShort,
                                                      root.motionEnabled)
            }
        }
    }
}
