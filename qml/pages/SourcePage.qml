import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root
    property var app
    property var tr: function(en, zh) { return en }
    clip: true

    ColumnLayout {
        width: root.availableWidth
        spacing: 18

        Label { text: root.tr("Source & editions", "來源同版本"); font.pixelSize: 30; font.weight: Font.Bold }
        Label {
            Layout.fillWidth: true
            text: root.tr("Paste or drop a source path. WimForge inventories it first; mounting and writes only happen in a reviewed job.",
                          "貼路徑或者拖隻映像入嚟。WimForge 會先點貨；睇完計劃先至會掛載同落筆，唔會一嚟就亂咁搞。")
            wrapMode: Text.Wrap
            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
        }

        Pane {
            Layout.fillWidth: true
            padding: 20
            background: Rectangle {
                radius: 20
                color: sourceDrop.containsDrag
                       ? (Material.theme === Material.Dark ? "#332D41" : "#F1EAFE")
                       : (Material.theme === Material.Dark ? "#211F26" : "#FFFBFE")
                border.color: sourceDrop.containsDrag ? Material.accent : (Material.theme === Material.Dark ? "#49454F" : "#CAC4D0")
                border.width: sourceDrop.containsDrag ? 2 : 1
            }
            DropArea {
                id: sourceDrop
                anchors.fill: parent
                onDropped: drop => {
                    if (drop.urls.length > 0) app.setProjectField("sourcePath", drop.urls[0].toLocalFile())
                }
            }
            ColumnLayout {
                anchors.fill: parent
                spacing: 12
                RowLayout {
                    Label { text: "◫"; font.pixelSize: 34; color: Material.accent }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: root.tr("Windows media or image", "Windows 安裝碟或者映像"); font.pixelSize: 18; font.weight: Font.DemiBold }
                        Label {
                            text: root.tr("ISO, extracted media folder, WIM, ESD or SWM", "ISO、已解壓安裝資料夾、WIM、ESD 或 SWM")
                            color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                        }
                    }
                    Button { icon.name: "view-refresh"; text: root.tr("Inspect", "點貨"); onClicked: app.inspectSource() }
                }
                TextField {
                    Layout.fillWidth: true
                    text: app.sourcePath
                    placeholderText: root.tr("D:\\Windows11.iso or D:\\media\\sources\\install.wim", "例如 D:\\Windows11.iso")
                    selectByMouse: true
                    onEditingFinished: app.setProjectField("sourcePath", text)
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: width > 850 ? 2 : 1
            columnSpacing: 14
            rowSpacing: 14

            GroupBox {
                Layout.fillWidth: true
                title: root.tr("Working copy", "工作副本")
                ColumnLayout {
                    anchors.fill: parent
                    TextField {
                        Layout.fillWidth: true
                        text: app.imagePath
                        placeholderText: root.tr("Path to install.wim / install.esd", "install.wim / install.esd 路徑")
                        onEditingFinished: app.setProjectField("imagePath", text)
                    }
                    TextField {
                        Layout.fillWidth: true
                        text: app.mountPath
                        placeholderText: root.tr("Empty mount directory", "空白掛載資料夾")
                        onEditingFinished: app.setProjectField("mountPath", text)
                    }
                    CheckBox {
                        text: root.tr("Clone source before editing (recommended)", "落手之前複製來源（推薦，咪慳呢啲時間）")
                        checked: app.cloneSource
                        onToggled: app.setProjectBool("cloneSource", checked)
                    }
                }
            }

            GroupBox {
                Layout.fillWidth: true
                title: root.tr("Edition target", "目標版本")
                ColumnLayout {
                    anchors.fill: parent
                    ComboBox {
                        Layout.fillWidth: true
                        model: app.editionNames
                        currentIndex: Math.max(0, app.imageIndex - 1)
                        onActivated: app.setProjectNumber("imageIndex", currentIndex + 1)
                    }
                    SpinBox {
                        from: 1; to: 999
                        value: app.imageIndex
                        editable: true
                        onValueModified: app.setProjectNumber("imageIndex", value)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: app.imageSummary
                        wrapMode: Text.Wrap
                        color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                    }
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: root.tr("Output", "輸出")
            GridLayout {
                anchors.fill: parent
                columns: width > 700 ? 3 : 1
                TextField {
                    Layout.fillWidth: true
                    text: app.outputPath
                    placeholderText: root.tr("Output ISO / image", "輸出 ISO / 映像")
                    onEditingFinished: app.setProjectField("outputPath", text)
                }
                ComboBox {
                    id: format
                    Layout.fillWidth: true
                    model: ["WIM", "ESD", "SWM", "ISO"]
                    currentIndex: Math.max(0, model.indexOf(app.outputFormat.toUpperCase()))
                    onActivated: app.setProjectField("outputFormat", currentText.toLowerCase())
                }
                TextField {
                    Layout.fillWidth: true
                    text: app.isoLabel
                    placeholderText: root.tr("ISO volume label", "ISO 碟名")
                    maximumLength: 32
                    onEditingFinished: app.setProjectField("isoLabel", text)
                }
            }
        }
        Item { Layout.preferredHeight: 20 }
    }
}
