pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var app
    required property var tr
    property var compareA: null
    property var compareB: null

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: root.tr("History Time Machine", "歷史時光機"); font.pixelSize: 30; font.weight: Font.Bold }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Event-sourced actions, selective undo, redo-of-undo, restore points, bookmarks, branches, diffs, raw Git commits and crash recovery—without rewriting the past.",
                                  "事件式動作、選擇性 Undo、Undo 嘅 Redo、還原點、書籤、分支、diff、原始 Git commit 同死機復原；唔會竄改過去。")
                    wrapMode: Text.Wrap; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                }
            }
            Button { text: "↶  " + root.tr("Undo here", "喺呢度 Undo"); highlighted: true; enabled: app.projectLoaded; onClicked: app.undoContext("", "") }
            Button { text: "↕  " + root.tr("Complete save", "完整儲存"); onClicked: app.requestExportProject() }
        }

        RowLayout {
            Layout.fillWidth: true
            TextField { id: historySearch; Layout.fillWidth: true; placeholderText: root.tr("Search title, context, branch or changed path…", "搜尋標題、context、分支或者改動路徑…") }
            ComboBox {
                id: branchPicker
                model: app.historyBranches
                currentIndex: Math.max(0, app.historyBranches.indexOf(app.historyBranch))
                onActivated: app.switchHistoryBranch(currentText)
            }
            Button { text: "⑂  " + root.tr("New branch", "新分支"); onClicked: branchPopup.open() }
            Button { text: "★  " + root.tr("Bookmark", "書籤"); onClicked: bookmarkPopup.open() }
            Button { text: "↻  " + root.tr("Refresh", "重新整理"); onClicked: app.refreshHistory() }
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: "✦  " + root.tr("Action timeline", "動作時間線") }
            TabButton { text: "⌘  " + root.tr("Git commits", "Git commit") }
            TabButton { text: "🛟  " + root.tr("Recovery & notifications", "復原同通知") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            Item {
                RowLayout {
                    anchors.fill: parent
                    spacing: 10
                    ListView {
                        id: actionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 7
                        model: app.actionHistory
                        delegate: Pane {
                            id: eventCard
                            required property var modelData
                            readonly property bool matches: historySearch.text.trim().length === 0
                                || JSON.stringify(modelData).toLowerCase().indexOf(historySearch.text.toLowerCase()) >= 0
                            width: actionList.width
                            height: matches ? implicitHeight : 0
                            visible: matches
                            padding: 13
                            background: Rectangle {
                                radius: 17
                                color: eventCard.modelData.effective ? (Material.theme === Material.Dark ? "#211F26" : "#FFFBFE") : (Material.theme === Material.Dark ? "#252329" : "#F3EEF5")
                                border.width: eventCard.modelData.destructive ? 2 : 1
                                border.color: eventCard.modelData.destructive ? "#BA1A1A" : (Material.theme === Material.Dark ? "#49454F" : "#E7E0EC")
                            }
                            ColumnLayout {
                                anchors.fill: parent
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: eventCard.modelData.type === "compensation" ? (eventCard.modelData.icon === "redo" ? "↷" : "↶") : eventCard.modelData.type === "bookmark" ? "★" : eventCard.modelData.type === "branch" ? "⑂" : "✦"; font.pixelSize: 20; color: Material.accent }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Label { Layout.fillWidth: true; text: eventCard.modelData.title; font.weight: Font.DemiBold; font.pixelSize: 16; elide: Text.ElideRight }
                                        Label { Layout.fillWidth: true; text: eventCard.modelData.diffSummary || eventCard.modelData.description; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"; elide: Text.ElideRight }
                                    }
                                    Label { text: "#" + eventCard.modelData.sequence + "  ·  " + eventCard.modelData.contextKey + "  ·  " + eventCard.modelData.branch; font.family: "Cascadia Mono"; font.pixelSize: 10 }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: eventCard.modelData.effective ? root.tr("ACTIVE", "生效中") : root.tr("UNDONE", "已逆轉"); color: eventCard.modelData.effective ? "#386A20" : "#8B5000"; font.bold: true; font.pixelSize: 10 }
                                    Label { Layout.fillWidth: true; text: eventCard.modelData.changedPaths ? eventCard.modelData.changedPaths.join("  ·  ") : ""; elide: Text.ElideMiddle; font.family: "Cascadia Mono"; font.pixelSize: 9 }
                                    Button { visible: eventCard.modelData.canUndo; text: "↶  " + root.tr("Undo", "逆轉"); flat: true; onClicked: app.undoHistoryEvent(eventCard.modelData.id) }
                                    Button { visible: eventCard.modelData.canRedo; text: "↷  " + root.tr("Redo", "重做"); flat: true; onClicked: app.redoHistoryEvent(eventCard.modelData.id) }
                                    Button { visible: eventCard.modelData.type === "action"; text: "⏱  " + root.tr("Restore", "還原"); flat: true; onClicked: app.restoreHistoryEvent(eventCard.modelData.id) }
                                    ToolButton { text: root.compareA && root.compareA.id === eventCard.modelData.id ? "A✓" : "A"; onClicked: root.compareA = eventCard.modelData; ToolTip.visible: hovered; ToolTip.text: root.tr("Compare from", "比較起點") }
                                    ToolButton { text: root.compareB && root.compareB.id === eventCard.modelData.id ? "B✓" : "B"; onClicked: root.compareB = eventCard.modelData; ToolTip.visible: hovered; ToolTip.text: root.tr("Compare to", "比較終點") }
                                }
                            }
                        }
                        Label { anchors.centerIn: parent; visible: actionList.count === 0; text: root.tr("Make a project change and its immutable event appears here.", "改一下工程，永久事件就會出現喺度。") }
                    }

                    Pane {
                        Layout.preferredWidth: 335
                        Layout.fillHeight: true
                        padding: 15
                        background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: "⇄  " + root.tr("Live comparison", "即時比較"); font.pixelSize: 18; font.weight: Font.Bold }
                            Label { Layout.fillWidth: true; text: root.compareA ? "A  #" + root.compareA.sequence + "  " + root.compareA.title : root.tr("Choose A on the timeline", "喺時間線揀 A"); wrapMode: Text.Wrap }
                            Label { Layout.fillWidth: true; text: root.compareB ? "B  #" + root.compareB.sequence + "  " + root.compareB.title : root.tr("Choose B on the timeline", "喺時間線揀 B"); wrapMode: Text.Wrap }
                            Rectangle { Layout.fillWidth: true; height: 1; color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                            ScrollView {
                                Layout.fillWidth: true; Layout.fillHeight: true
                                TextArea {
                                    readOnly: true
                                    wrapMode: TextEdit.WrapAnywhere
                                    font.family: "Cascadia Mono"; font.pixelSize: 10
                                    text: root.compareA && root.compareB
                                        ? "A inverse\n" + JSON.stringify(root.compareA.inverseDiff, null, 2) + "\n\nB forward\n" + JSON.stringify(root.compareB.forwardDiff, null, 2)
                                        : root.tr("Pick two events to inspect their reversible state patches side by side.", "揀兩個事件，就可以睇佢哋可逆狀態 patch。")
                                }
                            }
                        }
                    }
                }
            }

            Pane {
                padding: 0
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 14
                        Label { text: root.tr("Raw project repository", "原始工程 Git 倉"); font.pixelSize: 18; font.weight: Font.Bold }
                        Item { Layout.fillWidth: true }
                        Label { text: app.projectHistoryCount + " commits  ·  " + app.projectRoot; elide: Text.ElideMiddle; Layout.maximumWidth: 620 }
                    }
                    ListView {
                        id: gitList
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: app.projectHistory
                        delegate: ItemDelegate {
                            required property var modelData
                            width: gitList.width
                            contentItem: RowLayout {
                                Label { text: modelData.isRevert ? "↶" : "●"; color: modelData.isRevert ? "#8B5000" : Material.accent; font.pixelSize: 18 }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { Layout.fillWidth: true; text: modelData.subject; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                    Label { text: modelData.shortHash + "  ·  " + modelData.timestamp; font.family: "Cascadia Mono"; font.pixelSize: 10; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71" }
                                }
                                ToolButton { text: "⧉"; onClicked: app.copyText(modelData.hash) }
                            }
                        }
                    }
                }
            }

            RowLayout {
                spacing: 12
                Pane {
                    Layout.fillWidth: true; Layout.fillHeight: true; padding: 18
                    background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: "🛟  " + root.tr("Crash recovery", "死機復原"); font.pixelSize: 20; font.weight: Font.Bold }
                        Label { Layout.fillWidth: true; text: root.tr("The journal is flushed on every transition. Interrupted mounts return as in-app recovery work, never a blocking system dialog.", "每次狀態轉換都即寫日誌；中斷掛載會變成 app 入面嘅復原工作，唔會彈阻塞式系統對話框。") ; wrapMode: Text.Wrap }
                        Label { text: "✓  " + root.tr("Atomic config writes", "原子式設定寫入") }
                        Label { text: "✓  " + root.tr("DAG operation checkpoints", "DAG 工序檢查點") }
                        Label { text: "✓  " + root.tr("Interrupted-run detection and safe-unmount action", "中斷工序偵測同安全卸載動作") }
                        Label { text: "✓  " + root.tr("Source and payload verification gates", "來源同 payload 驗證閘") }
                        Item { Layout.fillHeight: true }
                        Label { text: root.tr("Recovery directory", "復原資料夾"); color: Material.accent }
                        Label { Layout.fillWidth: true; text: app.recoveryPath; wrapMode: Text.WrapAnywhere; font.family: "Cascadia Mono"; font.pixelSize: 10 }
                    }
                }
                Pane {
                    Layout.fillWidth: true; Layout.fillHeight: true; padding: 18
                    background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: "🔔  " + root.tr("Notification ledger", "通知帳簿"); font.pixelSize: 20; font.weight: Font.Bold }
                        Label { Layout.fillWidth: true; text: root.tr("A separate Git repository commits new, read, unread, dismiss, restore and tombstoned-delete events. The complete save bundle carries it too.", "另一個 Git 倉會 commit 新增、已讀、未讀、閂埋、還原同墓碑刪除；完整儲存 bundle 亦會帶埋。") ; wrapMode: Text.Wrap }
                        Label { text: app.notificationRepoPath; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.family: "Cascadia Mono"; font.pixelSize: 10; color: Material.accent }
                        Button { text: "↶  " + root.tr("Undo latest notification event", "Undo 最新通知事件"); onClicked: app.undoLatestNotificationChange() }
                        Button { text: "🔔  " + root.tr("Create test event", "建立測試事件"); onClicked: app.sendTestNotification() }
                        Item { Layout.fillHeight: true }
                        Label { Layout.fillWidth: true; text: root.tr("Soft-delete means the item disappears from normal view while its record and Git ancestry stay recoverable.", "軟刪除即係平時睇唔到，但記錄同 Git 祖先仍然可復原。") ; wrapMode: Text.Wrap; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71" }
                    }
                }
            }
        }
    }

    Popup {
        id: branchPopup
        anchors.centerIn: Overlay.overlay
        modal: false; focus: true; width: 420; padding: 18
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { radius: 22; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.accent }
        ColumnLayout {
            anchors.fill: parent
            Label { text: "⑂  " + root.tr("Create history branch", "建立歷史分支"); font.pixelSize: 20; font.weight: Font.Bold }
            TextField { id: branchName; Layout.fillWidth: true; placeholderText: "experiment/no-bloat" }
            Button { Layout.alignment: Qt.AlignRight; text: root.tr("Create", "建立"); highlighted: true; enabled: branchName.text.trim().length > 0; onClicked: { app.branchHistoryEvent("", branchName.text.trim()); branchPopup.close() } }
        }
    }

    Popup {
        id: bookmarkPopup
        anchors.centerIn: Overlay.overlay
        modal: false; focus: true; width: 420; padding: 18
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { radius: 22; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.accent }
        ColumnLayout {
            anchors.fill: parent
            Label { text: "★  " + root.tr("Bookmark current point", "將而家加書籤"); font.pixelSize: 20; font.weight: Font.Bold }
            TextField { id: bookmarkName; Layout.fillWidth: true; placeholderText: root.tr("Known-good baseline", "已知正常基準") }
            Button { Layout.alignment: Qt.AlignRight; text: root.tr("Save", "儲存"); highlighted: true; enabled: bookmarkName.text.trim().length > 0; onClicked: { app.bookmarkHistoryEvent("", bookmarkName.text.trim()); bookmarkPopup.close() } }
        }
    }
}
