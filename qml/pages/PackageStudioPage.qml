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
    readonly property bool compact: width < 820
    readonly property color surfaceLowest: DesignTokens.surfaceLowest(root.dark)
    readonly property color surfaceLow: DesignTokens.surfaceLow(root.dark)
    readonly property color surface: DesignTokens.surface(root.dark)
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)
    readonly property color surfaceVariantForeground: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color outlineVariant: DesignTokens.outlineVariant(root.dark)
    readonly property color primary: DesignTokens.primary(root.dark)
    readonly property color primaryContainer: DesignTokens.primaryContainer(root.dark)
    readonly property color primaryContainerForeground: DesignTokens.onPrimaryContainer(root.dark)
    readonly property color successContainer: DesignTokens.successContainer(root.dark)
    readonly property color successContainerForeground: DesignTokens.onSuccessContainer(root.dark)
    readonly property color warningContainer: DesignTokens.tertiaryContainer(root.dark)
    readonly property color warningContainerForeground: DesignTokens.onTertiaryContainer(root.dark)
    readonly property color errorContainer: DesignTokens.errorContainer(root.dark)
    readonly property color errorContainerForeground: DesignTokens.onErrorContainer(root.dark)
    readonly property var filteredPackageCatalog: {
        const source = root.app.packageCatalog
        const query = packageSearch.text.trim().toLowerCase()
        if (query.length === 0)
            return source

        const filtered = []
        for (let index = 0; index < source.length; ++index) {
            const entry = source[index]
            const searchableText = String(entry.name || "") + " "
                + String(entry.identifier || "") + " "
                + String(entry.provider || "")
            if (searchableText.toLowerCase().indexOf(query) >= 0)
                filtered.push(entry)
        }
        return filtered
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Package Manager Studio", "套件管理工房")
                    color: root.surfaceForeground
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 26
                    font.weight: Font.Bold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Choose software for the finished ISO. Dependencies, offline payloads, signatures, retries and crash-resume state stay explicit.",
                                  "揀完成 ISO 要裝咩軟件；依賴、離線 payload、簽署、重試同斷電續跑狀態全部寫清楚。")
                    color: root.surfaceVariantForeground
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
            }

            WfStatusChip {
                dark: root.dark
                tone: "primary"
                compact: false
                uppercase: false
                showDot: false
                text: root.app.selectedPackageCount + " " + root.tr("selected", "已揀")
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            outlined: true
            surfaceLevel: "low"
            padding: 12
            GridLayout {
                anchors.fill: parent
                columns: root.compact ? 1 : 3
                columnSpacing: 8
                rowSpacing: 8

                TextField {
                    id: packageSearch
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    placeholderText: root.tr("Search software, provider, package ID…", "搜尋軟件、provider、套件 ID…")
                    selectByMouse: true
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 13
                }
                WfButton {
                    Layout.fillWidth: root.compact
                    dark: root.dark
                    variant: "filled"
                    text: root.tr("Full AI Development ISO", "完整 AI 開發 ISO")
                    onClicked: root.app.loadAiDevelopmentPackageTemplate()
                }
                WfButton {
                    Layout.fillWidth: root.compact
                    dark: root.dark
                    variant: "tonal"
                    text: root.tr("Stage into ISO", "放入 ISO")
                    enabled: root.app.projectLoaded && root.app.selectedPackageCount > 0
                    onClicked: root.app.stagePackageProfile()
                }
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            outlined: true
            fillColor: root.app.openCodeReady ? root.successContainer
                     : root.app.openCodeState === "failed" ? root.errorContainer
                     : root.warningContainer
            outlineColor: root.app.openCodeReady ? root.successContainerForeground
                        : root.app.openCodeState === "failed" ? root.errorContainerForeground
                        : root.warningContainerForeground
            padding: 11
            RowLayout {
                anchors.fill: parent
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: root.app.openCodeReady ? root.successContainerForeground
                          : root.app.openCodeState === "failed" ? root.errorContainerForeground
                          : root.warningContainerForeground
                }
                Label {
                    text: root.app.openCodeState === "absent" ? root.tr("ABSENT", "未有")
                        : root.app.openCodeState === "installing" ? root.tr("INSTALLING", "安裝緊")
                        : root.app.openCodeState === "verifying" ? root.tr("VERIFYING", "驗證緊")
                        : root.app.openCodeState === "ready" ? root.tr("READY", "準備好")
                        : root.tr("FAILED", "失敗")
                    color: root.app.openCodeReady ? root.successContainerForeground
                          : root.app.openCodeState === "failed" ? root.errorContainerForeground
                          : root.warningContainerForeground
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    font.letterSpacing: 1
                }
                Label {
                    Layout.fillWidth: true
                    text: root.app.openCodeStatus
                    color: root.app.openCodeReady ? root.successContainerForeground
                          : root.app.openCodeState === "failed" ? root.errorContainerForeground
                          : root.warningContainerForeground
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                BusyIndicator {
                    visible: root.app.openCodeBusy
                    running: visible
                    implicitWidth: 26
                    implicitHeight: 26
                    Accessible.name: root.tr("OpenCode installation in progress", "OpenCode 安裝進行中")
                }
                WfButton {
                    visible: !root.app.openCodeReady
                    dark: root.dark
                    variant: "outlined"
                    enabled: root.app.openCodeCanRetry && !root.app.openCodeBusy
                    text: root.app.openCodeState === "failed"
                          ? root.tr("Retry approved setup", "再試已批准設定")
                          : root.tr("Verify / install now", "而家驗證／安裝")
                    onClicked: root.app.ensureOpenCode()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: root.tr("Package catalog", "套件目錄")
                color: root.surfaceForeground
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 15
                font.weight: Font.Bold
            }
            Item { Layout.fillWidth: true }
            Label {
                text: root.app.packageCatalog.length + " " + root.tr("available", "可用")
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 11
            }
        }

        ListView {
            id: packageList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 160
            clip: true
            spacing: 8
            boundsBehavior: Flickable.StopAtBounds
            model: root.filteredPackageCatalog

            delegate: WfCard {
                id: packageCard
                required property var modelData
                width: packageList.width
                height: Math.max(78, implicitHeight)
                dark: root.dark
                outlined: true
                surfaceLevel: "lowest"
                outlineColor: packageCard.modelData.enabled ? root.primary : root.outlineVariant
                padding: 12

                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    Switch {
                        checked: packageCard.modelData.enabled
                        Accessible.name: root.tr("Include %1", "包括 %1").arg(packageCard.modelData.name)
                        onClicked: root.app.setPackageEnabled(packageCard.modelData.id, checked)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                text: packageCard.modelData.name
                                color: root.surfaceForeground
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 14
                                font.weight: Font.Bold
                                wrapMode: Text.Wrap
                            }
                            Label {
                                text: packageCard.modelData.version
                                color: root.surfaceVariantForeground
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 11
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                text: packageCard.modelData.provider
                                color: root.primary
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            Rectangle {
                                visible: packageCard.modelData.optional
                                Layout.preferredWidth: optionalLabel.implicitWidth + 16
                                Layout.preferredHeight: 22
                                radius: 11
                                color: root.warningContainer
                                Label {
                                    id: optionalLabel
                                    anchors.centerIn: parent
                                    text: root.tr("OPTIONAL", "可選")
                                    color: root.warningContainerForeground
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.8
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: packageCard.modelData.description
                                color: root.surfaceVariantForeground
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: packageCard.modelData.identifier
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 10
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - DesignTokens.spacing24)
                visible: packageList.count === 0
                text: packageSearch.text.trim().length > 0
                      ? root.tr("No packages match this search.", "冇套件符合呢個搜尋。")
                      : root.tr("No packages are available in the catalog.", "套件目錄暫時冇可用套件。")
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 13
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            outlined: true
            surfaceLevel: "low"
            padding: 10
            GridLayout {
                anchors.fill: parent
                columns: root.width >= 780 ? 4 : 1
                columnSpacing: 8
                rowSpacing: 8
                Label {
                    text: root.tr("Profile file", "設定檔")
                    color: root.surfaceForeground
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
                TextField {
                    id: profilePath
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    placeholderText: "D:\\profiles\\software.json"
                    font.family: DesignTokens.fontMono
                    font.pixelSize: 11
                    selectByMouse: true
                }
                WfButton {
                    Layout.fillWidth: root.width < 780
                    dark: root.dark
                    variant: "outlined"
                    text: root.tr("Import", "匯入")
                    enabled: profilePath.text.trim().length > 0
                    onClicked: root.app.importPackageProfile(profilePath.text)
                }
                WfButton {
                    Layout.fillWidth: root.width < 780
                    dark: root.dark
                    variant: "outlined"
                    text: root.tr("Export", "匯出")
                    enabled: profilePath.text.trim().length > 0
                    onClicked: root.app.exportPackageProfile(profilePath.text)
                }
            }
        }
    }
}
