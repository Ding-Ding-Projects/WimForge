import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: root

    default property alias actions: actionRow.data
    property bool dark: Material.theme === Material.Dark
    property string eyebrow: ""
    property string title: ""
    property string description: ""
    property bool compact: width < 680

    implicitWidth: headerLayout.implicitWidth
    implicitHeight: headerLayout.implicitHeight
    Accessible.name: title + (description.length > 0 ? ". " + description : "")

    GridLayout {
        id: headerLayout
        anchors.fill: parent
        columns: root.compact ? 1 : 2
        columnSpacing: DesignTokens.spacing20
        rowSpacing: DesignTokens.spacing12

        ColumnLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spacing4

            Label {
                visible: root.eyebrow.length > 0
                Layout.fillWidth: true
                text: root.eyebrow.toUpperCase()
                color: DesignTokens.onSurfaceVariant(root.dark)
                font.family: DesignTokens.fontBody
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: root.title
                color: DesignTokens.onSurface(root.dark)
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 30
                font.weight: Font.DemiBold
                font.letterSpacing: -0.4
                wrapMode: Text.Wrap
            }
            Label {
                visible: root.description.length > 0
                Layout.fillWidth: true
                text: root.description
                color: DesignTokens.onSurfaceVariant(root.dark)
                font.family: DesignTokens.fontBody
                font.pixelSize: 13
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            id: actionRow
            Layout.fillWidth: root.compact
            Layout.alignment: root.compact ? Qt.AlignLeft : Qt.AlignRight | Qt.AlignTop
            spacing: DesignTokens.spacing8
        }
    }
}
