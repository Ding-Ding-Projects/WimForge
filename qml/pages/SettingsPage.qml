import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root
    property var app
    property var tr: function(en, zh) { return en }
    property int requestedTheme: 0
    clip: true

    ColumnLayout {
        width: root.availableWidth
        spacing: 16

        Label { text: root.tr("Settings", "設定"); font.pixelSize: 30; font.weight: Font.Bold }

        GroupBox {
            Layout.fillWidth: true
            title: "🌐  " + root.tr("Language", "語言")
            ColumnLayout {
                anchors.fill: parent
                Label {
                    text: root.tr("English, proper Hong Kong Cantonese, or both side-by-side.", "英文、正宗香港粵語，或者兩樣一齊睇。")
                    color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                }
                RowLayout {
                    RadioButton { text: "English"; checked: app.languageMode === 0; onClicked: app.languageMode = 0 }
                    RadioButton { text: "香港粵語"; checked: app.languageMode === 1; onClicked: app.languageMode = 1 }
                    RadioButton { text: "English + 粵語"; checked: app.languageMode === 2; onClicked: app.languageMode = 2 }
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "◐  " + root.tr("Appearance", "外觀")
            GridLayout {
                anchors.fill: parent
                columns: width > 620 ? 2 : 1
                Label { text: root.tr("Material color theme", "Material 顏色主題") }
                ComboBox {
                    Layout.fillWidth: true
                    model: [root.tr("Follow system", "跟系統"), root.tr("Light", "光猛"), root.tr("Dark", "熄燈")]
                    currentIndex: app.themeMode
                    onActivated: app.themeMode = currentIndex
                }
                Label { text: root.tr("Interface density", "介面密度") }
                Slider { Layout.fillWidth: true; from: 0.8; to: 1.25; stepSize: 0.05; value: app.interfaceScale; onMoved: app.interfaceScale = value }
                Label { text: root.tr("Motion", "動畫") }
                Switch { text: root.tr("Use gentle Material motion", "用柔和 Material 動畫"); checked: app.motionEnabled; onToggled: app.motionEnabled = checked }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "⚡  " + root.tr("Jobs & concurrency", "工序同平行處理")
            GridLayout {
                anchors.fill: parent
                columns: width > 620 ? 2 : 1
                Label { text: root.tr("Maximum parallel jobs", "最多平行工序") }
                SpinBox { from: 1; to: 16; value: app.maxParallelJobs; editable: true; onValueModified: app.maxParallelJobs = value }
                Label { text: root.tr("CPU thread ceiling", "CPU 執行緒上限") }
                RangeSlider { Layout.fillWidth: true; from: 1; to: Math.max(2, app.logicalCpuCount); first.value: 1; second.value: app.threadLimit; second.onMoved: app.threadLimit = Math.round(second.value) }
                Label { text: root.tr("Scratch-space reserve", "暫存空間保留") }
                SpinBox { from: 5; to: 500; value: app.scratchReserveGb; editable: true; textFromValue: value => value + " GB"; onValueModified: app.scratchReserveGb = value }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "🛟  " + root.tr("Failsafes", "保命措施")
            ColumnLayout {
                anchors.fill: parent
                SwitchDelegate { Layout.fillWidth: true; text: root.tr("Flush crash journal after every state transition", "每次狀態轉換都即刻寫死機日誌"); checked: app.crashJournalEnabled; onToggled: app.crashJournalEnabled = checked }
                SwitchDelegate { Layout.fillWidth: true; text: root.tr("Hash source before apply and verify it has not changed", "套用之前 hash 來源，確認冇人中途換貨"); checked: app.verifySourceHash; onToggled: app.verifySourceHash = checked }
                SwitchDelegate { Layout.fillWidth: true; text: root.tr("Require checkpoint before destructive operations", "危險工序之前強制落檢查點"); checked: app.checkpointBeforeDestructive; onToggled: app.checkpointBeforeDestructive = checked }
                SwitchDelegate { Layout.fillWidth: true; text: root.tr("Keep recoverable tombstones instead of permanent deletes", "刪除只留可還原墓碑，唔永久消失"); checked: true; enabled: false }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "⇄  " + root.tr("Automatic import & export", "自動匯入同匯出")
            ColumnLayout {
                anchors.fill: parent
                Switch { text: root.tr("Watch project config for external changes", "監察工程設定畀外面改咗未"); checked: app.autoImport; onToggled: app.autoImport = checked }
                Switch { text: root.tr("Export a portable config after every commit", "每個 commit 之後自動匯出可攜設定"); checked: app.autoExport; onToggled: app.autoExport = checked }
                TextField { Layout.fillWidth: true; text: app.autoExportPath; enabled: app.autoExport; placeholderText: root.tr("Auto-export destination", "自動匯出目的地"); onEditingFinished: app.autoExportPath = text }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "🔔  " + root.tr("Notification center", "通知中心")
            ColumnLayout {
                anchors.fill: parent
                Label { Layout.fillWidth: true; text: root.tr("Its state lives in a separate local Git repository so even reading or dismissing a message is recoverable.", "通知狀態放喺獨立本機 Git 倉，連已讀同閂埋都可以還原。夠晒有交帶。") ; wrapMode: Text.Wrap }
                Label { text: app.notificationRepoPath; font.family: "Cascadia Mono"; font.pixelSize: 11 }
                Button { icon.name: "notifications"; text: root.tr("Send a test notification", "發個測試通知"); onClicked: app.sendTestNotification() }
            }
        }
        Item { Layout.preferredHeight: 20 }
    }
}
