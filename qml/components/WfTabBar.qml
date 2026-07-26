import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Deterministic segmented tab strip. The Material TabBar positions its shared
// moving underline before late layout passes settle, leaving the indicator
// under the wrong tab in loaded-then-resized pages — so each segment here
// draws its own underline instead.
Item {
    id: root

    property bool dark: Material.theme === Material.Dark
    property int currentIndex: 0
    // Display strings, already translated by the caller.
    property var model: []
    property bool showBaseline: true

    implicitHeight: DesignTokens.rowHeight
    Accessible.role: Accessible.PageTabList

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        visible: root.showBaseline
        color: DesignTokens.outlineVariant(root.dark)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Repeater {
            model: root.model
            delegate: AbstractButton {
                id: segment
                required property string modelData
                required property int index
                readonly property bool selected: root.currentIndex === index
                Layout.fillWidth: true
                Layout.fillHeight: true
                focusPolicy: Qt.StrongFocus
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData
                Accessible.checkable: true
                Accessible.checked: selected
                onClicked: root.currentIndex = index

                background: Item {
                    Rectangle {
                        anchors.fill: parent
                        radius: 1 DesignTokens.radiusSmall
                        color: segment.hovered && !segment.selected
                               ? DesignTokens.surfaceContainer(root.dark) : "transparent"
                        border.width: segment.visualFocus ? 2 : 0
                        border.color: segment.visualFocus
                                      ? DesignTokens.primary(root.dark) : "transparent"
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        height: 2
                        color: segment.selected
                               ? DesignTokens.primary(root.dark) : "transparent"
                    }
                }
                contentItem: Label {
                    text: segment.modelData
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    font.weight: segment.selected ? Font.DemiBold : Font.Medium
                    color: segment.selected
                           ? DesignTokens.primary(root.dark)
                           : DesignTokens.onSurfaceVariant(root.dark)
                }
            }
        }
    }
}
