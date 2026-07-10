import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var app
    property var tr: function(en, zh) { return en }
    property int currentSection: 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        Label { text: root.tr("Customize the image", "調校個映像"); font.pixelSize: 30; font.weight: Font.Bold }
        Label {
            Layout.fillWidth: true
            text: root.tr("Everything here becomes declarative config first, a Git commit second, and a servicing command only after review.",
                          "呢度每樣嘢都係先寫入設定、再 Git commit，最後睇清楚計劃先變成維護指令。三重保險，穩過茶記凍奶茶。")
            wrapMode: Text.Wrap
            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
            RowLayout {
                spacing: 8
                Repeater {
                    model: [
                        {i:"◈", en:"Updates", zh:"更新"}, {i:"⌁", en:"Drivers", zh:"驅動程式"},
                        {i:"◆", en:"Features", zh:"功能"}, {i:"▦", en:"Apps", zh:"App"},
                        {i:"♜", en:"Components", zh:"元件"}, {i:"⚙", en:"Settings", zh:"設定"},
                        {i:"⌘", en:"Unattended", zh:"無人值守"}, {i:"▷", en:"Post-setup", zh:"裝完再做"}
                    ]
                    delegate: Button {
                        required property var modelData
                        required property int index
                        text: modelData.i + "  " + root.tr(modelData.en, modelData.zh)
                        highlighted: root.currentSection === index
                        flat: root.currentSection !== index
                        onClicked: root.currentSection = index
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.currentSection

            ConfigList {
                title: root.tr("Update & language packages", "更新同語言套件")
                subtitle: root.tr("CAB, MSU, language packs, FOD and enablement packages. Dependency order is calculated at run time.",
                                  "CAB、MSU、語言包、FOD 同啟用套件。開工嗰陣會自動排依賴次序。")
                placeholder: "D:\\updates\\KB123456.msu"
                items: app.packages
                addAction: value => app.addListItem("packages", value)
                removeAction: index => app.removeListItem("packages", index)
            }
            ConfigList {
                title: root.tr("INF drivers & hardware profiles", "INF 驅動程式同硬件設定檔")
                subtitle: root.tr("Add a single INF, a recursive driver folder, or import the current machine's third-party drivers.",
                                  "加單一 INF、成個驅動資料夾，或者抽返呢部機嘅第三方驅動。")
                placeholder: "D:\\drivers\\WiFi\\netadapter.inf"
                items: app.drivers
                addAction: value => app.addListItem("drivers", value)
                removeAction: index => app.removeListItem("drivers", index)
                extraActionText: root.tr("Import host drivers", "抽本機驅動")
                extraAction: () => app.importHostDrivers()
            }
            FeatureGrid { app: root.app; tr: root.tr }
            ConfigList {
                title: root.tr("Provisioned apps", "預載 App")
                subtitle: root.tr("Remove provisioned Appx/MSIX packages by package name, or add signed app bundles with dependencies.",
                                  "按套件名移除預載 Appx/MSIX，或者連依賴一齊加已簽署 App bundle。")
                placeholder: "Microsoft.BingNews_8wekyb3d8bbwe"
                items: app.appRemovals
                addAction: value => app.addListItem("appRemovals", value)
                removeAction: index => app.removeListItem("appRemovals", index)
            }
            ConfigList {
                title: root.tr("Components & scheduled tasks", "元件同排程工作")
                subtitle: root.tr("Removal is compatibility-aware. Red entries need an explicit override and a recovery checkpoint.",
                                  "移除會睇埋相容性。紅色項目要你親自解鎖，仲要先落復原檢查點，唔好一時手痕。")
                placeholder: root.tr("Component identity or scheduled task path", "元件 identity 或排程工作路徑")
                items: app.componentRemovals
                addAction: value => app.addListItem("componentRemovals", value)
                removeAction: index => app.removeListItem("componentRemovals", index)
            }
            SettingsGrid { app: root.app; tr: root.tr }
            ConfigList {
                title: root.tr("Unattended answer file", "無人值守答案檔")
                subtitle: root.tr("Apply an existing unattend.xml or generate setup-pass settings for OOBE, locale, accounts, edition, OEM and privacy.",
                                  "套用現成 unattend.xml，或者產生 OOBE、語言、帳戶、版本、OEM 同私隱設定。")
                placeholder: "D:\\profiles\\autounattend.xml"
                items: app.unattendFiles
                addAction: value => app.addListItem("unattendFiles", value)
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
                addAction: value => app.addListItem("postSetupItems", value)
                removeAction: index => app.removeListItem("postSetupItems", index)
            }
        }
    }

    component ConfigList: Pane {
        id: configList
        property string title
        property string subtitle
        property string placeholder
        property var items: []
        property var addAction: function(value) {}
        property var removeAction: function(index) {}
        property string extraActionText: ""
        property var extraAction: function() {}
        padding: 18
        background: Rectangle { radius: 20; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
        ColumnLayout {
            anchors.fill: parent
            Label { text: configList.title; font.pixelSize: 20; font.weight: Font.Bold }
            Label { Layout.fillWidth: true; text: configList.subtitle; wrapMode: Text.Wrap; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71" }
            RowLayout {
                Layout.fillWidth: true
                TextField { id: entry; Layout.fillWidth: true; placeholderText: configList.placeholder; onAccepted: addButton.clicked() }
                Button {
                    id: addButton
                    text: root.tr("＋ Add", "＋ 加入")
                    highlighted: true
                    enabled: entry.text.trim().length > 0
                    onClicked: { configList.addAction(entry.text.trim()); entry.clear() }
                }
                Button { visible: configList.extraActionText.length > 0; text: configList.extraActionText; onClicked: configList.extraAction() }
            }
            ListView {
                id: itemList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: configList.items
                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: itemList.width
                    text: "▹  " + modelData
                    ToolButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: "×"; onClicked: configList.removeAction(index) }
                }
                Label { anchors.centerIn: parent; visible: itemList.count === 0; text: root.tr("Nothing queued yet", "未有嘢排隊"); color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71" }
            }
        }
    }

    component FeatureGrid: ScrollView {
        required property var app
        required property var tr
        GridLayout {
            width: parent.width
            columns: width > 780 ? 2 : 1
            rowSpacing: 8; columnSpacing: 12
            Repeater {
                model: [
                    ["NetFx3", "NET Framework 3.5", ".NET Framework 3.5"],
                    ["Microsoft-Windows-Subsystem-Linux", "Windows Subsystem for Linux", "Windows Linux 子系統"],
                    ["VirtualMachinePlatform", "Virtual Machine Platform", "虛擬機平台"],
                    ["Microsoft-Hyper-V-All", "Hyper-V", "Hyper-V"],
                    ["Containers", "Containers", "容器"],
                    ["TelnetClient", "Telnet client", "Telnet 用戶端"],
                    ["SMB1Protocol", "SMB 1.0 (legacy / risky)", "SMB 1.0（古董兼高危）"],
                    ["Printing-PrintToPDFServices-Features", "Microsoft Print to PDF", "Microsoft 列印到 PDF"]
                ]
                delegate: CheckDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    text: "◆  " + root.tr(modelData[1], modelData[2])
                    checked: app.features.indexOf(modelData[0]) >= 0
                    onToggled: app.setFeature(modelData[0], checked)
                }
            }
        }
    }

    component SettingsGrid: ScrollView {
        required property var app
        required property var tr
        ColumnLayout {
            width: parent.width
            spacing: 8
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
                    required property var modelData
                    Layout.fillWidth: true
                    text: "⚙  " + root.tr(modelData[1], modelData[2])
                    checked: app.settingEnabled(modelData[0])
                    onToggled: app.setSetting(modelData[0], checked)
                }
            }
        }
    }
}
