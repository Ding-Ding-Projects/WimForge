import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Popup {
    id: root
    required property var app
    property var tr: function(en, zh) { return en }

    parent: Overlay.overlay
    x: Math.max(16, Math.round((parent.width - width) / 2))
    y: Math.max(16, Math.min(90, Math.round((parent.height - height) / 3)))
    width: Math.min(820, parent.width - 32)
    height: Math.min(660, parent.height - 32)
    modal: true
    focus: true
    padding: DesignTokens.spacing16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function openForQuery(query) {
        queryField.text = query
        open()
        queryField.forceActiveFocus()
        queryField.selectAll()
    }

    onClosed: app.clearSearch()

    background: Rectangle {
        radius: DesignTokens.radiusCard
        color: DesignTokens.surfaceLow(Material.theme === Material.Dark)
        border.width: 1
        border.color: DesignTokens.outline(Material.theme === Material.Dark)
    }

    Timer {
        id: searchDelay
        interval: 160
        repeat: false
        onTriggered: app.search(queryField.text)
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spacing12

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "⌕"
                color: DesignTokens.primary(Material.theme === Material.Dark)
                font.family: DesignTokens.fontBody
                font.pixelSize: 24
                Accessible.ignored: true
            }
            TextField {
                id: queryField
                Layout.fillWidth: true
                Layout.preferredHeight: DesignTokens.fieldHeight
                placeholderText: root.tr("Search pages, commands, settings, features, packages, policies and project data",
                                         "搜尋頁面、指令、設定、功能、套件、政策同工程資料")
                color: DesignTokens.onSurface(Material.theme === Material.Dark)
                placeholderTextColor: DesignTokens.onSurfaceVariant(Material.theme === Material.Dark)
                selectionColor: DesignTokens.primaryContainer(Material.theme === Material.Dark)
                selectedTextColor: DesignTokens.onPrimaryContainer(Material.theme === Material.Dark)
                font.family: DesignTokens.fontBody
                font.pixelSize: 13
                Accessible.name: placeholderText
                onTextEdited: searchDelay.restart()
                onAccepted: {
                    if (app.searchResults.length > 0) {
                        app.activateSearchResult(app.searchResults[0])
                        root.close()
                    } else {
                        app.search(text)
                    }
                }
                Keys.onDownPressed: {
                    if (resultList.count > 0) {
                        resultList.currentIndex = 0
                        resultList.forceActiveFocus()
                    }
                }
                background: Rectangle {
                    radius: DesignTokens.radiusControl
                    color: DesignTokens.surfaceLowest(Material.theme === Material.Dark)
                    border.width: queryField.activeFocus ? 2 : 1
                    border.color: queryField.activeFocus
                                  ? DesignTokens.primary(Material.theme === Material.Dark)
                                  : DesignTokens.outline(Material.theme === Material.Dark)
                }
            }
            WfIconButton {
                glyph: "×"
                accessibleName: root.tr("Close search", "關閉搜尋")
                toolTip: accessibleName
                dark: Material.theme === Material.Dark
                motionEnabled: app.motionEnabled
                onClicked: root.close()
            }
        }

        Label {
            Layout.fillWidth: true
            text: app.searchResults.length === 0
                  ? root.tr("No matching local result", "搵唔到相符本機結果")
                  : root.tr("%1 ranked result(s)", "%1 項排序結果").arg(app.searchResults.length)
            color: DesignTokens.onSurfaceVariant(Material.theme === Material.Dark)
            font.family: DesignTokens.fontBody
            wrapMode: Text.Wrap
        }

        ListView {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: app.searchResults
            clip: true
            spacing: DesignTokens.spacing4
            keyNavigationEnabled: true
            highlightMoveDuration: DesignTokens.motionDuration(100, app.motionEnabled)
            highlight: Rectangle {
                radius: DesignTokens.radiusControl
                color: DesignTokens.primaryContainer(Material.theme === Material.Dark)
            }

            delegate: ItemDelegate {
                id: resultDelegate
                required property var modelData
                required property int index
                width: resultList.width
                implicitHeight: Math.max(64, resultRow.implicitHeight + 16)
                highlighted: ListView.isCurrentItem
                Accessible.name: modelData.kindLabel + ": " + modelData.title + ". " + modelData.subtitle
                onClicked: {
                    app.activateSearchResult(modelData)
                    root.close()
                }
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                Keys.onEscapePressed: root.close()
                contentItem: RowLayout {
                    id: resultRow
                    spacing: 12
                    Label {
                        text: resultDelegate.modelData.icon
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 20
                        color: DesignTokens.primary(Material.theme === Material.Dark)
                        Accessible.ignored: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: resultDelegate.modelData.title
                            color: DesignTokens.onSurface(Material.theme === Material.Dark)
                            font.family: DesignTokens.fontBody
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: resultDelegate.modelData.subtitle
                            color: DesignTokens.onSurfaceVariant(Material.theme === Material.Dark)
                            font.family: DesignTokens.fontBody
                            wrapMode: Text.WrapAnywhere
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }
                    WfStatusChip {
                        text: resultDelegate.modelData.kindLabel
                        tone: "primary"
                        dark: Material.theme === Material.Dark
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.tr("↑/↓ move · Enter open · Esc close", "↑/↓ 移動 · Enter 開啟 · Esc 關閉")
            horizontalAlignment: Text.AlignRight
            font.family: DesignTokens.fontMono
            font.pixelSize: 10
            color: DesignTokens.onSurfaceVariant(Material.theme === Material.Dark)
        }
    }
}
