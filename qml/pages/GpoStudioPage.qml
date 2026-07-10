pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var app
    required property var tr
    property var selectedPolicy: null
    property var elementValues: ({})

    Component.onCompleted: app.loadGpoCatalog()

    function selectPolicy(policy) {
        selectedPolicy = policy
        var defaults = ({})
        for (var i = 0; i < policy.elements.length; ++i) {
            var element = policy.elements[i]
            if (element.control === "Switch") {
                defaults[element.id] = element.defaultChecked
            } else if (element.control === "ComboBox") {
                var selected = element.defaultValue
                if ((!selected || selected.length === 0) && element.options.length > 0)
                    selected = element.options[0].value
                defaults[element.id] = selected
            } else {
                defaults[element.id] = element.defaultValue
            }
        }
        elementValues = defaults
    }

    function policyState(action) {
        var scope = selectedPolicy && (selectedPolicy.policyClass === "User"
                    || (selectedPolicy.policyClass === "Both" && policyScope.currentIndex === 1)) ? "user" : "machine"
        return scope + action
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: root.tr("Group Policy Studio", "群組原則工房"); font.pixelSize: 30; font.weight: Font.Bold }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Every installed ADMX/ADML policy, its complete explanation, and a schema-matched Material editor.",
                                  "所有已安裝 ADMX／ADML 政策、完整解說，同埋跟 schema 自動配對嘅 Material 編輯器。")
                    wrapMode: Text.Wrap
                    color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"
                }
            }
            Label { text: app.gpoPolicyCount.toLocaleString() + "  " + root.tr("policies", "項政策"); font.pixelSize: 18; color: Material.accent }
            Button { text: "⇩  " + root.tr("Export all docs", "匯出全部文件"); enabled: app.gpoLoaded; onClicked: docsExport.open() }
        }

        Pane {
            Layout.fillWidth: true
            padding: 12
            background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
            ColumnLayout {
                anchors.fill: parent
                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: policySearch
                        Layout.fillWidth: true
                        placeholderText: root.tr("Search policy names, docs, categories, registry paths…", "搜尋政策名、文件、分類、登錄路徑…")
                        leftPadding: 38
                        Label { anchors.left: parent.left; anchors.leftMargin: 13; anchors.verticalCenter: parent.verticalCenter; text: "⌕"; font.pixelSize: 19 }
                        onAccepted: app.searchGpo(text, regexMode.checked)
                    }
                    Switch { id: regexMode; text: root.tr("Regex", "正規表示式") }
                    Button { text: "🧩  " + root.tr("Regex builder", "Regex 精靈"); onClicked: regexWizard.open() }
                    Button { text: root.tr("Search", "搜尋"); highlighted: true; onClicked: app.searchGpo(policySearch.text, regexMode.checked) }
                }
                RowLayout {
                    Layout.fillWidth: true
                    TextField { id: intent; Layout.fillWidth: true; placeholderText: root.tr("I don't know the policy name; I want Windows to…", "唔知政策叫咩名；我想 Windows…") }
                    Button {
                        text: "✦  " + root.tr("Ask OpenCode", "問 OpenCode")
                        enabled: intent.text.trim().length > 0 && !app.openCodeBusy
                        onClicked: app.askOpenCodeForGpo(intent.text)
                    }
                    BusyIndicator { running: app.openCodeBusy; visible: running; implicitWidth: 28; implicitHeight: 28 }
                }
                Label { Layout.fillWidth: true; text: app.gpoStatus; elide: Text.ElideMiddle; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71" }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Pane {
                Layout.preferredWidth: Math.max(340, parent.width * 0.39)
                Layout.fillHeight: true
                padding: 8
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                ListView {
                    id: policyList
                    anchors.fill: parent
                    clip: true
                    spacing: 4
                    model: app.gpoResults
                    delegate: ItemDelegate {
                        required property var modelData
                        width: policyList.width
                        highlighted: root.selectedPolicy && root.selectedPolicy.id === modelData.id
                        onClicked: root.selectPolicy(modelData)
                        contentItem: ColumnLayout {
                            spacing: 2
                            Label { Layout.fillWidth: true; text: modelData.name; font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                            Label { Layout.fillWidth: true; text: modelData.policyClass + "  ·  " + modelData.category; font.pixelSize: 10; elide: Text.ElideMiddle; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71" }
                        }
                        background: Rectangle { radius: 14; color: parent.highlighted ? (Material.theme === Material.Dark ? "#4A4458" : "#E8DEF8") : parent.hovered ? (Material.theme === Material.Dark ? "#2B292F" : "#F7F2FA") : "transparent" }
                    }
                }
            }

            Pane {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 0
                background: Rectangle { radius: 18; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.theme === Material.Dark ? "#49454F" : "#E7E0EC" }
                ScrollView {
                    anchors.fill: parent
                    clip: true
                    ColumnLayout {
                        width: Math.max(0, parent.width - 28)
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 12
                        Item { Layout.preferredHeight: 4 }
                        Label {
                            visible: !root.selectedPolicy
                            Layout.fillWidth: true
                            text: root.tr("Choose a policy to read its full documentation and edit it.", "揀一項政策，就可以睇完整文件同修改。")
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            font.pixelSize: 18
                        }
                        Label { visible: !!root.selectedPolicy; Layout.fillWidth: true; text: root.selectedPolicy ? root.selectedPolicy.name : ""; font.pixelSize: 24; font.weight: Font.Bold; wrapMode: Text.Wrap }
                        RowLayout {
                            visible: !!root.selectedPolicy
                            Layout.fillWidth: true
                            Label { text: root.selectedPolicy ? root.selectedPolicy.policyClass : ""; color: Material.accent; font.weight: Font.DemiBold }
                            ComboBox {
                                id: policyScope
                                visible: root.selectedPolicy && root.selectedPolicy.policyClass === "Both"
                                model: [root.tr("Computer", "電腦"), root.tr("User", "使用者")]
                            }
                            Label { text: "·" }
                            Label { Layout.fillWidth: true; text: root.selectedPolicy ? root.selectedPolicy.supportedOn : ""; elide: Text.ElideRight }
                        }
                        Label { visible: !!root.selectedPolicy; Layout.fillWidth: true; text: root.selectedPolicy ? root.selectedPolicy.category : ""; color: Material.theme === Material.Dark ? "#CAC4D0" : "#625B71"; wrapMode: Text.Wrap }
                        Label { visible: !!root.selectedPolicy; Layout.fillWidth: true; text: root.selectedPolicy ? root.selectedPolicy.documentation : ""; wrapMode: Text.Wrap; textFormat: Text.PlainText }

                        Repeater {
                            model: root.selectedPolicy ? root.selectedPolicy.elements : []
                            delegate: Pane {
                                id: elementCard
                                required property var modelData
                                Layout.fillWidth: true
                                padding: 12
                                background: Rectangle { radius: 14; color: Material.theme === Material.Dark ? "#2B292F" : "#F7F2FA" }
                                ColumnLayout {
                                    anchors.fill: parent
                                    Label { Layout.fillWidth: true; text: modelData.label + (modelData.required ? "  *" : ""); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                                    Label { text: modelData.control; font.pixelSize: 10; color: Material.accent }
                                    Loader {
                                        Layout.fillWidth: true
                                        property var elementModel: elementCard.modelData
                                        sourceComponent: elementModel.control === "Switch" ? switchEditor
                                                       : elementModel.control === "ComboBox" ? comboEditor
                                                       : elementModel.control === "SpinBox" && elementModel.numericTextEditor ? numericTextEditor
                                                       : elementModel.control === "SpinBox" ? spinEditor
                                                       : elementModel.control === "TextArea" || elementModel.control === "ListEditor" ? areaEditor
                                                       : textEditor
                                    }
                                }
                            }
                        }

                        RowLayout {
                            visible: !!root.selectedPolicy
                            Layout.fillWidth: true
                            Button { text: "✓  " + root.tr("Enable", "啟用"); highlighted: true; onClicked: app.applyGpoPolicy(root.selectedPolicy.id, root.policyState("Enabled"), root.elementValues) }
                            Button { text: "×  " + root.tr("Disable", "停用"); onClicked: app.applyGpoPolicy(root.selectedPolicy.id, root.policyState("Disabled"), root.elementValues) }
                            Button { text: "↺  " + root.tr("Not configured", "未設定"); flat: true; onClicked: app.applyGpoPolicy(root.selectedPolicy.id, root.policyState("NotConfigured"), {}) }
                            Item { Layout.fillWidth: true }
                            Label { text: root.tr("Every choice is committed", "每個選擇都會 commit"); font.pixelSize: 10; color: "#386A20" }
                        }
                        Item { Layout.preferredHeight: 8 }
                    }
                }
            }
        }
    }

    Component {
        id: switchEditor
        Switch {
            required property var elementModel
            text: checked ? root.tr("On", "開") : root.tr("Off", "關")
            checked: elementModel.defaultChecked
            onToggled: root.elementValues[elementModel.id] = checked
        }
    }
    Component {
        id: comboEditor
        ComboBox {
            required property var elementModel
            model: elementModel.options
            textRole: "label"
            Component.onCompleted: {
                for (var i = 0; i < model.length; ++i) {
                    if (model[i].value === elementModel.defaultValue) {
                        currentIndex = i
                        break
                    }
                }
            }
            onActivated: root.elementValues[elementModel.id] = model[index].value
        }
    }
    Component {
        id: spinEditor
        SpinBox {
            required property var elementModel
            from: Math.max(-2147483648, Number(elementModel.minimum))
            to: Math.min(2147483647, Number(elementModel.maximum))
            value: Number(elementModel.defaultValue || 0)
            stepSize: Math.max(1, Number(elementModel.spinStep))
            editable: true
            onValueModified: root.elementValues[elementModel.id] = value
        }
    }
    Component {
        id: numericTextEditor
        TextField {
            required property var elementModel
            text: elementModel.defaultValue
            maximumLength: 20
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            placeholderText: root.tr("Integer from %1 to %2", "整數範圍 %1 至 %2")
                                 .arg(elementModel.minimum).arg(elementModel.maximum)
            validator: RegularExpressionValidator { regularExpression: /^-?[0-9]+$/ }
            onTextEdited: root.elementValues[elementModel.id] = text
        }
    }
    Component {
        id: textEditor
        TextField {
            required property var elementModel
            text: elementModel.defaultValue
            maximumLength: elementModel.maximumLength >= 0 ? elementModel.maximumLength : 32767
            placeholderText: root.tr("Value", "值")
            onTextEdited: root.elementValues[elementModel.id] = text
        }
    }
    Component {
        id: areaEditor
        TextArea {
            required property var elementModel
            text: elementModel.defaultValue
            placeholderText: elementModel.control === "ListEditor" ? root.tr("One entry per line", "每行一項") : root.tr("Text", "文字")
            wrapMode: TextEdit.Wrap
            onTextChanged: root.elementValues[elementModel.id] = text
        }
    }

    Popup {
        id: regexWizard
        anchors.centerIn: Overlay.overlay
        width: Math.min(620, root.width - 50)
        modal: false; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 22
        background: Rectangle { radius: 24; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.accent }
        ColumnLayout {
            anchors.fill: parent
            Label { text: "🧩  " + root.tr("Regex builder wizard", "Regex 建構精靈"); font.pixelSize: 22; font.weight: Font.Bold }
            Label { Layout.fillWidth: true; text: root.tr("Build a safe expression without memorizing punctuation. Invalid expressions stay in this panel and never interrupt other work.", "唔使背符號都砌到安全 expression；錯咗只會留喺呢塊 panel，唔會阻住其他工序。") ; wrapMode: Text.Wrap }
            TextField { id: containsWords; Layout.fillWidth: true; placeholderText: root.tr("Contains words (space separated)", "包含字詞（空格分隔）") }
            TextField { id: startsWith; Layout.fillWidth: true; placeholderText: root.tr("Starts with (optional)", "開頭係（可選）") }
            TextField { id: endsWith; Layout.fillWidth: true; placeholderText: root.tr("Ends with (optional)", "結尾係（可選）") }
            CheckBox { id: wholeWord; text: root.tr("Whole words", "完整字詞") }
            Label {
                id: regexPreview
                Layout.fillWidth: true
                text: {
                    var parts = containsWords.text.trim().split(/\s+/).filter(function(x) { return x.length > 0 })
                    var body = parts.length ? parts.map(function(x) { return "(?=.*" + x.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + ")" }).join("") + ".*" : ".*"
                    if (wholeWord.checked && parts.length === 1) body = "\\b" + parts[0].replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "\\b"
                    return "^" + startsWith.text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + body + endsWith.text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "$"
                }
                font.family: "Cascadia Mono"; color: Material.accent; wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: root.tr("Cancel", "取消"); flat: true; onClicked: regexWizard.close() }
                Button {
                    text: root.tr("Use & search", "使用同搜尋"); highlighted: true
                    onClicked: { policySearch.text = regexPreview.text; regexMode.checked = true; regexWizard.close(); app.searchGpo(policySearch.text, true) }
                }
            }
        }
    }

    Popup {
        id: docsExport
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, root.width - 50)
        modal: false; focus: true; padding: 20
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { radius: 22; color: Material.theme === Material.Dark ? "#211F26" : "#FFFBFE"; border.color: Material.accent }
        ColumnLayout {
            anchors.fill: parent
            Label { text: root.tr("Export complete bilingual GPO reference", "匯出完整雙語 GPO 參考"); font.pixelSize: 20; font.weight: Font.Bold }
            TextField { id: docsPath; Layout.fillWidth: true; placeholderText: "D:\\docs\\all-installed-policies.md" }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: root.tr("Cancel", "取消"); flat: true; onClicked: docsExport.close() }
                Button { text: root.tr("Export", "匯出"); highlighted: true; enabled: docsPath.text.trim().length > 0; onClicked: { if (app.exportGpoDocumentation(docsPath.text)) docsExport.close() } }
            }
        }
    }
}
