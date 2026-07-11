import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

AbstractButton {
    id: root

    property bool dark: Material.theme === Material.Dark
    property string glyph: ""
    property string accessibleName: ""
    property string toolTip: ""
    property string variant: "standard"
    property int buttonSize: DesignTokens.controlHeight
    property bool motionEnabled: true

    readonly property color baseBackground: variant === "tonal"
                                                 ? DesignTokens.primaryContainer(dark)
                                                 : variant === "destructive"
                                                   ? DesignTokens.errorContainer(dark)
                                                   : "transparent"
    readonly property color foregroundColor: variant === "tonal"
                                                 ? DesignTokens.onPrimaryContainer(dark)
                                                 : variant === "destructive"
                                                   ? DesignTokens.onErrorContainer(dark)
                                                   : DesignTokens.onSurfaceVariant(dark)
    readonly property color stateBackground: pressed
                                                ? DesignTokens.surfaceHighest(dark)
                                                : hovered
                                                  ? DesignTokens.surfaceHigh(dark)
                                                  : baseBackground

    implicitWidth: buttonSize
    implicitHeight: buttonSize
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1 : 0.45
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName.length > 0 ? accessibleName : toolTip

    contentItem: Label {
        text: root.glyph
        color: root.foregroundColor
        font.family: DesignTokens.fontBody
        font.pixelSize: 17
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        Accessible.ignored: true
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: root.stateBackground

            Behavior on color {
                ColorAnimation {
                    duration: DesignTokens.motionDuration(DesignTokens.motionShort,
                                                          root.motionEnabled)
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: DesignTokens.primary(root.dark)
            visible: root.visualFocus
        }
    }

    ToolTip.visible: hovered && toolTip.length > 0
    ToolTip.text: toolTip
    ToolTip.delay: 500
}
