import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Control {
    id: root

    property bool dark: Material.theme === Material.Dark
    property string label: ""
    property alias text: input.text
    // Rendered by an inline overlay below instead of TextField.placeholderText:
    // the Material style floats a non-empty field's placeholder onto the top
    // border, striking through it, and WfField already draws its own label.
    property string placeholderText: ""
    property alias readOnly: input.readOnly
    property alias validator: input.validator
    property alias echoMode: input.echoMode
    property alias maximumLength: input.maximumLength
    property alias inputMethodHints: input.inputMethodHints
    property alias selectByMouse: input.selectByMouse
    property string helperText: ""
    property string errorText: ""
    property string accessibleDescription: hasError ? errorText : helperText
    property bool hasError: errorText.length > 0
    property bool mono: false
    property bool motionEnabled: true

    signal accepted()
    signal textEdited(string text)
    signal editingFinished()

    function selectAll() { input.selectAll() }
    function forceInputFocus() { input.forceActiveFocus(Qt.TabFocusReason) }

    implicitWidth: 240
    implicitHeight: fieldLayout.implicitHeight
    padding: 0
    focusPolicy: Qt.NoFocus
    Accessible.name: label.length > 0 ? label : placeholderText
    Accessible.description: accessibleDescription

    contentItem: ColumnLayout {
        id: fieldLayout
        spacing: DesignTokens.spacing4

        Label {
            visible: root.label.length > 0
            Layout.fillWidth: true
            text: root.label
            color: root.hasError ? DesignTokens.error(root.dark)
                                 : DesignTokens.onSurface(root.dark)
            font.family: DesignTokens.fontBody
            font.pixelSize: 12
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        TextField {
            id: input
            Layout.fillWidth: true
            Layout.preferredHeight: DesignTokens.fieldHeight
            leftPadding: DesignTokens.spacing12
            rightPadding: DesignTokens.spacing12
            topPadding: 0
            bottomPadding: 0
            selectByMouse: true
            color: DesignTokens.onSurface(root.dark)
            placeholderTextColor: DesignTokens.onSurfaceVariant(root.dark)
            selectionColor: DesignTokens.primaryContainer(root.dark)
            selectedTextColor: DesignTokens.onPrimaryContainer(root.dark)
            font.family: root.mono ? DesignTokens.fontMono : DesignTokens.fontBody
            font.pixelSize: 13
            Accessible.name: root.Accessible.name
            Accessible.description: root.accessibleDescription
            onAccepted: root.accepted()
            onTextEdited: root.textEdited(text)
            onEditingFinished: root.editingFinished()
            onTextChanged: {
                if (!activeFocus)
                    cursorPosition = 0
            }
            onActiveFocusChanged: {
                if (!activeFocus)
                    cursorPosition = 0
            }
            Component.onCompleted: cursorPosition = 0

            background: Rectangle {
                radius: DesignTokens.radiusControl
                color: DesignTokens.surfaceLowest(root.dark)
                border.width: input.activeFocus ? 2 : 1
                border.color: root.hasError ? DesignTokens.error(root.dark)
                                                 : input.activeFocus
                                                   ? DesignTokens.primary(root.dark)
                                                   : DesignTokens.outline(root.dark)

                Behavior on border.color {
                    ColorAnimation {
                        duration: DesignTokens.motionDuration(DesignTokens.motionShort,
                                                              root.motionEnabled)
                    }
                }
            }

            Label {
                anchors.fill: parent
                leftPadding: input.leftPadding
                rightPadding: input.rightPadding
                visible: root.placeholderText.length > 0 && input.displayText.length === 0
                text: root.placeholderText
                color: DesignTokens.onSurfaceVariant(root.dark)
                font: input.font
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                Accessible.ignored: true
            }
        }

        Label {
            visible: root.hasError || root.helperText.length > 0
            Layout.fillWidth: true
            text: root.hasError ? root.errorText : root.helperText
            color: root.hasError ? DesignTokens.error(root.dark)
                                 : DesignTokens.onSurfaceVariant(root.dark)
            font.family: DesignTokens.fontBody
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}
