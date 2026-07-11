import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Fully in-app Microsoft Update Catalog: search, browse results and download
// updates straight into the project's update queue without ever launching an
// external web browser.
Popup {
    id: sheet

    required property var app
    required property var tr
    required property bool dark

    // Which payload queue downloads are filed into ("updates" or "drivers").
    property string category: "updates"
    property string initialQuery: ""

    // Show the sheet with the query prefilled. The caller runs the search so
    // the entry point stays where the design-system contract expects it.
    function searchFor(query) {
        catalogQuery.text = query
        open()
    }

    Material.theme: dark ? Material.Dark : Material.Light
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(860, (parent ? parent.width : 900) - 48)
    height: Math.min(620, (parent ? parent.height : 700) - 48)
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 20
    onOpened: catalogQuery.forceActiveFocus()
    onClosed: sheet.app.cancelUpdateCatalog()

    background: Rectangle {
        radius: DesignTokens.radiusCard
        color: DesignTokens.surfaceLowest(sheet.dark)
        border.color: DesignTokens.outlineVariant(sheet.dark)
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: DesignTokens.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spacing8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: sheet.tr("Microsoft Update Catalog", "Microsoft Update Catalog")
                    color: DesignTokens.onSurface(sheet.dark)
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 20
                    font.weight: Font.Bold
                }
                Label {
                    text: sheet.tr("Search, download and queue updates without leaving WimForge.",
                                   "唔使離開 WimForge，就可以搜尋、下載同排入更新。")
                    color: DesignTokens.onSurfaceVariant(sheet.dark)
                    font.pixelSize: 11
                }
            }
            WfIconButton {
                glyph: "×"
                buttonSize: 32
                accessibleName: sheet.tr("Close", "關閉")
                toolTip: accessibleName
                onClicked: sheet.close()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spacing8
            TextField {
                id: catalogQuery
                Layout.fillWidth: true
                placeholderText: sheet.tr("KB number or update name, e.g. 2024-07 Cumulative Windows 11",
                                          "KB 編號或更新名稱，例如 2024-07 Cumulative Windows 11")
                color: DesignTokens.onSurface(sheet.dark)
                onAccepted: if (text.trim().length > 0 && !sheet.app.updateCatalogBusy)
                                sheet.app.openMicrosoftUpdateCatalog(text.trim())
            }
            WfButton {
                dark: sheet.dark
                variant: "filled"
                text: sheet.tr("Search", "搜尋")
                enabled: catalogQuery.text.trim().length > 0 && !sheet.app.updateCatalogBusy
                onClicked: sheet.app.openMicrosoftUpdateCatalog(catalogQuery.text.trim())
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spacing8
            BusyIndicator {
                running: sheet.app.updateCatalogBusy
                visible: running
                implicitWidth: 22
                implicitHeight: 22
            }
            Label {
                Layout.fillWidth: true
                text: sheet.app.updateCatalogStatus
                color: DesignTokens.onSurfaceVariant(sheet.dark)
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: sheet.app.updateCatalogBusy && sheet.app.updateCatalogDownloadProgress > 0
            value: sheet.app.updateCatalogDownloadProgress
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLow(sheet.dark)
            border.color: DesignTokens.outlineVariant(sheet.dark)
            border.width: 1

            ListView {
                id: resultsView
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                spacing: 4
                boundsBehavior: Flickable.StopAtBounds
                model: sheet.app.updateCatalogResults
                ScrollBar.vertical: ScrollBar { }

                delegate: Rectangle {
                    id: entryDelegate
                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    implicitHeight: rowLayout.implicitHeight + 16
                    radius: DesignTokens.radiusControl
                    color: DesignTokens.surfaceLowest(sheet.dark)
                    border.color: DesignTokens.outlineVariant(sheet.dark)
                    border.width: 1

                    RowLayout {
                        id: rowLayout
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: DesignTokens.spacing8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: entryDelegate.modelData.title
                                // Catalog text is remote data; never let it render as markup.
                                textFormat: Text.PlainText
                                color: DesignTokens.onSurface(sheet.dark)
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: [entryDelegate.modelData.product,
                                       entryDelegate.modelData.classification,
                                       entryDelegate.modelData.lastUpdated,
                                       entryDelegate.modelData.sizeText].filter(function(part) {
                                    return part && part.length > 0 && part !== "n/a"
                                }).join("  ·  ")
                                textFormat: Text.PlainText
                                color: DesignTokens.onSurfaceVariant(sheet.dark)
                                font.pixelSize: 10
                                wrapMode: Text.Wrap
                            }
                        }

                        WfButton {
                            dark: sheet.dark
                            variant: "tonal"
                            compact: true
                            text: sheet.tr("Download", "下載")
                            enabled: !sheet.app.updateCatalogBusy
                            onClicked: sheet.app.downloadUpdateCatalogItem(
                                           entryDelegate.modelData.updateId,
                                           entryDelegate.modelData.title,
                                           sheet.category,
                                           entryDelegate.modelData.sizeBytes || 0)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: resultsView.count === 0 && !sheet.app.updateCatalogBusy
                    horizontalAlignment: Text.AlignHCenter
                    text: sheet.tr("Search the catalog to list updates. Downloads are added straight to the update queue.",
                                   "搜尋目錄嚟列出更新。下載會直接加入更新隊列。")
                    color: DesignTokens.onSurfaceVariant(sheet.dark)
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
