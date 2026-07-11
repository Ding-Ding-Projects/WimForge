import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components"

ScrollView {
    id: root
    property var app
    property var tr: function(en, zh) { return en }
    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    function acceptSource(path) {
        if (!app.projectLoaded || String(path).trim().length === 0)
            return
        app.setProjectField("sourcePath", path)
        app.inspectSource()
    }

    FileDialog {
        id: sourceFileDialog
        title: root.tr("Choose a Windows ISO or image", "揀 Windows ISO 或映像")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            root.tr("Windows media and images (*.iso *.wim *.esd *.swm)", "Windows 媒體同映像 (*.iso *.wim *.esd *.swm)"),
            root.tr("All files (*)", "所有檔案 (*)")
        ]
        onAccepted: root.acceptSource(app.pathFromUrl(selectedFile))
    }
    FolderDialog {
        id: sourceFolderDialog
        title: root.tr("Choose an extracted Windows media folder", "揀已解壓嘅 Windows 安裝媒體資料夾")
        onAccepted: root.acceptSource(app.pathFromUrl(selectedFolder))
    }
    FileDialog {
        id: imageFileDialog
        title: root.tr("Choose install.wim, install.esd, or install.swm", "揀 install.wim、install.esd 或 install.swm")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            root.tr("Windows images (*.wim *.esd *.swm)", "Windows 映像 (*.wim *.esd *.swm)"),
            root.tr("All files (*)", "所有檔案 (*)")
        ]
        onAccepted: app.setProjectField("imagePath", app.pathFromUrl(selectedFile))
    }
    FolderDialog {
        id: mountFolderDialog
        title: root.tr("Choose an empty mount folder", "揀一個空白掛載資料夾")
        onAccepted: app.setProjectField("mountPath", app.pathFromUrl(selectedFolder))
    }
    FileDialog {
        id: outputFileDialog
        title: root.tr("Choose the output file", "揀輸出檔案")
        fileMode: FileDialog.SaveFile
        defaultSuffix: app.outputFormat.toLowerCase()
        nameFilters: app.outputFormat.toUpperCase() === "ISO"
                     ? [root.tr("ISO images (*.iso)", "ISO 映像 (*.iso)")]
                     : app.outputFormat.toUpperCase() === "ESD"
                       ? [root.tr("ESD images (*.esd)", "ESD 映像 (*.esd)")]
                       : app.outputFormat.toUpperCase() === "SWM"
                         ? [root.tr("Split WIM images (*.swm)", "分割 WIM 映像 (*.swm)")]
                         : [root.tr("WIM images (*.wim)", "WIM 映像 (*.wim)")]
        onAccepted: app.setProjectField("outputPath", app.pathFromUrl(selectedFile))
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 16

        WfPageHeader {
            Layout.fillWidth: true
            title: root.tr("Source & editions", "來源同版本")
            description: root.tr("Choose an ISO, image file, or extracted media folder. WimForge inventories it first; source media stays read-only and writes only happen in a reviewed job.",
                                 "揀 ISO、映像檔或者已解壓媒體資料夾。WimForge 會先點貨；來源保持唯讀，睇完計劃先至落筆。")
        }

        WfCard {
            Layout.fillWidth: true
            visible: !app.projectLoaded
            surfaceLevel: "container"
            outlined: false
            fillColor: DesignTokens.primaryContainer(root.dark)
            padding: 14

            RowLayout {
                width: parent.width
                spacing: 12
                Rectangle {
                    width: 30
                    height: 30
                    radius: DesignTokens.radiusControl
                    color: DesignTokens.primary(root.dark)
                    Label {
                        anchors.centerIn: parent
                        text: "1"
                        font.family: DesignTokens.fontDisplay
                        font.weight: Font.Bold
                        color: DesignTokens.onPrimary(root.dark)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Create or open a project first. Source choices are saved with Git history inside that project.",
                                  "請先建立或者開啟工程；來源選擇會連 Git 歷史一齊儲存。")
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    color: DesignTokens.onPrimaryContainer(root.dark)
                    wrapMode: Text.Wrap
                }
                WfButton {
                    text: root.tr("New project", "新工程")
                    variant: "tonal"
                    compact: true
                    onClicked: app.requestNewProject()
                }
                WfButton {
                    text: root.tr("Open project", "開工程")
                    compact: true
                    onClicked: app.requestOpenProject()
                }
            }
        }

        WfCard {
            id: sourceCard
            Layout.fillWidth: true
            Layout.preferredHeight: sourcePaneContent.implicitHeight + topPadding + bottomPadding
            padding: 18
            outlineColor: sourceDrop.containsDrag
                          ? DesignTokens.primary(root.dark)
                          : DesignTokens.outline(root.dark)
            fillColor: sourceDrop.containsDrag
                       ? DesignTokens.primaryContainer(root.dark)
                       : DesignTokens.surfaceLowest(root.dark)

            DropArea {
                id: sourceDrop
                anchors.fill: parent
                onDropped: function(drop) {
                    if (app.projectLoaded && drop.urls.length > 0)
                        root.acceptSource(app.pathFromUrl(drop.urls[0]))
                }
            }

            ColumnLayout {
                id: sourcePaneContent
                width: parent.width
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    Rectangle {
                        Layout.preferredWidth: 52
                        Layout.preferredHeight: 52
                        radius: DesignTokens.radiusCard
                        color: DesignTokens.primaryContainer(root.dark)
                        Label {
                            anchors.centerIn: parent
                            text: "◉"
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 24
                            color: DesignTokens.onPrimaryContainer(root.dark)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 180
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: root.tr("Windows media or image", "Windows 安裝碟或者映像")
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 15
                            font.weight: Font.Bold
                            color: DesignTokens.onSurface(root.dark)
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.tr("ISO, extracted media folder, WIM, ESD or SWM. Drag a file here, or browse.",
                                          "ISO、已解壓安裝資料夾、WIM、ESD 或 SWM。拖檔案到呢度，或者瀏覽。")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            color: DesignTokens.onSurfaceVariant(root.dark)
                            wrapMode: Text.Wrap
                        }
                    }

                    Flow {
                        Layout.preferredWidth: Math.min(520, implicitWidth)
                        Layout.maximumWidth: sourceCard.availableWidth
                        spacing: 8
                        WfButton {
                            text: root.tr("Browse ISO / image…", "瀏覽 ISO / 映像…")
                            glyph: "▱"
                            compact: true
                            enabled: app.projectLoaded && !app.busy
                            onClicked: sourceFileDialog.open()
                        }
                        WfButton {
                            text: root.tr("Browse media folder…", "瀏覽媒體資料夾…")
                            glyph: "▰"
                            compact: true
                            enabled: app.projectLoaded && !app.busy
                            onClicked: sourceFolderDialog.open()
                        }
                        WfButton {
                            text: app.busy ? root.tr("Inspecting…", "點貨中…") : root.tr("Inspect", "點貨")
                            variant: "filled"
                            compact: true
                            enabled: app.projectLoaded && !app.busy && app.sourcePath.length > 0
                            onClicked: app.inspectSource()
                        }
                    }
                }

                WfField {
                    Layout.fillWidth: true
                    text: app.sourcePath
                    placeholderText: app.sourcePath.length > 0 ? ""
                                     : root.tr("D:\\Windows11.iso or D:\\media\\sources\\install.wim", "例如 D:\\Windows11.iso")
                    mono: true
                    selectByMouse: true
                    readOnly: !app.projectLoaded || app.busy
                    onEditingFinished: app.setProjectField("sourcePath", text)
                }

                Label {
                    Layout.fillWidth: true
                    visible: app.sourceInspectionBusy || app.imageRelativePath.length > 0
                    text: app.sourceInspectionBusy
                          ? root.tr("For an ISO, WimForge mounts it read-only, inventories DISM metadata, then dismounts it automatically.",
                                    "ISO 會以唯讀方式掛載，讀完 DISM 資料之後自動卸載。")
                          : root.tr("Detected inside ISO: %1", "ISO 入面搵到：%1").arg(app.imageRelativePath)
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 11
                    color: DesignTokens.onSurfaceVariant(root.dark)
                    wrapMode: Text.Wrap
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.availableWidth >= 760 ? 2 : 1
            columnSpacing: 12
            rowSpacing: 12

            WfCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                enabled: app.projectLoaded && !app.busy
                padding: 18

                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    Label {
                        text: root.tr("Working copy", "工作副本")
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        color: DesignTokens.onSurface(root.dark)
                    }
                    Label {
                        text: root.tr("Image path", "映像路徑")
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: DesignTokens.onSurfaceVariant(root.dark)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        WfField {
                            Layout.fillWidth: true
                            text: app.imagePath
                            placeholderText: app.imagePath.length > 0 ? ""
                                             : app.imageRelativePath.length > 0
                                             ? root.tr("Detected inside ISO: %1", "ISO 入面搵到：%1").arg(app.imageRelativePath)
                                             : root.tr("Automatically detected install.wim / install.esd / install.swm", "自動偵測 install.wim / install.esd / install.swm")
                            mono: true
                            onEditingFinished: app.setProjectField("imagePath", text)
                        }
                        WfButton {
                            text: root.tr("Browse…", "瀏覽…")
                            compact: true
                            onClicked: imageFileDialog.open()
                        }
                    }
                    Label {
                        text: root.tr("Mount directory", "掛載資料夾")
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: DesignTokens.onSurfaceVariant(root.dark)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        WfField {
                            Layout.fillWidth: true
                            text: app.mountPath
                            placeholderText: app.mountPath.length > 0 ? ""
                                             : root.tr("Empty mount directory", "空白掛載資料夾")
                            mono: true
                            onEditingFinished: app.setProjectField("mountPath", text)
                        }
                        WfButton {
                            text: root.tr("Browse…", "瀏覽…")
                            compact: true
                            onClicked: mountFolderDialog.open()
                        }
                    }
                    CheckBox {
                        id: cloneSourceCheck
                        Layout.fillWidth: true
                        text: root.tr("Clone source before editing (recommended)", "落手之前複製來源（推薦，咪慳呢啲時間）")
                        checked: app.cloneSource
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 12
                        onToggled: app.setProjectBool("cloneSource", checked)
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                enabled: app.projectLoaded
                padding: 18

                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    Label {
                        text: root.tr("Edition target", "目標版本")
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        color: DesignTokens.onSurface(root.dark)
                    }
                    Label {
                        text: root.tr("Edition", "版本")
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: DesignTokens.onSurfaceVariant(root.dark)
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        model: app.editionNames
                        currentIndex: Math.max(0, app.imageIndex - 1)
                        onActivated: app.setProjectNumber("imageIndex", currentIndex + 1)
                    }
                    Label {
                        text: root.tr("Image index", "映像索引")
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: DesignTokens.onSurfaceVariant(root.dark)
                    }
                    SpinBox {
                        Layout.fillWidth: true
                        from: 1
                        to: 999
                        value: app.imageIndex
                        editable: true
                        onValueModified: app.setProjectNumber("imageIndex", value)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: app.imageSummary
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        color: DesignTokens.onSurfaceVariant(root.dark)
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        WfCard {
            Layout.fillWidth: true
            enabled: app.projectLoaded
            padding: 18

            ColumnLayout {
                width: parent.width
                spacing: 10
                Label {
                    text: root.tr("Output", "輸出")
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    color: DesignTokens.onSurface(root.dark)
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.availableWidth >= 840 ? 3 : 1
                    columnSpacing: 12
                    rowSpacing: 10

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Label {
                            text: root.tr("Output path", "輸出路徑")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            color: DesignTokens.onSurfaceVariant(root.dark)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            WfField {
                                Layout.fillWidth: true
                                text: app.outputPath
                                placeholderText: app.outputPath.length > 0 ? ""
                                                 : root.tr("Output ISO / image", "輸出 ISO / 映像")
                                mono: true
                                onEditingFinished: app.setProjectField("outputPath", text)
                            }
                            WfButton {
                                text: root.tr("Browse…", "瀏覽…")
                                compact: true
                                onClicked: outputFileDialog.open()
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Label {
                            text: root.tr("Format", "格式")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            color: DesignTokens.onSurfaceVariant(root.dark)
                        }
                        ComboBox {
                            id: format
                            Layout.fillWidth: true
                            model: ["WIM", "ESD", "SWM", "ISO"]
                            currentIndex: Math.max(0, model.indexOf(app.outputFormat.toUpperCase()))
                            onActivated: app.setProjectField("outputFormat", currentText.toLowerCase())
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Label {
                            text: root.tr("ISO volume label", "ISO 碟名")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            color: DesignTokens.onSurfaceVariant(root.dark)
                        }
                        WfField {
                            Layout.fillWidth: true
                            text: app.isoLabel
                            placeholderText: app.isoLabel.length > 0 ? ""
                                             : root.tr("ISO volume label", "ISO 碟名")
                            maximumLength: 32
                            onEditingFinished: app.setProjectField("isoLabel", text)
                        }
                    }
                }
            }
        }

        Item { Layout.preferredHeight: 20 }
    }
}
