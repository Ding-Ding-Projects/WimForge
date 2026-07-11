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
    property bool active: false

    property var selectedPolicy: null
    property var elementValues: ({})
    readonly property bool compact: width < 860
    property int draftState: 1
    property bool documentationExpanded: false

    readonly property bool hasSelection: selectedPolicy !== null && selectedPolicy !== undefined
    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
    readonly property color surfaceColor: DesignTokens.surfaceLowest(root.dark)
    readonly property color surfaceVariantColor: DesignTokens.surfaceLow(root.dark)
    readonly property color surfaceContainerColor: DesignTokens.surfaceContainer(root.dark)
    readonly property color outlineColor: DesignTokens.outlineVariant(root.dark)
    readonly property color secondaryTextColor: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color surfaceForegroundColor: DesignTokens.onSurface(root.dark)
    readonly property color primaryColor: DesignTokens.primary(root.dark)
    readonly property color primaryContainerColor: DesignTokens.primaryContainer(root.dark)
    readonly property color primaryContainerForegroundColor: DesignTokens.onPrimaryContainer(root.dark)
    readonly property color successColor: DesignTokens.success(root.dark)
    readonly property color successContainerColor: DesignTokens.successContainer(root.dark)
    readonly property color successContainerForegroundColor: DesignTokens.onSuccessContainer(root.dark)

    function ensureCatalog() {
        if (!active)
            return
        if (!app.gpoLoaded)
            app.loadGpoCatalog()
        Qt.callLater(root.syncSelection)
    }

    Component.onCompleted: root.ensureCatalog()
    onActiveChanged: root.ensureCatalog()

    Connections {
        target: root.app
        function onStudioChanged() {
            Qt.callLater(root.syncSelection)
        }
    }

    function syncSelection() {
        var results = app.gpoResults
        if (!results || results.length === 0) {
            selectedPolicy = null
            policyList.currentIndex = -1
            return
        }

        if (selectedPolicy) {
            for (var i = 0; i < results.length; ++i) {
                if (results[i].id === selectedPolicy.id) {
                    selectedPolicy = results[i]
                    policyList.currentIndex = i
                    return
                }
            }
        }

        // A policy with schema fields makes the first visit immediately useful,
        // while still falling back to the first installed policy.
        var initialIndex = 0
        for (var candidate = 0; candidate < results.length; ++candidate) {
            if (results[candidate].elements && results[candidate].elements.length > 0) {
                initialIndex = candidate
                break
            }
        }
        policyList.currentIndex = initialIndex
        policyList.positionViewAtIndex(initialIndex, ListView.Contain)
        selectPolicy(results[initialIndex])
    }

    function selectPolicy(policy) {
        selectedPolicy = policy
        documentationExpanded = false
        draftState = 1
        stateTabs.currentIndex = 1
        if (policyScope)
            policyScope.currentIndex = 0

        var defaults = ({})
        var elements = policy && policy.elements ? policy.elements : []
        for (var i = 0; i < elements.length; ++i) {
            var element = elements[i]
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
                    || (selectedPolicy.policyClass === "Both" && policyScope.currentIndex === 1))
                    ? "user" : "machine"
        return scope + action
    }

    function stateName() {
        if (draftState === 0)
            return root.tr("Not configured", "未設定")
        if (draftState === 2)
            return root.tr("Disabled", "停用")
        return root.tr("Enabled", "啟用")
    }

    function stateAction() {
        if (draftState === 0)
            return "NotConfigured"
        if (draftState === 2)
            return "Disabled"
        return "Enabled"
    }

    function scopeName() {
        if (!selectedPolicy)
            return ""
        if (selectedPolicy.policyClass === "Both")
            return policyScope.currentIndex === 1 ? root.tr("User", "用戶") : root.tr("Computer", "電腦")
        return selectedPolicy.policyClass === "User" ? root.tr("User", "用戶") : root.tr("Computer", "電腦")
    }

    function registryTarget() {
        if (!selectedPolicy)
            return ""
        var userScope = selectedPolicy.policyClass === "User"
                        || (selectedPolicy.policyClass === "Both" && policyScope.currentIndex === 1)
        var hive = userScope ? "HKCU" : "HKLM"
        return hive + (selectedPolicy.registryKey ? "\\" + selectedPolicy.registryKey : "")
    }

    function commitDraft() {
        if (!selectedPolicy)
            return
        var values = draftState === 1 ? elementValues : ({})
        app.applyGpoPolicy(selectedPolicy.id, policyState(stateAction()), values)
    }

    Timer {
        id: searchDebounce
        interval: 350
        repeat: false
        onTriggered: root.app.searchGpo(policySearch.text, regexMode.checked)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: DesignTokens.spacing12

        GridLayout {
            Layout.fillWidth: true
            columns: root.width >= 760 ? 3 : 1
            columnSpacing: DesignTokens.spacing12
            rowSpacing: DesignTokens.spacing8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Group Policy Studio", "群組原則工房")
                    color: root.surfaceForegroundColor
                    font.family: DesignTokens.fontDisplay
                    font.pixelSize: 26
                    font.weight: Font.Bold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Find an installed ADMX policy, choose its desired state, and commit a reviewable image-build change.",
                                  "搵出已安裝 ADMX 政策、揀目標狀態，再 commit 一項可審核嘅映像建置變更。")
                    wrapMode: Text.Wrap
                    color: root.secondaryTextColor
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 13
                }
            }

            WfStatusChip {
                Layout.fillWidth: root.width < 760
                dark: root.dark
                tone: "primary"
                uppercase: false
                showDot: false
                text: root.app.gpoPolicyCount.toLocaleString() + " " + root.tr("installed", "已安裝")
            }

            WfButton {
                Layout.fillWidth: root.width < 760
                dark: root.dark
                variant: "outlined"
                text: root.tr("Export reference", "匯出參考")
                enabled: root.app.gpoLoaded
                onClicked: docsExport.open()
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            outlined: true
            surfaceLevel: "low"
            padding: DesignTokens.spacing12

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                GridLayout {
                    Layout.fillWidth: true
                    columns: root.width >= 760 ? 4 : 1
                    columnSpacing: DesignTokens.spacing8
                    rowSpacing: DesignTokens.spacing8
                    TextField {
                        id: policySearch
                        Layout.fillWidth: true
                        Layout.preferredHeight: DesignTokens.controlHeight
                        placeholderText: root.tr("Search names, explanations, categories, or registry paths", "搜尋名稱、解說、分類或登錄路徑")
                        selectByMouse: true
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 13
                        onTextEdited: searchDebounce.restart()
                        onAccepted: root.app.searchGpo(text, regexMode.checked)
                    }

                    Switch {
                        id: regexMode
                        Layout.fillWidth: root.width < 760
                        text: root.tr("Regex", "正規表示式")
                        onToggled: {
                            if (policySearch.text.trim().length > 0)
                                searchDebounce.restart()
                        }
                    }

                    WfButton {
                        Layout.fillWidth: root.width < 760
                        dark: root.dark
                        variant: "outlined"
                        text: root.tr("Build regex", "建立 Regex")
                        onClicked: regexWizard.open()
                    }

                    WfButton {
                        Layout.fillWidth: root.width < 760
                        dark: root.dark
                        variant: "filled"
                        text: root.tr("Search", "搜尋")
                        onClicked: root.app.searchGpo(policySearch.text, regexMode.checked)
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: root.width >= 660 ? 3 : 1
                    columnSpacing: 8
                    rowSpacing: 8

                    TextField {
                        id: intent
                        Layout.fillWidth: true
                        Layout.preferredHeight: DesignTokens.controlHeight
                        placeholderText: root.tr("Describe what you want Windows to do; OpenCode will suggest catalog keywords", "描述你想 Windows 做咩；OpenCode 會建議目錄關鍵字")
                        selectByMouse: true
                        onAccepted: {
                            if (text.trim().length > 0 && !root.app.openCodeBusy)
                                root.app.askOpenCodeForGpo(text)
                        }
                    }

                    WfButton {
                        Layout.fillWidth: root.width < 660
                        dark: root.dark
                        variant: "tonal"
                        text: root.tr("Suggest search", "建議搜尋")
                        enabled: intent.text.trim().length > 0 && !root.app.openCodeBusy
                        onClicked: root.app.askOpenCodeForGpo(intent.text)
                    }

                    BusyIndicator {
                        running: root.app.openCodeBusy
                        visible: running
                        implicitWidth: 28
                        implicitHeight: 28
                        Accessible.name: root.tr("OpenCode policy search in progress", "OpenCode 政策搜尋進行中")
                    }
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: root.compact ? 1 : 2
            columnSpacing: DesignTokens.spacing16
            rowSpacing: DesignTokens.spacing12

            WfCard {
                Layout.fillWidth: true
                Layout.preferredWidth: root.compact ? -1 : 340
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.minimumHeight: 120
                dark: root.dark
                outlined: true
                padding: DesignTokens.spacing8

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        Layout.topMargin: 5
                        Label {
                            text: root.tr("Policy catalog", "政策目錄")
                            color: root.surfaceForegroundColor
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 15
                            font.weight: Font.Bold
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: root.app.gpoResults.length + " " + root.tr("shown", "顯示")
                            color: root.primaryColor
                            font.weight: Font.DemiBold
                            font.pixelSize: 11
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        text: root.app.gpoStatus
                        elide: Text.ElideMiddle
                        color: root.secondaryTextColor
                        font.pixelSize: 10
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: root.outlineColor
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ListView {
                            id: policyList
                            anchors.fill: parent
                            clip: true
                            spacing: 3
                            model: root.app.gpoResults
                            boundsBehavior: Flickable.StopAtBounds

                            delegate: ItemDelegate {
                                id: policyDelegate
                                required property var modelData
                                required property int index
                                width: policyList.width
                                highlighted: root.hasSelection && root.selectedPolicy.id === modelData.id
                                leftPadding: 10
                                rightPadding: 10
                                topPadding: 9
                                bottomPadding: 9

                                onClicked: {
                                    policyList.currentIndex = index
                                    root.selectPolicy(modelData)
                                }

                                contentItem: RowLayout {
                                    spacing: 10

                                    Rectangle {
                                        Layout.preferredWidth: 30
                                        Layout.preferredHeight: 30
                                        radius: DesignTokens.radiusControl
                                        color: policyDelegate.highlighted ? root.primaryColor : root.surfaceContainerColor
                                        Label {
                                            anchors.centerIn: parent
                                            text: policyDelegate.modelData.policyClass === "Machine" ? "C"
                                                  : policyDelegate.modelData.policyClass === "User" ? "U" : "B"
                                            color: policyDelegate.highlighted ? DesignTokens.onPrimary(root.dark) : root.primaryColor
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            Layout.fillWidth: true
                                            text: policyDelegate.modelData.name
                                            color: policyDelegate.highlighted ? root.primaryContainerForegroundColor : root.surfaceForegroundColor
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 12
                                            font.weight: Font.DemiBold
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 2
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: policyDelegate.modelData.category || root.tr("Uncategorized policy", "未分類政策")
                                            font.pixelSize: 10
                                            elide: Text.ElideMiddle
                                            color: policyDelegate.highlighted ? root.primaryContainerForegroundColor : root.secondaryTextColor
                                        }
                                    }

                                    Label {
                                        visible: policyDelegate.modelData.elements && policyDelegate.modelData.elements.length > 0
                                        text: policyDelegate.modelData.elements.length
                                        color: policyDelegate.highlighted ? root.primaryContainerForegroundColor : root.secondaryTextColor
                                        font.pixelSize: 10
                                        padding: 4
                                        background: Rectangle {
                                            radius: 8
                                            color: policyDelegate.highlighted ? "transparent" : root.surfaceContainerColor
                                        }
                                    }
                                }

                                background: Rectangle {
                                    radius: DesignTokens.radiusControl
                                    color: policyDelegate.highlighted
                                           ? root.primaryContainerColor
                                           : policyDelegate.hovered
                                             ? root.surfaceContainerColor
                                             : "transparent"
                                }
                            }
                        }

                        Column {
                            anchors.centerIn: parent
                            width: Math.min(parent.width - 40, 280)
                            spacing: 10
                            visible: root.app.gpoResults.length === 0
                            BusyIndicator {
                                anchors.horizontalCenter: parent.horizontalCenter
                                running: !root.app.gpoLoaded
                                visible: running
                            }
                            Label {
                                width: parent.width
                                text: root.app.gpoLoaded
                                      ? root.tr("No matching policies", "冇相符政策")
                                      : root.tr("Loading installed policies…", "正在載入已安裝政策…")
                                horizontalAlignment: Text.AlignHCenter
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                            }
                            Label {
                                width: parent.width
                                text: root.app.gpoLoaded
                                      ? root.tr("Try fewer terms or turn off Regex.", "試下減少字詞或關閉 Regex。")
                                      : root.tr("Reading ADMX and language resources from Windows.", "正在讀取 Windows 嘅 ADMX 同語言資源。")
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                color: root.secondaryTextColor
                            }
                        }
                    }
                }
            }

            WfCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.minimumHeight: 120
                dark: root.dark
                outlined: true
                padding: 0

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Column {
                            anchors.centerIn: parent
                            width: Math.min(440, parent.width - 48)
                            spacing: 14
                            visible: !root.hasSelection

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 76
                                height: 34
                                radius: DesignTokens.radiusControl
                                color: root.primaryContainerColor
                                Label {
                                    anchors.centerIn: parent
                                    text: root.tr("POLICY", "政策")
                                    font.family: DesignTokens.fontMono
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                    color: root.primaryContainerForegroundColor
                                }
                            }
                            Label {
                                width: parent.width
                                text: root.tr("Select a policy to start a reviewable change", "揀一項政策開始可審核變更")
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                color: root.surfaceForegroundColor
                                font.family: DesignTokens.fontDisplay
                                font.pixelSize: 18
                                font.weight: Font.Bold
                            }
                            Label {
                                width: parent.width
                                text: root.tr("You will see the official explanation, scope, registry target, and a schema-matched value editor before anything is committed.",
                                              "Commit 之前，你會見到官方解說、範圍、登錄目標同 schema 配對值編輯器。")
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                color: root.secondaryTextColor
                            }
                        }

                        ScrollView {
                            id: detailsScroll
                            anchors.fill: parent
                            visible: root.hasSelection
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                            ColumnLayout {
                                width: Math.max(0, detailsScroll.availableWidth - 30)
                                x: 15
                                spacing: 12

                                Item { Layout.preferredHeight: 3 }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Rectangle {
                                        Layout.preferredWidth: scopeChip.implicitWidth + 20
                                        Layout.preferredHeight: 28
                                        radius: 14
                                        color: root.primaryContainerColor
                                        Label {
                                            id: scopeChip
                                            anchors.centerIn: parent
                                            text: root.scopeName().toUpperCase()
                                            color: root.primaryContainerForegroundColor
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: root.selectedPolicy ? root.selectedPolicy.category : ""
                                        color: root.secondaryTextColor
                                        elide: Text.ElideMiddle
                                    }
                                    Label {
                                        text: root.tr("NEW DRAFT", "新草稿")
                                        color: root.primaryColor
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 10
                                        font.weight: Font.Bold
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.selectedPolicy ? root.selectedPolicy.name : ""
                                    color: root.surfaceForegroundColor
                                    font.family: DesignTokens.fontDisplay
                                    font.pixelSize: 20
                                    font.weight: Font.Bold
                                    wrapMode: Text.Wrap
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: root.selectedPolicy && root.selectedPolicy.policyClass === "Both"
                                    Label {
                                        text: root.tr("Apply for", "套用到")
                                        color: root.secondaryTextColor
                                    }
                                    ComboBox {
                                        id: policyScope
                                        model: [root.tr("Computer", "電腦"), root.tr("User", "用戶")]
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.selectedPolicy ? root.selectedPolicy.documentation : ""
                                    wrapMode: Text.Wrap
                                    textFormat: Text.PlainText
                                    color: root.secondaryTextColor
                                    maximumLineCount: root.documentationExpanded ? 100 : 3
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: root.selectedPolicy && root.selectedPolicy.supportedOn.length > 0
                                    text: root.tr("Supported on: %1", "支援：%1")
                                              .arg(root.selectedPolicy ? root.selectedPolicy.supportedOn : "")
                                    color: root.primaryColor
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }

                                WfButton {
                                    visible: root.selectedPolicy && root.selectedPolicy.documentation.length > 240
                                    dark: root.dark
                                    variant: "text"
                                    text: root.documentationExpanded ? root.tr("Show less", "顯示較少") : root.tr("Read full explanation", "閱讀完整解說")
                                    onClicked: root.documentationExpanded = !root.documentationExpanded
                                }

                                WfCard {
                                    Layout.fillWidth: true
                                    dark: root.dark
                                    outlined: true
                                    surfaceLevel: "low"
                                    padding: DesignTokens.spacing12
                                    ColumnLayout {
                                        anchors.fill: parent
                                        spacing: 8
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label {
                                                text: root.tr("Desired policy state", "目標政策狀態")
                                                color: root.surfaceForegroundColor
                                                font.family: DesignTokens.fontDisplay
                                                font.pixelSize: 15
                                                font.weight: Font.Bold
                                            }
                                            Item { Layout.fillWidth: true }
                                            Label {
                                                text: root.tr("Draft only", "只係草稿")
                                                color: root.primaryColor
                                                font.family: DesignTokens.fontMono
                                                font.pixelSize: 10
                                                font.weight: Font.Bold
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: root.tr("Choose the state that the built image should receive.", "揀建置完成嘅映像應該收到邊個狀態。")
                                            color: root.secondaryTextColor
                                            wrapMode: Text.Wrap
                                        }
                                        TabBar {
                                            id: stateTabs
                                            Layout.fillWidth: true
                                            currentIndex: 1
                                            onCurrentIndexChanged: root.draftState = currentIndex
                                            TabButton { text: root.tr("Not configured", "未設定") }
                                            TabButton { text: root.tr("Enabled", "啟用") }
                                            TabButton { text: root.tr("Disabled", "停用") }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    visible: root.selectedPolicy && root.selectedPolicy.elements.length > 0
                                    enabled: root.draftState === 1
                                    opacity: enabled ? 1.0 : 0.55

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: root.tr("Enabled values", "啟用值")
                                            color: root.surfaceForegroundColor
                                            font.family: DesignTokens.fontDisplay
                                            font.pixelSize: 15
                                            font.weight: Font.Bold
                                        }
                                        Item { Layout.fillWidth: true }
                                        Label {
                                            text: root.selectedPolicy ? root.selectedPolicy.elements.length + " " + root.tr("field(s)", "個欄位") : ""
                                            color: root.secondaryTextColor
                                        }
                                    }

                                    Repeater {
                                        model: root.selectedPolicy ? root.selectedPolicy.elements : []
                                        delegate: WfCard {
                                            id: elementCard
                                            required property var modelData
                                            Layout.fillWidth: true
                                            dark: root.dark
                                            outlined: true
                                            surfaceLevel: "low"
                                            padding: DesignTokens.spacing12
                                            ColumnLayout {
                                                anchors.fill: parent
                                                spacing: 7
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: elementCard.modelData.label + (elementCard.modelData.required ? "  *" : "")
                                                        color: root.surfaceForegroundColor
                                                        font.family: DesignTokens.fontBody
                                                        font.weight: Font.DemiBold
                                                        wrapMode: Text.Wrap
                                                    }
                                                    Label {
                                                        text: elementCard.modelData.control
                                                        font.family: DesignTokens.fontMono
                                                        font.pixelSize: 9
                                                        font.weight: Font.Bold
                                                        color: root.primaryColor
                                                    }
                                                }
                                                Loader {
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: elementModel.control === "TextArea"
                                                                            || elementModel.control === "ListEditor" ? 96 : 48
                                                    property var elementModel: elementCard.modelData
                                                    onLoaded: {
                                                        if (item)
                                                            item["elementModel"] = elementModel
                                                    }
                                                    onElementModelChanged: {
                                                        if (item)
                                                            item["elementModel"] = elementModel
                                                    }
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
                                }

                                WfCard {
                                    Layout.fillWidth: true
                                    dark: root.dark
                                    outlined: true
                                    fillColor: root.successContainerColor
                                    outlineColor: root.successColor
                                    padding: DesignTokens.spacing12
                                    ColumnLayout {
                                        anchors.fill: parent
                                        spacing: 5
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label {
                                                text: root.tr("Policy target", "政策目標")
                                                font.weight: Font.DemiBold
                                                color: root.successContainerForegroundColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Label {
                                                text: root.selectedPolicy && root.selectedPolicy.registryValue
                                                      ? root.tr("Value", "值") + ": " + root.selectedPolicy.registryValue
                                                      : root.tr("Policy-defined values", "政策定義值")
                                                color: root.secondaryTextColor
                                                font.pixelSize: 10
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: root.registryTarget()
                                            color: root.successContainerForegroundColor
                                            font.family: DesignTokens.fontMono
                                            font.pixelSize: 11
                                            wrapMode: Text.WrapAnywhere
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: root.selectedPolicy && root.selectedPolicy.elements.length === 0
                                    text: root.tr("This policy has no additional values. Its state fully defines the change.",
                                                  "呢項政策冇額外值；狀態已完整定義變更。")
                                    wrapMode: Text.Wrap
                                    color: root.secondaryTextColor
                                    horizontalAlignment: Text.AlignHCenter
                                    padding: 12
                                }

                                Item { Layout.preferredHeight: 4 }
                            }
                        }
                    }

                    Rectangle {
                        visible: root.hasSelection
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: root.outlineColor
                    }

                    WfCard {
                        visible: root.hasSelection
                        Layout.fillWidth: true
                        dark: root.dark
                        outlined: false
                        surfaceLevel: "low"
                        radius: 0
                        padding: DesignTokens.spacing12
                        RowLayout {
                            anchors.fill: parent
                            spacing: 12
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    text: root.tr("Git-backed recipe change", "Git 支援嘅配方變更")
                                    font.weight: Font.DemiBold
                                    color: root.successColor
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: root.app.projectLoaded
                                          ? root.tr("Updates the build project; this PC's live policy stays untouched.", "只更新建置工程；呢部電腦嘅即時政策唔會改。")
                                          : root.tr("Open a project before committing this draft.", "先開啟工程，先可以 commit 呢份草稿。")
                                    color: root.secondaryTextColor
                                    font.pixelSize: 10
                                    wrapMode: Text.Wrap
                                }
                            }
                            WfButton {
                                dark: root.dark
                                variant: "filled"
                                text: root.draftState === 0
                                      ? root.tr("Commit Not configured", "Commit 未設定")
                                      : root.draftState === 2
                                        ? root.tr("Commit Disabled", "Commit 停用")
                                        : root.tr("Commit Enabled", "Commit 啟用")
                                enabled: root.app.projectLoaded
                                onClicked: root.commitDraft()
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: switchEditor
        Switch {
            property var elementModel: ({})
            text: checked ? root.tr("On", "開") : root.tr("Off", "關")
            Accessible.name: elementModel.label
            checked: elementModel.defaultChecked
            onToggled: root.elementValues[elementModel.id] = checked
        }
    }

    Component {
        id: comboEditor
        ComboBox {
            property var elementModel: ({})
            function syncDefaultSelection() {
                if (!model || model.length === 0 || !elementModel)
                    return
                for (var i = 0; i < model.length; ++i) {
                    if (model[i].value === elementModel.defaultValue) {
                        currentIndex = i
                        return
                    }
                }
                currentIndex = 0
            }
            width: parent ? parent.width : implicitWidth
            implicitHeight: DesignTokens.controlHeight
            model: elementModel.options
            textRole: "label"
            Accessible.name: elementModel.label
            Component.onCompleted: syncDefaultSelection()
            onElementModelChanged: syncDefaultSelection()
            onModelChanged: syncDefaultSelection()
            onActivated: root.elementValues[elementModel.id] = model[index].value
        }
    }

    Component {
        id: spinEditor
        SpinBox {
            property var elementModel: ({})
            from: Math.max(-2147483648, Number(elementModel.minimum))
            to: Math.min(2147483647, Number(elementModel.maximum))
            value: Number(elementModel.defaultValue || 0)
            stepSize: Math.max(1, Number(elementModel.spinStep))
            editable: true
            implicitHeight: DesignTokens.controlHeight
            Accessible.name: elementModel.label
            onValueModified: root.elementValues[elementModel.id] = value
        }
    }

    Component {
        id: numericTextEditor
        TextField {
            property var elementModel: ({})
            width: parent ? parent.width : implicitWidth
            text: elementModel.defaultValue
            implicitHeight: DesignTokens.controlHeight
            font.family: DesignTokens.fontMono
            maximumLength: 20
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            Accessible.name: elementModel.label
            placeholderText: root.tr("Integer from %1 to %2", "整數範圍 %1 至 %2")
                                 .arg(elementModel.minimum).arg(elementModel.maximum)
            validator: RegularExpressionValidator { regularExpression: /^-?[0-9]+$/ }
            onTextEdited: root.elementValues[elementModel.id] = text
        }
    }

    Component {
        id: textEditor
        TextField {
            property var elementModel: ({})
            width: parent ? parent.width : implicitWidth
            text: elementModel.defaultValue
            implicitHeight: DesignTokens.controlHeight
            maximumLength: elementModel.maximumLength >= 0 ? elementModel.maximumLength : 32767
            placeholderText: root.tr("Value", "值")
            Accessible.name: elementModel.label
            onTextEdited: root.elementValues[elementModel.id] = text
        }
    }

    Component {
        id: areaEditor
        TextArea {
            property var elementModel: ({})
            width: parent ? parent.width : implicitWidth
            implicitHeight: 96
            font.family: DesignTokens.fontBody
            text: elementModel.defaultValue
            placeholderText: elementModel.control === "ListEditor"
                             ? root.tr("One entry per line", "每行一項") : root.tr("Text", "文字")
            wrapMode: TextEdit.Wrap
            Accessible.name: elementModel.label
            onTextChanged: root.elementValues[elementModel.id] = text
        }
    }

    Popup {
        id: regexWizard
        anchors.centerIn: Overlay.overlay
        width: Math.min(620, root.width - 50)
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: DesignTokens.spacing20
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: root.surfaceColor
            border.color: root.outlineColor
        }
        ColumnLayout {
            anchors.fill: parent
            Label {
                text: root.tr("Regex builder", "Regex 建構器")
                color: root.surfaceForegroundColor
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            Label {
                Layout.fillWidth: true
                text: root.tr("Build a safe expression without memorizing punctuation. Invalid expressions stay in this panel and never interrupt other work.",
                              "唔使背符號都砌到安全 expression；錯咗只會留喺呢塊 panel，唔會阻住其他工序。")
                wrapMode: Text.Wrap
            }
            TextField {
                id: containsWords
                Layout.fillWidth: true
                placeholderText: root.tr("Contains words (space separated)", "包含字詞（空格分隔）")
            }
            TextField {
                id: startsWith
                Layout.fillWidth: true
                placeholderText: root.tr("Starts with (optional)", "開頭係（可選）")
            }
            TextField {
                id: endsWith
                Layout.fillWidth: true
                placeholderText: root.tr("Ends with (optional)", "結尾係（可選）")
            }
            CheckBox { id: wholeWord; text: root.tr("Whole words", "完整字詞") }
            Label {
                id: regexPreview
                Layout.fillWidth: true
                text: {
                    var parts = containsWords.text.trim().split(/\s+/).filter(function(x) { return x.length > 0 })
                    var body = parts.length ? parts.map(function(x) { return "(?=.*" + x.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + ")" }).join("") + ".*" : ".*"
                    if (wholeWord.checked && parts.length === 1)
                        body = "\\b" + parts[0].replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "\\b"
                    return "^" + startsWith.text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + body
                           + endsWith.text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "$"
                }
                font.family: DesignTokens.fontMono
                color: root.primaryColor
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                WfButton {
                    dark: root.dark
                    variant: "text"
                    text: root.tr("Cancel", "取消")
                    onClicked: regexWizard.close()
                }
                WfButton {
                    dark: root.dark
                    variant: "filled"
                    text: root.tr("Use & search", "使用同搜尋")
                    onClicked: {
                        policySearch.text = regexPreview.text
                        regexMode.checked = true
                        regexWizard.close()
                        root.app.searchGpo(policySearch.text, true)
                    }
                }
            }
        }
    }

    Popup {
        id: docsExport
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, root.width - 50)
        modal: false
        focus: true
        padding: DesignTokens.spacing20
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: root.surfaceColor
            border.color: root.outlineColor
        }
        ColumnLayout {
            anchors.fill: parent
            Label {
                text: root.tr("Export complete bilingual GPO reference", "匯出完整雙語 GPO 參考")
                color: root.surfaceForegroundColor
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            TextField {
                id: docsPath
                Layout.fillWidth: true
                placeholderText: "D:\\docs\\all-installed-policies.md"
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                WfButton {
                    dark: root.dark
                    variant: "text"
                    text: root.tr("Cancel", "取消")
                    onClicked: docsExport.close()
                }
                WfButton {
                    dark: root.dark
                    variant: "filled"
                    text: root.tr("Export", "匯出")
                    enabled: docsPath.text.trim().length > 0
                    onClicked: {
                        if (root.app.exportGpoDocumentation(docsPath.text))
                            docsExport.close()
                    }
                }
            }
        }
    }
}
