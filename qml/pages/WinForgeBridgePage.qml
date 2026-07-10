pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var app
    required property var tr

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Label {
                    text: root.tr("WinForge Bridge", "WinForge 橋接工房")
                    font.pixelSize: 30
                    font.weight: Font.Bold
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr(
                              "Choose approved WinForge actions, then bake their versioned recipe and optional self-contained runtime into the ISO. The installed PC resumes safely until the recipe is complete.",
                              "揀好批准嘅 WinForge 動作，再將版本化 recipe 同可選自包含 runtime 焗入 ISO；裝機之後會安全續跑，直到成份 recipe 做完。")
                    wrapMode: Text.Wrap
                    color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                }
            }
            Pane {
                padding: 12
                background: Rectangle {
                    radius: 18
                    color: Material.theme === Material.Dark ? "#4A4458" : "#E8DEF8"
                }
                ColumnLayout {
                    Label {
                        text: (root.app.winForgeBridgeActions || []).length
                        font.pixelSize: 26
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Label {
                        text: root.tr("approved actions", "項批准動作")
                        font.pixelSize: 10
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }
        }

        Pane {
            Layout.fillWidth: true
            padding: 12
            background: Rectangle {
                radius: 18
                color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
                border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC"
            }
            ColumnLayout {
                anchors.fill: parent
                Label {
                    text: "✦  " + root.tr("Describe the result", "講低你想要嘅結果")
                    font.weight: Font.DemiBold
                }
                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: intentField
                        Layout.fillWidth: true
                        placeholderText: root.tr(
                                             "After Windows installs, use WinForge to…",
                                             "Windows 裝好之後，用 WinForge 幫我……")
                        onAccepted: if (text.trim().length > 0)
                                            root.app.proposeWinForgeBridgeActions(text)
                    }
                    Button {
                        text: "✦  " + root.tr("Propose actions", "提議動作")
                        highlighted: true
                        enabled: intentField.text.trim().length > 0
                        onClicked: root.app.proposeWinForgeBridgeActions(intentField.text)
                    }
                    Button {
                        text: "+  " + root.tr("Add typed action", "加 typed 動作")
                        onClicked: actionComposer.open()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr(
                              "Proposals stay drafts until you approve them. Commands keep the executable and each argument separate; no command string is evaluated.",
                              "提議只係草稿，要你批准先算；command 會分開 executable 同每個 argument，唔會 eval 一大串指令。")
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Pane {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 8
                background: Rectangle {
                    radius: 18
                    color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
                    border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC"
                }
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: root.tr("Recipe actions", "Recipe 動作")
                            font.pixelSize: 19
                            font.weight: Font.DemiBold
                        }
                        Label {
                            text: root.tr("Each edit is undoable", "每次修改都可 undo")
                            font.pixelSize: 10
                            color: "#386A20"
                        }
                    }
                    ListView {
                        id: actionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 7
                        model: root.app.winForgeBridgeActions || []
                        delegate: Pane {
                            id: actionCard
                            required property var modelData
                            width: actionList.width
                            padding: 12
                            background: Rectangle {
                                radius: 15
                                color: Material.theme === Material.Dark ? "#2B292F" : "#F7F2FA"
                                border.color: actionCard.modelData.supported === false
                                              ? "#BA1A1A"
                                              : actionCard.modelData.enabled ? Material.accent : "transparent"
                            }
                            RowLayout {
                                anchors.fill: parent
                                Switch {
                                    checked: actionCard.modelData.enabled
                                    onClicked: root.app.setWinForgeBridgeActionEnabled(
                                                   actionCard.modelData.id, checked)
                                }
                                Rectangle {
                                    Layout.preferredWidth: 36
                                    Layout.preferredHeight: 36
                                    radius: 12
                                    color: Material.theme === Material.Dark ? "#4A4458" : "#E8DEF8"
                                    Label {
                                        anchors.centerIn: parent
                                        text: actionCard.modelData.kind === "registry" ? "▦"
                                              : actionCard.modelData.kind === "copy" ? "▣"
                                              : actionCard.modelData.kind === "command" ? ">_"
                                              : actionCard.modelData.kind === "tweak" ? "⌁"
                                              : "◫"
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: actionCard.modelData.title || actionCard.modelData.id
                                            font.weight: Font.DemiBold
                                        }
                                        Label {
                                            text: (actionCard.modelData.kind || "") + " · "
                                                  + (actionCard.modelData.phase || "")
                                            color: Material.accent
                                            font.pixelSize: 10
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: actionCard.modelData.summary || actionCard.modelData.target || ""
                                        wrapMode: Text.Wrap
                                        color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                                    }
                                    Label {
                                        visible: actionCard.modelData.supported === false
                                        Layout.fillWidth: true
                                        text: actionCard.modelData.supportReason
                                              || root.tr("Selected runtime has not declared this capability.",
                                                         "揀咗嘅 runtime 未有聲明呢項 capability。")
                                        color: "#BA1A1A"
                                        font.pixelSize: 10
                                        wrapMode: Text.Wrap
                                    }
                                }
                                ToolButton {
                                    text: "×"
                                    Accessible.name: root.tr("Remove action", "移除動作")
                                    onClicked: root.app.removeWinForgeBridgeAction(actionCard.modelData.id)
                                }
                            }
                        }
                    }
                }
            }

            Pane {
                Layout.preferredWidth: Math.max(350, parent.width * 0.36)
                Layout.fillHeight: true
                padding: 14
                background: Rectangle {
                    radius: 18
                    color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
                    border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC"
                }
                ColumnLayout {
                    anchors.fill: parent
                    Label {
                        text: "▣  " + root.tr("Bundle and stage", "Bundle 同放入 ISO")
                        font.pixelSize: 19
                        font.weight: Font.DemiBold
                    }
                    CheckBox {
                        text: root.tr("Include full self-contained WinForge runtime",
                                      "包括完整自包含 WinForge runtime")
                        checked: root.app.winForgeBridgeIncludeRuntime
                        onToggled: root.app.setWinForgeBridgeIncludeRuntime(checked)
                    }
                    TextField {
                        id: runtimePath
                        Layout.fillWidth: true
                        text: root.app.winForgeBridgeRuntimePath
                        placeholderText: root.tr("Published WinForge runtime folder",
                                                 "已 publish 嘅 WinForge runtime 資料夾")
                        onEditingFinished: root.app.setWinForgeBridgeRuntimePath(text)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: "⌕  " + root.tr("Detect contract", "偵測 contract")
                            onClicked: {
                                root.app.setWinForgeBridgeRuntimePath(runtimePath.text)
                                root.app.detectWinForgeBridgeRuntime()
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.app.winForgeBridgeRuntimeStatus
                            wrapMode: Text.Wrap
                            font.pixelSize: 10
                            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC"
                    }
                    Label { text: root.tr("Portable recipe", "可攜 recipe"); font.weight: Font.DemiBold }
                    TextField {
                        id: recipePath
                        Layout.fillWidth: true
                        placeholderText: "D:\\profiles\\workstation.winforge.json"
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            Layout.fillWidth: true
                            text: "↧  " + root.tr("Import", "匯入")
                            enabled: recipePath.text.trim().length > 0
                            onClicked: root.app.importWinForgeBridgeRecipe(recipePath.text)
                        }
                        Button {
                            Layout.fillWidth: true
                            text: "↥  " + root.tr("Export", "匯出")
                            enabled: recipePath.text.trim().length > 0
                            onClicked: root.app.exportWinForgeBridgeRecipe(recipePath.text)
                        }
                    }
                    Label { text: root.tr("ISO staging folder", "ISO staging 資料夾"); font.weight: Font.DemiBold }
                    TextField {
                        id: isoPath
                        Layout.fillWidth: true
                        text: root.app.projectLoaded
                              ? root.app.projectRoot + "/.wimforge/generated/winforge-stage"
                              : ""
                        placeholderText: "D:\\ISO-workspace"
                    }
                    Button {
                        Layout.fillWidth: true
                        text: "▣  " + root.tr("Stage config + bundle into ISO",
                                               "將 config + bundle 放入 ISO")
                        highlighted: true
                        enabled: isoPath.text.trim().length > 0
                                 && (root.app.winForgeBridgeActions || []).length > 0
                        onClicked: {
                            root.app.setWinForgeBridgeRuntimePath(runtimePath.text)
                            root.app.stageWinForgeBridgeIntoIso(isoPath.text)
                        }
                    }
                    Pane {
                        Layout.fillWidth: true
                        padding: 10
                        background: Rectangle {
                            radius: 13
                            color: Material.theme === Material.Dark ? "#17351F" : "#EAF8E6"
                        }
                        Label {
                            anchors.fill: parent
                            text: root.app.winForgeBridgeStatus
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                        }
                    }
                    Item { Layout.fillHeight: true }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr(
                                  "Current legacy WinForge builds expose page deep-links only. Module and tweak replay becomes available only when that runtime declares a compatible bridge contract—WimForge never guesses a hidden CLI.",
                                  "而家 legacy WinForge 只公開 page deep-link；module 同 tweak replay 要 runtime 明確聲明相容 bridge contract 先會開，WimForge 唔會估一條根本冇嘅隱藏 CLI。")
                        wrapMode: Text.Wrap
                        font.pixelSize: 10
                        color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                    }
                }
            }
        }
    }

    Popup {
        id: actionComposer
        anchors.centerIn: Overlay.overlay
        width: Math.min(640, root.width - 50)
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 22
        background: Rectangle {
            radius: 24
            color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
            border.color: Material.accent
        }
        ColumnLayout {
            anchors.fill: parent
            Label {
                text: "+  " + root.tr("Add an approved typed action", "加入批准嘅 typed 動作")
                font.pixelSize: 22
                font.weight: Font.Bold
            }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: actionKind
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { text: root.tr("WinForge page", "WinForge 頁面"), value: "page" },
                        { text: root.tr("WinForge module", "WinForge module"), value: "module" },
                        { text: root.tr("WinForge tweak", "WinForge tweak"), value: "tweak" },
                        { text: root.tr("Direct executable", "直接 executable"), value: "command" }
                    ]
                }
                ComboBox {
                    id: actionPhase
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { text: root.tr("User phase", "用戶階段"), value: "user" },
                        { text: root.tr("Machine phase", "系統階段"), value: "machine" }
                    ]
                }
            }
            TextField {
                id: actionTarget
                Layout.fillWidth: true
                placeholderText: root.tr("Target / registry path / copy destination",
                                         "Target／登錄路徑／copy 目的地")
            }
            TextField {
                id: actionExecutable
                Layout.fillWidth: true
                visible: actionKind.currentValue === "command"
                placeholderText: root.tr("Executable only (for example winget.exe)",
                                         "淨係 executable（例如 winget.exe）")
            }
            TextArea {
                id: actionArguments
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                visible: actionKind.currentValue === "command"
                placeholderText: root.tr("JSON argument array, for example [\"install\",\"--id\",\"Git.Git\"]",
                                         "JSON argument array，例如 [\"install\",\"--id\",\"Git.Git\"]")
                wrapMode: TextEdit.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: root.tr(
                          "Security-sensitive registry and verified-copy actions use the strict portable recipe schema. Invalid input stays in-app and never blocks running work.",
                          "安全敏感嘅 registry 同 verified-copy 動作用嚴格可攜 recipe schema；輸入有錯只會喺 app 入面提示，唔會阻住其他工作。")
                wrapMode: Text.Wrap
                font.pixelSize: 10
                color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: root.tr("Cancel", "取消")
                    flat: true
                    onClicked: actionComposer.close()
                }
                Button {
                    text: root.tr("Add draft", "加入草稿")
                    highlighted: true
                    onClicked: {
                        root.app.addWinForgeBridgeAction(actionKind.currentValue,
                                                         actionTarget.text,
                                                         actionExecutable.text,
                                                         actionArguments.text,
                                                         actionPhase.currentValue)
                        actionComposer.close()
                    }
                }
            }
        }
    }
}
