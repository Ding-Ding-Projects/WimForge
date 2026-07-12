pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window
import "components"
import "pages"

ApplicationWindow {
    id: root
    width: 1440
    height: 900
    minimumWidth: 900
    minimumHeight: 640
    visible: true
    onClosing: Qt.quit()
    title: app.projectLoaded ? "WimForge — " + app.projectName : "WimForge"
    color: DesignTokens.surfaceDim(darkTheme)
    font.family: DesignTokens.fontBody
    font.pixelSize: 13

    Material.theme: app.themeMode === 1 ? Material.Light
                  : app.themeMode === 2 ? Material.Dark
                  : (Application.styleHints.colorScheme === Qt.ColorScheme.Dark ? Material.Dark : Material.Light)
    Material.accent: DesignTokens.primary(darkTheme)
    Material.primary: DesignTokens.primary(darkTheme)

    property int currentPage: startupPage
    property bool notificationsOpen: false
    readonly property var controller: app
    readonly property real uiScale: Math.max(0.8, Math.min(1.25, app.interfaceScale))
    readonly property bool compactNavigation: width / uiScale < 1280
    readonly property bool darkTheme: Material.theme === Material.Dark
    readonly property color secondaryTextColor: DesignTokens.onSurfaceVariant(darkTheme)
    readonly property color errorColor: DesignTokens.error(darkTheme)
    readonly property color errorBadgeColor: DesignTokens.error(darkTheme)
    readonly property color successColor: DesignTokens.success(darkTheme)
    readonly property color warningColor: DesignTokens.tertiary(darkTheme)
    readonly property color warningContainerColor: DesignTokens.tertiaryContainer(darkTheme)
    property var navigationItems: [
        { icon: "qrc:/qt/qml/WimForge/assets/icons/overview.svg", en: "Overview", zh: "總覽", context: "project" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/source.svg", en: "Source & editions", zh: "來源同版本", context: "source" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/customize.svg", en: "Customize", zh: "調校", context: "config" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/policy.svg", en: "Group Policy Studio", zh: "群組原則工房", context: "gpo" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/unattended.svg", en: "Unattended Studio", zh: "無人值守工房", context: "unattended" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/package.svg", en: "Package Studio", zh: "套件工房", context: "packages" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/bridge.svg", en: "WinForge Bridge", zh: "WinForge 橋接", context: "winforge" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/vm.svg", en: "Virtual Machine Lab", zh: "虛擬機實驗室", context: "vm-lab" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/run.svg", en: "Review & run", zh: "檢查同開工", context: "plan" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/history.svg", en: "History & recovery", zh: "歷史同復原", context: "history" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/settings.svg", en: "Settings", zh: "設定", context: "settings" },
        { icon: "qrc:/qt/qml/WimForge/assets/icons/terminal.svg", en: "Embedded terminal", zh: "內嵌終端機", context: "terminal" }
    ]

    Binding {
        target: DesignTokens
        property: "reducedMotion"
        value: !app.motionEnabled
    }

    Binding {
        target: DesignTokens
        property: "scheme"
        value: app.colorScheme
    }

    function navigateToPage(page) {
        var bounded = Math.max(0, Math.min(page, navigationItems.length - 1))
        currentPage = bounded
        if (app.projectLoaded
                && !app.navigateActiveWorkspaceTab(bounded, tr2(navigationItems[bounded].en, navigationItems[bounded].zh)))
            syncActiveWorkspaceTab()
    }

    function openPageInNewTab(page) {
        var bounded = Math.max(0, Math.min(page, navigationItems.length - 1))
        currentPage = bounded
        if (app.projectLoaded
                && !app.openWorkspaceTabForPage(bounded, tr2(navigationItems[bounded].en, navigationItems[bounded].zh)))
            syncActiveWorkspaceTab()
    }

    function tabTitleMatches(title, pattern, useRegex, caseSensitive) {
        if (!pattern || pattern.length === 0)
            return false
        if (useRegex) {
            try {
                return new RegExp(pattern, caseSensitive ? "" : "i").test(title)
            } catch (error) {
                return false
            }
        }
        var haystack = caseSensitive ? title : title.toLowerCase()
        var needle = caseSensitive ? pattern : pattern.toLowerCase()
        return haystack.indexOf(needle) >= 0
    }

    function syncActiveWorkspaceTab() {
        if (!app.projectLoaded || app.activeWorkspaceTab < 0
                || app.activeWorkspaceTab >= app.workspaceTabs.length)
            return
        currentPage = app.workspaceTabs[app.activeWorkspaceTab].page
    }

    Component.onCompleted: {
        if (startupPageRequested)
            navigateToPage(startupPage)
        else
            syncActiveWorkspaceTab()
    }

    function openContextHistory(x, y) {
        var item = navigationItems[Math.max(0, Math.min(currentPage, navigationItems.length - 1))]
        var recent = app.contextualHistory(item.context, "")
        if (!recent || recent.length === 0)
            recent = app.contextualHistory("", "")
        contextHistory.contextKey = item.context
        contextHistory.elementId = ""
        contextHistory.openAt(x, y, tr2(item.en, item.zh), recent)
    }

    function tr2(en, zh) {
        var dependency = app.languageMode
        if (dependency === 1) return zh
        if (dependency === 2) return en + "  ·  " + zh
        return en
    }

    function isTextEditor(item) {
        var candidate = item
        while (candidate) {
            if (candidate instanceof TextInput || candidate instanceof TextEdit)
                return true
            candidate = candidate.parent
        }
        return false
    }
    Connections {
        target: app
        function onSnackbarRequested(message, tone) { snackbar.show(message, tone, "") }
        function onNewProjectRequested() { newProjectSheet.open() }
        function onOpenProjectRequested() { openProjectSheet.open() }
        function onRunConfirmationRequested(summary, destructiveCount) {
            runSummary.text = summary
            sourceBackupAck.checked = false
            adminAck.checked = false
            destructiveBadge.visible = destructiveCount > 0
            destructiveBadge.text = root.tr2("⚠ %1 destructive", "⚠ %1 個破壞性步驟").arg(destructiveCount)
                                         + (app.languageMode === 1 ? " 項有破壞性" : "")
            runSheet.open()
        }
        function onExportProjectRequested() { exportProjectSheet.open() }
        function onExportScriptRequested() { exportScriptSheet.open() }
        function onUnattendedStudioRequested() { root.navigateToPage(4) }
        function onRecoveryReviewRequested() { root.navigateToPage(9) }
        function onSearchRequested(query) { searchPalette.openForQuery(query) }
        function onSearchNavigationRequested(page, focusId, query) {
            root.navigateToPage(page)
            globalSearch.clear()
            searchPalette.close()
        }
        function onWorkspaceTabsChanged() {
            root.syncActiveWorkspaceTab()
            workspaceTabsScroll.revealActiveTab()
        }
    }

    Shortcut { sequence: "Ctrl+N"; onActivated: newProjectSheet.open() }
    Shortcut { sequence: "Ctrl+O"; onActivated: openProjectSheet.open() }
    Shortcut {
        sequence: "Ctrl+K"
        context: Qt.ApplicationShortcut
        onActivated: { globalSearch.forceActiveFocus(); globalSearch.selectAll() }
    }
    Shortcut {
        sequence: "Ctrl+Z"
        context: Qt.ApplicationShortcut
        enabled: !root.isTextEditor(root.activeFocusItem)
        onActivated: app.undoContext(root.navigationItems[root.currentPage].context, "")
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"
        context: Qt.ApplicationShortcut
        enabled: !root.isTextEditor(root.activeFocusItem)
        onActivated: root.openContextHistory(root.width / 2 - 210, 90)
    }
    Shortcut { sequence: "Ctrl+Enter"; onActivated: app.requestRunPlan() }
    Shortcut {
        sequence: "Ctrl+W"
        context: Qt.ApplicationShortcut
        enabled: app.projectLoaded && !root.isTextEditor(root.activeFocusItem)
        onActivated: app.closeWorkspaceTab(app.activeWorkspaceTab)
    }
    Shortcut {
        sequence: "Ctrl+Tab"
        context: Qt.ApplicationShortcut
        enabled: app.projectLoaded && app.workspaceTabs.length > 1
        onActivated: app.activateWorkspaceTab((app.activeWorkspaceTab + 1) % app.workspaceTabs.length)
    }
    Shortcut {
        sequence: "Ctrl+Shift+Tab"
        context: Qt.ApplicationShortcut
        enabled: app.projectLoaded && app.workspaceTabs.length > 1
        onActivated: app.activateWorkspaceTab((app.activeWorkspaceTab - 1 + app.workspaceTabs.length) % app.workspaceTabs.length)
    }

    MouseArea {
        parent: root.contentItem
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        hoverEnabled: false
        onClicked: function(mouse) {
            root.openContextHistory(mouse.x, mouse.y)
            mouse.accepted = true
        }
    }

    RowLayout {
        id: applicationShell
        visible: app.projectLoaded
        width: root.width / root.uiScale
        height: root.height / root.uiScale
        transformOrigin: Item.TopLeft
        scale: root.uiScale
        spacing: 0

        Pane {
            id: navigation
            Layout.fillHeight: true
            Layout.preferredWidth: root.compactNavigation ? DesignTokens.navCompactWidth : DesignTokens.navWidth
            Layout.minimumWidth: Layout.preferredWidth
            padding: root.compactNavigation ? 8 : 10
            background: Rectangle {
                color: DesignTokens.navSurface(root.darkTheme)
                border.color: DesignTokens.outlineVariant(root.darkTheme)
                border.width: 0
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: parent.border.color }
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: DesignTokens.topBarHeight

                    Rectangle {
                        id: appLogo
                        anchors.left: root.compactNavigation ? undefined : parent.left
                        anchors.leftMargin: root.compactNavigation ? 0 : 6
                        anchors.horizontalCenter: root.compactNavigation ? parent.horizontalCenter : undefined
                        anchors.verticalCenter: parent.verticalCenter
                        width: 36
                        height: 36
                        radius: 12
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: DesignTokens.primaryContainer(root.darkTheme) }
                            GradientStop { position: 1.0; color: DesignTokens.secondaryContainer(root.darkTheme) }
                        }
                        layer.enabled: true
                        Accessible.name: root.tr2("WimForge application", "WimForge 應用程式")
                        Label {
                            anchors.centerIn: parent
                            text: "W"
                            font.family: DesignTokens.fontDisplay
                            font.pixelSize: 18
                            font.weight: Font.Bold
                            color: DesignTokens.onPrimaryContainer(root.darkTheme)
                            Accessible.ignored: true
                        }
                    }
                    Column {
                        visible: !root.compactNavigation
                        anchors.left: appLogo.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 0
                        Label { width: parent.width; text: "WimForge"; color: DesignTokens.navOn(root.darkTheme); font.family: DesignTokens.fontDisplay; font.pixelSize: 14; font.weight: Font.DemiBold; elide: Text.ElideRight }
                        Label { width: parent.width; text: root.tr2("IMAGE OPERATIONS", "映像作業台"); font.family: DesignTokens.fontBody; font.pixelSize: 10; font.letterSpacing: 0.8; color: DesignTokens.onSurfaceVariant(root.darkTheme); elide: Text.ElideRight }
                    }
                }

                WfButton {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.leftMargin: root.compactNavigation ? 0 : 2
                    Layout.rightMargin: root.compactNavigation ? 0 : 2
                    text: root.compactNavigation ? "" : root.tr2("New project", "開新工程")
                    glyph: "+"
                    variant: "filled"
                    Accessible.name: root.tr2("New project", "開新工程")
                    onClicked: newProjectSheet.open()
                    ToolTip.visible: root.compactNavigation && hovered
                    ToolTip.text: Accessible.name
                }

                ScrollView {
                    id: navigationScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                    Column {
                        width: navigationScroll.availableWidth
                        spacing: 4

                        Repeater {
                            model: root.navigationItems
                            delegate: AbstractButton {
                                id: navigationDelegate
                                required property var modelData
                                required property int index
                                width: parent.width
                                implicitHeight: root.compactNavigation
                                                ? DesignTokens.rowHeight
                                                : Math.max(DesignTokens.rowHeight, navigationLabels.implicitHeight + 14)
                                leftPadding: root.compactNavigation ? Math.max(0, (width - 20) / 2) : 12
                                rightPadding: root.compactNavigation ? Math.max(0, (width - 20) / 2) : 12
                                readonly property bool selected: root.currentPage === index
                                focusPolicy: Qt.StrongFocus
                                Accessible.name: (selected ? root.tr2("Current page: ", "而家呢頁：") : "")
                                                 + root.tr2(modelData.en, modelData.zh)
                                onClicked: root.navigateToPage(index)
                                ToolTip.visible: root.compactNavigation && hovered
                                ToolTip.text: root.tr2(modelData.en, modelData.zh)
                                background: Rectangle {
                                    radius: DesignTokens.radiusPill
                                    color: navigationDelegate.selected ? DesignTokens.secondaryContainer(root.darkTheme)
                                           : navigationDelegate.hovered ? DesignTokens.navHover(root.darkTheme) : "transparent"
                                    border.width: navigationDelegate.visualFocus ? 2 : 0
                                    border.color: navigationDelegate.visualFocus ? DesignTokens.primary(root.darkTheme) : "transparent"
                                }
                                contentItem: RowLayout {
                                    spacing: 12
                                    Item {
                                        Layout.preferredWidth: 20
                                        Layout.preferredHeight: 20
                                        Layout.alignment: Qt.AlignVCenter
                                        WfIcon {
                                            anchors.centerIn: parent
                                            iconSize: 18
                                            source: navigationDelegate.modelData.icon
                                            color: navigationDelegate.selected
                                                   ? DesignTokens.onSecondaryContainer(root.darkTheme)
                                                   : DesignTokens.navOn(root.darkTheme)
                                            opacity: navigationDelegate.selected ? 1 : 0.85
                                            Accessible.ignored: true
                                        }
                                    }
                                    Column {
                                        id: navigationLabels
                                        visible: !root.compactNavigation
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                        spacing: 0
                                        Label {
                                            width: parent.width
                                            visible: app.languageMode !== 1
                                            text: navigationDelegate.modelData.en
                                            color: navigationDelegate.selected
                                                   ? DesignTokens.onSecondaryContainer(root.darkTheme)
                                                   : DesignTokens.navOn(root.darkTheme)
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 12
                                            font.weight: navigationDelegate.selected ? Font.DemiBold : Font.Medium
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            width: parent.width
                                            visible: app.languageMode !== 0
                                            text: navigationDelegate.modelData.zh
                                            color: navigationDelegate.selected
                                                   ? DesignTokens.onSecondaryContainer(root.darkTheme)
                                                   : (app.languageMode === 1
                                                      ? DesignTokens.navOn(root.darkTheme)
                                                      : DesignTokens.onSurfaceVariant(root.darkTheme))
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: app.languageMode === 1 ? 12 : 10
                                            font.weight: navigationDelegate.selected && app.languageMode === 1
                                                         ? Font.DemiBold : Font.Medium
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Pane {
                    Layout.fillWidth: true
                    Layout.preferredHeight: projectSummary.implicitHeight + topPadding + bottomPadding
                    visible: app.projectLoaded && !root.compactNavigation
                    padding: 10
                    Accessible.name: root.tr2("Project status: ", "工程狀態：") + app.projectName + ". " + app.gitStatusText
                    background: Rectangle { radius: DesignTokens.radiusCard; color: DesignTokens.surfaceContainer(root.darkTheme); border.color: DesignTokens.outlineVariant(root.darkTheme) }
                    ColumnLayout {
                        id: projectSummary
                        width: parent.width
                        Label { text: app.projectName; color: DesignTokens.navOn(root.darkTheme); font.family: DesignTokens.fontBody; font.pixelSize: 12; font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label { text: app.gitStatusText; font.family: DesignTokens.fontBody; font.pixelSize: 11; color: DesignTokens.success(root.darkTheme); Layout.fillWidth: true; elide: Text.ElideRight }
                        ProgressBar { visible: app.busy; Layout.fillWidth: true; value: app.progress; indeterminate: app.progress <= 0; Accessible.name: root.tr2("Project job progress", "工程工作進度") }
                    }
                    HoverHandler { id: projectSummaryHover }
                    ToolTip.visible: root.compactNavigation && projectSummaryHover.hovered
                    ToolTip.text: app.projectName + " · " + app.gitStatusText
                }

                Label {
                    Layout.fillWidth: true
                    text: root.compactNavigation ? "v" + app.version : "v" + app.version + "  ·  MIT"
                    horizontalAlignment: Text.AlignHCenter
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 10
                    color: DesignTokens.onSurfaceVariant(root.darkTheme)
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Pane {
                    Layout.fillWidth: true
                    Layout.preferredHeight: DesignTokens.topBarHeight
                    leftPadding: 20; rightPadding: 20; topPadding: 9; bottomPadding: 9
                    background: Rectangle {
                        color: DesignTokens.surfaceLowest(root.darkTheme)
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: DesignTokens.outlineVariant(root.darkTheme) }
                    }
                    RowLayout {
                        anchors.fill: parent
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: Math.min(420, parent.width * 0.44)
                            Layout.preferredHeight: 36
                            radius: DesignTokens.radiusPill
                            color: DesignTokens.surfaceContainer(root.darkTheme)
                            border.width: 1
                            border.color: globalSearch.activeFocus
                                          ? DesignTokens.primary(root.darkTheme)
                                          : DesignTokens.outlineVariant(root.darkTheme)
                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                text: "⌕"
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 17
                                color: DesignTokens.onSurfaceVariant(root.darkTheme)
                                Accessible.ignored: true
                            }
                            TextField {
                                id: globalSearch
                                anchors.fill: parent
                                leftPadding: 36
                                rightPadding: 58
                                topPadding: 0
                                bottomPadding: 0
                                placeholderText: root.tr2("Search features, commands and settings", "搜尋功能、指令同設定")
                                placeholderTextColor: DesignTokens.onSurfaceVariant(root.darkTheme)
                                color: DesignTokens.onSurface(root.darkTheme)
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 13
                                Accessible.name: placeholderText
                                background: Item { }
                                onAccepted: app.search(text)
                            }
                            Rectangle {
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: shortcutHint.implicitWidth + 14
                                height: 20
                                radius: DesignTokens.radiusControl - 2
                                visible: !globalSearch.activeFocus && globalSearch.text.length === 0
                                color: DesignTokens.surfaceHigh(root.darkTheme)
                                border.width: 1
                                border.color: DesignTokens.outlineVariant(root.darkTheme)
                                Label {
                                    id: shortcutHint
                                    anchors.centerIn: parent
                                    text: "Ctrl K"
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 0.4
                                    color: DesignTokens.onSurfaceVariant(root.darkTheme)
                                    Accessible.ignored: true
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                        WfIconButton {
                            glyph: ""
                            accessibleName: app.busy ? root.tr2("Jobs are running; open job queue", "有工序行緊；開啟工序隊列") : root.tr2("Open job queue", "開啟工序隊列")
                            toolTip: accessibleName
                            onClicked: root.navigateToPage(8)
                            contentItem: Label {
                                text: app.busy ? "\uE895" : "\uE823"
                                font.family: "Segoe MDL2 Assets"
                                font.pixelSize: 16
                                color: app.busy ? DesignTokens.secondary(root.darkTheme)
                                                : DesignTokens.onSurfaceVariant(root.darkTheme)
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        WfIconButton {
                            id: bell
                            glyph: ""
                            accessibleName: app.notificationUnreadCount > 0
                                            ? root.tr2("Notification center, %1 unread", "通知中心，%1 個未讀").arg(app.notificationUnreadCount)
                                            : root.tr2("Notification center, no unread notifications", "通知中心，冇未讀通知")
                            toolTip: accessibleName
                            onClicked: root.notificationsOpen = !root.notificationsOpen
                            contentItem: Label {
                                text: "\uE7ED"
                                font.family: "Segoe MDL2 Assets"
                                font.pixelSize: 16
                                color: DesignTokens.onSurfaceVariant(root.darkTheme)
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            Rectangle {
                                visible: app.notificationUnreadCount > 0
                                anchors.right: parent.right; anchors.top: parent.top
                                anchors.rightMargin: 0; anchors.topMargin: 0
                                width: Math.max(15, unread.implicitWidth + 6); height: 15; radius: 8
                                color: root.errorBadgeColor
                                Label { id: unread; anchors.centerIn: parent; text: app.notificationUnreadCount; color: DesignTokens.onError(root.darkTheme); font.family: DesignTokens.fontBody; font.pixelSize: 9; font.bold: true; Accessible.ignored: true }
                            }
                        }
                        WfIconButton {
                            glyph: ""
                            accessibleName: root.darkTheme
                                            ? root.tr2("Use light theme", "使用淺色主題")
                                            : root.tr2("Use dark theme", "使用深色主題")
                            toolTip: accessibleName
                            onClicked: app.themeMode = root.darkTheme ? 1 : 2
                            contentItem: Label {
                                text: root.darkTheme ? "\uE706" : "\uE708"
                                font.family: "Segoe MDL2 Assets"
                                font.pixelSize: 16
                                color: DesignTokens.onSurfaceVariant(root.darkTheme)
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Row {
                            id: languageSegmented
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0
                            Accessible.role: Accessible.Grouping
                            Accessible.name: root.tr2("Display language", "顯示語言")
                            Repeater {
                                model: [
                                    { mode: 0, label: "EN", en: "English", zh: "英文" },
                                    { mode: 1, label: "粵", en: "Cantonese", zh: "粵語" },
                                    { mode: 2, label: "雙", en: "Bilingual", zh: "雙語" }
                                ]
                                delegate: AbstractButton {
                                    id: languageSegment
                                    required property var modelData
                                    required property int index
                                    readonly property bool active: app.languageMode === modelData.mode
                                    implicitHeight: 32
                                    implicitWidth: 40
                                    focusPolicy: Qt.StrongFocus
                                    Accessible.name: root.tr2(modelData.en, modelData.zh)
                                    Accessible.checkable: true
                                    Accessible.checked: active
                                    ToolTip.visible: hovered
                                    ToolTip.text: root.tr2(modelData.en, modelData.zh)
                                    onClicked: app.languageMode = modelData.mode
                                    background: Rectangle {
                                        color: languageSegment.active ? DesignTokens.secondaryContainer(root.darkTheme)
                                               : languageSegment.hovered ? DesignTokens.surfaceHigh(root.darkTheme)
                                               : DesignTokens.surfaceContainer(root.darkTheme)
                                        border.width: languageSegment.visualFocus ? 2 : 1
                                        border.color: languageSegment.visualFocus
                                                      ? DesignTokens.primary(root.darkTheme)
                                                      : DesignTokens.outlineVariant(root.darkTheme)
                                        topLeftRadius: languageSegment.index === 0 ? DesignTokens.radiusPill : 0
                                        bottomLeftRadius: languageSegment.index === 0 ? DesignTokens.radiusPill : 0
                                        topRightRadius: languageSegment.index === 2 ? DesignTokens.radiusPill : 0
                                        bottomRightRadius: languageSegment.index === 2 ? DesignTokens.radiusPill : 0
                                    }
                                    contentItem: Label {
                                        text: languageSegment.modelData.label
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 12
                                        font.weight: languageSegment.active ? Font.DemiBold : Font.Medium
                                        color: languageSegment.active
                                               ? DesignTokens.onSecondaryContainer(root.darkTheme)
                                               : DesignTokens.onSurfaceVariant(root.darkTheme)
                                    }
                                }
                            }
                        }
                        AbstractButton {
                            id: projectChip
                            Layout.preferredHeight: 36
                            Layout.maximumWidth: 220
                            implicitWidth: Math.min(220, Math.max(118, projectChipLabel.implicitWidth + 38))
                            focusPolicy: Qt.StrongFocus
                            Accessible.name: app.projectLoaded
                                             ? root.tr2("Active project: ", "而家工程：") + app.projectName
                                             : root.tr2("Open project", "開工程")
                            onClicked: openProjectSheet.open()
                            background: Rectangle {
                                radius: DesignTokens.radiusPill
                                color: DesignTokens.surfaceLowest(root.darkTheme)
                                border.width: projectChip.visualFocus ? 2 : 1
                                border.color: projectChip.visualFocus
                                              ? DesignTokens.primary(root.darkTheme)
                                              : DesignTokens.outlineVariant(root.darkTheme)
                            }
                            contentItem: RowLayout {
                                spacing: 8
                                Rectangle {
                                    Layout.preferredWidth: 8
                                    Layout.preferredHeight: 8
                                    radius: 4
                                    color: app.projectLoaded
                                           ? DesignTokens.success(root.darkTheme)
                                           : DesignTokens.outline(root.darkTheme)
                                }
                                Label {
                                    id: projectChipLabel
                                    Layout.fillWidth: true
                                    text: app.projectLoaded ? app.projectName : root.tr2("Open project", "開工程")
                                    color: DesignTokens.onSurface(root.darkTheme)
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }

                Pane {
                    Layout.fillWidth: true
                    Layout.preferredHeight: recoveryRow.implicitHeight + topPadding + bottomPadding
                    visible: app.pendingRecovery
                    padding: 10
                    background: Rectangle { color: root.warningContainerColor }
                    RowLayout {
                        id: recoveryRow
                        width: parent.width
                        Rectangle {
                            width: 24
                            height: 24
                            radius: 12
                            color: DesignTokens.tertiary(root.darkTheme)
                            Label { anchors.centerIn: parent; text: "!"; font.family: DesignTokens.fontBody; font.weight: Font.Bold; color: DesignTokens.onTertiary(root.darkTheme) }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.tr2("Interrupted work found. The image is protected; choose how to recover when ready.",
                                           "搵到上次中斷嘅工序。映像仲安全；得閒先揀點樣復原，唔使即刻畀個彈窗追住。")
                            wrapMode: Text.Wrap
                        }
                        WfButton { text: root.tr2("Review recovery", "檢查復原"); compact: true; onClicked: recoverySheet.open() }
                    }
                }

                Pane {
                    id: workspaceTabStrip
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    visible: app.projectLoaded
                    leftPadding: 12; rightPadding: 10; topPadding: 4; bottomPadding: 4
                    background: Rectangle {
                        color: DesignTokens.surfaceLow(root.darkTheme)
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: DesignTokens.outlineVariant(root.darkTheme) }
                    }
                    RowLayout {
                        anchors.fill: parent
                        spacing: 6
                        ScrollView {
                            id: workspaceTabsScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            contentHeight: availableHeight
                            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
                            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                            function revealActiveTab() {
                                Qt.callLater(function() {
                                    var item = workspaceTabRepeater.itemAt(app.activeWorkspaceTab)
                                    var flick = workspaceTabsScroll.contentItem
                                    if (!item || !flick)
                                        return
                                    var left = item.x
                                    var right = item.x + item.width
                                    if (left < flick.contentX)
                                        flick.contentX = Math.max(0, left - 5)
                                    else if (right > flick.contentX + workspaceTabsScroll.availableWidth)
                                        flick.contentX = Math.max(0, right - workspaceTabsScroll.availableWidth + 5)
                                })
                            }
                            Row {
                                height: workspaceTabsScroll.availableHeight
                                spacing: 5
                                Repeater {
                                    id: workspaceTabRepeater
                                    model: app.workspaceTabs
                                    delegate: Rectangle {
                                        id: workspaceTab
                                        required property var modelData
                                        required property int index
                                        height: 36
                                        width: Math.max(140, Math.min(280, tabTitle.implicitWidth + 76))
                                        radius: DesignTokens.radiusControl
                                        color: index === app.activeWorkspaceTab
                                               ? DesignTokens.surfaceLowest(root.darkTheme)
                                               : tabHover.hovered ? DesignTokens.surfaceContainer(root.darkTheme) : "transparent"
                                        border.width: index === app.activeWorkspaceTab ? 1 : 0
                                        border.color: index === app.activeWorkspaceTab
                                                      ? DesignTokens.outlineVariant(root.darkTheme) : "transparent"
                                        Accessible.name: root.tr2("Workspace tab: ", "工作分頁：") + modelData.title
                                        Rectangle {
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.bottom: parent.bottom
                                            anchors.leftMargin: 8
                                            anchors.rightMargin: 8
                                            height: 2
                                            color: workspaceTab.index === app.activeWorkspaceTab
                                                   ? DesignTokens.primary(root.darkTheme) : "transparent"
                                        }
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 4
                                            spacing: 3
                                            Label {
                                                id: tabTitle
                                                Layout.fillWidth: true
                                                text: workspaceTab.modelData.title
                                                elide: Text.ElideRight
                                                color: workspaceTab.modelData.fontColor && workspaceTab.modelData.fontColor.length > 0
                                                       ? workspaceTab.modelData.fontColor : root.secondaryTextColor
                                                font.family: workspaceTab.modelData.fontFamily || root.font.family
                                                font.pixelSize: workspaceTab.modelData.fontSize || 12
                                                font.bold: !!workspaceTab.modelData.bold
                                                font.italic: !!workspaceTab.modelData.italic
                                                font.strikeout: !!workspaceTab.modelData.strikeout
                                            }
                                            WfIconButton {
                                                glyph: "✎"
                                                buttonSize: 28
                                                accessibleName: root.tr2("Rename or style tab", "改名或設定分頁樣式")
                                                toolTip: accessibleName
                                                onClicked: tabEditor.openFor(workspaceTab.index, workspaceTab.modelData)
                                            }
                                            WfIconButton {
                                                glyph: "×"
                                                buttonSize: 28
                                                accessibleName: root.tr2("Close tab", "關閉分頁")
                                                toolTip: accessibleName
                                                onClicked: app.closeWorkspaceTab(workspaceTab.index)
                                            }
                                        }
                                        HoverHandler { id: tabHover }
                                        TapHandler { onTapped: app.activateWorkspaceTab(workspaceTab.index) }
                                        TapHandler {
                                            acceptedButtons: Qt.RightButton
                                            onTapped: {
                                                app.activateWorkspaceTab(workspaceTab.index)
                                                tabContextMenu.targetIndex = workspaceTab.index
                                                tabContextMenu.popup()
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        WfIconButton {
                            glyph: "+"
                            buttonSize: 32
                            accessibleName: root.tr2("Open page in a tab", "在分頁開啟頁面")
                            toolTip: accessibleName
                            onClicked: newTabMenu.open()
                            Menu {
                                id: newTabMenu
                                Instantiator {
                                    model: root.navigationItems
                                    delegate: MenuItem {
                                        required property var modelData
                                        required property int index
                                        text: root.tr2(modelData.en, modelData.zh)
                                        onTriggered: root.openPageInNewTab(index)
                                    }
                                    onObjectAdded: (index, object) => newTabMenu.insertItem(index, object)
                                    onObjectRemoved: (index, object) => newTabMenu.removeItem(object)
                                }
                            }
                        }
                        WfIconButton {
                            glyph: "⋮"
                            buttonSize: 32
                            accessibleName: root.tr2("Tab import and export", "分頁匯入與匯出")
                            toolTip: accessibleName
                            onClicked: tabTransferMenu.open()
                            Menu {
                                id: tabTransferMenu
                                MenuItem { text: root.tr2("Export portable tabs…", "匯出可攜分頁…"); onTriggered: exportTabsDialog.open() }
                                MenuItem { text: root.tr2("Import portable tabs…", "匯入可攜分頁…"); onTriggered: importTabsDialog.open() }
                                MenuSeparator { }
                                MenuItem { text: root.tr2("Export complete tab Git repo…", "匯出完整分頁 Git repo…"); onTriggered: exportTabRepoDialog.open() }
                                MenuItem { text: root.tr2("Import complete tab Git repo…", "匯入完整分頁 Git repo…"); onTriggered: importTabRepoDialog.open() }
                            }
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.leftMargin: 28
                    Layout.rightMargin: 28
                    Layout.topMargin: 24
                    Layout.bottomMargin: 24
                    currentIndex: root.currentPage
                    // StackLayout is not a Control, so make the Material theme
                    // boundary explicit for every plain-Item page and its
                    // default-styled labels/controls.
                    Material.theme: root.darkTheme ? Material.Dark : Material.Light

                    DashboardPage { app: root.controller; tr: root.tr2; dark: root.darkTheme; openPage: index => root.navigateToPage(index) }
                    SourcePage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    CustomizePage {
                        app: root.controller
                        tr: root.tr2
                        dark: root.darkTheme
                        currentSection: startupCustomizeSection
                    }
                    GpoStudioPage {
                        app: root.controller
                        tr: root.tr2
                        dark: root.darkTheme
                        active: root.currentPage === 3
                    }
                    UnattendedStudioPage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    PackageStudioPage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    WinForgeBridgePage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    VmLabPage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    PlanPage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    HistoryPage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    SettingsPage { app: root.controller; tr: root.tr2; dark: root.darkTheme }
                    TerminalPage {
                        app: root.controller
                        terminal: terminalSession
                        tr: root.tr2
                        dark: root.darkTheme
                    }
                }
            }

            NotificationCenter {
                id: notificationCenter
                anchors.top: parent.top
                opened: root.notificationsOpen
                entries: app.notifications
                unreadCount: app.notificationUnreadCount
                motionEnabled: app.motionEnabled
                onCloseRequested: root.notificationsOpen = false
                onMarkReadRequested: id => app.markNotificationRead(id)
                onMarkUnreadRequested: id => app.markNotificationUnread(id)
                onDismissRequested: id => app.dismissNotification(id)
                onDeleteRequested: id => app.deleteNotification(id)
                onRestoreRequested: id => app.restoreNotification(id)
                onUndoRequested: app.undoLatestNotificationChange()
            }

            Snackbar {
                id: snackbar
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 22
                motionEnabled: app.motionEnabled
            }

            SearchPalette {
                id: searchPalette
                app: root.controller
                tr: root.tr2
            }

            ContextHistoryPanel {
                id: contextHistory
                parent: Overlay.overlay
                branchName: "main"
                motionEnabled: app.motionEnabled
                onUndoRequested: function(eventId) {
                    if (app.undoHistoryEvent(eventId)) events = app.contextualHistory(contextKey, elementId)
                }
                onRedoRequested: function(eventId) {
                    if (app.redoHistoryEvent(eventId)) events = app.contextualHistory(contextKey, elementId)
                }
                onRestoreRequested: function(eventId) {
                    if (app.restoreHistoryEvent(eventId)) events = app.contextualHistory(contextKey, elementId)
                }
                onBookmarkRequested: function(eventId, name) {
                    if (app.bookmarkHistoryEvent(eventId, name)) events = app.contextualHistory(contextKey, elementId)
                }
                onBranchRequested: function(eventId, name) {
                    if (app.branchHistoryEvent(eventId, name)) events = app.contextualHistory(contextKey, elementId)
                }
            }
        }
    }

    ProjectStartPage {
        id: projectStart
        visible: !app.projectLoaded
        z: 1000
        width: root.width / root.uiScale
        height: root.height / root.uiScale
        transformOrigin: Item.TopLeft
        scale: root.uiScale
        leftPadding: 28
        rightPadding: 28
        topPadding: 28
        bottomPadding: 28
        dark: root.darkTheme
        tr: root.tr2
        recentProjects: projectStartCapture ? [] : app.recentProjects
        onCreateRequested: newProjectSheet.open()
        onOpenRequested: openProjectSheet.open()
        onImportRequested: {
            openProjectSheet.open()
            importProjectFileDialog.open()
        }
        onRecentRequested: function(path) {
            if (app.openProject(path))
                root.syncActiveWorkspaceTab()
        }
        onRemoveRecentRequested: function(path) { app.removeRecentProject(path) }
        onClearRecentRequested: app.clearRecentProjects()
    }

    Popup {
        id: tabEditor
        property int editIndex: -1
        function openFor(index, data) {
            editIndex = index
            tabName.text = data.title || ""
            var family = data.fontFamily || ""
            var familyIndex = fontFamily.model.indexOf(family)
            fontFamily.currentIndex = familyIndex
            if (familyIndex < 0)
                fontFamily.editText = family
            fontSize.value = data.fontSize || 13
            fontColor.text = data.fontColor || ""
            fontBold.checked = !!data.bold
            fontItalic.checked = !!data.italic
            fontStrikeout.checked = !!data.strikeout
            open()
        }
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, root.width - 32)
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 22
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.darkTheme)
            border.color: DesignTokens.outlineVariant(root.darkTheme)
        }
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: root.tr2("Rename and style tab", "改名及設定分頁樣式"); font.pixelSize: 22; font.bold: true }
            TextField { id: tabName; Layout.fillWidth: true; placeholderText: root.tr2("Tab name", "分頁名稱"); maximumLength: 120 }
            GridLayout {
                Layout.fillWidth: true
                columns: width > 460 ? 2 : 1
                columnSpacing: 10; rowSpacing: 10
                ComboBox {
                    id: fontFamily
                    Layout.fillWidth: true
                    editable: true
                    model: ["", "Segoe UI", "Arial", "Calibri", "Consolas", "Courier New", "Georgia", "Times New Roman", "Verdana"]
                    displayText: editText.length > 0 ? editText : root.tr2("System font", "系統字型")
                    Accessible.name: root.tr2("Font family", "字型")
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: root.tr2("Size", "大小") }
                    SpinBox { id: fontSize; Layout.fillWidth: true; from: 8; to: 48; editable: true }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: root.tr2("Color", "顏色") }
                    TextField {
                        id: fontColor
                        Layout.fillWidth: true
                        placeholderText: root.tr2("Font color, e.g. #2F6FED", "字色，例如 #2F6FED")
                        maximumLength: 9
                    }
                    Rectangle {
                        Layout.preferredWidth: 46
                        Layout.preferredHeight: 38
                        radius: DesignTokens.radiusControl
                        color: fontColor.text.length > 0 ? fontColor.text : DesignTokens.surfaceContainer(root.darkTheme)
                        border.width: 1
                        border.color: DesignTokens.outline(root.darkTheme)
                        Accessible.role: Accessible.Button
                        Accessible.name: root.tr2("Pick font color", "揀字色")
                        Label {
                            anchors.centerIn: parent
                            visible: fontColor.text.length === 0
                            text: root.tr2("Pick", "揀色")
                            font.pixelSize: 11
                            color: DesignTokens.onSurfaceVariant(root.darkTheme)
                        }
                        TapHandler {
                            onTapped: {
                                fontColorDialog.selectedColor = fontColor.text.length > 0
                                    ? fontColor.text : root.secondaryTextColor
                                fontColorDialog.open()
                            }
                        }
                    }
                    WfIconButton {
                        glyph: "×"
                        buttonSize: 30
                        visible: fontColor.text.length > 0
                        accessibleName: root.tr2("Clear font color", "清除字色")
                        toolTip: accessibleName
                        onClicked: fontColor.text = ""
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    CheckBox { id: fontBold; text: root.tr2("Bold", "粗體") }
                    CheckBox { id: fontItalic; text: root.tr2("Italic", "斜體") }
                    CheckBox { id: fontStrikeout; text: root.tr2("Strikeout", "刪除線") }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 46
                radius: 10
                color: DesignTokens.surfaceContainer(root.darkTheme)
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 20
                    text: tabName.text.length > 0 ? tabName.text : root.tr2("Tab preview", "分頁預覽")
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    color: fontColor.text.length > 0 ? fontColor.text : root.secondaryTextColor
                    font.family: fontFamily.editText.length > 0 ? fontFamily.editText : root.font.family
                    font.pixelSize: fontSize.value
                    font.bold: fontBold.checked
                    font.italic: fontItalic.checked
                    font.strikeout: fontStrikeout.checked
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Button { text: "←"; enabled: tabEditor.editIndex > 0; Accessible.name: root.tr2("Move tab left", "分頁左移"); onClicked: { app.moveWorkspaceTab(tabEditor.editIndex, tabEditor.editIndex - 1); tabEditor.editIndex-- } }
                Button { text: "→"; enabled: tabEditor.editIndex >= 0 && tabEditor.editIndex < app.workspaceTabs.length - 1; Accessible.name: root.tr2("Move tab right", "分頁右移"); onClicked: { app.moveWorkspaceTab(tabEditor.editIndex, tabEditor.editIndex + 1); tabEditor.editIndex++ } }
                Item { Layout.fillWidth: true }
                Button { text: root.tr2("Cancel", "取消"); onClicked: tabEditor.close() }
                Button {
                    text: root.tr2("Save", "儲存")
                    highlighted: true
                    enabled: tabName.text.trim().length > 0
                    onClicked: {
                        if (app.updateWorkspaceTab(tabEditor.editIndex, {
                            title: tabName.text.trim(),
                            fontFamily: fontFamily.editText.trim(),
                            fontSize: fontSize.value,
                            fontColor: fontColor.text.trim(),
                            bold: fontBold.checked,
                            italic: fontItalic.checked,
                            strikeout: fontStrikeout.checked,
                            custom: true
                        })) tabEditor.close()
                    }
                }
            }
        }
    }

    ColorDialog {
        id: fontColorDialog
        title: root.tr2("Font color", "字色")
        onAccepted: fontColor.text = selectedColor.toString().toUpperCase()
    }

    Menu {
        id: tabContextMenu
        property int targetIndex: -1
        MenuItem {
            text: root.tr2("Close tabs to the right", "關閉右邊嘅分頁")
            enabled: tabContextMenu.targetIndex >= 0
                     && tabContextMenu.targetIndex < app.workspaceTabs.length - 1
            onTriggered: {
                var indices = []
                for (var i = tabContextMenu.targetIndex + 1; i < app.workspaceTabs.length; i++)
                    indices.push(i)
                app.closeWorkspaceTabsByIndices(indices)
            }
        }
        MenuItem {
            text: root.tr2("Close tabs to the left", "關閉左邊嘅分頁")
            enabled: tabContextMenu.targetIndex > 0
            onTriggered: {
                var indices = []
                for (var i = 0; i < tabContextMenu.targetIndex; i++)
                    indices.push(i)
                app.closeWorkspaceTabsByIndices(indices)
            }
        }
        MenuItem {
            text: root.tr2("Close other tabs", "關閉其他分頁")
            enabled: app.workspaceTabs.length > 1 && tabContextMenu.targetIndex >= 0
            onTriggered: {
                var indices = []
                for (var i = 0; i < app.workspaceTabs.length; i++)
                    if (i !== tabContextMenu.targetIndex)
                        indices.push(i)
                app.closeWorkspaceTabsByIndices(indices)
            }
        }
        MenuItem {
            text: root.tr2("Close this tab", "關閉呢個分頁")
            enabled: app.workspaceTabs.length > 1 && tabContextMenu.targetIndex >= 0
            onTriggered: app.closeWorkspaceTab(tabContextMenu.targetIndex)
        }
        MenuSeparator { }
        MenuItem {
            text: root.tr2("Close tabs containing name…", "關閉含指定名稱嘅分頁…")
            onTriggered: closeByNameDialog.open()
        }
    }

    Popup {
        id: closeByNameDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(580, root.width - 32)
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 22
        onOpened: patternField.forceActiveFocus()
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.darkTheme)
            border.color: DesignTokens.outlineVariant(root.darkTheme)
            border.width: 1
        }
        readonly property bool regexValid: {
            if (!useRegexCheck.checked || patternField.text.length === 0)
                return true
            try {
                new RegExp(patternField.text)
                return true
            } catch (error) {
                return false
            }
        }
        readonly property var matches: {
            var probe = patternField.text + "|" + useRegexCheck.checked + "|" + caseSensitiveCheck.checked
            var result = []
            var tabs = app.workspaceTabs
            for (var i = 0; i < tabs.length; i++) {
                if (root.tabTitleMatches(tabs[i].title, patternField.text,
                                         useRegexCheck.checked, caseSensitiveCheck.checked))
                    result.push({ index: i, title: tabs[i].title })
            }
            return result
        }
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: root.tr2("Close tabs containing name", "關閉含指定名稱嘅分頁")
                font.pixelSize: 20; font.bold: true
                color: DesignTokens.onSurface(root.darkTheme)
            }
            Label {
                Layout.fillWidth: true
                text: root.tr2("Enter text or a regular expression; matching tabs close together.",
                               "輸入文字或正規表示式，符合嘅分頁會一齊關閉。")
                color: DesignTokens.onSurfaceVariant(root.darkTheme)
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            TextField {
                id: patternField
                Layout.fillWidth: true
                color: DesignTokens.onSurface(root.darkTheme)
                placeholderText: useRegexCheck.checked
                                 ? root.tr2("Pattern, e.g. ^Source|Studio$", "式樣，例如 ^Source|Studio$")
                                 : root.tr2("Text contained in the tab name", "分頁名內含嘅文字")
            }
            Flow {
                Layout.fillWidth: true
                visible: useRegexCheck.checked
                spacing: 6
                Repeater {
                    model: [
                        { label: ".*", insert: ".*" },
                        { label: "\\d+", insert: "\\d+" },
                        { label: "start ^", insert: "^" },
                        { label: "$ end", insert: "$" },
                        { label: "a|b", insert: "|" },
                        { label: "[set]", insert: "[]" },
                        { label: "\\bword\\b", insert: "\\b\\b" }
                    ]
                    delegate: WfButton {
                        required property var modelData
                        variant: "tonal"
                        compact: true
                        text: modelData.label
                        onClicked: {
                            patternField.insert(patternField.cursorPosition, modelData.insert)
                            patternField.forceActiveFocus()
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                CheckBox { id: useRegexCheck; text: root.tr2("Use regular expression", "用正規表示式") }
                CheckBox { id: caseSensitiveCheck; text: root.tr2("Case sensitive", "區分大小寫") }
            }
            Label {
                visible: !closeByNameDialog.regexValid
                text: root.tr2("The regular expression is not valid.", "正規表示式唔正確。")
                color: DesignTokens.error(root.darkTheme)
                font.pixelSize: 12
            }
            Label {
                Layout.fillWidth: true
                text: closeByNameDialog.matches.length === 0
                      ? root.tr2("No tabs match yet.", "暫時冇分頁符合。")
                      : root.tr2("Matching tabs: ", "符合嘅分頁：") + closeByNameDialog.matches.length
                color: DesignTokens.onSurfaceVariant(root.darkTheme)
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 118
                radius: 10
                color: DesignTokens.surfaceContainer(root.darkTheme)
                border.color: DesignTokens.outlineVariant(root.darkTheme)
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 8
                    clip: true
                    Column {
                        width: parent.width
                        spacing: 2
                        Repeater {
                            model: closeByNameDialog.matches
                            delegate: Label {
                                required property var modelData
                                width: parent.width
                                text: "• " + modelData.title
                                elide: Text.ElideRight
                                color: DesignTokens.onSurface(root.darkTheme)
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                WfButton {
                    variant: "text"
                    text: root.tr2("Cancel", "取消")
                    onClicked: closeByNameDialog.close()
                }
                WfButton {
                    variant: "destructive"
                    text: root.tr2("Close matching", "關閉符合項") + " (" + closeByNameDialog.matches.length + ")"
                    enabled: closeByNameDialog.matches.length > 0 && closeByNameDialog.regexValid
                    onClicked: {
                        var indices = closeByNameDialog.matches.map(function(entry) { return entry.index })
                        if (app.closeWorkspaceTabsByIndices(indices))
                            closeByNameDialog.close()
                    }
                }
            }
        }
    }

    FileDialog {
        id: exportTabsDialog
        title: root.tr2("Export portable tabs", "匯出可攜分頁")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wftabs"
        nameFilters: [root.tr2("WimForge tabs (*.wftabs)", "WimForge 分頁 (*.wftabs)"), root.tr2("All files (*)", "所有檔案 (*)")]
        onAccepted: app.exportWorkspaceTabs(app.pathFromUrl(selectedFile))
    }
    FileDialog {
        id: importTabsDialog
        title: root.tr2("Import portable tabs", "匯入可攜分頁")
        fileMode: FileDialog.OpenFile
        nameFilters: [root.tr2("WimForge tabs (*.wftabs)", "WimForge 分頁 (*.wftabs)"), root.tr2("All files (*)", "所有檔案 (*)")]
        onAccepted: app.importWorkspaceTabs(app.pathFromUrl(selectedFile))
    }
    FileDialog {
        id: exportTabRepoDialog
        title: root.tr2("Export complete tab Git repository", "匯出完整分頁 Git 儲存庫")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wftabrepo"
        nameFilters: [root.tr2("WimForge tab repository (*.wftabrepo)", "WimForge 分頁儲存庫 (*.wftabrepo)"), root.tr2("All files (*)", "所有檔案 (*)")]
        onAccepted: app.exportWorkspaceTabRepository(app.pathFromUrl(selectedFile))
    }
    FileDialog {
        id: importTabRepoDialog
        title: root.tr2("Import complete tab Git repository", "匯入完整分頁 Git 儲存庫")
        fileMode: FileDialog.OpenFile
        nameFilters: [root.tr2("WimForge tab repository (*.wftabrepo)", "WimForge 分頁儲存庫 (*.wftabrepo)"), root.tr2("All files (*)", "所有檔案 (*)")]
        onAccepted: app.importWorkspaceTabRepository(app.pathFromUrl(selectedFile))
    }

    Popup {
        id: newProjectSheet
        readonly property string heading: root.tr2("Create a Git-backed project", "開個有 Git 保護嘅工程")
        anchors.centerIn: Overlay.overlay
        width: Math.min(600, root.width - 32)
        height: Math.min(newProjectContent.implicitHeight + topPadding + bottomPadding, root.height - 32)
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 24
        onOpened: projectName.forceActiveFocus()
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.darkTheme)
            border.color: DesignTokens.outlineVariant(root.darkTheme)
            border.width: 1
        }

        contentItem: ScrollView {
            id: newProjectScroll
            Accessible.name: newProjectSheet.heading
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: newProjectContent
                width: newProjectSheet.availableWidth
                spacing: 12
                Label { Layout.fillWidth: true; text: newProjectSheet.heading; font.pixelSize: 22; font.weight: Font.Bold; wrapMode: Text.Wrap }
                Label { Layout.fillWidth: true; text: root.tr2("The folder becomes its own local repository. Every edit is committed automatically.", "呢個資料夾會變成獨立本機 Git 倉，每次改動都自動 commit。唔怕手快快。"); wrapMode: Text.Wrap }
                TextField { id: projectName; Layout.fillWidth: true; Accessible.name: placeholderText; placeholderText: root.tr2("Project name", "工程名"); text: "Windows 11 Custom" }
                RowLayout {
                    Layout.fillWidth: true
                    TextField { id: projectRoot; Layout.fillWidth: true; Accessible.name: placeholderText; placeholderText: root.tr2("Project folder", "工程資料夾"); text: app.defaultProjectPath }
                    Button { text: root.tr2("Browse…", "瀏覽…"); onClicked: newProjectFolderDialog.open() }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    Button { text: root.tr2("Cancel", "取消"); flat: true; onClicked: newProjectSheet.close() }
                    Button {
                        text: root.tr2("Create project", "建立工程")
                        highlighted: true
                        enabled: projectName.text.trim().length > 0 && projectRoot.text.trim().length > 0
                        onClicked: {
                            if (app.createProject(projectRoot.text.trim(), projectName.text.trim())) {
                                newProjectSheet.close(); root.navigateToPage(1)
                            }
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: newProjectFolderDialog
        title: root.tr2("Choose the new project folder", "揀新工程資料夾")
        onAccepted: projectRoot.text = app.pathFromUrl(selectedFolder)
    }

    Popup {
        id: openProjectSheet
        readonly property string heading: root.tr2("Open or import project", "開啟或者匯入工程")
        anchors.centerIn: Overlay.overlay
        width: Math.min(600, root.width - 32)
        height: Math.min(openProjectContent.implicitHeight + topPadding + bottomPadding, root.height - 32)
        modal: true; dim: true; focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 24
        onOpened: openPath.forceActiveFocus()
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.darkTheme)
            border.color: DesignTokens.outlineVariant(root.darkTheme)
            border.width: 1
        }

        contentItem: ScrollView {
            id: openProjectScroll
            Accessible.name: openProjectSheet.heading
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: openProjectContent
                width: openProjectSheet.availableWidth
                spacing: 12
                Label { Layout.fillWidth: true; text: openProjectSheet.heading; font.pixelSize: 22; font.weight: Font.Bold; wrapMode: Text.Wrap }
                Label { Layout.fillWidth: true; text: root.tr2("Choose a project folder, complete .wimforge save bundle, or legacy .json config. Paths can still be pasted for automation.", "揀工程資料夾、完整 .wimforge 儲存 bundle，或者舊式 .json 設定；自動化時亦可以貼路徑。"); wrapMode: Text.Wrap }
                RowLayout {
                    Layout.fillWidth: true
                    TextField { id: openPath; Layout.fillWidth: true; Accessible.name: placeholderText; placeholderText: root.tr2("Project folder or config file", "工程資料夾或者設定檔") }
                    Button { text: root.tr2("Project folder…", "工程資料夾…"); onClicked: openProjectFolderDialog.open() }
                    Button { text: root.tr2("Import file…", "匯入檔案…"); onClicked: importProjectFileDialog.open() }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: openPath.text.toLowerCase().endsWith(".json") || openPath.text.toLowerCase().endsWith(".wimforge")
                    TextField { id: importDestination; Layout.fillWidth: true; Accessible.name: placeholderText; placeholderText: root.tr2("Destination folder for imported project", "匯入工程目的資料夾"); text: app.defaultProjectPath }
                    Button { text: root.tr2("Browse…", "瀏覽…"); onClicked: importDestinationFolderDialog.open() }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    Button { text: root.tr2("Cancel", "取消"); flat: true; onClicked: openProjectSheet.close() }
                    Button {
                        text: root.tr2("Open", "開啟")
                        highlighted: true
                        enabled: openPath.text.trim().length > 0
                        onClicked: {
                            var ok = openPath.text.toLowerCase().endsWith(".json") || openPath.text.toLowerCase().endsWith(".wimforge")
                                   ? app.importProject(openPath.text.trim(), importDestination.text.trim())
                                   : app.openProject(openPath.text.trim())
                            if (ok) { openProjectSheet.close(); root.syncActiveWorkspaceTab() }
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: openProjectFolderDialog
        title: root.tr2("Choose a WimForge project folder", "揀 WimForge 工程資料夾")
        onAccepted: openPath.text = app.pathFromUrl(selectedFolder)
    }
    FileDialog {
        id: importProjectFileDialog
        title: root.tr2("Choose a project export", "揀工程匯出檔")
        fileMode: FileDialog.OpenFile
        nameFilters: [root.tr2("WimForge projects (*.wimforge *.json)", "WimForge 工程 (*.wimforge *.json)"), root.tr2("All files (*)", "所有檔案 (*)")]
        onAccepted: openPath.text = app.pathFromUrl(selectedFile)
    }
    FolderDialog {
        id: importDestinationFolderDialog
        title: root.tr2("Choose the project import destination", "揀工程匯入目的地")
        onAccepted: importDestination.text = app.pathFromUrl(selectedFolder)
    }

    Popup {
        id: runSheet
        readonly property string heading: root.tr2("Run the reviewed plan?", "執行已檢查計劃？")
        anchors.centerIn: Overlay.overlay
        width: Math.min(660, root.width - 32)
        height: Math.min(runContent.implicitHeight + topPadding + bottomPadding, root.height - 32)
        modal: true; dim: true; focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 24
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.darkTheme)
            border.color: destructiveBadge.visible
                          ? root.errorColor : DesignTokens.outlineVariant(root.darkTheme)
            border.width: destructiveBadge.visible ? 2 : 1
        }

        contentItem: ScrollView {
            id: runScroll
            Accessible.name: runSheet.heading
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: runContent
                width: runSheet.availableWidth
                spacing: 10
                Label { Layout.fillWidth: true; text: runSheet.heading; font.pixelSize: 22; font.weight: Font.Bold; wrapMode: Text.Wrap }
                Label { id: runSummary; Layout.fillWidth: true; wrapMode: Text.Wrap }
                Label { id: destructiveBadge; Layout.fillWidth: true; visible: false; color: root.errorColor; font.weight: Font.Bold; wrapMode: Text.Wrap }
                CheckBox { id: sourceBackupAck; Layout.fillWidth: true; text: root.tr2("I verified the source and output paths", "我睇清楚來源同輸出路徑") }
                CheckBox { id: adminAck; Layout.fillWidth: true; text: root.tr2("I understand servicing needs Administrator rights", "我明白映像維護要管理員權限") }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    Button { text: root.tr2("Keep reviewing", "再睇吓先"); flat: true; onClicked: runSheet.close() }
                    Button {
                        text: root.tr2("Start jobs", "開始工序")
                        highlighted: true
                        enabled: sourceBackupAck.checked && adminAck.checked
                        onClicked: { runSheet.close(); app.runPlan() }
                    }
                }
            }
        }
    }

    component PathExportSheet: Popup {
        id: exportSheet
        property string heading
        property string placeholder
        property var nameFilters: [root.tr2("All files (*)", "所有檔案 (*)")]
        property string defaultSuffix: ""
        property var acceptAction: function(path) { return false }
        anchors.centerIn: Overlay.overlay
        width: Math.min(600, root.width - 32)
        height: Math.min(exportContent.implicitHeight + topPadding + bottomPadding, root.height - 32)
        modal: true; dim: true; focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 24
        onOpened: exportPathField.forceActiveFocus()
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.darkTheme)
            border.color: DesignTokens.outlineVariant(root.darkTheme)
            border.width: 1
        }

        contentItem: ScrollView {
            id: exportScroll
            Accessible.name: exportSheet.heading
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: exportContent
                width: exportSheet.availableWidth
                spacing: 12
                Label { Layout.fillWidth: true; text: exportSheet.heading; font.pixelSize: 22; font.weight: Font.Bold; wrapMode: Text.Wrap }
                RowLayout {
                    Layout.fillWidth: true
                    TextField { id: exportPathField; Layout.fillWidth: true; Accessible.name: exportSheet.placeholder; placeholderText: exportSheet.placeholder }
                    Button { text: root.tr2("Browse…", "瀏覽…"); onClicked: exportFileDialog.open() }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    Button { text: root.tr2("Cancel", "取消"); flat: true; onClicked: exportSheet.close() }
                    Button { text: root.tr2("Export", "匯出"); highlighted: true; enabled: exportPathField.text.trim().length > 0; onClicked: { if (exportSheet.acceptAction(exportPathField.text.trim())) exportSheet.close() } }
                }
            }
        }
        FileDialog {
            id: exportFileDialog
            title: exportSheet.heading
            fileMode: FileDialog.SaveFile
            nameFilters: exportSheet.nameFilters
            defaultSuffix: exportSheet.defaultSuffix
            onAccepted: exportPathField.text = app.pathFromUrl(selectedFile)
        }
    }

    PathExportSheet {
        id: exportProjectSheet
        heading: root.tr2("Export complete project + Git histories", "匯出完整工程同所有 Git 歷史")
        placeholder: "D:\\profiles\\windows-build.wimforge"
        nameFilters: [root.tr2("WimForge project bundle (*.wimforge)", "WimForge 工程 bundle (*.wimforge)"), root.tr2("JSON configuration (*.json)", "JSON 設定 (*.json)")]
        defaultSuffix: "wimforge"
        acceptAction: path => app.exportProject(path)
    }
    PathExportSheet {
        id: exportScriptSheet
        heading: root.tr2("Export PowerShell build script", "匯出 PowerShell 建置 script")
        placeholder: "D:\\profiles\\build-image.ps1"
        nameFilters: [root.tr2("PowerShell scripts (*.ps1)", "PowerShell script (*.ps1)")]
        defaultSuffix: "ps1"
        acceptAction: path => app.exportScript(path)
    }

    Popup {
        id: recoverySheet
        readonly property string heading: root.tr2("Recovery workspace", "復原工作區")
        anchors.centerIn: Overlay.overlay
        width: Math.min(720, root.width - 32)
        height: Math.min(recoveryContent.implicitHeight + topPadding + bottomPadding, root.height - 32)
        modal: true; dim: true; focus: true
        closePolicy: Popup.CloseOnEscape
        padding: 24
        background: Rectangle {
            radius: DesignTokens.radiusCard
            color: DesignTokens.surfaceLowest(root.darkTheme)
            border.color: root.warningColor
            border.width: 2
        }

        contentItem: ScrollView {
            id: recoveryScroll
            Accessible.name: recoverySheet.heading
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: recoveryContent
                width: recoverySheet.availableWidth
                spacing: 12
                Label { Layout.fillWidth: true; text: recoverySheet.heading; font.pixelSize: 22; font.weight: Font.Bold; wrapMode: Text.Wrap }
                Label { Layout.fillWidth: true; text: app.recoverySummary; wrapMode: Text.Wrap }
                GridLayout {
                    Layout.fillWidth: true
                    columns: width >= 620 && app.languageMode !== 2 ? 4 : 1
                    rowSpacing: 6
                    columnSpacing: 6
                    Button { Layout.fillWidth: true; text: root.tr2("Rebuild & review plan", "重排同檢查計劃"); onClicked: { recoverySheet.close(); app.resumeRecovery() } }
                    Button { Layout.fillWidth: true; text: root.tr2("Undo latest config", "Undo 最新設定"); onClicked: { recoverySheet.close(); app.rollbackRecovery() } }
                    Button { Layout.fillWidth: true; text: root.tr2("Safe unmount", "安全卸載"); onClicked: { recoverySheet.close(); app.safeUnmountRecovery() } }
                    Button { Layout.fillWidth: true; text: root.tr2("Later", "遲啲先"); flat: true; onClicked: recoverySheet.close() }
                }
            }
        }
    }
}
