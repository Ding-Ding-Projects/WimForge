import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "../components"

ScrollView {
    id: root
    property var app
    property var tr: function(en, zh) { return en }
    property var openPage: function(index) {}
    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
    function toneContainer(tone) {
        if (tone === "primary") return DesignTokens.primaryContainer(root.dark)
        if (tone === "info") return DesignTokens.secondaryContainer(root.dark)
        if (tone === "warning") return DesignTokens.tertiaryContainer(root.dark)
        if (tone === "success") return DesignTokens.successContainer(root.dark)
        if (tone === "error") return DesignTokens.errorContainer(root.dark)
        return DesignTokens.surfaceHigh(root.dark)
    }
    function toneForeground(tone) {
        if (tone === "primary") return DesignTokens.onPrimaryContainer(root.dark)
        if (tone === "info") return DesignTokens.onSecondaryContainer(root.dark)
        if (tone === "warning") return DesignTokens.onTertiaryContainer(root.dark)
        if (tone === "success") return DesignTokens.onSuccessContainer(root.dark)
        if (tone === "error") return DesignTokens.onErrorContainer(root.dark)
        return DesignTokens.onSurfaceVariant(root.dark)
    }
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ColumnLayout {
        width: root.availableWidth
        spacing: 20

        WfPageHeader {
            Layout.fillWidth: true
            title: root.tr("Image operations overview", "映像作業總覽")
            description: app.projectLoaded
                         ? root.tr("Your workspace is protected and ready. Every recipe change is recorded in Git.",
                                   "工作空間已受保護並準備好。每次配方改動都會記錄落 Git。")
                         : root.tr("Start a project to prepare, validate, and deploy a Windows image with a fully reviewable plan.",
                                   "開個工程，透過完整可檢查嘅計劃準備、驗證同部署 Windows 映像。")
            WfButton {
                text: root.tr("New project", "開新工程")
                glyph: "+"
                variant: "filled"
                onClicked: app.requestNewProject()
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.availableWidth >= 960 ? 4 : root.availableWidth >= 520 ? 2 : 1
            columnSpacing: 12
            rowSpacing: 12

            OverviewMetric {
                Layout.fillWidth: true
                eyebrow: root.tr("PROJECT", "工程")
                value: app.projectLoaded ? app.projectName : root.tr("None", "未開")
                detail: app.projectLoaded ? app.projectRoot : root.tr("Ready when you are", "等你開工")
                iconSource: "qrc:/qt/qml/WimForge/assets/icons/package.svg"
                tone: "primary"
            }
            OverviewMetric {
                Layout.fillWidth: true
                eyebrow: root.tr("OPERATIONS", "工序")
                value: String(app.operationCount)
                detail: root.tr("in the reviewed plan", "已排入檢查過嘅計劃")
                iconSource: "qrc:/qt/qml/WimForge/assets/icons/customize.svg"
                tone: "info"
            }
            OverviewMetric {
                Layout.fillWidth: true
                eyebrow: root.tr("HISTORY", "歷史")
                value: String(app.projectHistoryCount)
                detail: root.tr("recoverable Git commits", "可還原 Git commit")
                iconSource: "qrc:/qt/qml/WimForge/assets/icons/history.svg"
                tone: "warning"
            }
            OverviewMetric {
                Layout.fillWidth: true
                eyebrow: root.tr("RUNNING", "執行緊")
                value: String(app.runningJobCount)
                detail: root.tr("of %1 parallel slots", "共 %1 個平行位").arg(app.maxParallelJobs)
                iconSource: "qrc:/qt/qml/WimForge/assets/icons/run.svg"
                tone: "success"
            }
        }

        WfCard {
            Layout.fillWidth: true
            padding: 18

            ColumnLayout {
                width: parent.width
                spacing: 4

                Label {
                    text: root.tr("Production workflow", "製作流程")
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    color: DesignTokens.onSurface(root.dark)
                }

                Repeater {
                    model: [
                        { icon: "1", en: "Choose Windows ISO / WIM / ESD / SWM", zh: "揀 Windows ISO / WIM / ESD / SWM", metaEn: "Source & editions", metaZh: "來源同版本", page: 1 },
                        { icon: "2", en: "Select editions and customize the recipe", zh: "揀版本，再調校你份配方", metaEn: "Customize", metaZh: "調校", page: 2 },
                        { icon: "3", en: "Review exact commands and safety checks", zh: "逐條睇清楚指令同安全檢查", metaEn: "Review & run", metaZh: "檢查同開工", page: 8 },
                        { icon: "4", en: "Run concurrently with crash-safe checkpoints", zh: "平行開工，仲有防死機檢查點", metaEn: "Review & run", metaZh: "檢查同開工", page: 8 },
                        { icon: "5", en: "Load the ISO in VMware or VirtualBox and record validation", zh: "將 ISO 載入 VMware 或 VirtualBox，再記錄驗證", metaEn: "Virtual Machine Lab", metaZh: "虛擬機實驗室", page: 7 }
                    ]

                    delegate: AbstractButton {
                        id: workflowStep
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 54
                        enabled: app.projectLoaded || modelData.page === 1
                        focusPolicy: Qt.StrongFocus
                        Accessible.name: root.tr(modelData.en, modelData.zh)
                        onClicked: root.openPage(modelData.page)

                        background: Rectangle {
                            radius: DesignTokens.radiusCard
                            color: workflowStep.hovered
                                   ? DesignTokens.surfaceContainer(root.dark)
                                   : DesignTokens.surfaceLowest(root.dark)
                            border.width: workflowStep.visualFocus ? 2 : 1
                            border.color: workflowStep.visualFocus
                                          ? DesignTokens.primary(root.dark)
                                          : DesignTokens.outlineVariant(root.dark)
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Rectangle {
                                Layout.preferredWidth: 34
                                Layout.preferredHeight: 34
                                radius: DesignTokens.radiusControl
                                color: DesignTokens.surfaceHigh(root.dark)
                                Label {
                                    anchors.centerIn: parent
                                    text: workflowStep.modelData.icon
                                    font.family: DesignTokens.fontDisplay
                                    font.pixelSize: 13
                                    font.weight: Font.Bold
                                    color: DesignTokens.onSurfaceVariant(root.dark)
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    Layout.fillWidth: true
                                    text: root.tr(workflowStep.modelData.en, workflowStep.modelData.zh)
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    color: DesignTokens.onSurface(root.dark)
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: root.tr(workflowStep.modelData.metaEn, workflowStep.modelData.metaZh)
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 11
                                    color: DesignTokens.onSurfaceVariant(root.dark)
                                    elide: Text.ElideRight
                                }
                            }
                            Label {
                                text: "›"
                                font.pixelSize: 20
                                color: DesignTokens.onSurfaceVariant(root.dark)
                            }
                        }
                    }
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.availableWidth >= 760 ? 2 : 1
            columnSpacing: 12
            rowSpacing: 12

            WfCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 18

                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    Label {
                        text: root.tr("Safety rails", "安全欄杆")
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        color: DesignTokens.onSurface(root.dark)
                    }
                    Repeater {
                        model: [
                            ["Source images are never overwritten by default", "預設永遠唔會覆蓋原裝映像"],
                            ["Every config edit is committed automatically", "每次改設定都自動 commit"],
                            ["Jobs checkpoint before destructive steps", "危險工序之前一定落檢查點"],
                            ["Interrupted mounts are detected on restart", "重開會搵返中斷咗嘅掛載"]
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 9
                            Rectangle {
                                width: 18
                                height: 18
                                radius: 9
                                color: DesignTokens.successContainer(root.dark)
                                Label {
                                    anchors.centerIn: parent
                                    text: "✓"
                                    font.pixelSize: 11
                                    font.weight: Font.Bold
                                    color: DesignTokens.onSuccessContainer(root.dark)
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.tr(modelData[0], modelData[1])
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 12
                                color: DesignTokens.onSurface(root.dark)
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 18

                ColumnLayout {
                    width: parent.width
                    spacing: 10
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: root.tr("Current job", "而家做緊")
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 15
                            font.weight: Font.Bold
                            color: DesignTokens.onSurface(root.dark)
                        }
                        WfStatusChip {
                            text: app.busy ? root.tr("Running", "執行中") : root.tr("Idle", "閒置")
                            tone: app.busy ? "info" : "neutral"
                            showDot: true
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: app.statusText
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 12
                        color: DesignTokens.onSurface(root.dark)
                        wrapMode: Text.Wrap
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        value: app.progress
                        indeterminate: app.busy && app.progress <= 0
                    }
                    Label {
                        Layout.fillWidth: true
                        text: app.busy
                              ? root.tr("You can keep editing another project while this runs.", "佢做緊嘢嗰陣，你照樣可以改第二個工程。")
                              : root.tr("No active servicing jobs", "而家冇工序行緊")
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        color: DesignTokens.onSurfaceVariant(root.dark)
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        WfCard {
            Layout.fillWidth: true
            padding: 18

            ColumnLayout {
                width: parent.width
                spacing: 10
                Label {
                    text: root.tr("All screens", "所有工作畫面")
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    color: DesignTokens.onSurface(root.dark)
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Every workspace surface in this rewrite, for quick navigation.",
                                  "呢次重寫嘅所有工作畫面，方便快速前往。")
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 12
                    color: DesignTokens.onSurfaceVariant(root.dark)
                    wrapMode: Text.Wrap
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.availableWidth >= 860 ? 3 : root.availableWidth >= 520 ? 2 : 1
                    columnSpacing: 10
                    rowSpacing: 10
                    Repeater {
                        model: [
                            ["Source & editions", "來源同版本", 1],
                            ["Customize", "調校", 2],
                            ["Group Policy Studio", "群組原則工房", 3],
                            ["Unattended Studio", "無人值守工房", 4],
                            ["Package Studio", "套件工房", 5],
                            ["WinForge Bridge", "WinForge 橋接", 6],
                            ["Virtual Machine Lab", "虛擬機實驗室", 7],
                            ["Review & run", "檢查同開工", 8],
                            ["History & recovery", "歷史同復原", 9],
                            ["Settings", "設定", 10],
                            ["Embedded terminal", "內嵌終端機", 11]
                        ]
                        delegate: AbstractButton {
                            id: screenLink
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 44
                            Accessible.name: root.tr(modelData[0], modelData[1])
                            onClicked: root.openPage(modelData[2])
                            background: Rectangle {
                                radius: DesignTokens.radiusCard
                                color: screenLink.hovered
                                       ? DesignTokens.surfaceContainer(root.dark)
                                       : DesignTokens.surfaceLowest(root.dark)
                                border.width: 1
                                border.color: DesignTokens.outlineVariant(root.dark)
                            }
                            contentItem: RowLayout {
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: root.tr(screenLink.modelData[0], screenLink.modelData[1])
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: DesignTokens.onSurface(root.dark)
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: "›"
                                    font.pixelSize: 17
                                    color: DesignTokens.onSurfaceVariant(root.dark)
                                }
                            }
                        }
                    }
                }
            }
        }

        Item { Layout.preferredHeight: 20 }
    }

    component OverviewMetric: WfCard {
        id: metric
        property string eyebrow
        property string value
        property string detail
        property string iconSource
        property string tone: "neutral"
        padding: 16
        Layout.minimumHeight: 116

        ColumnLayout {
            width: parent.width
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: metric.eyebrow
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1
                    color: DesignTokens.onSurfaceVariant(root.dark)
                }
                Rectangle {
                    width: 28
                    height: 28
                    radius: DesignTokens.radiusControl
                    color: root.dark ? root.toneContainer(metric.tone) : "#2A4F91"
                    Image {
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        source: metric.iconSource
                        sourceSize.width: 16
                        sourceSize.height: 16
                        opacity: 0.95
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                text: metric.value
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 25
                font.weight: Font.Bold
                color: DesignTokens.onSurface(root.dark)
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: metric.detail
                font.family: DesignTokens.fontBody
                font.pixelSize: 11
                color: DesignTokens.onSurfaceVariant(root.dark)
                elide: Text.ElideMiddle
            }
        }
    }
}
