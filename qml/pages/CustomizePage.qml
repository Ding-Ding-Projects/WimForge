import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components"

Item {
    id: root
    property var app
    property var tr: function(en, zh) { return en }
    property int currentSection: 0
    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        WfPageHeader {
            Layout.fillWidth: true
            title: root.tr("Customize the image", "調校個映像")
            description: root.tr("Everything here becomes declarative config first, a Git commit second, and a servicing command only after review.",
                                 "呢度每樣嘢都係先寫入設定、再 Git commit，最後睇清楚計劃先變成維護指令。三重保險，穩過茶記凍奶茶。")
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            contentHeight: availableHeight
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded

            Row {
                height: parent.availableHeight
                spacing: 4
                Repeater {
                    model: [
                        { en: "Updates", zh: "更新" },
                        { en: "Drivers", zh: "驅動程式" },
                        { en: "Features", zh: "功能" },
                        { en: "Apps", zh: "App" },
                        { en: "Components", zh: "元件" },
                        { en: "Settings", zh: "設定" },
                        { en: "Unattended", zh: "無人值守" },
                        { en: "Post-setup", zh: "裝完再做" }
                    ]
                    delegate: AbstractButton {
                        id: sectionTab
                        required property var modelData
                        required property int index
                        height: 38
                        width: Math.max(84, tabLabel.implicitWidth + 28)
                        Accessible.name: root.tr(modelData.en, modelData.zh)
                        Accessible.role: Accessible.PageTab
                        focusPolicy: Qt.StrongFocus
                        onClicked: root.currentSection = index
                        background: Item {
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                color: root.currentSection === sectionTab.index
                                       ? DesignTokens.primary(root.dark) : "transparent"
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: DesignTokens.radiusControl
                                color: sectionTab.hovered
                                       ? DesignTokens.surfaceContainer(root.dark) : "transparent"
                                z: -1
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: DesignTokens.radiusControl
                                color: "transparent"
                                border.width: sectionTab.visualFocus ? 2 : 0
                                border.color: sectionTab.visualFocus
                                              ? DesignTokens.primary(root.dark) : "transparent"
                            }
                        }
                        contentItem: Label {
                            id: tabLabel
                            text: root.tr(sectionTab.modelData.en, sectionTab.modelData.zh)
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: root.currentSection === sectionTab.index
                                   ? DesignTokens.primary(root.dark)
                                   : DesignTokens.onSurfaceVariant(root.dark)
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: DesignTokens.outlineVariant(root.dark)
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            currentIndex: root.currentSection

            PayloadPage {
                app: root.app
                tr: root.tr
                category: "updates"
                title: root.tr("Update & language packages", "更新同語言套件")
                subtitle: root.tr("Search Microsoft's official catalog, download a CAB/MSU matching the target build and architecture, then add it here for review.",
                                  "去 Microsoft 官方目錄搜尋，下載啱目標 build 同架構嘅 CAB/MSU，再加到呢度審閱。")
                placeholder: "D:\\updates\\KB123456.msu"
                sourcePlaceholder: root.tr("Example: Windows 11 24H2 x64 KB number", "例：Windows 11 24H2 x64 KB 號碼")
                items: app.updateCatalog
                fileFilters: [root.tr("Windows update packages (*.msu *.cab)", "Windows 更新套件 (*.msu *.cab)"), root.tr("All files (*)", "所有檔案 (*)")]
            }
            PayloadPage {
                app: root.app
                tr: root.tr
                category: "drivers"
                title: root.tr("INF drivers & hardware profiles", "INF 驅動程式同硬件設定檔")
                subtitle: root.tr("Prefer the device maker's signed INF package, or search Microsoft's official catalog by hardware ID. Add the INF or folder here for review.",
                                  "優先用裝置廠簽署嘅 INF，或者用 hardware ID 去 Microsoft 官方目錄搵。加 INF 或資料夾到呢度審閱。")
                placeholder: "D:\\drivers\\WiFi\\netadapter.inf"
                sourcePlaceholder: root.tr("Example: PCI\\VEN_8086&DEV_51F0", "例：PCI\\VEN_8086&DEV_51F0")
                items: app.driverCatalog
                fileFilters: [root.tr("Driver setup information (*.inf)", "驅動安裝資訊 (*.inf)"), root.tr("All files (*)", "所有檔案 (*)")]
                hostImportAvailable: true
            }
            FeatureWorkbench { app: root.app; tr: root.tr }
            AppsWorkbench { app: root.app; tr: root.tr }
            ComponentsWorkbench { app: root.app; tr: root.tr }
            SettingsGrid { app: root.app; tr: root.tr }
            ConfigList {
                title: root.tr("Unattended answer file", "無人值守答案檔")
                subtitle: root.tr("Apply an existing unattend.xml or generate setup-pass settings for OOBE, locale, accounts, edition, OEM and privacy.",
                                  "套用現成 unattend.xml，或者產生 OOBE、語言、帳戶、版本、OEM 同私隱設定。")
                placeholder: "D:\\profiles\\autounattend.xml"
                items: app.unattendedFiles
                addAction: value => app.tryAddListItem("unattendFiles", value)
                removeAction: index => app.removeListItem("unattendFiles", index)
                extraActionText: root.tr("Open generator", "開答案檔產生器")
                extraAction: () => app.openUnattendGenerator()
            }
            ConfigList {
                title: root.tr("Post-setup payloads", "裝完 Windows 再做嘅嘢")
                subtitle: root.tr("Copy files, run silent installers/scripts, apply delayed REG files, and stage $OEM$ content.",
                                  "複製檔案、靜默裝程式／行 script、延遲套 REG，同埋擺 $OEM$ 內容。")
                placeholder: root.tr("Command or payload path", "指令或者檔案路徑")
                items: app.postSetupItems
                addAction: value => app.tryAddListItem("postSetupItems", value)
                removeAction: index => app.removeListItem("postSetupItems", index)
            }
        }
    }

    component ConfigList: WfCard {
        id: configList
        implicitWidth: 0
        property string title
        property string subtitle
        property string placeholder
        property var items: []
        property var addAction: function(value) {}
        property var removeAction: function(index) {}
        property string extraActionText: ""
        property var extraAction: function() {}
        padding: 18

        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: configList.title
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 15
                font.weight: Font.Bold
                color: DesignTokens.onSurface(root.dark)
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: configList.subtitle
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                color: DesignTokens.onSurfaceVariant(root.dark)
                wrapMode: Text.Wrap
            }
            GridLayout {
                Layout.fillWidth: true
                columns: configList.availableWidth >= 720
                         ? (configList.extraActionText.length > 0 ? 3 : 2) : 1
                columnSpacing: 8
                rowSpacing: 8
                WfField {
                    id: entry
                    Layout.fillWidth: true
                    placeholderText: configList.placeholder
                    mono: true
                    onAccepted: addButton.clicked()
                }
                WfButton {
                    id: addButton
                    Layout.fillWidth: configList.availableWidth < 720
                    text: root.tr("Add", "加入")
                    glyph: "+"
                    variant: "filled"
                    enabled: entry.text.trim().length > 0
                    onClicked: {
                        if (configList.addAction(entry.text.trim()) !== false)
                            entry.text = ""
                    }
                }
                WfButton {
                    visible: configList.extraActionText.length > 0
                    Layout.fillWidth: configList.availableWidth < 720
                    text: configList.extraActionText
                    onClicked: configList.extraAction()
                }
            }
            ListView {
                id: itemList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: configList.items
                delegate: Rectangle {
                    id: configRow
                    required property string modelData
                    required property int index
                    width: itemList.width
                    height: Math.max(44, configText.implicitHeight + 18)
                    radius: DesignTokens.radiusControl
                    color: DesignTokens.surfaceLowest(root.dark)
                    border.width: 1
                    border.color: DesignTokens.outlineVariant(root.dark)
                    Accessible.name: modelData
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 6
                        spacing: 8
                        Label {
                            id: configText
                            Layout.fillWidth: true
                            text: configRow.modelData
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 12
                            color: DesignTokens.onSurface(root.dark)
                            wrapMode: Text.WrapAnywhere
                        }
                        WfIconButton {
                            glyph: "×"
                            buttonSize: 32
                            accessibleName: root.tr("Remove %1", "移除 %1").arg(configRow.modelData)
                            toolTip: accessibleName
                            onClicked: configList.removeAction(configRow.index)
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: itemList.count === 0
                    text: root.tr("Nothing queued yet", "未有嘢排隊")
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    color: DesignTokens.onSurfaceVariant(root.dark)
                }
            }
        }
    }

    component PayloadPage: Item {
        id: payloadPage
        implicitWidth: 0
        required property var app
        required property var tr
        property string category
        property string title
        property string subtitle
        property string placeholder
        property string sourcePlaceholder
        property var items: []
        property var fileFilters: []
        property bool hostImportAvailable: false

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            WfCard {
                Layout.fillWidth: true
                surfaceLevel: "container"
                outlined: false
                padding: 14
                ColumnLayout {
                    width: parent.width
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: payloadPage.title
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        color: DesignTokens.onSurface(root.dark)
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: payloadPage.subtitle
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        color: DesignTokens.onSurfaceVariant(root.dark)
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: payloadPage.tr("Official acquisition source", "官方下載來源")
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: DesignTokens.onSurface(root.dark)
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: payloadPage.width >= 700 ? 2 : 1
                        columnSpacing: 8
                        rowSpacing: 8
                        WfField {
                            id: sourceSearch
                            Layout.fillWidth: true
                            placeholderText: payloadPage.sourcePlaceholder
                            mono: payloadPage.category === "drivers"
                            onAccepted: sourceButton.clicked()
                        }
                        WfButton {
                            id: sourceButton
                            Layout.fillWidth: payloadPage.width < 700
                            text: payloadPage.tr("Search Microsoft Update Catalog", "搜尋 Microsoft Update Catalog")
                            onClicked: payloadPage.app.openMicrosoftUpdateCatalog(sourceSearch.text.trim())
                        }
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: payloadPage.width >= 920 ? 5 : payloadPage.width >= 560 ? 3 : 1
                columnSpacing: 8
                rowSpacing: 8
                WfField {
                    id: payloadEntry
                    Layout.fillWidth: true
                    placeholderText: payloadPage.placeholder
                    mono: true
                    onAccepted: addPathButton.clicked()
                }
                WfButton {
                    id: addPathButton
                    Layout.fillWidth: payloadPage.width < 560
                    text: payloadPage.tr("Add path", "加入路徑")
                    enabled: payloadPage.app.projectLoaded && payloadEntry.text.trim().length > 0
                    onClicked: {
                        if (payloadPage.app.addPayloadFiles(payloadPage.category,
                                                            [payloadEntry.text.trim()]))
                            payloadEntry.text = ""
                    }
                }
                WfButton {
                    Layout.fillWidth: payloadPage.width < 560
                    text: payloadPage.tr("Browse files…", "揀檔案…")
                    variant: "tonal"
                    enabled: payloadPage.app.projectLoaded
                    onClicked: payloadFiles.open()
                }
                WfButton {
                    Layout.fillWidth: payloadPage.width < 560
                    text: payloadPage.category === "drivers"
                          ? payloadPage.tr("Add driver folder…", "加驅動資料夾…")
                          : payloadPage.tr("Scan folder…", "掃描資料夾…")
                    enabled: payloadPage.app.projectLoaded
                    onClicked: payloadFolder.open()
                }
                WfButton {
                    visible: payloadPage.hostImportAvailable
                    Layout.fillWidth: payloadPage.width < 560
                    text: payloadPage.tr("Import host drivers", "抽本機驅動")
                    enabled: payloadPage.app.projectLoaded && !payloadPage.app.busy
                    onClicked: payloadPage.app.importHostDrivers()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: payloadPage.tr("Queued and locally inspected", "已排隊兼本機檢查")
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: DesignTokens.onSurfaceVariant(root.dark)
                }
                WfIconButton {
                    glyph: "↻"
                    buttonSize: 32
                    accessibleName: payloadPage.tr("Refresh payload metadata", "重新整理 payload 資料")
                    toolTip: accessibleName
                    enabled: payloadPage.app.projectLoaded
                    onClicked: payloadPage.app.refreshPayloadCatalog()
                }
            }

            ListView {
                id: payloadList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: payloadPage.items
                delegate: Rectangle {
                    id: payloadRow
                    required property var modelData
                    required property int index
                    width: payloadList.width
                    height: Math.max(58, payloadInfo.implicitHeight + 18)
                    radius: DesignTokens.radiusControl
                    color: DesignTokens.surfaceLowest(root.dark)
                    border.width: 1
                    border.color: DesignTokens.outlineVariant(root.dark)
                    Accessible.name: modelData.title + ", " + modelData.detail

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 6
                        spacing: 10
                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 4
                            color: payloadRow.modelData.exists && payloadRow.modelData.supported
                                   ? DesignTokens.success(root.dark) : DesignTokens.error(root.dark)
                        }
                        ColumnLayout {
                            id: payloadInfo
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                Layout.fillWidth: true
                                text: payloadRow.modelData.title
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: DesignTokens.onSurface(root.dark)
                                wrapMode: Text.WrapAnywhere
                            }
                            Label {
                                Layout.fillWidth: true
                                text: payloadRow.modelData.detail
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                color: DesignTokens.onSurfaceVariant(root.dark)
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: payloadRow.modelData.path
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 10
                                color: DesignTokens.onSurfaceVariant(root.dark)
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                        WfIconButton {
                            glyph: "×"
                            buttonSize: 32
                            accessibleName: payloadPage.tr("Remove %1", "移除 %1").arg(payloadRow.modelData.title)
                            toolTip: accessibleName
                            onClicked: payloadPage.app.removeListItem(payloadPage.category, payloadRow.index)
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 24, 620)
                    visible: payloadList.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    text: payloadPage.app.projectLoaded
                          ? payloadPage.tr("Nothing is queued. Search the official source above, then browse downloaded payloads or scan a local folder.",
                                           "未有 payload 排隊。先用上面官方來源搜尋，再揀下載咗嘅檔案或者掃描本機資料夾。")
                          : payloadPage.tr("Open or create a project before queueing payloads.",
                                           "先開或者建立工程，之後先可以排 payload。")
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    color: DesignTokens.onSurfaceVariant(root.dark)
                    wrapMode: Text.Wrap
                }
            }
        }

        FileDialog {
            id: payloadFiles
            title: payloadPage.category === "drivers"
                   ? payloadPage.tr("Choose INF driver packages", "揀 INF 驅動套件")
                   : payloadPage.tr("Choose CAB/MSU update packages", "揀 CAB/MSU 更新套件")
            fileMode: FileDialog.OpenFiles
            nameFilters: payloadPage.fileFilters
            onAccepted: payloadPage.app.addPayloadFiles(payloadPage.category, selectedFiles)
        }
        FolderDialog {
            id: payloadFolder
            title: payloadPage.category === "drivers"
                   ? payloadPage.tr("Choose a recursive INF driver folder", "揀一個遞迴 INF 驅動資料夾")
                   : payloadPage.tr("Choose a folder containing CAB/MSU updates", "揀一個有 CAB/MSU 更新嘅資料夾")
            onAccepted: payloadPage.app.addPayloadDirectory(payloadPage.category, selectedFolder)
        }
    }

    component TriStatePicker: RowLayout {
        id: statePicker
        required property int state
        required property var tr
        signal chosen(int value)
        spacing: 4

        Repeater {
            model: [
                { value: 0, en: "Unchanged", zh: "不變", variant: "tonal" },
                { value: 1, en: "Enable", zh: "啟用", variant: "filled" },
                { value: -1, en: "Disable", zh: "停用", variant: "destructive" }
            ]
            delegate: WfButton {
                required property var modelData
                compact: true
                text: statePicker.tr(modelData.en, modelData.zh)
                variant: statePicker.state === modelData.value
                         ? modelData.variant : "outlined"
                onClicked: statePicker.chosen(modelData.value)
            }
        }
    }

    component FeatureWorkbench: Item {
        id: featurePage
        implicitWidth: 0
        required property var app
        required property var tr

        ScrollView {
            id: featureScroll
            anchors.fill: parent
            contentWidth: availableWidth

            ColumnLayout {
                width: featureScroll.availableWidth
                spacing: 12

                WfCard {
                    Layout.fillWidth: true
                    padding: 18
                    ColumnLayout {
                        width: parent.width
                        spacing: 6
                        Label {
                            text: featurePage.tr("Windows optional features", "Windows 選用功能")
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 16
                            font.weight: Font.Bold
                            color: DesignTokens.onSurface(root.dark)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: featurePage.tr(
                                      "Choose Enable, Disable, or Unchanged. Unchanged removes the project override instead of forcing a boolean.",
                                      "每項可以揀啟用、停用或者不變。不變會清除工程覆寫，唔會硬係當成開或關。")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            color: DesignTokens.onSurfaceVariant(root.dark)
                            wrapMode: Text.Wrap
                        }
                    }
                }

                WfCard {
                    Layout.fillWidth: true
                    padding: 12
                    ColumnLayout {
                        width: parent.width
                        spacing: 6
                        Repeater {
                            model: [
                                { id: "NetFx3", en: ".NET Framework 3.5", zh: ".NET Framework 3.5" },
                                { id: "Microsoft-Windows-Subsystem-Linux", en: "Windows Subsystem for Linux", zh: "Windows Linux 子系統" },
                                { id: "VirtualMachinePlatform", en: "Virtual Machine Platform", zh: "虛擬機平台" },
                                { id: "Microsoft-Hyper-V-All", en: "Hyper-V", zh: "Hyper-V" },
                                { id: "Containers", en: "Windows Containers", zh: "Windows 容器" },
                                { id: "TelnetClient", en: "Telnet client", zh: "Telnet 用戶端" },
                                { id: "SMB1Protocol", en: "SMB 1.0 (legacy and risky)", zh: "SMB 1.0（舊式兼高危）" },
                                { id: "Printing-PrintToPDFServices-Features", en: "Microsoft Print to PDF", zh: "Microsoft 列印到 PDF" }
                            ]
                            delegate: Rectangle {
                                id: featureRow
                                required property var modelData
                                readonly property int changeState:
                                    featurePage.app.features.indexOf(modelData.id) >= 0 ? 1
                                    : featurePage.app.featureDisables.indexOf(modelData.id) >= 0 ? -1 : 0
                                Layout.fillWidth: true
                                implicitHeight: featurePage.width >= 760 ? 58 : 94
                                radius: DesignTokens.radiusControl
                                color: DesignTokens.surfaceLowest(root.dark)
                                border.width: 1
                                border.color: DesignTokens.outlineVariant(root.dark)

                                GridLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    columns: featurePage.width >= 760 ? 2 : 1
                                    columnSpacing: 12
                                    rowSpacing: 6
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Label {
                                            Layout.fillWidth: true
                                            text: featurePage.tr(featureRow.modelData.en,
                                                                 featureRow.modelData.zh)
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            color: DesignTokens.onSurface(root.dark)
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: featureRow.modelData.id
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 10
                                            color: DesignTokens.onSurfaceVariant(root.dark)
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                    TriStatePicker {
                                        Layout.alignment: Qt.AlignRight
                                        state: featureRow.changeState
                                        tr: featurePage.tr
                                        onChosen: value => featurePage.app.setFeatureState(
                                                      featureRow.modelData.id, value)
                                    }
                                }
                            }
                        }
                    }
                }

                WfCard {
                    Layout.fillWidth: true
                    padding: 18
                    ColumnLayout {
                        width: parent.width
                        spacing: 10
                        Label {
                            text: featurePage.tr("Capabilities & Features on Demand",
                                                 "Capabilities 同按需功能")
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 16
                            font.weight: Font.Bold
                            color: DesignTokens.onSurface(root.dark)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: featurePage.tr(
                                      "Enter the exact DISM capability identity for the selected Windows build. Add and Remove are mutually exclusive; Unchanged clears either action.",
                                      "請輸入同所揀 Windows build 完全相符嘅 DISM capability identity。加入同移除唔可以同時揀；不變會清除兩者。")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            color: DesignTokens.onSurfaceVariant(root.dark)
                            wrapMode: Text.Wrap
                        }
                        WfField {
                            id: capabilityIdentity
                            Layout.fillWidth: true
                            label: featurePage.tr("Capability identity", "Capability 識別碼")
                            placeholderText: "OpenSSH.Client~~~~0.0.1.0"
                            helperText: featurePage.tr(
                                            "Example: MathRecognizer~~~~0.0.1.0",
                                            "例：MathRecognizer~~~~0.0.1.0")
                            mono: true
                            onAccepted: capabilityAdd.clicked()
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            WfButton {
                                id: capabilityAdd
                                text: featurePage.tr("Add", "加入")
                                glyph: "+"
                                variant: "filled"
                                enabled: capabilityIdentity.text.trim().length > 0
                                onClicked: {
                                    if (featurePage.app.setCapabilityState(
                                                capabilityIdentity.text.trim(), 1))
                                        capabilityIdentity.text = ""
                                }
                            }
                            WfButton {
                                text: featurePage.tr("Remove", "移除")
                                variant: "destructive"
                                enabled: capabilityIdentity.text.trim().length > 0
                                onClicked: {
                                    if (featurePage.app.setCapabilityState(
                                                capabilityIdentity.text.trim(), -1))
                                        capabilityIdentity.text = ""
                                }
                            }
                            WfButton {
                                text: featurePage.tr("Unchanged", "不變")
                                variant: "outlined"
                                enabled: capabilityIdentity.text.trim().length > 0
                                onClicked: {
                                    if (featurePage.app.setCapabilityState(
                                                capabilityIdentity.text.trim(), 0))
                                        capabilityIdentity.text = ""
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: DesignTokens.outlineVariant(root.dark)
                        }
                        Label {
                            text: featurePage.tr("Queued capability changes", "已排隊嘅 capability 變更")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: DesignTokens.onSurface(root.dark)
                        }
                        Repeater {
                            model: featurePage.app.capabilityChanges
                            delegate: Rectangle {
                                id: capabilityRow
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 46
                                radius: DesignTokens.radiusControl
                                color: DesignTokens.surfaceLowest(root.dark)
                                border.width: 1
                                border.color: DesignTokens.outlineVariant(root.dark)
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 6
                                    spacing: 8
                                    WfStatusChip {
                                        text: capabilityRow.modelData.disposition === "add"
                                              ? featurePage.tr("Add", "加入")
                                              : featurePage.tr("Remove", "移除")
                                        tone: capabilityRow.modelData.disposition === "add"
                                              ? "success" : "error"
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: capabilityRow.modelData.identity
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 11
                                        color: DesignTokens.onSurface(root.dark)
                                        elide: Text.ElideMiddle
                                    }
                                    WfIconButton {
                                        glyph: "×"
                                        buttonSize: 32
                                        accessibleName: featurePage.tr(
                                                            "Clear capability change",
                                                            "清除 capability 變更")
                                        toolTip: accessibleName
                                        onClicked: featurePage.app.setCapabilityState(
                                                       capabilityRow.modelData.identity, 0)
                                    }
                                }
                            }
                        }
                        Label {
                            visible: featurePage.app.capabilityChanges.length === 0
                            text: featurePage.tr("No capability changes are queued.",
                                                 "未有 capability 變更排隊。")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            color: DesignTokens.onSurfaceVariant(root.dark)
                        }
                    }
                }
                Item { Layout.preferredHeight: 8 }
            }
        }
    }

    component AppsWorkbench: Item {
        id: appsPage
        implicitWidth: 0
        required property var app
        required property var tr

        ScrollView {
            id: appsScroll
            anchors.fill: parent
            contentWidth: availableWidth
            ColumnLayout {
                width: appsScroll.availableWidth
                spacing: 12

                WfCard {
                    Layout.fillWidth: true
                    padding: 18
                    ColumnLayout {
                        width: parent.width
                        spacing: 6
                        Label {
                            text: appsPage.tr("Provisioned Appx / MSIX", "預載 Appx / MSIX")
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 16
                            font.weight: Font.Bold
                            color: DesignTokens.onSurface(root.dark)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: appsPage.tr(
                                      "Package-name removals and signed-package provisioning are separate typed actions. Review the exact image build before running either one.",
                                      "按套件名移除同預載已簽署套件係兩種獨立動作。執行之前，請先對清楚映像 build。")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            color: DesignTokens.onSurfaceVariant(root.dark)
                            wrapMode: Text.Wrap
                        }
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: appsPage.width >= 860 ? 2 : 1
                    columnSpacing: 12
                    rowSpacing: 12

                    WfCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 390
                        padding: 16
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8
                            Label {
                                text: appsPage.tr("Remove for new users", "由新用戶預載清單移除")
                                font.family: DesignTokens.fontDisplay
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                color: DesignTokens.onSurface(root.dark)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: appsPage.tr(
                                          "Use the provisioned package name reported by DISM, not a Store display name.",
                                          "請用 DISM 顯示嘅預載套件名，唔好填 Store 顯示名稱。")
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                color: DesignTokens.onSurfaceVariant(root.dark)
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                WfField {
                                    id: appRemovalIdentity
                                    Layout.fillWidth: true
                                    placeholderText: "Microsoft.BingNews_8wekyb3d8bbwe"
                                    mono: true
                                    onAccepted: appRemovalAdd.clicked()
                                }
                                WfButton {
                                    id: appRemovalAdd
                                    text: appsPage.tr("Queue removal", "排隊移除")
                                    glyph: "+"
                                    variant: "destructive"
                                    compact: true
                                    enabled: appRemovalIdentity.text.trim().length > 0
                                    onClicked: {
                                        if (appsPage.app.tryAddListItem(
                                                    "appRemovals",
                                                    appRemovalIdentity.text.trim()))
                                            appRemovalIdentity.text = ""
                                    }
                                }
                            }
                            ListView {
                                id: appRemovalList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 6
                                model: appsPage.app.appRemovals
                                delegate: Rectangle {
                                    id: appRemovalRow
                                    required property string modelData
                                    required property int index
                                    width: appRemovalList.width
                                    height: 48
                                    radius: DesignTokens.radiusControl
                                    color: DesignTokens.surfaceLowest(root.dark)
                                    border.width: 1
                                    border.color: DesignTokens.outlineVariant(root.dark)
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 6
                                        spacing: 8
                                        WfStatusChip {
                                            text: appsPage.tr("Remove", "移除")
                                            tone: "error"
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: appRemovalRow.modelData
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 11
                                            color: DesignTokens.onSurface(root.dark)
                                            elide: Text.ElideMiddle
                                        }
                                        WfIconButton {
                                            glyph: "×"
                                            buttonSize: 32
                                            accessibleName: appsPage.tr("Clear removal", "清除移除動作")
                                            toolTip: accessibleName
                                            onClicked: appsPage.app.removeListItem(
                                                           "appRemovals", appRemovalRow.index)
                                        }
                                    }
                                }
                                Label {
                                    anchors.centerIn: parent
                                    visible: appRemovalList.count === 0
                                    text: appsPage.tr("No app removals queued", "未有 App 移除動作排隊")
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 12
                                    color: DesignTokens.onSurfaceVariant(root.dark)
                                }
                            }
                        }
                    }

                    WfCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 390
                        padding: 16
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8
                            Label {
                                text: appsPage.tr("Provision signed packages", "預載已簽署套件")
                                font.family: DesignTokens.fontDisplay
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                color: DesignTokens.onSurface(root.dark)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: appsPage.tr(
                                          "Choose existing Appx/MSIX packages. Queue signed framework dependencies before the main bundle; DISM validates signatures during servicing.",
                                          "請揀現有 Appx/MSIX 套件。已簽署 framework 依賴要排喺主 bundle 前面；DISM 會喺維護期間驗證簽署。")
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                color: DesignTokens.onSurfaceVariant(root.dark)
                                wrapMode: Text.Wrap
                            }
                            WfButton {
                                text: appsPage.tr("Browse signed packages…", "瀏覽已簽署套件…")
                                glyph: "+"
                                variant: "filled"
                                onClicked: appxProvisionFiles.open()
                            }
                            ListView {
                                id: appProvisionList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 6
                                model: appsPage.app.appProvisions
                                delegate: Rectangle {
                                    id: appProvisionRow
                                    required property string modelData
                                    required property int index
                                    width: appProvisionList.width
                                    height: 54
                                    radius: DesignTokens.radiusControl
                                    color: DesignTokens.surfaceLowest(root.dark)
                                    border.width: 1
                                    border.color: DesignTokens.outlineVariant(root.dark)
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 6
                                        spacing: 8
                                        WfStatusChip {
                                            text: appsPage.tr("Provision", "預載")
                                            tone: "success"
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Label {
                                                Layout.fillWidth: true
                                                text: appProvisionRow.modelData.split(/[\\/]/).pop()
                                                font.family: DesignTokens.fontBody
                                                font.pixelSize: 11
                                                font.weight: Font.DemiBold
                                                color: DesignTokens.onSurface(root.dark)
                                                elide: Text.ElideMiddle
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: appProvisionRow.modelData
                                                font.family: DesignTokens.fontMono
                                                font.pixelSize: 9
                                                color: DesignTokens.onSurfaceVariant(root.dark)
                                                elide: Text.ElideMiddle
                                            }
                                        }
                                        WfIconButton {
                                            glyph: "×"
                                            buttonSize: 32
                                            accessibleName: appsPage.tr("Clear provisioning", "清除預載動作")
                                            toolTip: accessibleName
                                            onClicked: appsPage.app.removeListItem(
                                                           "appProvision", appProvisionRow.index)
                                        }
                                    }
                                }
                                Label {
                                    anchors.centerIn: parent
                                    visible: appProvisionList.count === 0
                                    text: appsPage.tr("No signed packages queued", "未有已簽署套件排隊")
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 12
                                    color: DesignTokens.onSurfaceVariant(root.dark)
                                }
                            }
                        }
                    }
                }
                Item { Layout.preferredHeight: 8 }
            }
        }

        FileDialog {
            id: appxProvisionFiles
            title: appsPage.tr("Choose signed Appx/MSIX packages", "揀已簽署 Appx/MSIX 套件")
            fileMode: FileDialog.OpenFiles
            nameFilters: [
                appsPage.tr("Appx and MSIX packages (*.appx *.appxbundle *.msix *.msixbundle)",
                            "Appx 同 MSIX 套件 (*.appx *.appxbundle *.msix *.msixbundle)"),
                appsPage.tr("All files (*)", "所有檔案 (*)")
            ]
            onAccepted: appsPage.app.addAppxProvisionFiles(selectedFiles)
        }
    }

    component ComponentsWorkbench: Item {
        id: componentsPage
        implicitWidth: 0
        required property var app
        required property var tr
        property int activeTool: 0

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            WfCard {
                Layout.fillWidth: true
                padding: 8
                RowLayout {
                    width: parent.width
                    spacing: 6
                    WfButton {
                        text: componentsPage.tr("Component packages", "元件套件")
                        variant: componentsPage.activeTool === 0 ? "tonal" : "text"
                        onClicked: componentsPage.activeTool = 0
                    }
                    WfButton {
                        text: componentsPage.tr("Scheduled tasks", "排程工作")
                        variant: componentsPage.activeTool === 1 ? "tonal" : "text"
                        onClicked: componentsPage.activeTool = 1
                    }
                    Item { Layout.fillWidth: true }
                    WfStatusChip {
                        text: componentsPage.activeTool === 0
                              ? componentsPage.tr("Destructive", "破壞性")
                              : componentsPage.tr("Typed actions", "類型化動作")
                        tone: componentsPage.activeTool === 0 ? "error" : "warning"
                    }
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: componentsPage.activeTool

                ConfigList {
                    title: componentsPage.tr("Remove component packages", "移除元件套件")
                    subtitle: componentsPage.tr(
                                  "Enter an exact build-specific DISM package identity. Component removal can reduce serviceability and always requires plan review.",
                                  "請輸入同 build 完全相符嘅 DISM 套件 identity。移除元件可能會影響日後維護，所以一定要先審閱計劃。")
                    placeholder: "Microsoft-Windows-Example-Package~31bf3856ad364e35~amd64~~10.0.1.0"
                    items: componentsPage.app.componentRemovals
                    addAction: value => componentsPage.app.tryAddListItem(
                                                   "componentRemovals", value)
                    removeAction: index => componentsPage.app.removeListItem(
                                                     "componentRemovals", index)
                }

                WfCard {
                    padding: 18
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10
                        Label {
                            text: componentsPage.tr("Offline scheduled-task changes",
                                                     "離線排程工作變更")
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 16
                            font.weight: Font.Bold
                            color: DesignTokens.onSurface(root.dark)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: componentsPage.tr(
                                      "Enable and Disable update the task XML atomically. Delete removes the task definition and needs an explicit compatibility override plus a checkpoint.",
                                      "啟用同停用會原子更新工作 XML。刪除會移走工作定義，必須明確確認相容性解鎖，亦會先做檢查點。")
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            color: DesignTokens.onSurfaceVariant(root.dark)
                            wrapMode: Text.Wrap
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: componentsPage.width >= 760 ? 3 : 1
                            columnSpacing: 8
                            rowSpacing: 8
                            WfField {
                                id: taskPath
                                Layout.fillWidth: true
                                label: componentsPage.tr("Task path", "工作路徑")
                                placeholderText: "Microsoft/Windows/Maps/MapsUpdateTask"
                                helperText: componentsPage.tr(
                                                "Relative to Windows\\System32\\Tasks",
                                                "相對於 Windows\\System32\\Tasks")
                                mono: true
                                onAccepted: queueTaskChange.clicked()
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Label {
                                    text: componentsPage.tr("Action", "動作")
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: DesignTokens.onSurface(root.dark)
                                }
                                ComboBox {
                                    id: taskDisposition
                                    Layout.fillWidth: true
                                    model: [
                                        componentsPage.tr("Disable", "停用"),
                                        componentsPage.tr("Enable", "啟用"),
                                        componentsPage.tr("Delete", "刪除")
                                    ]
                                    readonly property var values: ["disable", "enable", "remove"]
                                    readonly property string selectedValue: values[currentIndex]
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                CheckBox {
                                    id: taskCompatibilityOverride
                                    Layout.fillWidth: true
                                    visible: taskDisposition.selectedValue === "remove"
                                    text: componentsPage.tr(
                                              "I verified compatibility for this exact build",
                                              "我已確認同呢個準確 build 相容")
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 11
                                }
                                WfButton {
                                    id: queueTaskChange
                                    Layout.fillWidth: true
                                    text: componentsPage.tr("Queue typed change", "排隊類型化變更")
                                    variant: taskDisposition.selectedValue === "remove"
                                             ? "destructive" : "filled"
                                    enabled: taskPath.text.trim().length > 0
                                             && (taskDisposition.selectedValue !== "remove"
                                                 || taskCompatibilityOverride.checked)
                                    onClicked: {
                                        if (componentsPage.app.setScheduledTaskChange(
                                                    taskPath.text.trim(),
                                                    taskDisposition.selectedValue,
                                                    taskCompatibilityOverride.checked)) {
                                            taskPath.text = ""
                                            taskCompatibilityOverride.checked = false
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: DesignTokens.outlineVariant(root.dark)
                        }
                        ListView {
                            id: scheduledTaskList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 6
                            model: componentsPage.app.scheduledTaskChanges
                            delegate: Rectangle {
                                id: scheduledTaskRow
                                required property var modelData
                                required property int index
                                width: scheduledTaskList.width
                                height: 60
                                radius: DesignTokens.radiusControl
                                color: DesignTokens.surfaceLowest(root.dark)
                                border.width: 1
                                border.color: DesignTokens.outlineVariant(root.dark)
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 6
                                    spacing: 8
                                    WfStatusChip {
                                        text: scheduledTaskRow.modelData.disposition === "enable"
                                              ? componentsPage.tr("Enable", "啟用")
                                              : scheduledTaskRow.modelData.disposition === "remove"
                                                ? componentsPage.tr("Delete", "刪除")
                                                : componentsPage.tr("Disable", "停用")
                                        tone: scheduledTaskRow.modelData.disposition === "enable"
                                              ? "success"
                                              : scheduledTaskRow.modelData.disposition === "remove"
                                                ? "error" : "warning"
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Label {
                                            Layout.fillWidth: true
                                            text: scheduledTaskRow.modelData.path
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 11
                                            color: DesignTokens.onSurface(root.dark)
                                            elide: Text.ElideMiddle
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            visible: scheduledTaskRow.modelData.compatibilityOverride
                                            text: componentsPage.tr(
                                                      "Compatibility override recorded",
                                                      "已記錄相容性解鎖")
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 10
                                            color: DesignTokens.error(root.dark)
                                        }
                                    }
                                    WfIconButton {
                                        glyph: "×"
                                        buttonSize: 32
                                        accessibleName: componentsPage.tr(
                                                            "Clear scheduled-task change",
                                                            "清除排程工作變更")
                                        toolTip: accessibleName
                                        onClicked: componentsPage.app.removeScheduledTaskChange(
                                                       scheduledTaskRow.index)
                                    }
                                }
                            }
                            Label {
                                anchors.centerIn: parent
                                visible: scheduledTaskList.count === 0
                                text: componentsPage.tr("No scheduled-task changes queued",
                                                        "未有排程工作變更排隊")
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 12
                                color: DesignTokens.onSurfaceVariant(root.dark)
                            }
                        }
                    }
                }
            }
        }
    }

    component SettingsGrid: WfCard {
        id: settingsPage
        implicitWidth: 0
        required property var app
        required property var tr
        padding: 18
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            Label {
                text: settingsPage.tr("Baseline settings", "基準設定")
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 15
                font.weight: Font.Bold
                color: DesignTokens.onSurface(root.dark)
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                GridLayout {
                    width: parent.availableWidth
                    columns: width >= 720 ? 2 : 1
                    rowSpacing: 8
                    columnSpacing: 12
                    Repeater {
                        model: [
                            ["disableTelemetry", "Reduce diagnostics and advertising telemetry", "減少診斷同廣告追蹤（少啲八卦）"],
                            ["localAccountOobe", "Allow local account during OOBE", "OOBE 俾你用本機帳戶"],
                            ["showFileExtensions", "Show known file extensions", "顯示已知副檔名"],
                            ["classicContextMenu", "Use classic context menu", "用返經典右鍵選單"],
                            ["disableConsumerFeatures", "Disable consumer app suggestions", "停用硬銷 App 建議"],
                            ["enableLongPaths", "Enable Win32 long paths", "啟用 Win32 長路徑"],
                            ["performanceVisuals", "Prefer performance visual effects", "視覺效果偏向效能"],
                            ["disableRecall", "Disable Recall policy", "停用 Recall 政策"]
                        ]
                        delegate: SwitchDelegate {
                            id: settingToggle
                            required property var modelData
                            Layout.fillWidth: true
                            text: settingsPage.tr(modelData[1], modelData[2])
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 12
                            checked: app.settingEnabled(modelData[0])
                            onToggled: app.setSetting(modelData[0], checked)
                        }
                    }
                }
            }
        }
    }
}
