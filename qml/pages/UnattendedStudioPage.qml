pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "../components"

Item {
    id: root
    required property var app
    required property var tr

    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
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
                        text: root.tr("Export", "匯出")
                        enabled: answerPath.text.trim().length > 0
                        onClicked: root.app.exportUnattended(answerPath.text)
                    }
                }
            }

            Item { Layout.preferredHeight: DesignTokens.spacing16 }
        }
    }
}
