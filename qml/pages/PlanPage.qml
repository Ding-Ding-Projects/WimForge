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
    readonly property bool compact: width < 760
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)
    readonly property color surfaceVariantForeground: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color outlineVariant: DesignTokens.outlineVariant(root.dark)
    readonly property color primary: DesignTokens.primary(root.dark)
    readonly property color success: DesignTokens.success(root.dark)
    readonly property color warning: DesignTokens.tertiary(root.dark)
    readonly property color error: DesignTokens.error(root.dark)

    function statusText(status) {
        if (status === "running") return root.tr("Running", "執行中")
        if (status === "done") return root.tr("Completed", "已完成")
        if (status === "failed") return root.tr("Failed", "失敗")
        if (status === "skipped") return root.tr("Skipped", "已略過")
        if (status === "blocked") return root.tr("Blocked", "已封鎖")
        if (status === "cancelled") return root.tr("Cancelled", "已取消")
        return root.tr("Queued", "排隊中")
    }

    function statusTone(status) {
        if (status === "running") return "info"
        if (status === "done") return "success"
        if (status === "failed" || status === "blocked") return "error"
        if (status === "cancelled" || status === "skipped") return "warning"
        return "neutral"
    }

    function statusGlyph(status, index) {
        if (status === "running") return "RUN"
        if (status === "done") return "OK"
        if (status === "failed") return "ERR"
        if (status === "skipped") return "SKIP"
        if (status === "blocked") return "BLOCK"
        if (status === "cancelled") return "STOP"
        return String(index + 1)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: DesignTokens.spacing12

        WfPageHeader {
            Layout.fillWidth: true
            dark: root.dark
            eyebrow: root.tr("Servicing plan", "維護計劃")
            title: root.tr("Review & run", "檢查同開工")
            description: root.tr("Inspect exact commands, dependencies, checkpoints and risk before any change reaches the image.",
                                 "任何改動落到映像之前，先檢查實際指令、依賴、檢查點同風險。")

            WfButton {
                dark: root.dark
                variant: "outlined"
                glyph: "↻"
                text: root.tr("Rebuild plan", "重排計劃")
                onClicked: root.app.refreshPlan()
            }
            WfButton {
                dark: root.dark
                variant: "tonal"
                text: root.tr("Export script", "匯出 script")
                onClicked: root.app.requestExportScript()
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            surfaceLevel: "low"
            padding: DesignTokens.spacing12

            GridLayout {
                anchors.fill: parent
                columns: root.compact ? 1 : 3
                columnSpacing: DesignTokens.spacing12
                rowSpacing: DesignTokens.spacing8

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.columnSpan: root.compact ? 1 : 2
                    spacing: DesignTokens.spacing4
                    Label {
                        text: root.tr("Concurrent job engine", "平行工序引擎")
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Independent preparation jobs may run together. Writes to one mounted image remain serialized.",
                                      "互不相干嘅準備工序可以一齊跑；寫入同一個掛載映像仍會順序執行。")
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    spacing: DesignTokens.spacing8
                    Label {
                        text: root.tr("Parallel jobs", "平行工序")
                        color: root.surfaceVariantForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 12
                    }
                    SpinBox {
                        Layout.preferredHeight: DesignTokens.controlHeight
                        from: 1
                        to: 16
                        value: root.app.maxParallelJobs
                        editable: true
                        Accessible.name: root.tr("Maximum parallel jobs", "最多平行工序")
                        onValueModified: root.app.maxParallelJobs = value
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: DesignTokens.spacing4
            Layout.rightMargin: DesignTokens.spacing4
            spacing: DesignTokens.spacing8
            Label {
                Layout.preferredWidth: 78
                text: root.tr("STATE", "狀態")
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 10
                font.weight: Font.Bold
                font.letterSpacing: 0.8
            }
            Label {
                Layout.fillWidth: true
                text: root.tr("OPERATION / EXACT COMMAND", "工序／實際指令")
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 10
                font.weight: Font.Bold
                font.letterSpacing: 0.8
            }
            Label {
                visible: !root.compact
                Layout.preferredWidth: 190
                text: root.tr("DEPENDENCY / RISK", "依賴／風險")
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 10
                font.weight: Font.Bold
                font.letterSpacing: 0.8
            }
            Item { Layout.preferredWidth: DesignTokens.controlHeight }
        }

        ListView {
            id: planList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 180
            model: root.app.operationPlan
            spacing: DesignTokens.spacing8
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            delegate: WfCard {
                id: operationCard
                required property var modelData
                required property int index
                width: planList.width
                dark: root.dark
                surfaceLevel: "lowest"
                padding: DesignTokens.spacing12
                outlineColor: operationCard.modelData.destructive
                              ? root.error : root.outlineVariant

                GridLayout {
                    anchors.fill: parent
                    columns: root.compact ? 2 : 4
                    columnSpacing: DesignTokens.spacing12
                    rowSpacing: DesignTokens.spacing8

                    Rectangle {
                        Layout.preferredWidth: 64
                        Layout.preferredHeight: 34
                        radius: DesignTokens.radiusControl
                        color: DesignTokens.toneContainer(root.statusTone(operationCard.modelData.status), root.dark)
                        border.width: 1
                        border.color: DesignTokens.toneStrong(root.statusTone(operationCard.modelData.status), root.dark)
                        Label {
                            anchors.centerIn: parent
                            text: root.statusGlyph(operationCard.modelData.status, operationCard.index)
                            color: DesignTokens.toneForeground(root.statusTone(operationCard.modelData.status), root.dark)
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 10
                            font.weight: Font.Bold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: DesignTokens.spacing4
                        Label {
                            Layout.fillWidth: true
                            text: operationCard.modelData.title
                            color: root.surfaceForeground
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 14
                            font.weight: Font.Bold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: operationCard.modelData.description
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: commandLabel.implicitHeight + DesignTokens.spacing12
                            radius: DesignTokens.radiusControl
                            color: DesignTokens.surfaceDim(root.dark)
                            Label {
                                id: commandLabel
                                anchors.fill: parent
                                anchors.margins: DesignTokens.spacing8
                                text: operationCard.modelData.command
                                color: root.surfaceForeground
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 10
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                    }

                    ColumnLayout {
                        visible: !root.compact
                        Layout.preferredWidth: 190
                        Layout.minimumWidth: 0
                        spacing: DesignTokens.spacing4
                        WfStatusChip {
                            dark: root.dark
                            tone: operationCard.modelData.destructive ? "error"
                                  : operationCard.modelData.admin ? "warning" : "neutral"
                            text: operationCard.modelData.destructive
                                  ? root.tr("DESTRUCTIVE", "有破壞性")
                                  : operationCard.modelData.admin
                                    ? root.tr("ADMIN", "管理員")
                                    : root.tr("STANDARD", "標準")
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.tr("Depends on", "依賴") + ": "
                                  + operationCard.modelData.dependsOn.length
                                  + "  ·  " + operationCard.modelData.writeScope
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 10
                            wrapMode: Text.WrapAnywhere
                        }
                        Label {
                            Layout.fillWidth: true
                            text: operationCard.modelData.checkpointRequired
                                  ? root.tr("Checkpoint required", "需要檢查點")
                                  : root.tr("No checkpoint", "毋須檢查點")
                            color: operationCard.modelData.checkpointRequired ? root.warning : root.surfaceVariantForeground
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: operationCard.modelData.parallelEligible
                                  ? root.tr("Parallel eligible", "可平行")
                                  : root.tr("Serialized write", "順序寫入")
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 11
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: operationCard.modelData.compatibilityNotes.length > 0
                            text: operationCard.modelData.compatibilityNotes.join(" · ")
                            color: root.warning
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 10
                            wrapMode: Text.Wrap
                        }
                    }

                    WfIconButton {
                        dark: root.dark
                        glyph: "⋮"
                        accessibleName: root.tr("Operation actions for %1", "%1 工序動作").arg(operationCard.modelData.title)
                        toolTip: accessibleName
                        onClicked: commandMenu.open()
                        Menu {
                            id: commandMenu
                            MenuItem {
                                text: root.tr("Copy exact command", "複製實際指令")
                                onTriggered: root.app.copyText(operationCard.modelData.command)
                            }
                            MenuItem {
                                text: root.tr("Move earlier", "移前")
                                onTriggered: root.app.moveOperation(operationCard.index, -1)
                            }
                            MenuItem {
                                text: root.tr("Move later", "移後")
                                onTriggered: root.app.moveOperation(operationCard.index, 1)
                            }
                            MenuSeparator {}
                            MenuItem {
                                text: operationCard.modelData.status === "skipped"
                                      ? root.tr("Restore operation", "還原工序")
                                      : root.tr("Skip optional operation", "略過可選工序")
                                enabled: !root.app.busy
                                         && (operationCard.modelData.status === "skipped"
                                             || operationCard.modelData.skipConsequence === "omits-optional-change")
                                onTriggered: root.app.skipOperation(operationCard.index)
                            }
                        }
                    }

                    Flow {
                        visible: root.compact
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        spacing: DesignTokens.spacing8
                        WfStatusChip {
                            dark: root.dark
                            tone: operationCard.modelData.destructive ? "error"
                                  : operationCard.modelData.admin ? "warning" : "neutral"
                            text: operationCard.modelData.destructive
                                  ? root.tr("DESTRUCTIVE", "有破壞性")
                                  : operationCard.modelData.admin
                                    ? root.tr("ADMIN", "管理員")
                                    : root.statusText(operationCard.modelData.status).toUpperCase()
                        }
                        Label {
                            text: root.tr("Dependencies", "依賴") + ": " + operationCard.modelData.dependsOn.length
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 10
                        }
                        Label {
                            text: operationCard.modelData.reversible
                                  ? root.tr("Reversible", "可復原")
                                  : root.tr("Not reversible", "不可復原")
                            color: operationCard.modelData.reversible ? root.success : root.warning
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 10
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - DesignTokens.spacing24)
                visible: planList.count === 0
                text: root.tr("The plan is empty. Add a source and customizations first.",
                              "計劃仲係空嘅。先加來源同調校。")
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
            surfaceLevel: "low"
            padding: DesignTokens.spacing12

            GridLayout {
                anchors.fill: parent
                columns: root.width >= 820 ? 3 : 1
                columnSpacing: DesignTokens.spacing12
                rowSpacing: DesignTokens.spacing8

                CheckBox {
                    id: checkpointCheck
                    Layout.fillWidth: true
                    text: root.tr("Create recovery checkpoint before destructive steps", "危險工序之前建立復原檢查點")
                    checked: root.app.checkpointBeforeDestructive
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    onToggled: root.app.checkpointBeforeDestructive = checked
                }
                WfButton {
                    visible: root.app.busy
                    Layout.fillWidth: root.width < 820
                    dark: root.dark
                    variant: "destructive"
                    text: root.tr("Cancel safely", "安全取消")
                    onClicked: root.app.cancelJobs()
                }
                WfButton {
                    Layout.fillWidth: root.width < 820
                    dark: root.dark
                    variant: "filled"
                    glyph: "▶"
                    enabled: root.app.projectLoaded && root.app.operationCount > 0 && !root.app.busy
                    text: root.tr("Run reviewed plan", "執行已檢查計劃")
                    onClicked: root.app.requestRunPlan()
                }
            }
        }
    }
}
