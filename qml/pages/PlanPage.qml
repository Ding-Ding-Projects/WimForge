import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var app
    property var tr: function(en, zh) { return en }
    readonly property bool compact: width < 760
    readonly property color errorText: Material.theme === Material.Dark ? "#FFB4AB" : "#BA1A1A"
    readonly property color warningText: Material.theme === Material.Dark ? "#FFD18B" : "#8B5000"
    readonly property color successFill: Material.theme === Material.Dark ? "#4F7A49" : "#2E7D32"

    function statusText(status) {
        if (status === "running") return root.tr("Running", "執行中")
        if (status === "done") return root.tr("Completed", "已完成")
        if (status === "failed") return root.tr("Failed", "失敗")
        if (status === "skipped") return root.tr("Skipped", "已略過")
        if (status === "blocked") return root.tr("Blocked", "已封鎖")
        if (status === "cancelled") return root.tr("Cancelled", "已取消")
        return root.tr("Queued", "排隊中")
    }

    function statusGlyph(status, index) {
        if (status === "running") return "▶"
        if (status === "done") return "✓"
        if (status === "failed") return "!"
        if (status === "skipped") return "–"
        if (status === "blocked") return "⊘"
        if (status === "cancelled") return "×"
        return String(index + 1)
    }

    function detailLabel(en, zh, value) {
        return root.tr(en, zh) + ": " + value
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        GridLayout {
            Layout.fillWidth: true
            columns: root.width >= 760 ? 3 : 1
            columnSpacing: 8
            rowSpacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                Label { Layout.fillWidth: true; text: root.tr("Review & run", "檢查同開工"); font.pixelSize: 30; font.weight: Font.Bold; wrapMode: Text.Wrap }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Exact commands, dependencies, checkpoints and risk flags—nothing hidden behind a magical button.",
                                  "指令、依賴、檢查點同風險全部攤開畀你睇，冇粒神秘掣撳落去先知出事。")
                    wrapMode: Text.Wrap
                    color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                }
            }
            Button { Layout.fillWidth: root.width < 760; icon.name: "view-refresh"; text: root.tr("Rebuild plan", "重排計劃"); onClicked: app.refreshPlan() }
            Button { Layout.fillWidth: root.width < 760; icon.name: "document-save"; text: root.tr("Export script", "匯出 script"); onClicked: app.requestExportScript() }
        }

        Pane {
            Layout.fillWidth: true
            padding: 14
            background: Rectangle { radius: 16; color: Material.theme === Material.Dark ? "#211F26" : "#F7F2FA" }
            GridLayout {
                anchors.fill: parent
                columns: root.width >= 700 ? 4 : 1
                columnSpacing: 8
                rowSpacing: 6
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
                Label { Layout.fillWidth: root.width < 700; text: root.tr("Parallel", "平行"); wrapMode: Text.Wrap }
                SpinBox {
                    from: 1; to: 16
                    value: app.maxParallelJobs
                    Accessible.name: root.tr("Maximum parallel jobs", "最多平行工序")
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
                    border.color: modelData.destructive ? root.errorText : (Material.theme === Material.Dark ? "#49454F" : "#E7E0EC")
                }
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    Rectangle {
                        width: 38; height: 38; radius: 12
                        color: modelData.status === "running" ? Material.accent
                             : modelData.status === "done" ? root.successFill
                             : (modelData.status === "failed" || modelData.status === "blocked") ? (Material.theme === Material.Dark ? "#8C1D18" : "#BA1A1A")
                             : modelData.status === "cancelled" ? (Material.theme === Material.Dark ? "#7A5A20" : "#8B5000")
                             : (Material.theme === Material.Dark ? "#36343B" : "#E7E0EC")
                        Accessible.name: root.statusText(modelData.status)
                        Label {
                            anchors.centerIn: parent
                            text: root.statusGlyph(modelData.status, index)
                            color: modelData.status === "queued" ? (Material.theme === Material.Dark ? "white" : "#1D1B20") : "white"
                            font.weight: Font.Bold
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: modelData.title
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: planList.width >= 700 ? 4 : 1
                            columnSpacing: 8
                            rowSpacing: 2
                            Label { text: "● " + root.statusText(modelData.status); color: (modelData.status === "failed" || modelData.status === "blocked") ? root.errorText : (modelData.status === "cancelled" ? root.warningText : Material.accent); font.weight: Font.DemiBold; font.pixelSize: 10 }
                            Label { visible: modelData.admin; text: "🛡 " + root.tr("Admin", "管理員"); color: root.warningText; font.pixelSize: 10 }
                            Label { visible: modelData.destructive; text: "⚠ " + root.tr("Destructive", "有破壞性"); color: root.errorText; font.pixelSize: 10 }
                            Label { visible: modelData.reboot; text: "↻ " + root.tr("Reboot", "要重開"); font.pixelSize: 10 }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.description
                            wrapMode: Text.Wrap
                            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 8
                            Label { text: root.detailLabel("Scope", "範圍", modelData.writeScope); font.pixelSize: 10 }
                            Label { text: root.detailLabel("Dependencies", "依賴", modelData.dependsOn.length); font.pixelSize: 10 }
                            Label { text: modelData.checkpointRequired ? root.tr("Checkpoint required", "需要檢查點") : root.tr("No checkpoint", "毋須檢查點"); font.pixelSize: 10 }
                            Label { text: modelData.parallelEligible ? root.tr("Parallel eligible", "可平行") : root.tr("Serialized", "順序執行"); font.pixelSize: 10 }
                            Label { text: modelData.reversible ? root.tr("Reversible", "可復原") : root.tr("Not reversible", "不可復原"); font.pixelSize: 10; color: modelData.reversible ? Material.accent : root.warningText }
                            Label { text: root.detailLabel("Skip effect", "略過影響", modelData.skipConsequence); font.pixelSize: 10; color: modelData.skipConsequence === "omits-optional-change" ? Material.accent : root.warningText }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: modelData.compatibilityNotes.length > 0
                            text: root.tr("Compatibility: ", "相容性：") + modelData.compatibilityNotes.join(" • ")
                            wrapMode: Text.Wrap
                            font.pixelSize: 10
                            color: root.warningText
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.command
                            font.family: "Cascadia Mono"
                            font.pixelSize: 11
                            wrapMode: Text.WrapAnywhere
                            color: Material.theme === Material.Dark ? "#D0BCFF" : "#6750A4"
                        }
                    }
                    ToolButton {
                        text: "⋮"
                        Accessible.name: root.tr("Operation actions for %1", "%1 工序動作").arg(modelData.title)
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: commandMenu.open()
                        Menu {
                            id: commandMenu
                            MenuItem { text: "⧉  " + root.tr("Copy command", "複製指令"); onTriggered: app.copyText(modelData.command) }
                            MenuItem { text: "↑  " + root.tr("Move earlier", "移前") ; onTriggered: app.moveOperation(index, -1) }
                            MenuItem { text: "↓  " + root.tr("Move later", "移後"); onTriggered: app.moveOperation(index, 1) }
                            MenuSeparator {}
                            MenuItem {
                                text: modelData.status === "skipped" ? "↺  " + root.tr("Restore operation", "還原工序") : "×  " + root.tr("Skip optional operation", "略過可選工序")
                                enabled: !app.busy && (modelData.status === "skipped" || modelData.skipConsequence === "omits-optional-change")
                                onTriggered: app.skipOperation(index)
                            }
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

        GridLayout {
            Layout.fillWidth: true
            columns: root.width >= 820 ? 3 : 1
            columnSpacing: 8
            rowSpacing: 8
            CheckBox {
                id: checkpointCheck
                Layout.fillWidth: true
                text: root.tr("Create recovery checkpoint before destructive steps", "危險工序之前建立復原檢查點")
                checked: app.checkpointBeforeDestructive
                contentItem: Label {
                    leftPadding: checkpointCheck.indicator.width + checkpointCheck.spacing
                    text: checkpointCheck.text
                    font: checkpointCheck.font
                    color: checkpointCheck.palette.windowText
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: app.checkpointBeforeDestructive = checked
            }
            Button {
                visible: app.busy
                Layout.fillWidth: root.width < 820
                icon.name: "process-stop"
                text: root.tr("Cancel safely", "安全取消")
                onClicked: app.cancelJobs()
            }
            Button {
                Layout.fillWidth: root.width < 820
                highlighted: true
                enabled: app.projectLoaded && app.operationCount > 0 && !app.busy
                icon.name: "media-playback-start"
                text: root.tr("Run reviewed plan", "執行已檢查計劃")
                onClicked: app.requestRunPlan()
            }
        }
    }
}
