pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import "../components"

Item {
    id: root
    required property var app
    required property var tr

    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light

    // Look up the current value of a typed answer-file setting so convenience
    // cards can reflect the live profile that the generic editor shares.
    function settingValue(pass, component, path) {
        var list = root.app.unattendedSettings
        for (var i = 0; i < list.length; i++) {
            if (list[i].pass === pass && list[i].component === component && list[i].path === path)
                return list[i].value
        }
        return ""
    }
    function settingChecked(pass, component, path) {
        return root.settingValue(pass, component, path) === "true"
    }
    readonly property color surfaceLowest: DesignTokens.surfaceLowest(root.dark)
    readonly property color surfaceLow: DesignTokens.surfaceLow(root.dark)
    readonly property color surfaceContainer: DesignTokens.surfaceContainer(root.dark)
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)
    readonly property color surfaceVariantForeground: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color outlineVariant: DesignTokens.outlineVariant(root.dark)
    readonly property color primary: DesignTokens.primary(root.dark)
    readonly property color primaryContainer: DesignTokens.primaryContainer(root.dark)
    readonly property color primaryContainerForeground: DesignTokens.onPrimaryContainer(root.dark)
    readonly property color tertiaryContainer: DesignTokens.tertiaryContainer(root.dark)
    readonly property color tertiaryContainerForeground: DesignTokens.onTertiaryContainer(root.dark)

    ScrollView {
        id: pageScroll
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: pageScroll.availableWidth
            spacing: DesignTokens.spacing12

            RowLayout {
                Layout.fillWidth: true
                spacing: DesignTokens.spacing16

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Unattended Windows Studio", "Windows 無人值守工房")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 26
                        font.weight: Font.Bold
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("All seven setup passes, XML and JSON round-trips, generic schema paths, templates, and validated computer-name behavior.",
                                      "七個 setup pass、XML／JSON 來回匯入匯出、通用 schema 路徑、範本，同埋驗證過嘅電腦名行為。")
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignTop
                    spacing: DesignTokens.spacing8
                    WfButton {
                        dark: root.dark
                        variant: "outlined"
                        text: root.tr("Full automation", "全自動")
                        onClicked: root.app.loadUnattendedTemplate("full-automation")
                    }
                    WfButton {
                        dark: root.dark
                        variant: "filled"
                        text: root.tr("AI development", "AI 開發")
                        onClicked: root.app.loadUnattendedTemplate("ai-development")
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8

                    Label {
                        text: root.tr("Computer name", "電腦名")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 720 ? 3 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                text: root.tr("Naming mode", "命名模式")
                                color: root.surfaceVariantForeground
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            ComboBox {
                                id: nameMode
                                Layout.fillWidth: true
                                Layout.preferredHeight: DesignTokens.controlHeight
                                model: [root.tr("Random generated", "隨機產生"), root.tr("Fixed", "固定"), "[Prompt]", root.tr("Firmware serial", "韌體序號")]
                                currentIndex: root.app.computerNameMode
                                Accessible.name: root.tr("Computer name mode", "電腦名模式")
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                text: root.tr("Name / prefix", "名稱／前綴")
                                color: root.surfaceVariantForeground
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            TextField {
                                id: computerName
                                Layout.fillWidth: true
                                Layout.preferredHeight: DesignTokens.controlHeight
                                enabled: nameMode.currentIndex === 1 || nameMode.currentIndex === 3
                                text: root.app.computerNameValue
                                placeholderText: nameMode.currentIndex === 3 ? root.tr("Optional prefix", "可選前綴") : root.tr("Up to 15 bytes", "最多 15 bytes")
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 12
                                selectByMouse: true
                                onAccepted: root.app.setComputerNameBehavior(nameMode.currentIndex, text)
                            }
                        }
                        WfButton {
                            Layout.fillWidth: root.width < 720
                            Layout.alignment: Qt.AlignBottom
                            dark: root.dark
                            variant: "tonal"
                            text: root.tr("Apply", "套用")
                            onClicked: root.app.setComputerNameBehavior(nameMode.currentIndex, computerName.text)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: nameMode.currentIndex === 2
                            ? root.tr("[Prompt] gives Windows a valid temporary name, then pauses that target's first-logon session until a valid name is entered. WimForge and its job queue keep running; the literal [Prompt] is never written to ComputerName.",
                                      "[Prompt] 會先俾 Windows 一個合法臨時名，再喺目標機首次登入時停低問名。WimForge 同工序隊會繼續運作；絕對唔會將字面 [Prompt] 寫入 ComputerName。")
                            : root.tr("Microsoft naming limits are validated before XML is committed.", "commit XML 前會先驗證 Microsoft 命名限制。")
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("Microsoft-published installation keys", "Microsoft 公開安裝 key")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 680 ? 2 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8
                        ComboBox {
                            id: productKey
                            Layout.fillWidth: true
                            Layout.preferredHeight: DesignTokens.controlHeight
                            model: root.app.microsoftProductKeys
                            textRole: "edition"
                            Accessible.name: root.tr("Windows edition installation key", "Windows 版本安裝 key")
                        }
                        WfButton {
                            Layout.fillWidth: root.width < 680
                            dark: root.dark
                            variant: "outlined"
                            text: root.tr("Use for edition selection", "用嚟揀版本")
                            enabled: productKey.currentIndex >= 0
                            onClicked: root.app.setUnattendedValue("windowsPE", "Microsoft-Windows-Setup", "UserData/ProductKey/Key", productKey.model[productKey.currentIndex].key)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: productKey.currentIndex >= 0
                            ? productKey.model[productKey.currentIndex].key + "  ·  " + productKey.model[productKey.currentIndex].licensingNotice
                            : ""
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontMono
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("Accessibility", "無障礙")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spacing12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: root.tr("Start Narrator at sign-in", "登入時啟動 Narrator")
                                color: root.surfaceForeground
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Adds an oobeSystem first-logon command that enables the Narrator screen reader on the sign-in desktop.",
                                              "會加一條 oobeSystem 首次登入指令，喺登入畫面啟用 Narrator 螢幕朗讀。")
                                color: root.surfaceVariantForeground
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }
                        Switch {
                            checked: root.app.unattendedNarratorAutostart
                            onToggled: root.app.setUnattendedNarratorAutostart(checked)
                            Accessible.name: root.tr("Start Narrator at sign-in", "登入時啟動 Narrator")
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("Out-of-box experience (OOBE)", "開箱體驗（OOBE）")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Hide OOBE screens in the oobeSystem pass of Microsoft-Windows-Shell-Setup.",
                                      "喺 Microsoft-Windows-Shell-Setup 嘅 oobeSystem 階段隱藏 OOBE 畫面。")
                        color: root.surfaceVariantForeground
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 720 ? 2 : 1
                        columnSpacing: DesignTokens.spacing16
                        rowSpacing: DesignTokens.spacing4
                        Repeater {
                            model: [
                                { en: "Hide EULA page", zh: "隱藏授權條款頁", path: "OOBE/HideEULAPage" },
                                { en: "Hide OEM registration", zh: "隱藏 OEM 註冊頁", path: "OOBE/HideOEMRegistrationScreen" },
                                { en: "Skip Microsoft account sign-in", zh: "跳過 Microsoft 帳戶登入", path: "OOBE/HideOnlineAccountScreens" },
                                { en: "Hide local account screen", zh: "隱藏本機帳戶頁", path: "OOBE/HideLocalAccountScreen" },
                                { en: "Hide wireless setup", zh: "隱藏無線網絡設定", path: "OOBE/HideWirelessSetupInOOBE" }
                            ]
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: DesignTokens.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    text: root.tr(modelData.en, modelData.zh)
                                    color: root.surfaceForeground
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                Switch {
                                    checked: root.settingChecked("oobeSystem", "Microsoft-Windows-Shell-Setup", modelData.path)
                                    onToggled: root.app.setUnattendedValue("oobeSystem", "Microsoft-Windows-Shell-Setup",
                                                                           modelData.path, checked ? "true" : "false")
                                    Accessible.name: root.tr(modelData.en, modelData.zh)
                                }
                            }
                        }
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 720 ? 2 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                text: root.tr("Network location", "網絡位置")
                                color: root.surfaceVariantForeground
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            ComboBox {
                                id: networkLocation
                                Layout.fillWidth: true
                                Layout.preferredHeight: DesignTokens.controlHeight
                                readonly property var values: ["", "Home", "Work", "Other"]
                                model: [root.tr("Not set", "未設定"), "Home", "Work", "Other"]
                                currentIndex: Math.max(0, values.indexOf(
                                    root.settingValue("oobeSystem", "Microsoft-Windows-Shell-Setup", "OOBE/NetworkLocation")))
                                onActivated: function(index) {
                                    if (index <= 0)
                                        root.app.clearUnattendedValue("oobeSystem", "Microsoft-Windows-Shell-Setup", "OOBE/NetworkLocation")
                                    else
                                        root.app.setUnattendedValue("oobeSystem", "Microsoft-Windows-Shell-Setup", "OOBE/NetworkLocation", values[index])
                                }
                                Accessible.name: root.tr("Network location", "網絡位置")
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                text: root.tr("Protect your PC (updates)", "保護電腦（更新）")
                                color: root.surfaceVariantForeground
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            ComboBox {
                                id: protectYourPc
                                Layout.fillWidth: true
                                Layout.preferredHeight: DesignTokens.controlHeight
                                readonly property var values: ["", "1", "2", "3"]
                                model: [root.tr("Not set", "未設定"),
                                        root.tr("Recommended settings (1)", "建議設定 (1)"),
                                        root.tr("Important updates only (2)", "只裝重要更新 (2)"),
                                        root.tr("Off (3)", "關閉 (3)")]
                                currentIndex: Math.max(0, values.indexOf(
                                    root.settingValue("oobeSystem", "Microsoft-Windows-Shell-Setup", "OOBE/ProtectYourPC")))
                                onActivated: function(index) {
                                    if (index <= 0)
                                        root.app.clearUnattendedValue("oobeSystem", "Microsoft-Windows-Shell-Setup", "OOBE/ProtectYourPC")
                                    else
                                        root.app.setUnattendedValue("oobeSystem", "Microsoft-Windows-Shell-Setup", "OOBE/ProtectYourPC", values[index])
                                }
                                Accessible.name: root.tr("Protect your PC", "保護電腦")
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("Regional and language", "地區同語言")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Microsoft-Windows-International-Core, oobeSystem pass. Use BCP-47 tags such as en-US, zh-HK, or keyboard IDs such as 0409:00000409.",
                                      "Microsoft-Windows-International-Core，oobeSystem 階段。用 BCP-47 標籤例如 en-US、zh-HK，或鍵盤 ID 例如 0409:00000409。")
                        color: root.surfaceVariantForeground
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 720 ? 2 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8
                        Repeater {
                            model: [
                                { en: "UI language", zh: "介面語言", path: "UILanguage", ph: "en-US" },
                                { en: "System locale", zh: "系統地區", path: "SystemLocale", ph: "en-US" },
                                { en: "User locale", zh: "使用者地區", path: "UserLocale", ph: "zh-HK" },
                                { en: "Input locale (keyboard)", zh: "輸入地區（鍵盤）", path: "InputLocale", ph: "0409:00000409" }
                            ]
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 4
                                Label {
                                    text: root.tr(modelData.en, modelData.zh)
                                    color: root.surfaceVariantForeground
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: DesignTokens.controlHeight
                                    text: root.settingValue("oobeSystem", "Microsoft-Windows-International-Core", modelData.path)
                                    placeholderText: modelData.ph
                                    font.family: DesignTokens.fontMono
                                    font.pixelSize: 12
                                    onEditingFinished: {
                                        if (text.trim().length === 0)
                                            root.app.clearUnattendedValue("oobeSystem", "Microsoft-Windows-International-Core", modelData.path)
                                        else
                                            root.app.setUnattendedValue("oobeSystem", "Microsoft-Windows-International-Core", modelData.path, text.trim())
                                    }
                                    Accessible.name: root.tr(modelData.en, modelData.zh)
                                }
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("Owner, time zone and licensing", "擁有者、時區同授權")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 720 ? 2 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8
                        Repeater {
                            model: [
                                { en: "Registered owner", zh: "註冊擁有者", path: "RegisteredOwner", ph: "WimForge user" },
                                { en: "Registered organization", zh: "註冊機構", path: "RegisteredOrganization", ph: "Contoso" },
                                { en: "Time zone", zh: "時區", path: "TimeZone", ph: "China Standard Time" }
                            ]
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 4
                                Label {
                                    text: root.tr(modelData.en, modelData.zh)
                                    color: root.surfaceVariantForeground
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: DesignTokens.controlHeight
                                    text: root.settingValue("specialize", "Microsoft-Windows-Shell-Setup", modelData.path)
                                    placeholderText: modelData.ph
                                    font.pixelSize: 12
                                    onEditingFinished: {
                                        if (text.trim().length === 0)
                                            root.app.clearUnattendedValue("specialize", "Microsoft-Windows-Shell-Setup", modelData.path)
                                        else
                                            root.app.setUnattendedValue("specialize", "Microsoft-Windows-Shell-Setup", modelData.path, text.trim())
                                    }
                                    Accessible.name: root.tr(modelData.en, modelData.zh)
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spacing16
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: DesignTokens.spacing8
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Copy profile (apply default-user tweaks)", "複製設定檔（套用預設使用者調整）")
                                color: root.surfaceForeground
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                            Switch {
                                checked: root.settingChecked("specialize", "Microsoft-Windows-Shell-Setup", "CopyProfile")
                                onToggled: root.app.setUnattendedValue("specialize", "Microsoft-Windows-Shell-Setup",
                                                                       "CopyProfile", checked ? "true" : "false")
                                Accessible.name: root.tr("Copy profile", "複製設定檔")
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: DesignTokens.spacing8
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Skip automatic activation", "跳過自動啟用")
                                color: root.surfaceForeground
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                            Switch {
                                checked: root.settingChecked("specialize", "Microsoft-Windows-Security-SPP", "SkipAutoActivation")
                                onToggled: root.app.setUnattendedValue("specialize", "Microsoft-Windows-Security-SPP",
                                                                       "SkipAutoActivation", checked ? "true" : "false")
                                Accessible.name: root.tr("Skip automatic activation", "跳過自動啟用")
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("OEM information", "OEM 資料")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Shown in Settings › System › About. Microsoft-Windows-Shell-Setup/OEMInformation, specialize pass.",
                                      "會喺設定 › 系統 › 關於顯示。Microsoft-Windows-Shell-Setup/OEMInformation，specialize 階段。")
                        color: root.surfaceVariantForeground
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 720 ? 2 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8
                        Repeater {
                            model: [
                                { en: "Manufacturer", zh: "製造商", path: "OEMInformation/Manufacturer", ph: "Contoso" },
                                { en: "Model", zh: "型號", path: "OEMInformation/Model", ph: "Studio 15" },
                                { en: "Support phone", zh: "支援電話", path: "OEMInformation/SupportPhone", ph: "+852 1234 5678" },
                                { en: "Support hours", zh: "支援時間", path: "OEMInformation/SupportHours", ph: "Mon-Fri 9-6" },
                                { en: "Support URL", zh: "支援網址", path: "OEMInformation/SupportURL", ph: "https://support.contoso.com" }
                            ]
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 4
                                Label {
                                    text: root.tr(modelData.en, modelData.zh)
                                    color: root.surfaceVariantForeground
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: DesignTokens.controlHeight
                                    text: root.settingValue("specialize", "Microsoft-Windows-Shell-Setup", modelData.path)
                                    placeholderText: modelData.ph
                                    font.pixelSize: 12
                                    onEditingFinished: {
                                        if (text.trim().length === 0)
                                            root.app.clearUnattendedValue("specialize", "Microsoft-Windows-Shell-Setup", modelData.path)
                                        else
                                            root.app.setUnattendedValue("specialize", "Microsoft-Windows-Shell-Setup", modelData.path, text.trim())
                                    }
                                    Accessible.name: root.tr(modelData.en, modelData.zh)
                                }
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("Any unattend setting", "任何 unattend 設定")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("The generic editor preserves components and repeated XML paths that are not yet represented by a convenience card.",
                                      "未有方便卡片嘅 component 同重複 XML 路徑，都會由通用編輯器原樣保留。")
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 1000 ? 5 : root.width >= 700 ? 2 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8

                        ComboBox {
                            id: setupPass
                            Layout.fillWidth: true
                            Layout.preferredHeight: DesignTokens.controlHeight
                            model: ["windowsPE", "offlineServicing", "generalize", "specialize", "auditSystem", "auditUser", "oobeSystem"]
                            Accessible.name: root.tr("Setup pass", "Setup pass")
                        }
                        TextField {
                            id: component
                            Layout.fillWidth: true
                            Layout.preferredHeight: DesignTokens.controlHeight
                            placeholderText: "Microsoft-Windows-Shell-Setup"
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 11
                        }
                        TextField {
                            id: settingPath
                            Layout.fillWidth: true
                            Layout.preferredHeight: DesignTokens.controlHeight
                            placeholderText: "OOBE/HideEULAPage"
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 11
                        }
                        TextField {
                            id: settingValue
                            Layout.fillWidth: true
                            Layout.preferredHeight: DesignTokens.controlHeight
                            placeholderText: root.tr("Value", "值")
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 11
                            onAccepted: root.app.setUnattendedValue(setupPass.currentText, component.text, settingPath.text, text)
                        }
                        WfButton {
                            Layout.fillWidth: root.width < 1000
                            dark: root.dark
                            variant: "filled"
                            text: root.tr("Add / update", "加入／更新")
                            onClicked: root.app.setUnattendedValue(setupPass.currentText, component.text, settingPath.text, settingValue.text)
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                Layout.preferredHeight: 260
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing8

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    GridLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        Layout.rightMargin: 12
                        Layout.preferredHeight: 32
                        columns: 4
                        columnSpacing: DesignTokens.spacing12
                        Label { Layout.fillWidth: true; Layout.preferredWidth: 110; text: root.tr("PASS", "階段"); color: root.surfaceVariantForeground; font.pixelSize: 10; font.weight: Font.Bold; font.letterSpacing: 0.8 }
                        Label { Layout.fillWidth: true; Layout.preferredWidth: 245; text: root.tr("COMPONENT", "COMPONENT"); color: root.surfaceVariantForeground; font.pixelSize: 10; font.weight: Font.Bold; font.letterSpacing: 0.8 }
                        Label { Layout.fillWidth: true; text: root.tr("PATH", "路徑"); color: root.surfaceVariantForeground; font.pixelSize: 10; font.weight: Font.Bold; font.letterSpacing: 0.8 }
                        Label { Layout.fillWidth: true; Layout.preferredWidth: 180; text: root.tr("VALUE", "值"); color: root.surfaceVariantForeground; font.pixelSize: 10; font.weight: Font.Bold; font.letterSpacing: 0.8 }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.outlineVariant }
                    ListView {
                        id: settingList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: root.app.unattendedSettings
                        delegate: ItemDelegate {
                            required property var modelData
                            width: settingList.width
                            height: 42
                            leftPadding: 12
                            rightPadding: 12
                            Accessible.name: modelData.pass + ", " + modelData.component + ", " + modelData.path + ", " + modelData.value
                            contentItem: GridLayout {
                                columns: 4
                                columnSpacing: DesignTokens.spacing12
                                Label { Layout.fillWidth: true; Layout.preferredWidth: 110; text: modelData.pass; color: root.primary; font.family: DesignTokens.fontMono; font.pixelSize: 10; elide: Text.ElideRight }
                                Label { Layout.fillWidth: true; Layout.preferredWidth: 245; text: modelData.component; color: root.surfaceForeground; font.family: DesignTokens.fontMono; font.pixelSize: 10; elide: Text.ElideMiddle }
                                Label { Layout.fillWidth: true; text: modelData.path; color: root.surfaceVariantForeground; font.family: DesignTokens.fontMono; font.pixelSize: 10; elide: Text.ElideMiddle }
                                Label { Layout.fillWidth: true; Layout.preferredWidth: 180; text: modelData.value; color: root.surfaceForeground; font.family: DesignTokens.fontMono; font.pixelSize: 10; elide: Text.ElideRight }
                            }
                            background: Rectangle {
                                color: hovered ? root.surfaceContainer : "transparent"
                                border.color: root.outlineVariant
                                border.width: 0
                                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: root.outlineVariant }
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                fillColor: root.primaryContainer
                outlineColor: root.primary
                padding: DesignTokens.spacing12

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("OpenCode fill-in helper", "OpenCode 自動填寫助手")
                        color: root.primaryContainerForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: root.width >= 760 ? 3 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing8
                        TextField {
                            id: aiIntent
                            Layout.fillWidth: true
                            Layout.preferredHeight: DesignTokens.controlHeight
                            placeholderText: root.tr("Example: Hong Kong locale, Toronto time zone, local account, skip consumer OOBE…",
                                                     "例如：香港地區、Toronto 時區、本機帳戶、跳過消費者 OOBE…")
                            onAccepted: if (text.trim().length > 0 && !root.app.openCodeBusy)
                                            root.app.askOpenCodeToFillUnattended(text)
                        }
                        WfButton {
                            Layout.fillWidth: root.width < 760
                            dark: root.dark
                            variant: "filled"
                            text: root.tr("Validate & fill", "驗證同填寫")
                            enabled: aiIntent.text.trim().length > 0 && !root.app.openCodeBusy
                            onClicked: root.app.askOpenCodeToFillUnattended(aiIntent.text)
                        }
                        BusyIndicator {
                            visible: root.app.openCodeBusy
                            running: visible
                            implicitWidth: 28
                            implicitHeight: 28
                            Accessible.name: root.tr("OpenCode validation in progress", "OpenCode 驗證進行中")
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.app.openCodeStatus
                        color: root.primaryContainerForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                dark: root.dark
                outlined: true
                surfaceLevel: "low"
                            padding: DesignTokens.spacing12

                GridLayout {
                    anchors.fill: parent
                    columns: root.width >= 760 ? 4 : 1
                    columnSpacing: DesignTokens.spacing8
                    rowSpacing: DesignTokens.spacing8
                    Label {
                        text: "XML / JSON"
                        color: root.surfaceForeground
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: DesignTokens.spacing4
                        TextField {
                            id: answerPath
                            Layout.fillWidth: true
                            Layout.preferredHeight: DesignTokens.controlHeight
                            placeholderText: "D:\\profiles\\autounattend.xml"
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 11
                            selectByMouse: true
                        }
                        WfButton {
                            dark: root.dark
                            variant: "tonal"
                            text: root.tr("Browse…", "瀏覽…")
                            Accessible.name: root.tr("Browse for an answer file", "瀏覽答案檔")
                            onClicked: openAnswerFileDialog.open()
                        }
                    }
                    WfButton {
                        Layout.fillWidth: root.width < 760
                        dark: root.dark
                        variant: "outlined"
                        text: root.tr("Import", "匯入")
                        enabled: answerPath.text.trim().length > 0
                        onClicked: root.app.importUnattended(answerPath.text)
                    }
                    WfButton {
                        Layout.fillWidth: root.width < 760
                        dark: root.dark
                        variant: "outlined"
                        text: root.tr("Export…", "匯出…")
                        onClicked: saveAnswerFileDialog.open()
                    }
                }
            }

            Item { Layout.preferredHeight: DesignTokens.spacing16 }
        }
    }

    FileDialog {
        id: openAnswerFileDialog
        title: root.tr("Choose an answer file", "揀一個答案檔")
        fileMode: FileDialog.OpenFile
        nameFilters: [root.tr("Answer files (*.xml *.json)", "答案檔 (*.xml *.json)"),
                      root.tr("All files (*)", "所有檔案 (*)")]
        onAccepted: {
            answerPath.text = root.app.pathFromUrl(selectedFile)
            root.app.importUnattended(answerPath.text)
        }
    }

    FileDialog {
        id: saveAnswerFileDialog
        title: root.tr("Export answer file", "匯出答案檔")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "xml"
        nameFilters: [root.tr("Answer file XML (*.xml)", "答案檔 XML (*.xml)"),
                      root.tr("Profile JSON (*.json)", "設定檔 JSON (*.json)")]
        onAccepted: {
            answerPath.text = root.app.pathFromUrl(selectedFile)
            root.app.exportUnattended(answerPath.text)
        }
    }
}
