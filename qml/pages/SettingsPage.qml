pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components"

Item {
    id: root

    required property var app
    required property var tr
    property int requestedTheme: 0
    property int categoryIndex: 0

    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
    readonly property bool railMode: width >= 900
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)
    readonly property color surfaceVariantForeground: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color outlineVariant: DesignTokens.outlineVariant(root.dark)
    readonly property color primary: DesignTokens.primary(root.dark)

    readonly property var categories: [
        { en: "Appearance & language", zh: "外觀同語言", code: "A" },
        { en: "Jobs & resources", zh: "工序同資源", code: "J" },
        { en: "Safety", zh: "安全", code: "S" },
        { en: "Automation", zh: "自動化", code: "AU" },
        { en: "Diagnostics", zh: "診斷", code: "D" }
    ]

    FileDialog {
        id: autoExportDialog
        title: root.tr("Choose the automatic project export", "揀自動工程匯出位置")
        modality: Qt.NonModal
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wimforge"
        nameFilters: [root.tr("WimForge project bundles (*.wimforge)", "WimForge 工程 bundle (*.wimforge)")]
        onAccepted: root.app.autoExportPath = root.app.pathFromUrl(selectedFile)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: DesignTokens.spacing16

        WfPageHeader {
            Layout.fillWidth: true
            dark: root.dark
            eyebrow: root.tr("Application preferences", "應用程式偏好")
            title: root.tr("Settings", "設定")
            description: root.tr("Preferences save automatically and remain available across projects.",
                                 "偏好設定會自動儲存，並套用到所有工程。")
            WfStatusChip {
                dark: root.dark
                tone: "success"
                uppercase: false
                text: root.tr("Saved automatically", "已自動儲存")
            }
        }

        WfTabBar {
            id: compactCategories
            visible: !root.railMode
            Layout.fillWidth: true
            dark: root.dark
            currentIndex: root.categoryIndex
            onCurrentIndexChanged: root.categoryIndex = currentIndex
            model: root.categories.map(category => root.tr(category.en, category.zh))
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: root.railMode ? 2 : 1
            columnSpacing: DesignTokens.spacing16
            rowSpacing: 0

            WfCard {
                visible: root.railMode
                Layout.preferredWidth: 235
                Layout.fillHeight: true
                dark: root.dark
                surfaceLevel: "low"
                padding: DesignTokens.spacing8
                Accessible.role: Accessible.PageTabList
                Accessible.name: root.tr("Settings categories", "設定類別")

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing4
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: DesignTokens.spacing12
                        Layout.rightMargin: DesignTokens.spacing12
                        Layout.topMargin: DesignTokens.spacing8
                        Layout.bottomMargin: DesignTokens.spacing4
                        text: root.tr("CATEGORIES", "類別")
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 10
                        font.weight: Font.Bold
                        font.letterSpacing: 0.8
                    }
                    Repeater {
                        model: root.categories
                        delegate: ItemDelegate {
                            id: categoryDelegate
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            implicitHeight: Math.max(DesignTokens.rowHeight,
                                                     categoryContent.implicitHeight + DesignTokens.spacing16)
                            leftPadding: DesignTokens.spacing12
                            rightPadding: DesignTokens.spacing12
                            highlighted: root.categoryIndex === index
                            focusPolicy: Qt.StrongFocus
                            Accessible.role: Accessible.PageTab
                            Accessible.name: root.tr(categoryDelegate.modelData.en,
                                                      categoryDelegate.modelData.zh)
                            Accessible.selected: highlighted
                            onClicked: root.categoryIndex = index
                            background: Rectangle {
                                radius: 1 DesignTokens.radiusControl
                                color: categoryDelegate.highlighted
                                       ? DesignTokens.primaryContainer(root.dark)
                                       : categoryDelegate.hovered
                                         ? DesignTokens.surfaceHigh(root.dark) : "transparent"
                                border.width: categoryDelegate.visualFocus ? 2 : 0
                                border.color: categoryDelegate.visualFocus
                                              ? DesignTokens.primary(root.dark) : "transparent"
                            }
                            contentItem: RowLayout {
                                id: categoryContent
                                spacing: DesignTokens.spacing8
                                Label {
                                    Layout.preferredWidth: 24
                                    text: categoryDelegate.modelData.code
                                    color: categoryDelegate.highlighted
                                           ? DesignTokens.onPrimaryContainer(root.dark)
                                           : root.surfaceVariantForeground
                                    font.family: DesignTokens.fontMono
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: root.tr(categoryDelegate.modelData.en, categoryDelegate.modelData.zh)
                                    color: categoryDelegate.highlighted
                                           ? DesignTokens.onPrimaryContainer(root.dark) : root.surfaceForeground
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 12
                                    font.weight: categoryDelegate.highlighted ? Font.DemiBold : Font.Normal
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                    Label {
                        Layout.fillWidth: true
                        Layout.margins: DesignTokens.spacing12
                        text: root.tr("Changes take effect immediately.", "變更會即時生效。")
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.categoryIndex

                SettingsPanel {
                    panelTitle: root.tr("Appearance & language", "外觀同語言")
                    panelDescription: root.tr("Match Windows, choose English or Hong Kong Cantonese, and scale the desktop UI.",
                                              "配合 Windows、選擇英文或香港粵語，同調整桌面介面比例。")

                    SettingsValueRow {
                        title: root.tr("Display language", "顯示語言")
                        description: root.tr("Bilingual shows English and 香港粵語 together across supported surfaces.",
                                             "雙語會喺支援嘅介面同時顯示英文同香港粵語。")
                        RowLayout {
                            spacing: DesignTokens.spacing4
                            RadioButton { text: "English"; checked: root.app.languageMode === 0; onClicked: root.app.languageMode = 0 }
                            RadioButton { text: "香港粵語"; checked: root.app.languageMode === 1; onClicked: root.app.languageMode = 1 }
                            RadioButton { text: "English + 粵語"; checked: root.app.languageMode === 2; onClicked: root.app.languageMode = 2 }
                        }
                    }

                    SettingsValueRow {
                        title: root.tr("Material color theme", "Material 顏色主題")
                        description: root.tr("Follow Windows by default, with explicit light and dark overrides.",
                                             "預設跟 Windows，亦可以指定光亮或深色。")
                        ComboBox {
                            Layout.preferredWidth: 220
                            Layout.preferredHeight: DesignTokens.controlHeight
                            model: [root.tr("Follow system", "跟系統"), root.tr("Light", "光亮"), root.tr("Dark", "深色")]
                            currentIndex: root.app.themeMode
                            Accessible.name: root.tr("Material color theme", "Material 顏色主題")
                            onActivated: root.app.themeMode = currentIndex
                        }
                    }

                    SettingsValueRow {
                        title: root.tr("Material color scheme", "Material 配色")
                        description: root.tr("Copper, Indigo or Spruce tonal palette, applied across light and dark.",
                                             "赤銅、靛藍或雲杉色系，淺色深色都套用。")
                        RowLayout {
                            spacing: DesignTokens.spacing8
                            Repeater {
                                model: [0, 1, 2]
                                delegate: AbstractButton {
                                    id: schemeButton
                                    required property int modelData
                                    readonly property bool active: root.app.colorScheme === modelData
                                    readonly property var swatch: DesignTokens._schemes[modelData][root.dark ? "dark" : "light"]
                                    implicitHeight: DesignTokens.controlHeight
                                    leftPadding: DesignTokens.spacing12
                                    rightPadding: DesignTokens.spacing16
                                    focusPolicy: Qt.StrongFocus
                                    Accessible.role: Accessible.RadioButton
                                    Accessible.name: root.tr(DesignTokens.schemeNames[modelData].en,
                                                             DesignTokens.schemeNames[modelData].zh)
                                    Accessible.checkable: true
                                    Accessible.checked: active
                                    onClicked: root.app.colorScheme = modelData
                                    background: Rectangle {
                                        radius: 1 DesignTokens.radiusPill
                                        color: schemeButton.active ? DesignTokens.secondaryContainer(root.dark)
                                               : schemeButton.hovered ? DesignTokens.surfaceHigh(root.dark) : "transparent"
                                        border.width: schemeButton.active || schemeButton.visualFocus ? 2 : 1
                                        border.color: schemeButton.visualFocus
                                                      ? DesignTokens.primary(root.dark)
                                                      : schemeButton.active
                                                        ? DesignTokens.secondaryContainer(root.dark)
                                                        : DesignTokens.outline(root.dark)
                                    }
                                    contentItem: RowLayout {
                                        spacing: DesignTokens.spacing8
                                        Rectangle {
                                            Layout.preferredWidth: 14
                                            Layout.preferredHeight: 14
                                            radius: 1 7
                                            color: schemeButton.swatch.p
                                            border.width: 1
                                            border.color: DesignTokens.outlineVariant(root.dark)
                                        }
                                        Label {
                                            text: root.tr(DesignTokens.schemeNames[schemeButton.modelData].en,
                                                          DesignTokens.schemeNames[schemeButton.modelData].zh)
                                            color: schemeButton.active
                                                   ? DesignTokens.onSecondaryContainer(root.dark)
                                                   : root.surfaceForeground
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 12
                                            font.weight: schemeButton.active ? Font.DemiBold : Font.Medium
                                        }
                                    }
                                }
                            }
                        }
                    }

                    SettingsValueRow {
                        title: root.tr("Interface scale", "介面比例")
                        description: root.tr("Adjust the full desktop surface from 80% to 125%.",
                                             "將整個桌面介面由 80% 調至 125%。")
                        RowLayout {
                            Layout.preferredWidth: 270
                            spacing: DesignTokens.spacing8
                            Slider {
                                Layout.fillWidth: true
                                from: 0.8
                                to: 1.25
                                stepSize: 0.05
                                value: root.app.interfaceScale
                                Accessible.name: root.tr("Interface scale", "介面比例")
                                onMoved: root.app.interfaceScale = value
                            }
                            Label {
                                text: Math.round(root.app.interfaceScale * 100) + "%"
                                color: root.surfaceForeground
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 11
                            }
                        }
                    }

                    SettingsToggle {
                        title: root.tr("Material motion", "Material 動畫")
                        description: root.tr("Use brief state transitions. Disable to make shared motion immediate.",
                                             "使用短暫狀態轉場；停用後所有共用動畫會即時完成。")
                        checked: root.app.motionEnabled
                        onToggled: checked => root.app.motionEnabled = checked
                    }
                }

                SettingsPanel {
                    panelTitle: root.tr("Jobs & resources", "工序同資源")
                    panelDescription: root.tr("Apply the active parallel-job limit and save CPU and scratch-space preferences for future scheduling.",
                                              "套用而家嘅平行工序上限，並儲存 CPU 同暫存空間偏好，留畀之後嘅排程支援使用。")

                    SettingsValueRow {
                        title: root.tr("Maximum parallel jobs", "最多平行工序")
                        description: root.tr("Applies to independent mount, package and validation work.",
                                             "套用於獨立嘅掛載、套件同驗證工作。")
                        SpinBox {
                            Layout.preferredWidth: 130
                            Layout.preferredHeight: DesignTokens.controlHeight
                            from: 1
                            to: 16
                            value: root.app.maxParallelJobs
                            editable: true
                            Accessible.name: root.tr("Maximum parallel jobs", "最多平行工序")
                            onValueModified: root.app.maxParallelJobs = value
                        }
                    }

                    SettingsValueRow {
                        title: root.tr("CPU thread ceiling", "CPU 執行緒上限")
                        description: root.tr("Saved for future scheduling; current external tool launches do not use this preference.",
                                             "呢個偏好會儲存，留畀之後嘅排程支援使用；而家啟動外部工具時未會套用。")
                        RowLayout {
                            Layout.preferredWidth: 270
                            Slider {
                                Layout.fillWidth: true
                                from: 1
                                to: Math.max(2, root.app.logicalCpuCount)
                                stepSize: 1
                                value: root.app.threadLimit
                                Accessible.name: root.tr("CPU thread ceiling", "CPU 執行緒上限")
                                onMoved: root.app.threadLimit = Math.round(value)
                            }
                            Label {
                                text: root.app.threadLimit + " " + root.tr("threads", "執行緒")
                                color: root.surfaceForeground
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 11
                            }
                        }
                    }

                    SettingsValueRow {
                        title: root.tr("Scratch-space reserve", "暫存空間保留")
                        description: root.tr("Saved as a future planning target; the current planner does not pause jobs at this threshold.",
                                             "呢個門檻會儲存做之後嘅規劃目標；而家嘅 planner 未會喺跌到呢個數值時暫停工序。")
                        SpinBox {
                            Layout.preferredWidth: 150
                            Layout.preferredHeight: DesignTokens.controlHeight
                            from: 5
                            to: 500
                            value: root.app.scratchReserveGb
                            editable: true
                            textFromValue: value => value + " GB"
                            Accessible.name: root.tr("Scratch-space reserve in gigabytes", "暫存空間保留 GB")
                            onValueModified: root.app.scratchReserveGb = value
                        }
                    }
                }

                SettingsPanel {
                    panelTitle: root.tr("Safety", "安全")
                    panelDescription: root.tr("Keep destructive servicing work traceable, verifiable and recoverable.",
                                              "令破壞性維護工作可以追蹤、驗證同復原。")

                    SettingsToggle {
                        title: root.tr("Crash journal", "死機日誌")
                        description: root.tr("Saves this preference for future journal policy; the current job engine maintains its recovery journal independently.",
                                             "會儲存呢個偏好，留畀之後嘅 journal policy；而家 job engine 會獨立維護復原 journal。")
                        checked: root.app.crashJournalEnabled
                        onToggled: checked => root.app.crashJournalEnabled = checked
                    }
                    SettingsToggle {
                        title: root.tr("Verify source hash", "驗證來源 hash")
                        description: root.tr("Compute SHA-256 before writes and reject a mismatch when an expected hash is configured.",
                                             "寫入前計算 SHA-256；有設定預期 hash 時，唔一致就會拒絕繼續。")
                        checked: root.app.verifySourceHash
                        onToggled: checked => root.app.verifySourceHash = checked
                    }
                    SettingsToggle {
                        title: root.tr("Future checkpoint policy", "日後檢查點 policy")
                        description: root.tr("Saves checkpoint intent for future enforcement; current runs do not create a Git checkpoint from this preference.",
                                             "會儲存檢查點意向，留畀日後實作；而家執行唔會因為呢個偏好自動建立 Git 檢查點。")
                        checked: root.app.checkpointBeforeDestructive
                        onToggled: checked => root.app.checkpointBeforeDestructive = checked
                    }
                    SettingsValueRow {
                        title: root.tr("Recoverable notification deletion", "可復原通知刪除")
                        description: root.tr("Deleting a notification writes a tombstone so its history remains recoverable.",
                                             "刪除通知會寫入墓碑記錄，保留可以復原嘅歷史。")
                        WfStatusChip { dark: root.dark; tone: "success"; text: root.tr("NOTIFICATIONS", "通知") }
                    }
                }

                SettingsPanel {
                    panelTitle: root.tr("Automation", "自動化")
                    panelDescription: root.tr("Coordinate portable project configuration with external editors and Git commits.",
                                              "配合外部編輯器同 Git commit 管理可攜工程設定。")

                    SettingsToggle {
                        title: root.tr("Watch project config", "監察工程設定")
                        description: root.tr("Import validated external changes when the project configuration updates.",
                                             "工程設定更新時，匯入通過驗證嘅外部變更。")
                        checked: root.app.autoImport
                        onToggled: checked => root.app.autoImport = checked
                    }
                    SettingsToggle {
                        title: root.tr("Export after every commit", "每次 commit 後匯出")
                        description: root.tr("Write a complete .wimforge project bundle whenever project history advances.",
                                             "工程歷史每次推進時，寫出完整 .wimforge 工程 bundle。")
                        checked: root.app.autoExport
                        onToggled: checked => root.app.autoExport = checked
                    }
                    SettingsValueRow {
                        title: root.tr("Export destination", "匯出目的地")
                        description: root.tr("Choose a .wimforge path before enabling automatic export; other extensions are rejected.",
                                             "啟用自動匯出之前先揀 .wimforge 路徑；其他副檔名會被拒絕。")
                        WfField {
                            Layout.preferredWidth: 320
                            dark: root.dark
                            mono: true
                            text: root.app.autoExportPath
                            readOnly: !root.app.projectLoaded
                            placeholderText: root.tr("Auto-export destination", "自動匯出目的地")
                            onEditingFinished: root.app.autoExportPath = text
                        }
                        WfButton {
                            dark: root.dark
                            compact: true
                            variant: "tonal"
                            text: root.tr("Browse…", "瀏覽……")
                            Accessible.name: root.tr("Browse for the automatic project export destination", "瀏覽自動工程匯出目的地")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            enabled: root.app.projectLoaded
                            onClicked: autoExportDialog.open()
                        }
                    }
                }

                SettingsPanel {
                    panelTitle: root.tr("Diagnostics", "診斷")
                    panelDescription: root.tr("Open structured local logs with Windows and inspect Git-backed notification history in WimForge.",
                                               "用 Windows 開結構化本機日誌，並喺 WimForge 檢查 Git-backed 通知歷史。")

                    SettingsValueRow {
                        title: root.tr("Structured logging", "結構化記錄")
                        description: root.tr("Best-effort JSONL logging rotates and redacts secret-like values when startup succeeds.",
                                             "JSONL 記錄會盡力啟動；成功後會自動輪替同遮蔽似密碼嘅值。")
                        WfStatusChip { dark: root.dark; tone: "neutral"; text: root.tr("BEST EFFORT", "盡力啟用") }
                    }
                    SettingsValueRow {
                        title: root.tr("Current log", "而家嘅 log")
                        description: root.app.applicationLogPath
                        descriptionMono: true
                        RowLayout {
                            spacing: DesignTokens.spacing8
                            WfButton {
                                dark: root.dark
                                variant: "outlined"
                                text: root.tr("Open log", "開日誌")
                                onClicked: root.app.openApplicationLog()
                            }
                            WfButton {
                                dark: root.dark
                                variant: "outlined"
                                text: root.tr("Open folder", "開資料夾")
                                onClicked: root.app.openApplicationLogFolder()
                            }
                        }
                    }
                    SettingsValueRow {
                        title: root.tr("Notification history", "通知歷史")
                        description: root.tr("Every read, dismiss and delete action is committed to its local repository.",
                                             "每次已讀、閂埋同刪除動作都會 commit 去本機 repository。")
                        WfButton {
                            dark: root.dark
                            variant: "tonal"
                            text: root.tr("Send test notification", "發測試通知")
                            onClicked: root.app.sendTestNotification()
                        }
                    }
                    SettingsValueRow {
                        title: root.tr("Notification repository", "通知 repository")
                        description: root.app.notificationRepoPath
                        descriptionMono: true
                        WfStatusChip { dark: root.dark; tone: "neutral"; text: root.tr("LOCAL GIT", "本機 GIT") }
                    }
                }
            }
        }
    }

    component SettingsPanel: ScrollView {
        id: settingsPanel
        required property string panelTitle
        required property string panelDescription
        default property alias rows: panelRows.data
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        contentWidth: availableWidth

        WfCard {
            width: settingsPanel.availableWidth
            dark: root.dark
            surfaceLevel: "lowest"
            padding: DesignTokens.spacing20
            ColumnLayout {
                id: panelRows
                anchors.fill: parent
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    text: settingsPanel.panelTitle
                    color: root.surfaceForeground
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 20
                    font.weight: Font.Bold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: DesignTokens.spacing4
                    Layout.bottomMargin: DesignTokens.spacing12
                    text: settingsPanel.panelDescription
                    color: root.surfaceVariantForeground
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    component SettingsValueRow: Item {
        id: valueRow
        required property string title
        required property string description
        property bool descriptionMono: false
        default property alias action: valueAction.data
        Layout.fillWidth: true
        implicitHeight: Math.max(72, valueLayout.implicitHeight + DesignTokens.spacing16)

        ColumnLayout {
            anchors.fill: parent
            spacing: 0
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.outlineVariant }
            GridLayout {
                id: valueLayout
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: DesignTokens.spacing8
                Layout.bottomMargin: DesignTokens.spacing8
                columns: root.width >= 720 ? 2 : 1
                columnSpacing: DesignTokens.spacing20
                rowSpacing: DesignTokens.spacing8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spacing4
                    Label {
                        Layout.fillWidth: true
                        text: valueRow.title
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: valueRow.description
                        color: root.surfaceVariantForeground
                        font.family: valueRow.descriptionMono ? DesignTokens.fontMono : DesignTokens.fontBody
                        font.pixelSize: valueRow.descriptionMono ? 10 : 11
                        wrapMode: valueRow.descriptionMono ? Text.WrapAnywhere : Text.Wrap
                    }
                }
                RowLayout {
                    id: valueAction
                    Layout.fillWidth: root.width < 720
                    Layout.alignment: root.width >= 720 ? Qt.AlignRight | Qt.AlignVCenter : Qt.AlignLeft
                    spacing: DesignTokens.spacing8
                }
            }
        }
    }

    component SettingsToggle: Item {
        id: toggleRow
        required property string title
        required property string description
        property bool checked: false
        signal toggled(bool checked)
        Layout.fillWidth: true
        implicitHeight: Math.max(72, toggleLayout.implicitHeight + DesignTokens.spacing16)

        ColumnLayout {
            anchors.fill: parent
            spacing: 0
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.outlineVariant }
            GridLayout {
                id: toggleLayout
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: DesignTokens.spacing8
                Layout.bottomMargin: DesignTokens.spacing8
                columns: 2
                columnSpacing: DesignTokens.spacing20
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spacing4
                    Label {
                        Layout.fillWidth: true
                        text: toggleRow.title
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: toggleRow.description
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }
                Switch {
                    checked: toggleRow.checked
                    Accessible.name: toggleRow.title
                    onToggled: toggleRow.toggled(checked)
                }
            }
        }
    }
}
