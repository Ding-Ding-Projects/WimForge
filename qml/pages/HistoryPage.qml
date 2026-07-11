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
    property var compareA: null
    property var compareB: null

    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
    readonly property bool compact: width < 820
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)
    readonly property color surfaceVariantForeground: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color outlineVariant: DesignTokens.outlineVariant(root.dark)
    readonly property color primary: DesignTokens.primary(root.dark)
    readonly property color success: DesignTokens.success(root.dark)
    readonly property color warning: DesignTokens.tertiary(root.dark)
    readonly property color error: DesignTokens.error(root.dark)
    readonly property var filteredActionHistory: {
        const source = root.app.actionHistory
        const query = historySearch.text.trim().toLowerCase()
        if (query.length === 0)
            return source

        const filtered = []
        for (let index = 0; index < source.length; ++index) {
            const serialized = JSON.stringify(source[index])
            if (String(serialized || "").toLowerCase().indexOf(query) >= 0)
                filtered.push(source[index])
        }
        return filtered
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: DesignTokens.spacing12

        WfPageHeader {
            Layout.fillWidth: true
            dark: root.dark
            eyebrow: root.tr("Project record", "工程記錄")
            title: root.tr("History Time Machine", "歷史時光機")
            description: root.tr("Inspect immutable actions, compare reversible patches, restore commits and recover interrupted work.",
                                 "檢查不可變動作、比較可逆 patch、還原 commit，同復原中斷工作。")

            WfButton {
                dark: root.dark
                variant: "tonal"
                glyph: "↶"
                text: root.tr("Undo here", "喺呢度 Undo")
                enabled: root.app.projectLoaded
                onClicked: root.app.undoContext("", "")
            }
            WfButton {
                dark: root.dark
                variant: "outlined"
                text: root.tr("Complete save", "完整儲存")
                onClicked: root.app.requestExportProject()
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            surfaceLevel: "low"
            padding: DesignTokens.spacing12

            GridLayout {
                anchors.fill: parent
                columns: root.width >= 980 ? 5 : root.width >= 620 ? 2 : 1
                columnSpacing: DesignTokens.spacing8
                rowSpacing: DesignTokens.spacing8

                WfField {
                    id: historySearch
                    Layout.fillWidth: true
                    dark: root.dark
                    placeholderText: root.tr("Search title, context, branch or changed path…",
                                             "搜尋標題、context、分支或者改動路徑…")
                }
                ComboBox {
                    id: branchPicker
                    Layout.fillWidth: root.width < 980
                    Layout.preferredHeight: DesignTokens.controlHeight
                    model: root.app.historyBranches
                    currentIndex: Math.max(0, root.app.historyBranches.indexOf(root.app.historyBranch))
                    Accessible.name: root.tr("History branch", "歷史分支")
                    onActivated: root.app.switchHistoryBranch(currentText)
                }
                WfButton {
                    Layout.fillWidth: root.width < 620
                    dark: root.dark
                    variant: "outlined"
                    text: root.tr("New branch", "新分支")
                    onClicked: branchPopup.open()
                }
                WfButton {
                    Layout.fillWidth: root.width < 620
                    dark: root.dark
                    variant: "outlined"
                    text: root.tr("Bookmark", "書籤")
                    onClicked: bookmarkPopup.open()
                }
                WfButton {
                    Layout.fillWidth: root.width < 620
                    dark: root.dark
                    variant: "text"
                    glyph: "↻"
                    text: root.tr("Refresh", "重新整理")
                    onClicked: root.app.refreshHistory()
                }
            }
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: root.outlineVariant
                }
            }
            TabButton { text: root.tr("Action timeline", "動作時間線"); font.family: DesignTokens.fontBody }
            TabButton { text: root.tr("Git commits", "Git commit"); font.family: DesignTokens.fontBody }
            TabButton { text: root.tr("Recovery & notifications", "復原同通知"); font.family: DesignTokens.fontBody }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            Item {
                GridLayout {
                    anchors.fill: parent
                    columns: root.compact ? 1 : 2
                    columnSpacing: DesignTokens.spacing12
                    rowSpacing: DesignTokens.spacing12

                    ListView {
                        id: actionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: 160
                        clip: true
                        spacing: DesignTokens.spacing8
                        boundsBehavior: Flickable.StopAtBounds
                        model: root.filteredActionHistory

                        delegate: WfCard {
                            id: eventCard
                            required property var modelData
                            width: actionList.width
                            height: implicitHeight
                            dark: root.dark
                            padding: DesignTokens.spacing12
                            fillColor: eventCard.modelData.effective
                                     ? DesignTokens.surfaceLowest(root.dark)
                                     : DesignTokens.surfaceLow(root.dark)
                            outlineColor: eventCard.modelData.destructive ? root.error : root.outlineVariant

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: DesignTokens.spacing8

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: actionList.width >= 620 ? 3 : 1
                                    columnSpacing: DesignTokens.spacing8
                                    rowSpacing: DesignTokens.spacing4

                                    Rectangle {
                                        Layout.preferredWidth: 34
                                        Layout.preferredHeight: 34
                                        radius: DesignTokens.radiusControl
                                        color: eventCard.modelData.destructive
                                               ? DesignTokens.errorContainer(root.dark)
                                               : DesignTokens.primaryContainer(root.dark)
                                        Label {
                                            anchors.centerIn: parent
                                            text: eventCard.modelData.type === "compensation"
                                                  ? (eventCard.modelData.icon === "redo" ? "R" : "U")
                                                  : eventCard.modelData.type === "bookmark" ? "B"
                                                  : eventCard.modelData.type === "branch" ? "BR" : "A"
                                            color: eventCard.modelData.destructive
                                                   ? DesignTokens.onErrorContainer(root.dark)
                                                   : DesignTokens.onPrimaryContainer(root.dark)
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: DesignTokens.spacing4
                                        Label {
                                            Layout.fillWidth: true
                                            text: eventCard.modelData.title
                                            color: root.surfaceForeground
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 14
                                            font.weight: Font.Bold
                                            wrapMode: Text.Wrap
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: eventCard.modelData.diffSummary || eventCard.modelData.description
                                            color: root.surfaceVariantForeground
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 11
                                            wrapMode: Text.Wrap
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: actionList.width < 620
                                        text: "#" + eventCard.modelData.sequence + " · "
                                              + eventCard.modelData.contextKey + " · "
                                              + eventCard.modelData.branch
                                        color: root.surfaceVariantForeground
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 10
                                        wrapMode: Text.WrapAnywhere
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: DesignTokens.spacing8
                                    WfStatusChip {
                                        dark: root.dark
                                        tone: eventCard.modelData.effective ? "success" : "warning"
                                        text: eventCard.modelData.effective
                                              ? root.tr("ACTIVE", "生效中")
                                              : root.tr("UNDONE", "已逆轉")
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: eventCard.modelData.changedPaths
                                              ? eventCard.modelData.changedPaths.join(" · ") : ""
                                        color: root.surfaceVariantForeground
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 9
                                        elide: Text.ElideMiddle
                                    }
                                    WfStatusChip {
                                        visible: eventCard.modelData.destructive
                                        dark: root.dark
                                        tone: "error"
                                        text: root.tr("DESTRUCTIVE", "有破壞性")
                                    }
                                }

                                ScrollView {
                                    id: eventActionsScroll
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: eventActions.implicitHeight
                                    ScrollBar.vertical.policy: ScrollBar.AlwaysOff
                                    ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                                    RowLayout {
                                        id: eventActions
                                        width: Math.max(implicitWidth, eventActionsScroll.availableWidth)
                                        spacing: DesignTokens.spacing4
                                        WfButton {
                                            visible: eventCard.modelData.canUndo
                                            dark: root.dark
                                            compact: true
                                            variant: "text"
                                            text: root.tr("Undo", "逆轉")
                                            onClicked: root.app.undoHistoryEvent(eventCard.modelData.id)
                                        }
                                        WfButton {
                                            visible: eventCard.modelData.canRedo
                                            dark: root.dark
                                            compact: true
                                            variant: "text"
                                            text: root.tr("Redo", "重做")
                                            onClicked: root.app.redoHistoryEvent(eventCard.modelData.id)
                                        }
                                        WfButton {
                                            visible: eventCard.modelData.type === "action"
                                            dark: root.dark
                                            compact: true
                                            variant: "text"
                                            text: root.tr("Restore", "還原")
                                            onClicked: root.app.restoreHistoryEvent(eventCard.modelData.id)
                                        }
                                        WfButton {
                                            dark: root.dark
                                            compact: true
                                            variant: root.compareA && root.compareA.id === eventCard.modelData.id
                                                     ? "tonal" : "outlined"
                                            text: root.compareA && root.compareA.id === eventCard.modelData.id ? "A selected" : "A"
                                            Accessible.name: root.tr("Compare from event %1", "由事件 %1 比較").arg(eventCard.modelData.sequence)
                                            onClicked: root.compareA = eventCard.modelData
                                        }
                                        WfButton {
                                            dark: root.dark
                                            compact: true
                                            variant: root.compareB && root.compareB.id === eventCard.modelData.id
                                                     ? "tonal" : "outlined"
                                            text: root.compareB && root.compareB.id === eventCard.modelData.id ? "B selected" : "B"
                                            Accessible.name: root.tr("Compare to event %1", "比較到事件 %1").arg(eventCard.modelData.sequence)
                                            onClicked: root.compareB = eventCard.modelData
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: Math.min(implicitWidth, parent.width - DesignTokens.spacing24)
                            visible: actionList.count === 0
                            text: historySearch.text.trim().length > 0
                                  ? root.tr("No history events match this search.",
                                            "冇歷史事件符合呢個搜尋。")
                                  : root.tr("Make a project change and its immutable event appears here.",
                                            "改一下工程，永久事件就會出現喺度。")
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontBody
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    WfCard {
                        Layout.fillWidth: root.compact
                        Layout.preferredWidth: root.compact ? -1 : 320
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: 160
                        dark: root.dark
                        surfaceLevel: "low"
                        padding: DesignTokens.spacing16

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: DesignTokens.spacing8
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Live comparison", "即時比較")
                                color: root.surfaceForeground
                                font.family: DesignTokens.fontDisplay
                                font.pixelSize: 18
                                font.weight: Font.Bold
                            }
                            ComparisonSlot {
                                Layout.fillWidth: true
                                marker: "A"
                                value: root.compareA
                                emptyText: root.tr("Choose A on the timeline", "喺時間線揀 A")
                            }
                            ComparisonSlot {
                                Layout.fillWidth: true
                                marker: "B"
                                value: root.compareB
                                emptyText: root.tr("Choose B on the timeline", "喺時間線揀 B")
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: root.outlineVariant
                            }
                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                TextArea {
                                    readOnly: true
                                    Accessible.name: root.tr("Event comparison", "事件比較")
                                    wrapMode: TextEdit.WrapAnywhere
                                    text: root.compareA && root.compareB
                                          ? "A inverse\n" + JSON.stringify(root.compareA.inverseDiff, null, 2)
                                            + "\n\nB forward\n" + JSON.stringify(root.compareB.forwardDiff, null, 2)
                                          : root.tr("Pick two events to inspect their reversible state patches.",
                                                    "揀兩個事件，就可以檢查佢哋嘅可逆狀態 patch。")
                                    color: root.surfaceForeground
                                    font.family: DesignTokens.fontMono
                                    font.pixelSize: 10
                                    background: Rectangle {
                                        radius: DesignTokens.radiusControl
                                        color: DesignTokens.surfaceDim(root.dark)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            WfCard {
                dark: root.dark
                padding: 0
                surfaceLevel: "lowest"
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    GridLayout {
                        Layout.fillWidth: true
                        Layout.margins: DesignTokens.spacing16
                        columns: root.width >= 700 ? 2 : 1
                        columnSpacing: DesignTokens.spacing8
                        rowSpacing: DesignTokens.spacing4
                        Label {
                            Layout.fillWidth: true
                            text: root.tr("Raw project repository", "原始工程 Git 倉")
                            color: root.surfaceForeground
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 18
                            font.weight: Font.Bold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.app.projectHistoryCount + " commits · " + root.app.projectRoot
                            color: root.surfaceVariantForeground
                            font.family: DesignTokens.fontMono
                            font.pixelSize: 10
                            wrapMode: Text.WrapAnywhere
                            horizontalAlignment: root.width >= 700 ? Text.AlignRight : Text.AlignLeft
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.outlineVariant }
                    ListView {
                        id: gitList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: root.app.projectHistory
                        delegate: ItemDelegate {
                            id: gitCommitDelegate
                            required property var modelData
                            width: gitList.width
                            implicitHeight: DesignTokens.rowHeight + DesignTokens.spacing12
                            contentItem: RowLayout {
                                spacing: DesignTokens.spacing12
                                Rectangle {
                                    Layout.preferredWidth: 8
                                    Layout.preferredHeight: 8
                                    radius: 4
                                    color: gitCommitDelegate.modelData.isRevert ? root.warning : root.primary
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: DesignTokens.spacing4
                                    Label {
                                        Layout.fillWidth: true
                                        text: gitCommitDelegate.modelData.subject
                                        color: root.surfaceForeground
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: gitCommitDelegate.modelData.shortHash + " · " + gitCommitDelegate.modelData.timestamp
                                        color: root.surfaceVariantForeground
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 10
                                        wrapMode: Text.Wrap
                                    }
                                }
                                WfIconButton {
                                    dark: root.dark
                                    glyph: "⧉"
                                    accessibleName: root.tr("Copy commit %1", "複製 commit %1").arg(gitCommitDelegate.modelData.shortHash)
                                    toolTip: accessibleName
                                    onClicked: root.app.copyText(gitCommitDelegate.modelData.hash)
                                }
                            }
                        }
                    }
                }
            }

            ScrollView {
                id: recoveryScroll
                clip: true
                GridLayout {
                    width: recoveryScroll.availableWidth
                    columns: root.width >= 800 ? 2 : 1
                    columnSpacing: DesignTokens.spacing12
                    rowSpacing: DesignTokens.spacing12

                    RecoveryCard {
                        Layout.fillWidth: true
                        Layout.minimumHeight: 300
                        title: root.tr("Crash recovery", "死機復原")
                        description: root.tr("The journal is flushed on every transition. Interrupted mounts return as in-app recovery work.",
                                             "每次狀態轉換都即寫日誌；中斷掛載會變成 app 入面嘅復原工作。")
                        pathLabel: root.tr("Recovery directory", "復原資料夾")
                        pathValue: root.app.recoveryPath
                        items: [
                            root.tr("Atomic configuration writes", "原子式設定寫入"),
                            root.tr("DAG operation checkpoints", "DAG 工序檢查點"),
                            root.tr("Interrupted-run detection and safe-unmount action", "中斷工序偵測同安全卸載動作"),
                            root.tr("Source and payload verification gates", "來源同 payload 驗證閘")
                        ]
                    }

                    WfCard {
                        Layout.fillWidth: true
                        Layout.minimumHeight: 300
                        dark: root.dark
                        surfaceLevel: "lowest"
                        padding: DesignTokens.spacing16
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: DesignTokens.spacing8
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: root.tr("Notification ledger", "通知帳簿")
                                    color: root.surfaceForeground
                                    font.family: DesignTokens.fontDisplay
                                    font.pixelSize: 18
                                    font.weight: Font.Bold
                                }
                                WfStatusChip { dark: root.dark; tone: "success"; text: root.tr("RECOVERABLE", "可復原") }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("A separate Git repository commits new, read, unread, dismiss, restore and tombstoned-delete events. The complete save carries it too.",
                                              "另一個 Git 倉會 commit 新增、已讀、未讀、閂埋、還原同墓碑刪除；完整儲存亦會帶埋。")
                                color: root.surfaceVariantForeground
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.app.notificationRepoPath
                                color: root.surfaceForeground
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 10
                                wrapMode: Text.WrapAnywhere
                            }
                            WfButton {
                                Layout.fillWidth: root.compact
                                dark: root.dark
                                variant: "outlined"
                                text: root.tr("Undo latest notification event", "Undo 最新通知事件")
                                onClicked: root.app.undoLatestNotificationChange()
                            }
                            WfButton {
                                Layout.fillWidth: root.compact
                                dark: root.dark
                                variant: "tonal"
                                text: root.tr("Create test event", "建立測試事件")
                                onClicked: root.app.sendTestNotification()
                            }
                            Item { Layout.fillHeight: true }
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Soft-delete hides an item while its record and Git ancestry remain recoverable.",
                                              "軟刪除會隱藏項目，但記錄同 Git 祖先仍然可以復原。")
                                color: root.surfaceVariantForeground
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: branchPopup
        anchors.centerIn: Overlay.overlay
        modal: false
        focus: true
        width: Math.min(420, Math.max(260, root.width - 40))
        padding: DesignTokens.spacing16
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.dark)
            border.width: 1
            border.color: root.outlineVariant
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: DesignTokens.spacing12
            Label {
                text: root.tr("Create history branch", "建立歷史分支")
                color: root.surfaceForeground
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 18
                font.weight: Font.Bold
            }
            WfField { id: branchName; Layout.fillWidth: true; dark: root.dark; placeholderText: "experiment/no-bloat" }
            WfButton {
                Layout.alignment: Qt.AlignRight
                dark: root.dark
                variant: "filled"
                text: root.tr("Create", "建立")
                enabled: branchName.text.trim().length > 0
                onClicked: {
                    root.app.branchHistoryEvent("", branchName.text.trim())
                    branchPopup.close()
                }
            }
        }
    }

    Popup {
        id: bookmarkPopup
        anchors.centerIn: Overlay.overlay
        modal: false
        focus: true
        width: Math.min(420, Math.max(260, root.width - 40))
        padding: DesignTokens.spacing16
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.dark)
            border.width: 1
            border.color: root.outlineVariant
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: DesignTokens.spacing12
            Label {
                text: root.tr("Bookmark current point", "將而家加書籤")
                color: root.surfaceForeground
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 18
                font.weight: Font.Bold
            }
            WfField {
                id: bookmarkName
                Layout.fillWidth: true
                dark: root.dark
                placeholderText: root.tr("Known-good baseline", "已知正常基準")
            }
            WfButton {
                Layout.alignment: Qt.AlignRight
                dark: root.dark
                variant: "filled"
                text: root.tr("Save", "儲存")
                enabled: bookmarkName.text.trim().length > 0
                onClicked: {
                    root.app.bookmarkHistoryEvent("", bookmarkName.text.trim())
                    bookmarkPopup.close()
                }
            }
        }
    }

    component ComparisonSlot: Rectangle {
        id: comparisonSlot
        required property string marker
        required property var value
        required property string emptyText
        implicitHeight: Math.max(DesignTokens.rowHeight, slotContent.implicitHeight + DesignTokens.spacing12)
        radius: DesignTokens.radiusControl
        color: DesignTokens.surfaceDim(root.dark)
        border.width: 1
        border.color: root.outlineVariant
        RowLayout {
            id: slotContent
            anchors.fill: parent
            anchors.margins: DesignTokens.spacing8
            spacing: DesignTokens.spacing8
            Rectangle {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                radius: 12
                color: DesignTokens.primaryContainer(root.dark)
                Label {
                    anchors.centerIn: parent
                    text: comparisonSlot.marker
                    color: DesignTokens.onPrimaryContainer(root.dark)
                    font.family: DesignTokens.fontMono
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }
            }
            Label {
                Layout.fillWidth: true
                text: comparisonSlot.value
                      ? "#" + comparisonSlot.value.sequence + "  " + comparisonSlot.value.title
                      : comparisonSlot.emptyText
                color: comparisonSlot.value ? root.surfaceForeground : root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
        }
    }

    component RecoveryCard: WfCard {
        id: recoveryCard
        required property string title
        required property string description
        required property string pathLabel
        required property string pathValue
        required property var items
        dark: root.dark
        surfaceLevel: "lowest"
        padding: DesignTokens.spacing16
        ColumnLayout {
            anchors.fill: parent
            spacing: DesignTokens.spacing8
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: recoveryCard.title
                    color: root.surfaceForeground
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 18
                    font.weight: Font.Bold
                }
                WfStatusChip { dark: root.dark; tone: "success"; text: root.tr("READY", "就緒") }
            }
            Label {
                Layout.fillWidth: true
                text: recoveryCard.description
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Repeater {
                model: recoveryCard.items
                delegate: RowLayout {
                    id: recoveryItem
                    required property string modelData
                    Layout.fillWidth: true
                    spacing: DesignTokens.spacing8
                    Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: root.success }
                    Label {
                        Layout.fillWidth: true
                        text: recoveryItem.modelData
                        color: root.surfaceForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }
            }
            Item { Layout.fillHeight: true }
            Label {
                text: recoveryCard.pathLabel
                color: root.surfaceVariantForeground
                font.family: DesignTokens.fontBody
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: recoveryCard.pathValue
                color: root.surfaceForeground
                font.family: DesignTokens.fontMono
                font.pixelSize: 10
                wrapMode: Text.WrapAnywhere
            }
        }
    }
}
