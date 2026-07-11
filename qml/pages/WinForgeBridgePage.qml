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
    readonly property bool compact: width < 860
    readonly property color surfaceLowest: DesignTokens.surfaceLowest(root.dark)
    readonly property color surfaceLow: DesignTokens.surfaceLow(root.dark)
    readonly property color surfaceContainer: DesignTokens.surfaceContainer(root.dark)
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)
    readonly property color surfaceVariantForeground: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color outlineVariant: DesignTokens.outlineVariant(root.dark)
    readonly property color primary: DesignTokens.primary(root.dark)
    readonly property color primaryContainer: DesignTokens.primaryContainer(root.dark)
    readonly property color primaryContainerForeground: DesignTokens.onPrimaryContainer(root.dark)
    readonly property color successContainer: DesignTokens.successContainer(root.dark)
    readonly property color successContainerForeground: DesignTokens.onSuccessContainer(root.dark)
    readonly property color error: DesignTokens.error(root.dark)
    readonly property color errorContainer: DesignTokens.errorContainer(root.dark)
    readonly property color errorContainerForeground: DesignTokens.onErrorContainer(root.dark)

    ColumnLayout {
        anchors.fill: parent
        spacing: DesignTokens.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spacing16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Label {
                    Layout.fillWidth: true
                    text: root.tr("WinForge Bridge", "WinForge 橋接工房")
                    color: root.surfaceForeground
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 26
                    font.weight: Font.Bold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr(
                              "Choose approved WinForge actions, then bake their versioned recipe and optional self-contained runtime into the ISO. The installed PC resumes safely until the recipe is complete.",
                              "揀好批准嘅 WinForge 動作，再將版本化 recipe 同可選自包含 runtime 焗入 ISO；裝機之後會安全續跑，直到成份 recipe 做完。")
                    color: root.surfaceVariantForeground
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
            }
            WfStatusChip {
                dark: root.dark
                tone: "primary"
                uppercase: false
                showDot: false
                text: (root.app.winForgeBridgeActions || []).length + " " + root.tr("approved actions", "項批准動作")
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            outlined: true
            padding: DesignTokens.spacing12

            ColumnLayout {
                anchors.fill: parent
                spacing: DesignTokens.spacing8
                Label {
                    text: root.tr("Describe the result", "講低你想要嘅結果")
                    color: root.surfaceForeground
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 14
                    font.weight: Font.Bold
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.width >= 740 ? 3 : 1
                    columnSpacing: DesignTokens.spacing8
                    rowSpacing: DesignTokens.spacing8
                    TextField {
                        id: intentField
                        Layout.fillWidth: true
                        Layout.preferredHeight: DesignTokens.controlHeight
                        placeholderText: root.tr("After Windows installs, use WinForge to…",
                                                 "Windows 裝好之後，用 WinForge 幫我……")
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 13
                        onAccepted: if (text.trim().length > 0)
                                        root.app.proposeWinForgeBridgeActions(text)
                    }
                    WfButton {
                        Layout.fillWidth: root.width < 740
                        dark: root.dark
                        variant: "filled"
                        text: root.tr("Propose actions", "提議動作")
                        enabled: intentField.text.trim().length > 0
                        onClicked: root.app.proposeWinForgeBridgeActions(intentField.text)
                    }
                    WfButton {
                        Layout.fillWidth: root.width < 740
                        dark: root.dark
                        variant: "outlined"
                        glyph: "+"
                        text: root.tr("Add typed action", "加 typed 動作")
                        onClicked: actionComposer.open()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr(
                              "Proposals stay drafts until you approve them. Commands keep the executable and each argument separate; no command string is evaluated.",
                              "提議只係草稿，要你批准先算；command 會分開 executable 同每個 argument，唔會 eval 一大串指令。")
                    color: root.surfaceVariantForeground
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: root.compact ? 1 : 2
            columnSpacing: DesignTokens.spacing16
            rowSpacing: DesignTokens.spacing12

            WfCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.minimumHeight: 160
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing8

                ColumnLayout {
                    anchors.fill: parent
                    spacing: DesignTokens.spacing8
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: DesignTokens.spacing8
                        Layout.rightMargin: DesignTokens.spacing8
                        Layout.topMargin: DesignTokens.spacing4
                        Label {
                            Layout.fillWidth: true
                            text: root.tr("Recipe actions", "Recipe 動作")
                            color: root.surfaceForeground
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 15
                            font.weight: Font.Bold
                        }
                        WfStatusChip {
                            dark: root.dark
                            tone: "success"
                            compact: true
                            uppercase: false
                            showDot: true
                            text: root.tr("Each edit is undoable", "每次修改都可 undo")
                        }
                    }

                    ListView {
                        id: actionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: DesignTokens.spacing8
                        boundsBehavior: Flickable.StopAtBounds
                        model: root.app.winForgeBridgeActions || []

                        delegate: WfCard {
                            id: actionCard
                            required property var modelData
                            width: actionList.width
                            dark: root.dark
                            outlined: true
                            surfaceLevel: "low"
                            radius: DesignTokens.radiusCard
                            outlineColor: actionCard.modelData.supported === false
                                          ? root.error
                                          : actionCard.modelData.enabled ? root.primary : root.outlineVariant
                            padding: DesignTokens.spacing12

                            RowLayout {
                                anchors.fill: parent
                                spacing: DesignTokens.spacing12
                                Switch {
                                    checked: actionCard.modelData.enabled
                                    Accessible.name: root.tr("Enable %1", "啟用 %1").arg(actionCard.modelData.title || actionCard.modelData.id)
                                    onClicked: root.app.setWinForgeBridgeActionEnabled(actionCard.modelData.id, checked)
                                }
                                Rectangle {
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 32
                                    radius: DesignTokens.radiusControl
                                    color: root.primaryContainer
                                    Label {
                                        anchors.centerIn: parent
                                        text: actionCard.modelData.kind === "registry" ? "REG"
                                              : actionCard.modelData.kind === "copy" ? "COPY"
                                              : actionCard.modelData.kind === "command" ? "CMD"
                                              : actionCard.modelData.kind === "tweak" ? "TWEAK"
                                              : "PAGE"
                                        color: root.primaryContainerForeground
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 9
                                        font.weight: Font.Bold
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: DesignTokens.spacing8
                                        Label {
                                            Layout.fillWidth: true
                                            text: actionCard.modelData.title || actionCard.modelData.id
                                            color: root.surfaceForeground
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            wrapMode: Text.Wrap
                                        }
                                        Label {
                                            text: (actionCard.modelData.kind || "").toUpperCase() + " · "
                                                  + (actionCard.modelData.phase || "").toUpperCase()
                                            color: root.primary
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 9
                                            font.weight: Font.Bold
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: actionCard.modelData.summary || actionCard.modelData.target || ""
                                        color: root.surfaceVariantForeground
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        visible: actionCard.modelData.supported === false
                                        Layout.fillWidth: true
                                        text: root.tr("Unsupported", "不支援") + ": " + (actionCard.modelData.supportReason
                                              || root.tr("Selected runtime has not declared this capability.",
                                                         "揀咗嘅 runtime 未有聲明呢項 capability。"))
                                        color: root.error
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 10
                                        wrapMode: Text.Wrap
                                    }
                                }
                                WfIconButton {
                                    dark: root.dark
                                    glyph: "×"
                                    accessibleName: root.tr("Remove action", "移除動作")
                                    toolTip: accessibleName
                                    variant: "destructive"
                                    buttonSize: 34
                                    onClicked: root.app.removeWinForgeBridgeAction(actionCard.modelData.id)
                                }
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: root.compact
                Layout.preferredWidth: root.compact ? -1 : 360
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.minimumHeight: 160
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing16

                ScrollView {
                    id: stagingScroll
                    anchors.fill: parent
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: stagingScroll.availableWidth
                        spacing: DesignTokens.spacing8

                        Label {
                            Layout.fillWidth: true
                            text: root.tr("Bundle and stage", "Bundle 同放入 ISO")
                            color: root.surfaceForeground
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 15
                            font.weight: Font.Bold
                            wrapMode: Text.Wrap
                        }
                        CheckBox {
                            id: includeRuntimeCheck
                            Layout.fillWidth: true
                            text: root.tr("Include full self-contained WinForge runtime",
                                          "包括完整自包含 WinForge runtime")
                            checked: root.app.winForgeBridgeIncludeRuntime
                            contentItem: Label {
                                leftPadding: includeRuntimeCheck.indicator.width + includeRuntimeCheck.spacing
                                text: includeRuntimeCheck.text
                                font: includeRuntimeCheck.font
                                color: root.surfaceForeground
                                wrapMode: Text.Wrap
                                verticalAlignment: Text.AlignVCenter
                            }
                            onToggled: root.app.setWinForgeBridgeIncludeRuntime(checked)
                        }
                        WfField {
                            id: runtimePath
                            Layout.fillWidth: true
                            dark: root.dark
                            label: root.tr("Runtime folder", "Runtime 資料夾")
                            text: root.app.winForgeBridgeRuntimePath
                            placeholderText: root.tr("Published WinForge runtime folder",
                                                     "已 publish 嘅 WinForge runtime 資料夾")
                            mono: true
                            onEditingFinished: root.app.setWinForgeBridgeRuntimePath(text)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: DesignTokens.spacing8
                            WfButton {
                                dark: root.dark
                                compact: true
                                variant: "outlined"
                                text: root.tr("Detect contract", "偵測 contract")
                                onClicked: {
                                    root.app.setWinForgeBridgeRuntimePath(runtimePath.text)
                                    root.app.detectWinForgeBridgeRuntime()
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.app.winForgeBridgeRuntimeStatus
                                color: root.surfaceVariantForeground
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 10
                                wrapMode: Text.Wrap
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.topMargin: DesignTokens.spacing4
                            Layout.bottomMargin: DesignTokens.spacing4
                            Layout.preferredHeight: 1
                            color: root.outlineVariant
                        }

                        WfField {
                            id: recipePath
                            Layout.fillWidth: true
                            dark: root.dark
                            label: root.tr("Portable recipe", "可攜 recipe")
                            placeholderText: "D:\\profiles\\workstation.winforge.json"
                            mono: true
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: DesignTokens.spacing8
                            WfButton {
                                Layout.fillWidth: true
                                dark: root.dark
                                compact: true
                                text: root.tr("Import", "匯入")
                                enabled: recipePath.text.trim().length > 0
                                onClicked: root.app.importWinForgeBridgeRecipe(recipePath.text)
                            }
                            WfButton {
                                Layout.fillWidth: true
                                dark: root.dark
                                compact: true
                                text: root.tr("Export", "匯出")
                                enabled: recipePath.text.trim().length > 0
                                onClicked: root.app.exportWinForgeBridgeRecipe(recipePath.text)
                            }
                        }

                        WfField {
                            id: isoPath
                            Layout.fillWidth: true
                            dark: root.dark
                            label: root.tr("ISO staging folder", "ISO staging 資料夾")
                            text: root.app.projectLoaded
                                  ? root.app.projectRoot + "/.wimforge/generated/winforge-stage"
                                  : ""
                            placeholderText: "D:\\ISO-workspace"
                            mono: true
                        }
                        WfButton {
                            Layout.fillWidth: true
                            dark: root.dark
                            variant: "filled"
                            text: root.tr("Stage into ISO", "放入 ISO")
                            enabled: isoPath.text.trim().length > 0
                                     && (root.app.winForgeBridgeActions || []).length > 0
                            onClicked: {
                                root.app.setWinForgeBridgeRuntimePath(runtimePath.text)
                                root.app.stageWinForgeBridgeIntoIso(isoPath.text)
                            }
                        }

                        WfCard {
                            Layout.fillWidth: true
                            dark: root.dark
                            outlined: false
                            fillColor: root.successContainer
                            padding: DesignTokens.spacing12
                            Label {
                                anchors.fill: parent
                                text: root.app.winForgeBridgeStatus
                                color: root.successContainerForeground
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.tr(
                                      "Current legacy WinForge builds expose page deep-links only. Module and tweak replay becomes available only when that runtime declares a compatible bridge contract—WimForge never guesses a hidden CLI.",
                                      "而家 legacy WinForge 只公開 page deep-link；module 同 tweak replay 要 runtime 明確聲明相容 bridge contract 先會開，WimForge 唔會估一條根本冇嘅隱藏 CLI。")
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 10
                            wrapMode: Text.Wrap
                        }
                        Item { Layout.preferredHeight: DesignTokens.spacing4 }
                    }
                }
            }
        }
    }

    Popup {
        id: actionComposer
        anchors.centerIn: Overlay.overlay
        width: Math.min(640, Math.max(320, root.width - 50))
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: DesignTokens.spacing20
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: root.surfaceLowest
            border.color: root.outlineVariant
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: DesignTokens.spacing8
            Label {
                Layout.fillWidth: true
                text: root.tr("Add an approved typed action", "加入批准嘅 typed 動作")
                color: root.surfaceForeground
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 20
                font.weight: Font.Bold
                wrapMode: Text.Wrap
            }
            GridLayout {
                Layout.fillWidth: true
                columns: actionComposer.availableWidth >= 500 ? 2 : 1
                columnSpacing: DesignTokens.spacing8
                rowSpacing: DesignTokens.spacing8
                ComboBox {
                    id: actionKind
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: root.tr("Action type", "動作類型")
                    model: [
                        { text: root.tr("WinForge page", "WinForge 頁面"), value: "page" },
                        { text: root.tr("WinForge module", "WinForge module"), value: "module" },
                        { text: root.tr("WinForge tweak", "WinForge tweak"), value: "tweak" },
                        { text: root.tr("Direct executable", "直接 executable"), value: "command" }
                    ]
                }
                ComboBox {
                    id: actionPhase
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: root.tr("Action phase", "動作階段")
                    model: [
                        { text: root.tr("User phase", "用戶階段"), value: "user" },
                        { text: root.tr("Machine phase", "系統階段"), value: "machine" }
                    ]
                }
            }
            WfField {
                id: actionTarget
                Layout.fillWidth: true
                dark: root.dark
                label: root.tr("Target", "Target")
                placeholderText: root.tr("Target / registry path / copy destination",
                                         "Target／登錄路徑／copy 目的地")
                mono: true
            }
            WfField {
                id: actionExecutable
                Layout.fillWidth: true
                visible: actionKind.currentValue === "command"
                dark: root.dark
                label: root.tr("Executable", "Executable")
                placeholderText: root.tr("Executable only (for example winget.exe)",
                                         "淨係 executable（例如 winget.exe）")
                mono: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: actionKind.currentValue === "command"
                spacing: 4
                Label {
                    text: root.tr("Arguments", "Arguments")
                    color: root.surfaceVariantForeground
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }
                TextArea {
                    id: actionArguments
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    placeholderText: root.tr("JSON argument array, for example [\"install\",\"--id\",\"Git.Git\"]",
                                             "JSON argument array，例如 [\"install\",\"--id\",\"Git.Git\"]")
                    wrapMode: TextEdit.Wrap
                    font.family: DesignTokens.fontMono
                    font.pixelSize: 11
                }
            }
            Label {
                Layout.fillWidth: true
                text: root.tr(
                          "Security-sensitive registry and verified-copy actions use the strict portable recipe schema. Invalid input stays in-app and never blocks running work.",
                          "安全敏感嘅 registry 同 verified-copy 動作用嚴格可攜 recipe schema；輸入有錯只會喺 app 入面提示，唔會阻住其他工作。")
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 10
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                WfButton {
                    dark: root.dark
                    variant: "text"
                    text: root.tr("Cancel", "取消")
                    onClicked: actionComposer.close()
                }
                WfButton {
                    dark: root.dark
                    variant: "filled"
                    text: root.tr("Add draft", "加入草稿")
                    onClicked: {
                        if (root.app.addWinForgeBridgeAction(actionKind.currentValue,
                                                             actionTarget.text,
                                                             actionExecutable.text,
                                                             actionArguments.text,
                                                             actionPhase.currentValue))
                            actionComposer.close()
                    }
                }
            }
        }
    }
}
