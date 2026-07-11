import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

WfCard {
    id: root
    property string eyebrow: ""
    property string value: ""
    property string detail: ""
    property color accent: Material.accent
    property string glyph: "●"
    readonly property color effectiveAccent: dark ? Qt.lighter(accent, 1.55) : accent

    implicitWidth: 230
    implicitHeight: 140
    padding: DesignTokens.spacing16
    surfaceLevel: "lowest"
    radius: DesignTokens.radiusCard
    Accessible.name: eyebrow + ": " + value + ". " + detail

    ColumnLayout {
        anchors.fill: parent
        spacing: DesignTokens.spacing4

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: root.eyebrow
                color: DesignTokens.onSurfaceVariant(root.dark)
                font.family: DesignTokens.fontBody
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
                font.capitalization: Font.AllUppercase
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                radius: DesignTokens.radiusControl
                color: Qt.rgba(root.effectiveAccent.r, root.effectiveAccent.g, root.effectiveAccent.b, 0.16)
                Label {
                    anchors.centerIn: parent
                    text: root.glyph
                    color: root.effectiveAccent
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 14
                    Accessible.ignored: true
                }
            }
        }
        Label {
            text: root.value
            color: DesignTokens.onSurface(root.dark)
            font.family: DesignTokens.fontDisplay
            font.pixelSize: 28
            font.weight: Font.Bold
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            text: root.detail
            color: DesignTokens.onSurfaceVariant(root.dark)
            font.family: DesignTokens.fontBody
            font.pixelSize: 12
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }
}
