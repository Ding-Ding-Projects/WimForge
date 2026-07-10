pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var app
    required property var tr

    ScrollView {
        anchors.fill: parent
        clip: true
        ColumnLayout {
            width: Math.max(0, parent.width - 18)
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: root.tr("Unattended Windows Studio", "Windows 無人值守工房"); font.pixelSize: 30; font.weight: Font.Bold }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("All seven setup passes, XML and JSON round-trips, generic schema paths, templates, and validated computer-name behavior.",
                                      "七個 setup pass、XML／JSON 來回匯入匯出、通用 schema 路徑、範本，同埋驗證過嘅電腦名行為。")
                        wrapMode: Text.Wrap; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                    }
                }
                Button { text: root.tr("Full automation", "全自動"); onClicked: app.loadUnattendedTemplate("full-automation") }
                Button { text: "✦  " + root.tr("AI development", "AI 開發"); highlighted: true; onClicked: app.loadUnattendedTemplate("ai-development") }
            }

            Pane {
                Layout.fillWidth: true
                padding: 16
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: "🖥  " + root.tr("Computer name", "電腦名"); font.pixelSize: 19; font.weight: Font.Bold }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            id: nameMode
                            model: [root.tr("Random generated", "隨機產生"), root.tr("Fixed", "固定"), "[Prompt]", root.tr("Firmware serial", "韌體序號")]
                            currentIndex: app.computerNameMode
                        }
                        TextField {
                            id: computerName
                            Layout.fillWidth: true
                            enabled: nameMode.currentIndex === 1 || nameMode.currentIndex === 3
                            text: app.computerNameValue
                            placeholderText: nameMode.currentIndex === 3 ? root.tr("Optional prefix", "可選前綴") : root.tr("Up to 15 bytes", "最多 15 bytes")
                        }
                        Button { text: root.tr("Apply", "套用"); onClicked: app.setComputerNameBehavior(nameMode.currentIndex, computerName.text) }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: nameMode.currentIndex === 2
                            ? root.tr("[Prompt] is a WimForge/NTLite-style behavior: Windows receives a valid temporary generated name, then an explicit modal prompt pauses that target's first-logon session until a valid name is entered. It does not block WimForge or its job queue. The literal [Prompt] is never written to Microsoft's ComputerName setting.",
                                      "[Prompt] 係 WimForge／NTLite 式行為：Windows 先收到合法臨時名，首次登入嗰部目標機會真係停低問名，入啱先繼續；唔會阻住 WimForge 同佢條工序隊。絕對唔會將字面 [Prompt] 寫入 Microsoft ComputerName。")
                            : root.tr("Microsoft naming limits are validated before XML is committed.", "commit XML 前會先驗證 Microsoft 命名限制。")
                        wrapMode: Text.Wrap; font.pixelSize: 11; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                    }
                }
            }

            Pane {
                Layout.fillWidth: true
                padding: 16
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: "🔑  " + root.tr("Microsoft-published installation keys", "Microsoft 公開安裝 key"); font.pixelSize: 19; font.weight: Font.Bold }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox { id: productKey; Layout.fillWidth: true; model: app.microsoftProductKeys; textRole: "edition" }
                        Button {
                            text: root.tr("Use for edition selection", "用嚟揀版本")
                            enabled: productKey.currentIndex >= 0
                            onClicked: app.setUnattendedValue("windowsPE", "Microsoft-Windows-Setup", "UserData/ProductKey/Key", productKey.model[productKey.currentIndex].key)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: productKey.currentIndex >= 0 ? productKey.model[productKey.currentIndex].key + "  ·  " + productKey.model[productKey.currentIndex].licensingNotice : ""
                        wrapMode: Text.Wrap; font.family: "Cascadia Mono"; font.pixelSize: 10; color: "#8B5000"
                    }
                }
            }

            Pane {
                Layout.fillWidth: true
                padding: 16
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: "＋  " + root.tr("Any unattend setting", "任何 unattend 設定"); font.pixelSize: 19; font.weight: Font.Bold }
                    Label { Layout.fillWidth: true; text: root.tr("The generic editor preserves components and repeated XML paths that are not yet represented by a convenience card.", "未有方便卡片嘅 component 同重複 XML 路徑，都會由通用編輯器原樣保留。") ; wrapMode: Text.Wrap }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox { id: setupPass; model: ["windowsPE", "offlineServicing", "generalize", "specialize", "auditSystem", "auditUser", "oobeSystem"] }
                        TextField { id: component; Layout.fillWidth: true; placeholderText: "Microsoft-Windows-Shell-Setup" }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField { id: settingPath; Layout.fillWidth: true; placeholderText: "OOBE/HideEULAPage" }
                        TextField { id: settingValue; Layout.fillWidth: true; placeholderText: root.tr("Value", "值") }
                        Button { text: root.tr("Add / update", "加入／更新"); highlighted: true; onClicked: app.setUnattendedValue(setupPass.currentText, component.text, settingPath.text, settingValue.text) }
                    }
                }
            }

            Pane {
                Layout.fillWidth: true
                Layout.preferredHeight: 300
                padding: 10
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                ListView {
                    id: settingList
                    anchors.fill: parent
                    clip: true
                    spacing: 4
                    model: app.unattendedSettings
                    delegate: ItemDelegate {
                        required property var modelData
                        width: settingList.width
                        contentItem: RowLayout {
                            Label { text: modelData.pass; color: Material.accent; Layout.preferredWidth: 115 }
                            Label { text: modelData.component; Layout.preferredWidth: 260; elide: Text.ElideMiddle }
                            Label { text: modelData.path; Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: "Cascadia Mono"; font.pixelSize: 10 }
                            Label { text: modelData.value; Layout.preferredWidth: 210; elide: Text.ElideRight }
                        }
                    }
                }
            }

            Pane {
                Layout.fillWidth: true
                padding: 12
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#332D41" : "#F1EAFE" }
                ColumnLayout {
                    anchors.fill: parent
                    Label { text: "✦  " + root.tr("OpenCode fill-in helper", "OpenCode 自動填寫助手"); font.pixelSize: 18; font.weight: Font.Bold }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField { id: aiIntent; Layout.fillWidth: true; placeholderText: root.tr("Example: Hong Kong locale, Toronto time zone, local account, skip consumer OOBE…", "例如：香港地區、Toronto 時區、本機帳戶、跳過消費者 OOBE…") }
                        Button { text: root.tr("Validate & fill", "驗證同填寫"); enabled: aiIntent.text.trim().length > 0 && !app.openCodeBusy; onClicked: app.askOpenCodeToFillUnattended(aiIntent.text) }
                        BusyIndicator { visible: app.openCodeBusy; running: visible; implicitWidth: 28; implicitHeight: 28 }
                    }
                    Label { Layout.fillWidth: true; text: app.openCodeStatus; font.pixelSize: 10; wrapMode: Text.Wrap }
                }
            }

            Pane {
                Layout.fillWidth: true
                padding: 10
                background: Rectangle { radius: 14; color: Material.theme === Material.Dark ? "#211F26" : "#F7F2FA" }
                RowLayout {
                    anchors.fill: parent
                    Label { text: "↕  XML / JSON" }
                    TextField { id: answerPath; Layout.fillWidth: true; placeholderText: "D:\\profiles\\autounattend.xml" }
                    Button { text: root.tr("Import", "匯入"); enabled: answerPath.text.trim().length > 0; onClicked: app.importUnattended(answerPath.text) }
                    Button { text: root.tr("Export", "匯出"); enabled: answerPath.text.trim().length > 0; onClicked: app.exportUnattended(answerPath.text) }
                }
            }
            Item { Layout.preferredHeight: 8 }
        }
    }
}
