import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

AbstractButton {
    id: root

    property bool dark: Material.theme === Material.Dark
    property string variant: "outlined"
    property string glyph: ""
    property bool compact: false
    property bool motionEnabled: true
    property int maximumLabelWidth: 360

    readonly property color baseBackground: {
        if (variant === "filled") return DesignTokens.primary(dark)
        if (variant === "tonal") return DesignTokens.primaryContainer(dark)
        if (variant === "destructive") return DesignTokens.errorContainer(dark)
        return "transparent"
    }
    readonly property color foregroundColor: {
        if (variant === "filled") return DesignTokens.onPrimary(dark)
        if (variant === "tonal") return DesignTokens.onPrimaryContainer(dark)
        if (variant === "destructive") return DesignTokens.onErrorContainer(dark)
        if (variant === "text") return DesignTokens.primary(dark)
        return DesignTokens.onSurface(dark)
    }
    readonly property color stateBackground: {
        if (pressed) return variant === "text" || variant === "outlined"
                            ? DesignTokens.surfaceHighest(dark)
                            : Qt.darker(baseBackground, 1.10)
        if (hovered) return variant === "text" || variant === "outlined"
                            ? DesignTokens.surfaceHigh(dark)
                            : Qt.lighter(baseBackground, dark ? 1.10 : 1.04)
        return baseBackground
    }

    implicitHeight: compact ? 34 : DesignTokens.controlHeight
    implicitWidth: Math.max(compact ? 64 : 76,
                            contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: compact ? DesignTokens.spacing12 : DesignTokens.spacing16
    rightPadding: leftPadding
    topPadding: 0
    bottomPadding: 0
    spacing: DesignTokens.spacing8
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1 : 0.45
    font.family: DesignTokens.fontBody
    font.pixelSize: 13
    font.weight: Font.DemiBold
    Accessible.role: Accessible.Button
    Accessible.name: text

    // The control resizes its contentItem to the full available area, and a
    // plain Row top-aligns children — so wrap the Row in an Item and center
    // it explicitly to keep the label optically centered in the pill.
    contentItem: Item {
        implicitWidth: buttonRow.implicitWidth
        implicitHeight: buttonRow.implicitHeight

        Row {
            id: buttonRow
            anchors.centerIn: parent
            spacing: root.spacing

            Label {
                visible: root.glyph.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: root.glyph
                color: root.foregroundColor
                font.family: DesignTokens.fontBody
                font.pixelSize: 15
                Accessible.ignored: true
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: root.text
                color: root.foregroundColor
                font: root.font
                width: Math.min(implicitWidth, root.maximumLabelWidth)
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: DesignTokens.radiusPill
            color: root.stateBackground
            border.width: root.variant === "outlined" ? 1 : 0
            border.color: DesignTokens.outline(root.dark)

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
            radius: DesignTokens.radiusPill + 3
            color: "transparent"
            border.width: 2
            border.color: DesignTokens.primary(root.dark)
            visible: root.visualFocus
        }
    }
}
