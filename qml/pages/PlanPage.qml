import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var app
    property var tr: function(en, zh) { return en }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: root.tr("Review & run", "檢查同開工"); font.pixelSize: 30; font.weight: Font.Bold }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Exact commands, dependencies, checkpoints and risk flags—nothing hidden behind a magical button.",
                                  "指令、依賴、檢查點同風險全部攤開畀你睇，冇粒神秘掣撳落去先知出事。")
                    wrapMode: Text.Wrap
                    color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                }
            }
            Button { icon.name: "view-refresh"; text: root.tr("Rebuild plan", "重排計劃"); onClicked: app.refreshPlan() }
            Button { icon.name: "document-save"; text: root.tr("Export script", "匯出 script"); onClicked: app.requestExportScript() }
        }

        Pane {
            Layout.fillWidth: true
            padding: 14
            background: Rectangle { radius: 16; color: Material.theme === Material.Dark ? "#211F26" : "#F7F2FA" }
            RowLayout {
                anchors.fill: parent
                Label { text: "⚡"; font.pixelSize: 24; color: Material.accent }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: root.tr("Concurrent job engine", "平行工序引擎"); font.weight: Font.DemiBold }
                    Label {
                        text: root.tr("Independent preparation jobs can run together; writes to the same mounted image are serialized automatically.",
                                      "互不相干嘅準備工序可以一齊跑；寫入同一個掛載映像就會自動排隊，唔會鬥快撞車。")
                        wrapMode: Text.Wrap
                        color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                    }
                }
                Label { text: root.tr("Parallel", "平行") }
                SpinBox {
                    from: 1; to: 16
                    value: app.maxParallelJobs
                    onValueModified: app.maxParallelJobs = value
                }
            }
        }

        ListView {
            id: planList
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: app.operationPlan
            spacing: 8
            clip: true

            delegate: Pane {
                required property var modelData
                required property int index
                width: planList.width
                padding: 14
                background: Rectangle {
                    radius: 16
                    color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"
                    border.width: 1
                    border.color: modelData.destructive ? "#BA1A1A" : (Material.theme === Material.Dark ? "#49454F" : "#E7E0EC")
                }
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    Rectangle {
                        width: 38; height: 38; radius: 12
                        color: modelData.status === "running" ? Material.accent
                             : modelData.status === "done" ? "#2E7D32"
                             : modelData.status === "failed" ? "#BA1A1A"
                             : (Material.theme === Material.Dark ? "#36343B" : "#E7E0EC")
                        Label {
                            anchors.centerIn: parent
                            text: modelData.status === "running" ? "▶" : modelData.status === "done" ? "✓" : String(index + 1)
                            color: modelData.status === "queued" ? (Material.theme === Material.Dark ? "white" : "#1D1B20") : "white"
                            font.weight: Font.Bold
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: modelData.title; font.weight: Font.DemiBold; Layout.fillWidth: true }
                            Label { visible: modelData.admin; text: "🛡 " + root.tr("Admin", "管理員"); color: "#8B5000" }
                            Label { visible: modelData.destructive; text: "⚠ " + root.tr("Destructive", "有破壞性"); color: "#BA1A1A" }
                            Label { visible: modelData.reboot; text: "↻ " + root.tr("Reboot", "要重開") }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.description
                            wrapMode: Text.Wrap
                            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.command
                            font.family: "Cascadia Mono"
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                            color: Material.theme === Material.Dark ? "#D0BCFF" : "#6750A4"
                        }
                    }
                    ToolButton {
                        text: "⋮"
                        onClicked: commandMenu.open()
                        Menu {
                            id: commandMenu
                            MenuItem { text: "⧉  " + root.tr("Copy command", "複製指令"); onTriggered: app.copyText(modelData.command) }
                            MenuItem { text: "↑  " + root.tr("Move earlier", "移前") ; onTriggered: app.moveOperation(index, -1) }
                            MenuItem { text: "↓  " + root.tr("Move later", "移後"); onTriggered: app.moveOperation(index, 1) }
                            MenuSeparator {}
                            MenuItem { text: modelData.status === "skipped" ? "↺  " + root.tr("Restore operation", "還原工序") : "×  " + root.tr("Skip optional operation", "略過可選工序"); onTriggered: app.skipOperation(index) }
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: planList.count === 0
                text: root.tr("The plan is empty. Add a source and some customizations first.", "計劃仲係空嘅。先加來源同揀啲調校啦。")
                color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            CheckBox {
                text: root.tr("Create recovery checkpoint before destructive steps", "危險工序之前建立復原檢查點")
                checked: app.checkpointBeforeDestructive
                onToggled: app.checkpointBeforeDestructive = checked
            }
            Item { Layout.fillWidth: true }
            Button {
                visible: app.busy
                icon.name: "process-stop"
                text: root.tr("Cancel safely", "安全取消")
                onClicked: app.cancelJobs()
            }
            Button {
                highlighted: true
                enabled: app.projectLoaded && app.operationCount > 0 && !app.busy
                icon.name: "media-playback-start"
                text: root.tr("Run reviewed plan", "執行已檢查計劃")
                onClicked: app.requestRunPlan()
            }
        }
    }
}
