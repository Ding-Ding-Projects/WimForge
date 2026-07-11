pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ScrollView {
    id: root

    required property bool dark
    required property var tr
    property var recentProjects: []

    signal createRequested()
    signal openRequested()
    signal importRequested()
    signal recentRequested(string path)
    signal removeRecentRequested(string path)
    signal clearRecentRequested()

    Material.theme: dark ? Material.Dark : Material.Light
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    function formattedLastOpened(value) {
        if (!value)
            return tr("Time unavailable", "未有時間記錄")
        var parsed = new Date(value)
        if (isNaN(parsed.getTime()))
            return tr("Time unavailable", "未有時間記錄")
        return Qt.formatDateTime(parsed, "yyyy-MM-dd  HH:mm")
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: DesignTokens.spacing20

        WfCard {
            dark: root.dark
            Layout.fillWidth: true
            padding: root.availableWidth < 620 ? 20 : 28
            fillColor: DesignTokens.primaryContainer(root.dark)
            outlineColor: Qt.rgba(DesignTokens.primary(root.dark).r,
                                  DesignTokens.primary(root.dark).g,
                                  DesignTokens.primary(root.dark).b, 0.34)

            RowLayout {
                width: parent.width
                spacing: DesignTokens.spacing20

                Rectangle {
                    Layout.preferredWidth: 54
                    Layout.preferredHeight: 54
                    radius: DesignTokens.radiusCard
                    color: DesignTokens.primary(root.dark)

                    Label {
                        anchors.centerIn: parent
                        text: "W"
                        color: DesignTokens.onPrimary(root.dark)
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 26
                        font.weight: Font.Bold
                        Accessible.ignored: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spacing4

                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Welcome to WimForge", "歡迎使用 WimForge")
                        color: DesignTokens.onPrimaryContainer(root.dark)
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: root.availableWidth < 620 ? 24 : 30
                        font.weight: Font.Bold
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr(
                            "Create, open, or restore a Git-backed Windows image project.",
                            "開新工程、開返現有工程，或者還原有 Git 歷史嘅 Windows 映像工程。")
                        color: DesignTokens.onPrimaryContainer(root.dark)
                        opacity: 0.82
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.availableWidth >= 860 ? 2 : 1
            columnSpacing: DesignTokens.spacing16
            rowSpacing: DesignTokens.spacing16

            WfCard {
                dark: root.dark
                Layout.fillWidth: true
                Layout.fillHeight: root.availableWidth >= 860
                padding: 20

                ColumnLayout {
                    width: parent.width
                    spacing: DesignTokens.spacing12

                    Label {
                        text: root.tr("Start", "開始")
                        color: DesignTokens.onSurface(root.dark)
                        font.family: DesignTokens.fontDisplay
                        font.pixelSize: 18
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr(
                            "Choose how you want to begin. Nothing is changed until you confirm a location.",
                            "揀你想點樣開始；未確認位置之前，WimForge 唔會改任何嘢。")
                        color: DesignTokens.onSurfaceVariant(root.dark)
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        model: [
                            {
                                action: "create",
                                icon: "qrc:/qt/qml/WimForge/assets/icons/package.svg",
                                titleEn: "Create a new project",
                                titleZh: "開個新工程",
                                detailEn: "Set up a clean workspace with local Git history.",
                                detailZh: "整個乾淨工作空間，連本機 Git 歷史一齊開好。"
                            },
                            {
                                action: "open",
                                icon: "qrc:/qt/qml/WimForge/assets/icons/source.svg",
                                titleEn: "Open a local project",
                                titleZh: "開本機工程",
                                detailEn: "Choose an existing folder containing project.json.",
                                detailZh: "揀一個有 project.json 嘅現有資料夾。"
                            },
                            {
                                action: "import",
                                icon: "qrc:/qt/qml/WimForge/assets/icons/bridge.svg",
                                titleEn: "Import a project bundle",
                                titleZh: "匯入工程 bundle",
                                detailEn: "Restore a .wimforge bundle or project JSON into a new folder.",
                                detailZh: "將 .wimforge bundle 或工程 JSON 還原去新資料夾。"
                            }
                        ]

                        delegate: AbstractButton {
                            id: startAction
                            required property var modelData

                            Layout.fillWidth: true
                            implicitHeight: 76
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Accessible.role: Accessible.Button
                            Accessible.name: root.tr(modelData.titleEn, modelData.titleZh)
                            onClicked: {
                                if (modelData.action === "create")
                                    root.createRequested()
                                else if (modelData.action === "open")
                                    root.openRequested()
                                else
                                    root.importRequested()
                            }

                            background: Rectangle {
                                radius: DesignTokens.radiusCard
                                color: startAction.pressed
                                       ? DesignTokens.surfaceHighest(root.dark)
                                       : startAction.hovered
                                         ? DesignTokens.surfaceHigh(root.dark)
                                         : DesignTokens.surfaceLow(root.dark)
                                border.width: startAction.visualFocus ? 2 : 1
                                border.color: startAction.visualFocus
                                              ? DesignTokens.primary(root.dark)
                                              : DesignTokens.outlineVariant(root.dark)
                            }

                            contentItem: RowLayout {
                                spacing: DesignTokens.spacing12

                                Rectangle {
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 42
                                    radius: DesignTokens.radiusControl
                                    color: DesignTokens.navSurface(root.dark)

                                    Image {
                                        anchors.centerIn: parent
                                        width: 22
                                        height: 22
                                        source: startAction.modelData.icon
                                        sourceSize: Qt.size(22, 22)
                                        Accessible.ignored: true
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.tr(startAction.modelData.titleEn,
                                                      startAction.modelData.titleZh)
                                        color: DesignTokens.onSurface(root.dark)
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: root.tr(startAction.modelData.detailEn,
                                                      startAction.modelData.detailZh)
                                        color: DesignTokens.onSurfaceVariant(root.dark)
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 2
                                        elide: Text.ElideRight
                                    }
                                }

                                Label {
                                    text: "›"
                                    color: DesignTokens.onSurfaceVariant(root.dark)
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 22
                                    Accessible.ignored: true
                                }
                            }
                        }
                    }
                }
            }

            WfCard {
                dark: root.dark
                Layout.fillWidth: true
                Layout.fillHeight: root.availableWidth >= 860
                padding: 20

                ColumnLayout {
                    width: parent.width
                    spacing: DesignTokens.spacing12

                    RowLayout {
                        Layout.fillWidth: true

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Recent projects", "最近工程")
                                color: DesignTokens.onSurface(root.dark)
                                font.family: DesignTokens.fontDisplay
                                font.pixelSize: 18
                                font.weight: Font.Bold
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Select a project to open it.", "揀一個工程就可以開返。")
                                color: DesignTokens.onSurfaceVariant(root.dark)
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 12
                            }
                        }

                        WfButton {
                            dark: root.dark
                            visible: root.recentProjects.length > 0
                            text: root.tr("Clear", "清除")
                            variant: "text"
                            compact: true
                            onClicked: root.clearRecentRequested()
                        }
                    }

                    WfCard {
                        dark: root.dark
                        visible: root.recentProjects.length === 0
                        Layout.fillWidth: true
                        surfaceLevel: "low"
                        outlined: false
                        padding: 20

                        ColumnLayout {
                            width: parent.width
                            spacing: DesignTokens.spacing4

                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: root.tr("No recent projects yet", "仲未有最近工程")
                                color: DesignTokens.onSurface(root.dark)
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: root.tr(
                                    "Projects you create, open, or import will appear here.",
                                    "你開新、開返或者匯入過嘅工程會喺呢度出現。")
                                color: DesignTokens.onSurfaceVariant(root.dark)
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    Repeater {
                        model: root.recentProjects

                        delegate: AbstractButton {
                            id: recentProject
                            required property var modelData

                            Layout.fillWidth: true
                            implicitHeight: 66
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Accessible.role: Accessible.Button
                            Accessible.name: root.tr("Open %1", "開啟 %1").arg(modelData.name)
                            onClicked: root.recentRequested(modelData.path)

                            background: Rectangle {
                                radius: DesignTokens.radiusControl
                                color: recentProject.pressed
                                       ? DesignTokens.surfaceHighest(root.dark)
                                       : recentProject.hovered
                                         ? DesignTokens.surfaceHigh(root.dark)
                                         : "transparent"
                                border.width: recentProject.visualFocus ? 2 : 1
                                border.color: recentProject.visualFocus
                                              ? DesignTokens.primary(root.dark)
                                              : DesignTokens.outlineVariant(root.dark)
                            }

                            contentItem: RowLayout {
                                spacing: DesignTokens.spacing12

                                Rectangle {
                                    Layout.preferredWidth: 34
                                    Layout.preferredHeight: 34
                                    radius: DesignTokens.radiusControl
                                    color: DesignTokens.primaryContainer(root.dark)

                                    Label {
                                        anchors.centerIn: parent
                                        text: "W"
                                        color: DesignTokens.onPrimaryContainer(root.dark)
                                        font.family: DesignTokens.fontDisplay
                                        font.pixelSize: 14
                                        font.weight: Font.Bold
                                        Accessible.ignored: true
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1

                                    Label {
                                        Layout.fillWidth: true
                                        text: recentProject.modelData.name
                                        color: DesignTokens.onSurface(root.dark)
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: recentProject.modelData.path
                                        color: DesignTokens.onSurfaceVariant(root.dark)
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 10
                                        elide: Text.ElideMiddle
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: root.tr("Last opened: %1", "上次開啟：%1")
                                                  .arg(root.formattedLastOpened(
                                                           recentProject.modelData.lastOpened))
                                        color: DesignTokens.onSurfaceVariant(root.dark)
                                        opacity: 0.78
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }

                                WfIconButton {
                                    dark: root.dark
                                    glyph: "×"
                                    buttonSize: 30
                                    accessibleName: root.tr("Remove from recent projects",
                                                            "由最近工程移除")
                                    toolTip: accessibleName
                                    onClicked: root.removeRecentRequested(
                                                   recentProject.modelData.path)
                                }
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: DesignTokens.spacing8

            WfStatusChip {
                dark: root.dark
                text: root.tr("Local Git history", "本機 Git 歷史")
                tone: "success"
                showDot: true
            }
            Label {
                Layout.fillWidth: true
                text: root.tr(
                    "Opening the app never changes or automatically opens a project.",
                    "開 App 唔會改工程，亦唔會自動開任何工程。")
                color: DesignTokens.onSurfaceVariant(root.dark)
                font.family: DesignTokens.fontBody
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
        }
    }
}
