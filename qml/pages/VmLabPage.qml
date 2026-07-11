pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components"

Item {
    id: root

    required property var app
    required property var tr

    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
    readonly property bool compact: width < 900
    readonly property bool compactHeight: height < 520
    readonly property color cardColor: DesignTokens.surfaceLowest(root.dark)
    readonly property color raisedColor: DesignTokens.surfaceLow(root.dark)
    readonly property color outlineColor: DesignTokens.outlineVariant(root.dark)
    readonly property color secondaryText: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color errorText: DesignTokens.error(root.dark)
    readonly property color warningText: DesignTokens.tertiary(root.dark)
    readonly property color successText: DesignTokens.success(root.dark)
    readonly property color primaryText: DesignTokens.primary(root.dark)
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)

    property string selectedSnapshotId: ""
    property string selectedValidationRunId: ""
    property bool inventoryDetailOpen: false
    property string pendingDispatchKind: "vm"
    property string pendingAction: ""
    property var pendingOptions: ({})
    readonly property var pendingPreview: root.app.vmPendingPreview
    readonly property bool reviewNeedsToken:
        root.value(root.pendingPreview, "confirmation", "").length > 0

    // Keep the controller contract in this block. The views below never call the
    // VM controller directly, so API changes remain local to these helpers.
    function refreshLab() {
        root.app.refreshVmLab()
    }

    function chooseVm(providerId, id) {
        root.app.selectVm(providerId, id)
        root.selectedSnapshotId = ""
        if (root.compact)
            root.inventoryDetailOpen = true
        Qt.callLater(root.resetConfigurationEditor)
    }

    component VmSectionCard: WfCard {
        id: sectionCard

        property string titleText: ""
        default property alias sectionContent: sectionLayout.data

        dark: root.dark
        outlined: true
        surfaceLevel: "lowest"
        padding: DesignTokens.spacing16

        ColumnLayout {
            id: sectionLayout
            anchors.fill: parent
            spacing: DesignTokens.spacing12

            Label {
                Layout.fillWidth: true
                text: sectionCard.titleText
                color: DesignTokens.onSurface(sectionCard.dark)
                font.family: DesignTokens.fontDisplay
                font.pixelSize: 15
                font.weight: Font.Bold
                wrapMode: Text.Wrap
            }
        }
    }

    function submitCreate(spec) {
        root.app.createVm(spec)
    }

    function submitVmAction(action, options) {
        root.app.runVmAction(action, options)
    }

    function submitConfiguration(spec) {
        root.app.updateVmConfiguration(spec)
    }

    function submitDeviceAction(action, spec) {
        root.app.vmDeviceAction(action, spec)
    }

    function submitSnapshotAction(action, spec) {
        root.app.vmSnapshotAction(action, spec)
    }

    function submitValidation(spec) {
        root.app.startVmValidation(spec)
    }

    function submitMilestone(runId, spec) {
        root.app.recordVmValidationMilestone(runId, spec)
    }

    function submitValidationResult(runId, result) {
        root.app.finishVmValidation(runId, result)
    }

    function cancelCurrentAction() {
        root.app.cancelVmAction()
    }

    function requestConfirmation(kind, action, title, message, options) {
        root.pendingDispatchKind = kind
        root.pendingAction = action
        root.pendingOptions = options || ({})
        confirmationDialog.title = title
        confirmationMessage.text = message
        deleteFilesCheck.checked = false
        confirmationDialog.open()
    }

    function dispatchConfirmedAction() {
        var options = {}
        var key
        for (key in root.pendingOptions)
            options[key] = root.pendingOptions[key]
        if (root.pendingDispatchKind === "vm" && root.pendingAction === "delete")
            options.deleteFiles = deleteFilesCheck.checked

        if (root.pendingDispatchKind === "device")
            root.submitDeviceAction(root.pendingAction, options)
        else if (root.pendingDispatchKind === "snapshot")
            root.submitSnapshotAction(root.pendingAction, options)
        else
            root.submitVmAction(root.pendingAction, options)
    }

    function commandText(command) {
        var executable = root.value(command, "executable", "")
        var arguments = root.list(root.value(command, "arguments", []))
        var text = executable
        if (arguments.length > 0)
            text += "\n    " + arguments.join("\n    ")
        var workingDirectory = root.value(command, "workingDirectory", "")
        if (workingDirectory.length > 0)
            text += "\n  in: " + workingDirectory
        text += "\n  timeout: " + root.value(command, "timeoutMs", 0) + " ms"
        if (root.value(command, "detached", false))
            text += " · detached"
        return text
    }

    function value(object, key, fallback) {
        if (object === null || object === undefined)
            return fallback
        var candidate = object[key]
        return candidate === null || candidate === undefined ? fallback : candidate
    }

    function hasValue(object, key) {
        return object !== null && object !== undefined
                && object[key] !== null && object[key] !== undefined
    }

    function resetConfigurationEditor() {
        changeCpu.checked = false
        changeMemory.checked = false
        changeFirmware.checked = false
        changeSecureBoot.checked = false
        changeTpm.checked = false
        configCpu.value = root.value(root.selectedVm(), "cpuCount", 2)
        configMemory.value = root.value(root.selectedVm(), "memoryMiB", 4096)
        configFirmware.currentIndex = root.value(root.selectedVm(), "firmware", "efi") === "bios" ? 1 : 0
        configSecureBoot.checked = Boolean(root.value(root.selectedVm(), "secureBoot", false))
        configTpm.checked = Boolean(root.value(root.selectedVm(), "tpm", false))
    }

    function list(candidate) {
        return candidate || []
    }

    function selectedVm() {
        return root.app.vmSelected || null
    }

    function selectedVmId() {
        return root.value(root.selectedVm(), "id", root.app.vmSelectedId || "")
    }

    function providerIds(availableOnly, requiredCapability) {
        var result = []
        var providers = root.list(root.app.vmProviders)
        for (var i = 0; i < providers.length; ++i) {
            var available = Boolean(root.value(providers[i], "available", false))
            var supported = !requiredCapability
                    || root.providerSupports(root.value(providers[i], "id", ""), requiredCapability)
            if ((!availableOnly || available) && supported)
                result.push(root.value(providers[i], "id", ""))
        }
        return result
    }

    function providerById(id) {
        var providers = root.list(root.app.vmProviders)
        for (var i = 0; i < providers.length; ++i) {
            if (root.value(providers[i], "id", "") === id)
                return providers[i]
        }
        return null
    }

    function importProviderIds() {
        var providers = root.providerIds(true)
        var result = []
        for (var i = 0; i < providers.length; ++i) {
            var id = providers[i]
            if (id.indexOf("vmware-") === 0 || root.providerSupports(id, "register"))
                result.push(id)
        }
        return result
    }

    function providerName(id) {
        var provider = root.providerById(id)
        return root.value(provider, "displayName", id || root.tr("Unknown provider", "未知供應器"))
    }

    function providerSupports(id, capability) {
        var provider = root.providerById(id)
        var capabilities = root.value(provider, "capabilities", [])
        if (capabilities && capabilities.indexOf)
            return capabilities.indexOf(capability) >= 0
        return Boolean(capabilities && capabilities[capability])
    }

    function capabilitySummary(provider) {
        var capabilities = root.list(root.value(provider, "capabilities", []))
        if (capabilities.join)
            return capabilities.length > 0 ? capabilities.join("  ·  ") : root.tr("No proven capabilities", "未證實任何能力")
        var names = []
        for (var key in capabilities) {
            if (capabilities[key])
                names.push(key)
        }
        return names.length > 0 ? names.join("  ·  ") : root.tr("No proven capabilities", "未證實任何能力")
    }

    function powerState(machine) {
        return String(root.value(machine, "powerState", root.value(machine, "state", "unknown"))).toLowerCase()
    }

    function isPoweredOff(machine) {
        var state = root.powerState(machine).replace(/[^a-z]/g, "")
        return state === "poweredoff"
    }

    function allowsVmAction(action) {
        var actions = root.list(root.value(root.selectedVm(), "allowedActions", []))
        return actions.indexOf(action) >= 0
    }

    function lifecycleAvailable() {
        return root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "lifecycle")
    }

    function ambiguousVmwareActive() {
        return root.value(root.selectedVm(), "providerId", "").indexOf("vmware-") === 0
                && root.powerState(root.selectedVm()) === "running"
    }

    function stateTone(machine) {
        var state = root.powerState(machine)
        if (state === "running") return "success"
        if (state === "inaccessible" || state === "error") return "error"
        if (state === "paused" || state === "suspended" || state === "saved") return "warning"
        return "neutral"
    }

    function stateColor(machine) {
        var tone = root.stateTone(machine)
        if (tone === "success") return DesignTokens.success(root.dark)
        if (tone === "error") return DesignTokens.error(root.dark)
        if (tone === "warning") return DesignTokens.tertiary(root.dark)
        return DesignTokens.outline(root.dark)
    }

    function stateLabel(state) {
        var normalized = String(state || "unknown").toLowerCase()
        if (normalized === "all") return root.tr("All states", "全部狀態")
        if (normalized === "running") return root.tr("Running", "運作緊")
        if (normalized === "paused") return root.tr("Paused", "已暫停")
        if (normalized === "suspended") return root.tr("Suspended", "已擱置")
        if (normalized === "saved") return root.tr("Saved", "已儲存")
        if (normalized === "powered-off" || normalized === "poweredoff") return root.tr("Powered off", "已關機")
        if (normalized === "aborted") return root.tr("Aborted", "已中止")
        if (normalized === "inaccessible") return root.tr("Inaccessible", "存取唔到")
        return normalized.length > 0 ? normalized : root.tr("Unknown", "未知")
    }

    function filteredInventory() {
        var inventory = root.list(root.app.vmInventory)
        var result = []
        var query = inventorySearch.text.trim().toLowerCase()
        for (var i = 0; i < inventory.length; ++i) {
            var machine = inventory[i]
            var providerMatch = providerFilter.currentText === "all"
                    || root.value(machine, "providerId", "") === providerFilter.currentText
            var state = root.powerState(machine)
            var wantedState = stateFilter.currentText
            var stateMatch = wantedState === "all"
                    || state === wantedState
                    || (wantedState === "powered-off" && root.isPoweredOff(machine))
            var searchable = (root.value(machine, "name", "") + " "
                              + root.value(machine, "id", "") + " "
                              + root.value(machine, "configPath", "") + " "
                              + root.value(machine, "providerId", "")).toLowerCase()
            if (providerMatch && stateMatch && (query.length === 0 || searchable.indexOf(query) >= 0))
                result.push(machine)
        }
        return result
    }

    function selectedSnapshot() {
        var snapshots = root.list(root.app.vmSnapshots)
        for (var i = 0; i < snapshots.length; ++i) {
            if (root.value(snapshots[i], "id", "") === root.selectedSnapshotId)
                return snapshots[i]
        }
        return null
    }

    function selectedValidationRun() {
        var runs = root.list(root.app.vmValidationRuns)
        for (var i = 0; i < runs.length; ++i) {
            if (root.value(runs[i], "id", "") === root.selectedValidationRunId)
                return runs[i]
        }
        return null
    }

    function selectedValidationRunning() {
        return root.selectedValidationRun() !== null
                && String(root.value(root.selectedValidationRun(), "status", "")).toLowerCase() === "running"
    }

    function statusMessage() {
        var status = root.app.vmStatus
        if (typeof status === "string")
            return status
        return root.value(status, "message", root.value(status, "detail", root.tr("VM Lab is ready.", "VM 實驗室準備好。")))
    }

    function createSpec() {
        return {
            providerId: createProvider.currentText,
            name: createName.text.trim(),
            directory: createDirectory.text.trim(),
            guestType: guestType.currentText,
            firmware: firmware.currentText,
            secureBoot: secureBoot.enabled && secureBoot.checked,
            tpm: tpmEnabled.enabled && tpmEnabled.checked,
            cpuCount: cpuCount.value,
            memoryMiB: memoryMiB.value,
            diskMiB: diskGiB.value * 1024,
            networkMode: networkMode.currentText,
            bridgedInterface: bridgeName.text.trim(),
            isoPath: sourceIso.text.trim(),
            unattendedBoot: unattendedBoot.checked,
            bootAfterCreate: bootAfterCreate.checked
        }
    }

    function configurationSpec() {
        var spec = {
            vmId: root.selectedVmId(),
            providerId: root.value(root.selectedVm(), "providerId", "")
        }
        if (changeCpu.enabled && changeCpu.checked) spec.cpuCount = configCpu.value
        if (changeMemory.enabled && changeMemory.checked) spec.memoryMiB = configMemory.value
        if (changeFirmware.enabled && changeFirmware.checked) spec.firmware = configFirmware.currentText
        if (changeSecureBoot.enabled && changeSecureBoot.checked) spec.secureBoot = configSecureBoot.checked
        if (changeTpm.enabled && changeTpm.checked) spec.tpm = configTpm.checked
        return spec
    }

    Connections {
        target: root.app
        function onVmPreviewReady() {
            reviewConfirmation.text = ""
            reviewDialog.open()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: DesignTokens.spacing16

        WfPageHeader {
            Layout.fillWidth: true
            dark: root.dark
            title: root.tr("Virtual Machine Lab", "虛擬機實驗室")
            description: root.compactHeight ? "" : root.tr(
                             "Boot the finished ISO in VMware or VirtualBox, snapshot state, and record a validation run before it ships.",
                             "喺 VMware 或 VirtualBox 啟動完成嘅 ISO、建立狀態快照，並喺交付前記錄驗證執行。")

            WfStatusChip {
                visible: !root.compact
                dark: root.dark
                tone: root.list(root.app.vmInventory).length > 0 ? "success" : "neutral"
                showDot: true
                uppercase: false
                compact: false
                text: root.tr("%1 machines", "%1 部虛擬機").arg(root.list(root.app.vmInventory).length)
            }
            WfButton {
                dark: root.dark
                variant: "outlined"
                text: root.tr("Refresh", "重新整理")
                Accessible.name: root.tr("Refresh VM providers and inventory", "重新整理虛擬機供應器及清單")
                enabled: !root.app.vmBusy
                onClicked: root.refreshLab()
            }
            WfButton {
                dark: root.dark
                variant: "filled"
                text: root.tr("New VM", "新增虛擬機")
                Accessible.name: root.tr("Open virtual machine creation", "開啟虛擬機建立畫面")
                onClicked: sectionTabs.currentIndex = 1
            }
        }

        TabBar {
            id: sectionTabs
            Layout.fillWidth: true
            currentIndex: 0

            background: Rectangle { color: "transparent" }

            TabButton {
                width: sectionTabs.width / 5
                text: root.tr("Inventory", "清單")
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                implicitHeight: DesignTokens.rowHeight
            }
            TabButton {
                width: sectionTabs.width / 5
                text: root.compact ? root.tr("Create", "建立／載入")
                                   : root.tr("Create / load", "建立／載入")
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                implicitHeight: DesignTokens.rowHeight
            }
            TabButton {
                width: sectionTabs.width / 5
                text: root.tr("Hardware", "硬件")
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                implicitHeight: DesignTokens.rowHeight
            }
            TabButton {
                width: sectionTabs.width / 5
                text: root.tr("Snapshots", "快照")
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                implicitHeight: DesignTokens.rowHeight
            }
            TabButton {
                width: sectionTabs.width / 5
                text: root.tr("Validation", "驗證")
                font.family: DesignTokens.fontBody
                font.pixelSize: 12
                implicitHeight: DesignTokens.rowHeight
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.compactHeight ? 180 : 260
            currentIndex: sectionTabs.currentIndex

            Item {
                GridLayout {
                    anchors.fill: parent
                    columns: root.compact ? 1 : 2
                    columnSpacing: DesignTokens.spacing16
                    rowSpacing: DesignTokens.spacing16

                    WfCard {
                        visible: !root.compact || !root.inventoryDetailOpen
                        Layout.fillWidth: root.compact
                        Layout.preferredWidth: root.compact ? -1 : 300
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: root.compactHeight ? 180 : 230
                        dark: root.dark
                        outlined: true
                        surfaceLevel: "lowest"
                        padding: DesignTokens.spacing12

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: DesignTokens.spacing8

                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Virtual machines", "虛擬機")
                                color: root.surfaceForeground
                                font.family: DesignTokens.fontDisplay
                                font.pixelSize: 15
                                font.weight: Font.Bold
                            }

                            ListView {
                                id: providerList
                                Layout.fillWidth: true
                                Layout.preferredHeight: 46
                                orientation: ListView.Horizontal
                                spacing: DesignTokens.spacing8
                                clip: true
                                model: root.app.vmProviders || []
                                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

                                delegate: Rectangle {
                                    id: providerCard
                                    required property var modelData
                                    width: Math.max(142, providerNameLabel.implicitWidth + 38)
                                    height: DesignTokens.rowHeight
                                    radius: DesignTokens.radiusControl
                                    color: DesignTokens.surfaceLow(root.dark)
                                    border.width: 1
                                    border.color: root.outlineColor
                                    Accessible.name: root.tr("Provider %1", "供應器 %1").arg(root.value(providerCard.modelData, "displayName", ""))

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: DesignTokens.spacing8
                                        Rectangle {
                                            Layout.preferredWidth: 8
                                            Layout.preferredHeight: 8
                                            radius: 4
                                            color: root.value(providerCard.modelData, "available", false)
                                                   ? root.successText : root.errorText
                                        }
                                        Label {
                                            id: providerNameLabel
                                            Layout.fillWidth: true
                                            text: root.value(providerCard.modelData, "displayName",
                                                             root.value(providerCard.modelData, "id", ""))
                                            color: root.surfaceForeground
                                            font.family: DesignTokens.fontBody
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                            ToolTip.visible: providerHover.containsMouse
                                            ToolTip.text: root.value(providerCard.modelData, "version", root.tr("Version not reported", "未回報版本"))
                                                          + "\n" + root.capabilitySummary(providerCard.modelData)
                                        }
                                    }
                                    MouseArea { id: providerHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: providerList.count === 0
                                    text: root.tr("No providers detected", "未偵測到供應器")
                                    color: root.secondaryText
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 11
                                }
                            }

                            TextField {
                                id: inventorySearch
                                Layout.fillWidth: true
                                Layout.preferredHeight: DesignTokens.fieldHeight
                                placeholderText: root.tr("Search machine, ID or path…", "搜尋虛擬機、ID 或路徑……")
                                Accessible.name: root.tr("Search virtual machines", "搜尋虛擬機")
                                selectByMouse: true
                                font.family: DesignTokens.fontBody
                                font.pixelSize: 12
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: DesignTokens.spacing8
                                ComboBox {
                                    id: providerFilter
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: DesignTokens.controlHeight
                                    model: ["all"].concat(root.providerIds(false))
                                    displayText: currentText === "all" ? root.tr("All providers", "全部供應器") : root.providerName(currentText)
                                    Accessible.name: root.tr("Filter by provider", "按供應器篩選")
                                    delegate: ItemDelegate {
                                        required property var modelData
                                        width: providerFilter.width
                                        text: modelData === "all" ? root.tr("All providers", "全部供應器") : root.providerName(modelData)
                                    }
                                }
                                ComboBox {
                                    id: stateFilter
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: DesignTokens.controlHeight
                                    model: ["all", "running", "paused", "suspended", "saved", "powered-off", "inaccessible"]
                                    displayText: root.stateLabel(currentText)
                                    Accessible.name: root.tr("Filter by power state", "按電源狀態篩選")
                                    delegate: ItemDelegate {
                                        required property var modelData
                                        width: stateFilter.width
                                        text: root.stateLabel(modelData)
                                    }
                                }
                            }

                            ListView {
                                id: inventoryList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 120
                                clip: true
                                spacing: DesignTokens.spacing8
                                boundsBehavior: Flickable.StopAtBounds
                                model: root.filteredInventory()
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                                delegate: Rectangle {
                                    id: machineDelegate
                                    required property var modelData
                                    readonly property bool selected:
                                        root.value(machineDelegate.modelData, "id", "") === root.app.vmSelectedId
                                        && root.value(machineDelegate.modelData, "providerId", "")
                                           === root.value(root.app.vmSelected, "providerId", "")
                                    width: inventoryList.width
                                    height: 62
                                    radius: DesignTokens.radiusCard
                                    color: selected ? DesignTokens.primaryContainer(root.dark)
                                                    : DesignTokens.surfaceLowest(root.dark)
                                    border.width: 1
                                    border.color: selected ? DesignTokens.primary(root.dark) : root.outlineColor
                                    Accessible.name: root.tr("Select %1, %2", "選取 %1，%2")
                                                        .arg(root.value(machineDelegate.modelData, "name", ""))
                                                        .arg(root.stateLabel(root.powerState(machineDelegate.modelData)))

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: DesignTokens.spacing12
                                        anchors.rightMargin: DesignTokens.spacing12
                                        spacing: DesignTokens.spacing12
                                        Rectangle {
                                            Layout.preferredWidth: 9
                                            Layout.preferredHeight: 9
                                            radius: 5
                                            color: root.stateColor(machineDelegate.modelData)
                                            Accessible.ignored: true
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label {
                                                Layout.fillWidth: true
                                                text: root.value(machineDelegate.modelData, "name",
                                                                 root.value(machineDelegate.modelData, "id", ""))
                                                color: selected ? DesignTokens.onPrimaryContainer(root.dark) : root.surfaceForeground
                                                font.family: DesignTokens.fontBody
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: root.providerName(root.value(machineDelegate.modelData, "providerId", ""))
                                                      + " · " + root.stateLabel(root.powerState(machineDelegate.modelData))
                                                color: selected ? DesignTokens.onPrimaryContainer(root.dark) : root.secondaryText
                                                font.family: DesignTokens.fontBody
                                                font.pixelSize: 10
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.chooseVm(root.value(machineDelegate.modelData, "providerId", ""),
                                                                 root.value(machineDelegate.modelData, "id", ""))
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    width: Math.max(80, parent.width - 24)
                                    visible: inventoryList.count === 0
                                    text: root.tr("No machines match these filters.", "冇虛擬機符合篩選條件。")
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    color: root.secondaryText
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }

                    ScrollView {
                        id: machineDetailScroll
                        visible: !root.compact || root.inventoryDetailOpen
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: root.compactHeight ? 180 : 230
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: machineDetailScroll.availableWidth
                            spacing: DesignTokens.spacing12

                            WfButton {
                                visible: root.compact
                                Layout.alignment: Qt.AlignLeft
                                dark: root.dark
                                variant: "text"
                                glyph: "←"
                                text: root.tr("Back to machines", "返去虛擬機清單")
                                Accessible.name: text
                                onClicked: root.inventoryDetailOpen = false
                            }

                            WfCard {
                                Layout.fillWidth: true
                                visible: root.selectedVm() !== null
                                dark: root.dark
                                outlined: true
                                surfaceLevel: "lowest"
                                padding: DesignTokens.spacing16
                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: DesignTokens.spacing12
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: DesignTokens.spacing12
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label {
                                                Layout.fillWidth: true
                                                text: root.value(root.selectedVm(), "name", root.tr("Selected machine", "已選虛擬機"))
                                                color: root.surfaceForeground
                                                font.family: DesignTokens.fontDisplay
                                                font.pixelSize: 18
                                                font.weight: Font.Bold
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: root.providerName(root.value(root.selectedVm(), "providerId", ""))
                                                      + " · " + root.value(root.selectedVm(), "cpuCount", 0) + " vCPU"
                                                      + " · " + root.value(root.selectedVm(), "memoryMiB", 0) + " MiB"
                                                      + " · " + String(root.value(root.selectedVm(), "firmware", "")).toUpperCase()
                                                color: root.secondaryText
                                                font.family: DesignTokens.fontBody
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }
                                        }
                                        WfStatusChip {
                                            dark: root.dark
                                            tone: root.stateTone(root.selectedVm())
                                            showDot: true
                                            uppercase: false
                                            text: root.stateLabel(root.powerState(root.selectedVm()))
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: root.value(root.selectedVm(), "configPath", root.tr("Configuration path unavailable", "設定路徑不可用"))
                                        color: root.secondaryText
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 10
                                        wrapMode: Text.WrapAnywhere
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: root.value(root.selectedVm(), "inaccessibleReason", "").length > 0
                                        text: root.value(root.selectedVm(), "inaccessibleReason", "")
                                        color: root.errorText
                                        font.family: DesignTokens.fontBody
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: DesignTokens.spacing8
                                        WfButton {
                                            dark: root.dark; compact: true; variant: "filled"
                                            text: root.tr("Start", "啟動")
                                            Accessible.name: root.tr("Start selected virtual machine", "啟動已選虛擬機")
                                            enabled: !root.app.vmBusy && root.lifecycleAvailable() && root.allowsVmAction("start")
                                            onClicked: root.submitVmAction("start", { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true
                                            text: root.tr("Open console", "開啟主控台")
                                            Accessible.name: root.tr("Open selected virtual machine console", "開啟已選虛擬機主控台")
                                            enabled: !root.app.vmBusy && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "open-console")
                                            onClicked: root.submitVmAction("openConsole", { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true
                                            text: root.tr("Shut down", "關機")
                                            Accessible.name: root.tr("Request guest shutdown", "要求客體系統關機")
                                            enabled: !root.app.vmBusy && root.lifecycleAvailable() && root.allowsVmAction("shutdown")
                                            onClicked: root.submitVmAction("shutdown", { vmId: root.selectedVmId(), mode: "guest" })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true
                                            text: root.tr("Pause", "暫停")
                                            Accessible.name: root.tr("Pause selected virtual machine", "暫停已選虛擬機")
                                            enabled: !root.app.vmBusy && root.lifecycleAvailable() && root.allowsVmAction("pause")
                                            ToolTip.visible: hovered && root.ambiguousVmwareActive()
                                            ToolTip.text: root.tr("VMware inventory cannot distinguish running from paused. The reviewed provider may report that this VM is already paused.",
                                                                  "VMware 清單分辨唔到開緊定已暫停；已審閱供應器可能回報 VM 已經暫停。")
                                            onClicked: root.submitVmAction("pause", { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true
                                            text: root.tr("Resume", "繼續")
                                            Accessible.name: root.tr("Resume selected virtual machine", "繼續已選虛擬機")
                                            enabled: !root.app.vmBusy && root.lifecycleAvailable() && root.allowsVmAction("resume")
                                            ToolTip.visible: hovered && root.ambiguousVmwareActive()
                                            ToolTip.text: root.tr("VMware inventory cannot distinguish running from paused. Resume is offered so a paused active VM is not stranded.",
                                                                  "VMware 清單分辨唔到開緊定已暫停；提供繼續功能，避免已暫停 VM 返唔到去運作狀態。")
                                            onClicked: root.submitVmAction("resume", { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true
                                            text: root.tr("Save state", "儲存狀態")
                                            Accessible.name: root.tr("Save selected virtual machine state", "儲存已選虛擬機狀態")
                                            enabled: !root.app.vmBusy && root.lifecycleAvailable() && root.allowsVmAction("saveState")
                                            onClicked: root.submitVmAction("saveState", { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true
                                            text: root.tr("Reset", "重設")
                                            Accessible.name: root.tr("Reset selected virtual machine", "重設已選虛擬機")
                                            enabled: !root.app.vmBusy && root.lifecycleAvailable() && root.allowsVmAction("reset")
                                            onClicked: root.requestConfirmation("vm", "reset", root.tr("Reset virtual machine?", "重設虛擬機？"),
                                                                                root.tr("The guest loses unsaved work. The reviewed provider command runs only after confirmation.", "客體系統未儲存的工作會遺失。確認後才會執行已檢查的供應器命令。"),
                                                                                { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true; variant: "destructive"
                                            text: root.tr("Power off", "強制關機")
                                            Accessible.name: root.tr("Force power off selected virtual machine", "強制關閉已選虛擬機")
                                            enabled: !root.app.vmBusy && root.lifecycleAvailable() && root.allowsVmAction("powerOff")
                                            onClicked: root.requestConfirmation("vm", "powerOff", root.tr("Force power off?", "強制關機？"),
                                                                                root.tr("This is equivalent to pulling the power cable and can corrupt the guest filesystem.", "咁樣等同直接拔電源線，有機會整壞客體檔案系統。"),
                                                                                { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true; variant: "destructive"
                                            text: root.value(root.selectedVm(), "ownership", "") === "managed"
                                                  ? root.tr("Delete managed VM", "刪除受管理 VM")
                                                  : root.tr("Unregister", "取消註冊")
                                            Accessible.name: root.value(root.selectedVm(), "ownership", "") === "managed"
                                                             ? root.tr("Delete selected managed virtual machine", "刪除選取嘅受管理虛擬機")
                                                             : root.tr("Unregister selected external virtual machine", "取消註冊選取嘅外部虛擬機")
                                            enabled: !root.app.vmBusy && root.allowsVmAction("delete")
                                                     && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "delete")
                                            onClicked: root.requestConfirmation("vm", "delete", root.tr("Remove virtual machine?", "移除虛擬機？"),
                                                                                root.value(root.selectedVm(), "ownership", "") === "managed"
                                                                                    ? root.tr("Select permanent file deletion to continue. The next screen shows the immutable provider commands and exact typed token.", "選取永久刪除檔案以繼續。下一個畫面會顯示不可變更嘅供應器命令同完整輸入字句。")
                                                                                    : root.tr("Provider registration will be removed while every provider file is preserved. The next screen shows the exact command before execution.", "供應器註冊會被移除，但所有供應器檔案都會保留。下一個畫面會喺執行前顯示完整命令。"),
                                                                                { vmId: root.selectedVmId() })
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true; variant: "text"
                                            visible: root.allowsVmAction("forget")
                                            text: root.tr("Forget in WimForge", "喺 WimForge 忘記")
                                            Accessible.name: root.tr("Forget external virtual machine in WimForge without changing provider files or registration", "喺 WimForge 忘記外部虛擬機而唔變更供應器檔案或註冊")
                                            enabled: !root.app.vmBusy && root.allowsVmAction("forget")
                                            onClicked: root.requestConfirmation("vm", "forget", root.tr("Forget this catalog entry?", "忘記呢個目錄項目？"),
                                                                                root.tr("Only WimForge's project catalog entry is removed. Provider registration and every VM file remain untouched.", "只會移除 WimForge 工程目錄項目。供應器註冊同所有 VM 檔案都唔會改動。"),
                                                                                { vmId: root.selectedVmId() })
                                        }
                                    }
                                }
                            }

                            WfCard {
                                Layout.fillWidth: true
                                visible: root.selectedVm() !== null
                                dark: root.dark
                                outlined: true
                                surfaceLevel: "lowest"
                                padding: DesignTokens.spacing16
                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: DesignTokens.spacing12
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            Layout.fillWidth: true
                                            text: root.tr("Configuration", "設定")
                                            color: root.surfaceForeground
                                            font.family: DesignTokens.fontDisplay
                                            font.pixelSize: 15
                                            font.weight: Font.Bold
                                        }
                                        WfButton {
                                            dark: root.dark; compact: true; variant: "text"
                                            text: root.tr("Edit hardware", "編輯硬件")
                                            onClicked: sectionTabs.currentIndex = 2
                                        }
                                    }
                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: width >= 560 ? 4 : 2
                                        columnSpacing: DesignTokens.spacing12
                                        rowSpacing: DesignTokens.spacing8
                                        ColumnLayout { Label { text: root.tr("vCPU", "vCPU"); color: root.secondaryText; font.pixelSize: 10 } Label { text: root.value(root.selectedVm(), "cpuCount", "—"); color: root.surfaceForeground; font.family: DesignTokens.fontMono; font.pixelSize: 13 } }
                                        ColumnLayout { Label { text: root.tr("Memory", "記憶體"); color: root.secondaryText; font.pixelSize: 10 } Label { text: root.value(root.selectedVm(), "memoryMiB", "—") + " MiB"; color: root.surfaceForeground; font.family: DesignTokens.fontMono; font.pixelSize: 13 } }
                                        ColumnLayout { Label { text: root.tr("Firmware", "韌體"); color: root.secondaryText; font.pixelSize: 10 } Label { text: String(root.value(root.selectedVm(), "firmware", "—")).toUpperCase(); color: root.surfaceForeground; font.family: DesignTokens.fontBody; font.pixelSize: 13 } }
                                        ColumnLayout { Label { text: root.tr("TPM", "TPM"); color: root.secondaryText; font.pixelSize: 10 } Label { text: root.value(root.selectedVm(), "tpm", false) ? root.tr("Enabled", "已啟用") : root.tr("Disabled", "已停用"); color: root.surfaceForeground; font.family: DesignTokens.fontBody; font.pixelSize: 13 } }
                                    }
                                }
                            }

                            WfCard {
                                Layout.fillWidth: true
                                visible: root.selectedVm() !== null
                                dark: root.dark
                                outlined: true
                                surfaceLevel: "lowest"
                                padding: DesignTokens.spacing16
                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: DesignTokens.spacing8
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { Layout.fillWidth: true; text: root.tr("Snapshots", "快照"); color: root.surfaceForeground; font.family: DesignTokens.fontDisplay; font.pixelSize: 15; font.weight: Font.Bold }
                                        Label { text: root.list(root.app.vmSnapshots).length; color: root.secondaryText; font.family: DesignTokens.fontMono; font.pixelSize: 11 }
                                        WfButton { dark: root.dark; compact: true; variant: "text"; text: root.tr("Manage", "管理"); onClicked: sectionTabs.currentIndex = 3 }
                                    }
                                    Repeater {
                                        model: root.list(root.app.vmSnapshots)
                                        delegate: RowLayout {
                                            required property var modelData
                                            required property int index
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: index < 3 ? DesignTokens.rowHeight : 0
                                            visible: index < 3
                                            spacing: DesignTokens.spacing8
                                            Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4; color: root.value(modelData, "current", false) ? root.successText : DesignTokens.outline(root.dark) }
                                            Label { Layout.fillWidth: true; text: root.value(modelData, "name", root.value(modelData, "id", "")); color: root.surfaceForeground; font.family: DesignTokens.fontBody; font.pixelSize: 12; elide: Text.ElideRight }
                                            Label { text: root.value(modelData, "createdAt", root.value(modelData, "description", "")); color: root.secondaryText; font.family: DesignTokens.fontMono; font.pixelSize: 10; elide: Text.ElideRight; Layout.maximumWidth: 180 }
                                        }
                                    }
                                    Label { Layout.fillWidth: true; visible: root.list(root.app.vmSnapshots).length === 0; text: root.tr("No snapshots reported.", "未回報快照。"); color: root.secondaryText; font.family: DesignTokens.fontBody; font.pixelSize: 11 }
                                }
                            }

                            WfCard {
                                Layout.fillWidth: true
                                visible: root.selectedVm() !== null
                                dark: root.dark
                                outlined: true
                                surfaceLevel: "lowest"
                                padding: DesignTokens.spacing16
                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: DesignTokens.spacing8
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { Layout.fillWidth: true; text: root.tr("Validation runs", "驗證執行"); color: root.surfaceForeground; font.family: DesignTokens.fontDisplay; font.pixelSize: 15; font.weight: Font.Bold }
                                        Label { text: root.list(root.app.vmValidationRuns).length; color: root.secondaryText; font.family: DesignTokens.fontMono; font.pixelSize: 11 }
                                        WfButton { dark: root.dark; compact: true; variant: "text"; text: root.tr("Open evidence", "開啟證據"); onClicked: sectionTabs.currentIndex = 4 }
                                    }
                                    Repeater {
                                        model: root.list(root.app.vmValidationRuns)
                                        delegate: RowLayout {
                                            required property var modelData
                                            required property int index
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: index < 3 ? DesignTokens.rowHeight : 0
                                            visible: index < 3
                                            spacing: DesignTokens.spacing8
                                            Rectangle {
                                                Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                                                color: root.value(modelData, "result", "") === "pass" ? root.successText
                                                     : root.value(modelData, "result", "") === "fail" ? root.errorText
                                                     : DesignTokens.secondary(root.dark)
                                            }
                                            Label { Layout.fillWidth: true; text: root.value(modelData, "name", root.tr("Validation run", "驗證執行")); color: root.surfaceForeground; font.family: DesignTokens.fontBody; font.pixelSize: 12; elide: Text.ElideRight }
                                            Label { text: root.value(modelData, "result", root.value(modelData, "status", "running")); color: root.secondaryText; font.family: DesignTokens.fontBody; font.pixelSize: 10 }
                                        }
                                    }
                                    Label { Layout.fillWidth: true; visible: root.list(root.app.vmValidationRuns).length === 0; text: root.tr("No validation evidence recorded.", "未記錄驗證證據。"); color: root.secondaryText; font.family: DesignTokens.fontBody; font.pixelSize: 11 }
                                }
                            }

                            WfCard {
                                Layout.fillWidth: true
                                visible: root.selectedVm() !== null
                                dark: root.dark
                                outlined: true
                                surfaceLevel: "low"
                                padding: DesignTokens.spacing12
                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: DesignTokens.spacing8
                                    Label { Layout.fillWidth: true; text: root.tr("Provider operation log", "供應器操作記錄"); color: root.surfaceForeground; font.family: DesignTokens.fontDisplay; font.pixelSize: 13; font.weight: Font.Bold }
                                    TextArea {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 86
                                        readOnly: true
                                        selectByMouse: true
                                        wrapMode: TextEdit.WrapAnywhere
                                        font.family: DesignTokens.fontMono
                                        font.pixelSize: 10
                                        Accessible.name: root.tr("Virtual machine operation log", "虛擬機操作記錄")
                                        text: root.value(root.selectedVm(), "consoleLog", root.statusMessage())
                                    }
                                }
                            }

                            WfCard {
                                Layout.fillWidth: true
                                visible: root.selectedVm() === null
                                dark: root.dark
                                outlined: true
                                surfaceLevel: "low"
                                padding: DesignTokens.spacing24
                                Label {
                                    anchors.fill: parent
                                    text: root.tr("Choose a machine to manage power, inspect configuration, review snapshots, or record validation evidence.",
                                                  "選取虛擬機以管理電源、檢查設定、審閱快照或記錄驗證證據。")
                                    color: root.secondaryText
                                    font.family: DesignTokens.fontBody
                                    font.pixelSize: 13
                                    wrapMode: Text.Wrap
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                        }
                    }
                }
            }

            ScrollView {
                id: createScroll
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: createScroll.availableWidth
                    spacing: DesignTokens.spacing12

                    WfCard {
                        Layout.fillWidth: true
                        dark: root.dark
                        outlined: true
                        fillColor: DesignTokens.primaryContainer(root.dark)
                        outlineColor: DesignTokens.primary(root.dark)
                        padding: DesignTokens.spacing16
                        GridLayout {
                            width: parent.width
                            columns: width >= 650 ? 3 : 1
                            columnSpacing: 8
                            rowSpacing: 6
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { Layout.fillWidth: true; text: root.tr("Current WimForge output", "而家嘅 WimForge 輸出"); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                                Label {
                                    Layout.fillWidth: true
                                    text: root.app.currentOutput || root.tr("Build an ISO first, or enter another ISO below.", "請先建立 ISO，或在下方輸入其他 ISO。")
                                    wrapMode: Text.WrapAnywhere
                                    font.pixelSize: 10
                                }
                            }
                            WfButton {
                                Layout.fillWidth: createScroll.availableWidth < 650
                                dark: root.dark
                                variant: "outlined"
                                text: root.tr("Use current output", "用而家嘅輸出")
                                Accessible.name: root.tr("Use the current WimForge output ISO", "用而家嘅 WimForge 輸出 ISO")
                                enabled: String(root.app.currentOutput || "").length > 0
                                onClicked: sourceIso.text = root.app.currentOutput
                            }
                            WfButton {
                                Layout.fillWidth: createScroll.availableWidth < 650
                                dark: root.dark
                                variant: "tonal"
                                text: root.tr("Refresh detection", "重新偵測")
                                Accessible.name: root.tr("Refresh VM providers before creating", "建立前重新偵測虛擬機供應器")
                                enabled: !root.app.vmBusy
                                onClicked: root.refreshLab()
                            }
                        }
                    }

                    VmSectionCard {
                        Layout.fillWidth: true
                        titleText: root.tr("Machine and installation media", "虛擬機及安裝媒體")
                        GridLayout {
                            width: parent.width
                            columns: width >= 720 ? 2 : 1
                            columnSpacing: 10
                            rowSpacing: 8

                            ComboBox {
                                id: createProvider
                                Layout.fillWidth: true
                                model: root.providerIds(true, "create")
                                displayText: root.providerName(currentText)
                                Accessible.name: root.tr("Virtual machine provider", "虛擬機供應器")
                                delegate: ItemDelegate {
                                    required property var modelData
                                    width: createProvider.width
                                    text: root.providerName(modelData)
                                }
                            }
                            TextField {
                                id: createName
                                Layout.fillWidth: true
                                placeholderText: root.tr("Machine name", "虛擬機名稱")
                                Accessible.name: root.tr("New virtual machine name", "新虛擬機名稱")
                                selectByMouse: true
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                TextField {
                                    id: createDirectory
                                    Layout.fillWidth: true
                                    readOnly: true
                                    text: {
                                        var base = root.value(root.app.vmStatus, "managedRoot", "")
                                        if (createProvider.currentText === "virtualbox" || createName.text.trim().length === 0)
                                            return base
                                        return base + "/" + createName.text.trim()
                                    }
                                    placeholderText: root.tr("Safe managed machine directory", "安全受管理虛擬機目錄")
                                    Accessible.name: root.tr("New virtual machine directory", "新虛擬機目錄")
                                    selectByMouse: true
                                }
                            }
                            TextField {
                                id: sourceIso
                                Layout.fillWidth: true
                                text: root.app.currentOutput || ""
                                placeholderText: root.tr("Absolute path to Windows ISO", "Windows ISO 絕對路徑")
                                Accessible.name: root.tr("Installation ISO path", "安裝 ISO 路徑")
                                selectByMouse: true
                            }
                            ComboBox {
                                id: guestType
                                Layout.fillWidth: true
                                model: ["Windows 11 x64", "Windows 10 x64", "Windows Server 2025 x64", "Windows Server 2022 x64", "Other Windows x64"]
                                Accessible.name: root.tr("Guest operating system type", "客體作業系統類型")
                            }
                            ComboBox {
                                id: firmware
                                Layout.fillWidth: true
                                model: ["efi", "bios"]
                                displayText: currentText === "efi" ? root.tr("UEFI", "UEFI") : root.tr("Legacy BIOS", "傳統 BIOS")
                                Accessible.name: root.tr("Firmware type", "韌體類型")
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.columnSpan: parent.columns
                                text: root.tr("Managed VM binaries stay in WimForge application data, outside the Git project. VirtualBox receives this base folder; VMware receives a new per-machine child folder.",
                                              "受管理 VM 檔案留喺 Git 工程之外嘅 WimForge 應用程式資料。VirtualBox 使用呢個 base folder；VMware 使用新嘅獨立虛擬機子目錄。")
                                wrapMode: Text.Wrap
                                color: root.secondaryText
                                font.pixelSize: 10
                            }
                        }
                    }

                    VmSectionCard {
                        Layout.fillWidth: true
                        titleText: root.tr("Load an existing provider machine", "載入現有供應器虛擬機")
                        GridLayout {
                            width: parent.width
                            columns: width >= 760 ? 4 : width >= 500 ? 2 : 1
                            columnSpacing: 8
                            rowSpacing: 8
                            ComboBox {
                                id: importProvider
                                Layout.fillWidth: true
                                model: root.importProviderIds()
                                displayText: root.providerName(currentText)
                                Accessible.name: root.tr("Existing machine provider", "現有虛擬機供應器")
                                delegate: ItemDelegate {
                                    required property var modelData
                                    width: importProvider.width
                                    text: root.providerName(modelData)
                                }
                            }
                            TextField {
                                id: importConfigurationPath
                                Layout.fillWidth: true
                                placeholderText: root.tr("Absolute .vmx or .vbox path", ".vmx 或 .vbox 絕對路徑")
                                Accessible.name: root.tr("Existing VM configuration path", "現有 VM 設定路徑")
                                selectByMouse: true
                            }
                            TextField {
                                id: importDisplayName
                                Layout.fillWidth: true
                                placeholderText: root.tr("Optional display name", "選填顯示名稱")
                                Accessible.name: root.tr("Imported VM display name", "匯入 VM 顯示名稱")
                                selectByMouse: true
                            }
                            WfButton {
                                Layout.fillWidth: true
                                dark: root.dark
                                variant: "outlined"
                                text: root.tr("Load machine", "載入虛擬機")
                                Accessible.name: root.tr("Load existing VMware or VirtualBox machine", "載入現有 VMware 或 VirtualBox 虛擬機")
                                enabled: !root.app.vmBusy && importProvider.count > 0
                                         && importConfigurationPath.text.trim().length > 0
                                onClicked: root.submitVmAction("register", {
                                                                  providerId: importProvider.currentText,
                                                                  path: importConfigurationPath.text.trim(),
                                                                  name: importDisplayName.text.trim()
                                                              })
                            }
                        }
                    }

                    VmSectionCard {
                        Layout.fillWidth: true
                        titleText: root.tr("Security and hardware", "安全性及硬件")
                        GridLayout {
                            width: parent.width
                            columns: width >= 760 ? 3 : width >= 470 ? 2 : 1
                            columnSpacing: 10
                            rowSpacing: 8

                            CheckBox {
                                id: secureBoot
                                Layout.fillWidth: true
                                text: root.tr("Secure Boot", "安全開機")
                                Accessible.name: text
                                checked: true
                                enabled: root.providerSupports(createProvider.currentText, "secure-boot")
                            }
                            CheckBox {
                                id: tpmEnabled
                                Layout.fillWidth: true
                                text: root.tr("Virtual TPM 2.0", "虛擬 TPM 2.0")
                                Accessible.name: text
                                checked: true
                                enabled: root.providerSupports(createProvider.currentText, "tpm")
                            }
                            CheckBox {
                                id: unattendedBoot
                                Layout.fillWidth: true
                                text: root.tr("Unattended first boot", "無人值守首次開機")
                                Accessible.name: text
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { text: root.tr("Processors", "處理器"); font.pixelSize: 10; color: root.secondaryText }
                                SpinBox {
                                    id: cpuCount
                                    Layout.fillWidth: true
                                    from: 1
                                    to: 64
                                    value: 4
                                    editable: true
                                    Accessible.name: root.tr("Virtual processor count", "虛擬處理器數量")
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { text: root.tr("Memory (MiB)", "記憶體（MiB）"); font.pixelSize: 10; color: root.secondaryText }
                                SpinBox {
                                    id: memoryMiB
                                    Layout.fillWidth: true
                                    from: 1024
                                    to: 262144
                                    stepSize: 1024
                                    value: 8192
                                    editable: true
                                    Accessible.name: root.tr("Virtual machine memory in MiB", "虛擬機記憶體 MiB")
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { text: root.tr("System disk (GiB)", "系統磁碟（GiB）"); font.pixelSize: 10; color: root.secondaryText }
                                SpinBox {
                                    id: diskGiB
                                    Layout.fillWidth: true
                                    from: 32
                                    to: 2048
                                    value: 80
                                    editable: true
                                    Accessible.name: root.tr("Virtual disk size in GiB", "虛擬磁碟大小 GiB")
                                }
                            }
                        }
                    }

                    VmSectionCard {
                        Layout.fillWidth: true
                        titleText: root.tr("Network and launch", "網絡及啟動")
                        GridLayout {
                            width: parent.width
                            columns: width >= 720 ? 3 : 1
                            columnSpacing: 10
                            rowSpacing: 8
                            ComboBox {
                                id: networkMode
                                Layout.fillWidth: true
                                model: createProvider.currentText.indexOf("vmware-") === 0
                                       ? ["nat", "bridged", "host-only", "disconnected"]
                                       : ["nat", "bridged", "host-only", "internal", "disconnected"]
                                Accessible.name: root.tr("Initial network mode", "初始網絡模式")
                            }
                            TextField {
                                id: bridgeName
                                Layout.fillWidth: true
                                enabled: createProvider.currentText === "virtualbox"
                                         && ["bridged", "host-only", "internal"].indexOf(networkMode.currentText) >= 0
                                placeholderText: root.tr("Host interface or internal network name", "主機介面或內部網絡名稱")
                                Accessible.name: root.tr("VirtualBox host interface or network name", "VirtualBox 主機介面或網絡名稱")
                            }
                            CheckBox {
                                id: bootAfterCreate
                                Layout.fillWidth: true
                                checked: false
                                text: root.tr("Boot after creation", "建立後啟動")
                                Accessible.name: text
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: createProvider.count === 0
                        text: root.tr("No creation-capable provider is available. Install or repair VMware/VirtualBox, then refresh detection.",
                                             "冇可用嚟建立虛擬機嘅供應器。請安裝或修復 VMware／VirtualBox，之後再偵測。")
                        wrapMode: Text.Wrap
                        color: root.warningText
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !secureBoot.enabled && secureBoot.checked
                        text: root.tr("The detected provider/version has not proven Secure Boot support; it will be omitted unless the provider capability is available.",
                                             "偵測到的供應器／版本未證實支援安全開機；除非能力可用，否則會省略。")
                        wrapMode: Text.Wrap
                        color: root.warningText
                    }
                    WfButton {
                        Layout.fillWidth: createScroll.availableWidth < 560
                        Layout.alignment: Qt.AlignRight
                        dark: root.dark
                        variant: "filled"
                        text: root.tr("Create VM and load ISO", "建立虛擬機並載入 ISO")
                        Accessible.name: root.tr("Create virtual machine and load installation ISO", "建立虛擬機並載入安裝 ISO")
                        enabled: !root.app.vmBusy && createProvider.count > 0
                                 && createName.text.trim().length > 0
                                 && createDirectory.text.trim().length > 0
                                 && sourceIso.text.trim().length > 0
                                 && (!bridgeName.enabled || bridgeName.text.trim().length > 0)
                        onClicked: root.submitCreate(root.createSpec())
                    }
                }
            }

            ScrollView {
                id: hardwareScroll
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: hardwareScroll.availableWidth
                    spacing: DesignTokens.spacing12

                    WfCard {
                        Layout.fillWidth: true
                        dark: root.dark
                        outlined: true
                        fillColor: root.selectedVm() && root.isPoweredOff(root.selectedVm())
                                   ? DesignTokens.successContainer(root.dark)
                                   : DesignTokens.tertiaryContainer(root.dark)
                        outlineColor: root.selectedVm() && root.isPoweredOff(root.selectedVm())
                                      ? root.successText : root.warningText
                        padding: DesignTokens.spacing12
                        GridLayout {
                            width: parent.width
                            columns: width >= 680 ? 2 : 1
                            Label {
                                Layout.fillWidth: true
                                text: root.selectedVm()
                                      ? root.value(root.selectedVm(), "name", "") + " · " + root.stateLabel(root.powerState(root.selectedVm()))
                                      : root.tr("Select a machine from Inventory first.", "請先從清單選取虛擬機。")
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Hardware edits require a refreshed, powered-off machine.", "硬件變更需要已重新整理並關機的虛擬機。")
                                horizontalAlignment: width >= 680 ? Text.AlignRight : Text.AlignLeft
                                wrapMode: Text.Wrap
                                font.pixelSize: 10
                            }
                        }
                    }

                    VmSectionCard {
                        Layout.fillWidth: true
                        enabled: root.selectedVm() !== null
                        titleText: root.tr("Powered-off machine settings", "關機狀態設定")
                        GridLayout {
                            width: parent.width
                            columns: width >= 760 ? 3 : width >= 480 ? 2 : 1
                            columnSpacing: 8
                            rowSpacing: 8

                            ColumnLayout {
                                Layout.fillWidth: true
                                CheckBox {
                                    id: changeCpu
                                    text: root.tr("Change processors", "變更處理器")
                                    enabled: root.hasValue(root.selectedVm(), "cpuCount")
                                    Accessible.name: text
                                }
                                SpinBox {
                                    id: configCpu
                                    Layout.fillWidth: true
                                    enabled: changeCpu.checked
                                    from: 1; to: 64
                                    value: root.value(root.selectedVm(), "cpuCount", 2)
                                    editable: true
                                    Accessible.name: root.tr("Requested virtual processor count", "要求嘅虛擬處理器數量")
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                CheckBox {
                                    id: changeMemory
                                    text: root.tr("Change memory (MiB)", "變更記憶體（MiB）")
                                    enabled: root.hasValue(root.selectedVm(), "memoryMiB")
                                    Accessible.name: text
                                }
                                SpinBox {
                                    id: configMemory
                                    Layout.fillWidth: true
                                    enabled: changeMemory.checked
                                    from: 1024; to: 262144; stepSize: 1024
                                    value: root.value(root.selectedVm(), "memoryMiB", 4096)
                                    editable: true
                                    Accessible.name: root.tr("Requested virtual machine memory", "要求嘅虛擬機記憶體")
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                CheckBox {
                                    id: changeFirmware
                                    text: root.tr("Change firmware", "變更韌體")
                                    enabled: root.hasValue(root.selectedVm(), "firmware")
                                    Accessible.name: text
                                }
                                ComboBox {
                                    id: configFirmware
                                    Layout.fillWidth: true
                                    enabled: changeFirmware.checked
                                    model: ["efi", "bios"]
                                    currentIndex: root.value(root.selectedVm(), "firmware", "efi") === "bios" ? 1 : 0
                                    Accessible.name: root.tr("Requested firmware", "要求嘅韌體")
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                CheckBox {
                                    id: changeSecureBoot
                                    text: root.tr("Change Secure Boot", "變更安全開機")
                                    enabled: root.hasValue(root.selectedVm(), "secureBoot")
                                             && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "secure-boot")
                                    Accessible.name: text
                                }
                                CheckBox {
                                    id: configSecureBoot
                                    Layout.fillWidth: true
                                    enabled: changeSecureBoot.checked
                                             && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "secure-boot")
                                    text: root.tr("Enable Secure Boot", "啟用安全開機")
                                    checked: Boolean(root.value(root.selectedVm(), "secureBoot", false))
                                    Accessible.name: text
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                CheckBox {
                                    id: changeTpm
                                    text: root.tr("Change virtual TPM", "變更虛擬 TPM")
                                    enabled: root.hasValue(root.selectedVm(), "tpm")
                                             && root.value(root.selectedVm(), "providerId", "") === "virtualbox"
                                             && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "tpm")
                                    Accessible.name: text
                                }
                                CheckBox {
                                    id: configTpm
                                    Layout.fillWidth: true
                                    enabled: changeTpm.checked
                                             && root.value(root.selectedVm(), "providerId", "") === "virtualbox"
                                             && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "tpm")
                                    text: root.tr("Enable virtual TPM 2.0", "啟用虛擬 TPM 2.0")
                                    checked: Boolean(root.value(root.selectedVm(), "tpm", false))
                                    Accessible.name: text
                                }
                            }
                            WfButton {
                                Layout.fillWidth: true
                                dark: root.dark
                                variant: "filled"
                                text: root.tr("Apply settings", "套用設定")
                                Accessible.name: root.tr("Apply powered-off virtual machine settings", "套用關機狀態虛擬機設定")
                                enabled: !root.app.vmBusy && root.selectedVm() !== null
                                         && root.allowsVmAction("configure")
                                         && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "configure")
                                         && ((changeCpu.enabled && changeCpu.checked)
                                             || (changeMemory.enabled && changeMemory.checked)
                                             || (changeFirmware.enabled && changeFirmware.checked)
                                             || (changeSecureBoot.enabled && changeSecureBoot.checked)
                                             || (changeTpm.enabled && changeTpm.checked))
                                onClicked: root.submitConfiguration(root.configurationSpec())
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.columnSpan: parent.columns
                                text: root.tr("Only values proven by refreshed provider inventory can be selected. Check only the fields you intend to replace.",
                                              "只有重新整理供應器清單證實嘅值先可以選擇。只剔選你確實想取代嘅欄位。")
                                wrapMode: Text.Wrap
                                color: root.warningText
                                font.pixelSize: 10
                            }
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: hardwareScroll.availableWidth >= 780 ? 2 : 1
                        columnSpacing: 10
                        rowSpacing: 10

                        VmSectionCard {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            enabled: root.selectedVm() !== null
                            titleText: root.tr("Storage topology", "儲存拓撲")
                            ColumnLayout {
                                width: parent.width
                                Repeater {
                                    model: root.list(root.value(root.selectedVm(), "storageDevices", root.value(root.selectedVm(), "storagePaths", [])))
                                    delegate: WfCard {
                                        id: storageCard
                                        required property var modelData
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        outlined: true
                                        surfaceLevel: "low"
                                        padding: DesignTokens.spacing8
                                        GridLayout {
                                            width: parent.width
                                            columns: width >= 420 ? 3 : 1
                                            Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4; color: DesignTokens.secondary(root.dark) }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: typeof storageCard.modelData === "string" ? storageCard.modelData
                                                          : root.value(storageCard.modelData, "name", root.value(storageCard.modelData, "path", root.tr("Virtual disk", "虛擬磁碟")))
                                                    wrapMode: Text.WrapAnywhere
                                                    font.weight: Font.DemiBold
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: root.value(storageCard.modelData, "bus", "") + "  "
                                                          + root.value(storageCard.modelData, "controllerName", "") + "  "
                                                          + root.value(storageCard.modelData, "controller", 0) + ":"
                                                          + root.value(storageCard.modelData, "port", 0) + ":"
                                                          + root.value(storageCard.modelData, "device", 0)
                                                    wrapMode: Text.Wrap
                                                    font.pixelSize: 10
                                                    color: root.secondaryText
                                                }
                                            }
                                            WfButton {
                                                dark: root.dark
                                                compact: true
                                                variant: "text"
                                                text: root.tr("Detach", "卸載")
                                                visible: typeof storageCard.modelData !== "string"
                                                enabled: root.isPoweredOff(root.selectedVm()) && !root.app.vmBusy
                                                         && root.value(storageCard.modelData, "bus", "").length > 0
                                                         && root.value(storageCard.modelData, "port", -1) >= 0
                                                         && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "media")
                                                Accessible.name: root.tr("Detach virtual disk", "卸載虛擬磁碟")
                                                ToolTip.visible: hovered
                                                ToolTip.text: Accessible.name
                                                onClicked: root.requestConfirmation("device", "detachDisk", root.tr("Detach virtual disk?", "卸載虛擬磁碟？"),
                                                                                    root.tr("The disk is detached from the machine. The backing file is preserved unless the reviewed operation explicitly says otherwise.", "磁碟會從虛擬機卸載。除非已檢查的操作明確指出，否則保留後端檔案。"),
                                                                                    { vmId: root.selectedVmId(),
                                                                                      bus: root.value(storageCard.modelData, "bus", ""),
                                                                                      controller: root.value(storageCard.modelData, "controller", 0),
                                                                                      controllerName: root.value(storageCard.modelData, "controllerName", ""),
                                                                                      port: root.value(storageCard.modelData, "port", -1),
                                                                                      device: root.value(storageCard.modelData, "device", 0),
                                                                                      path: root.value(storageCard.modelData, "path", "") })
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: root.list(root.value(root.selectedVm(), "storageDevices", root.value(root.selectedVm(), "storagePaths", []))).length === 0
                                    text: root.tr("No storage devices reported.", "未回報儲存裝置。")
                                    color: root.secondaryText
                                    wrapMode: Text.Wrap
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: width >= 520 ? 3 : 1
                                    ComboBox {
                                        id: newDiskBus
                                        Layout.fillWidth: true
                                        model: ["sata", "nvme", "scsi"]
                                        Accessible.name: root.tr("Disk storage bus", "磁碟儲存 bus")
                                    }
                                    SpinBox {
                                        id: newDiskControllerIndex
                                        Layout.fillWidth: true
                                        from: 0; to: 3; value: 0
                                        Accessible.name: root.tr("Disk controller index", "磁碟 controller index")
                                    }
                                    TextField {
                                        id: newDiskControllerName
                                        Layout.fillWidth: true
                                        enabled: root.value(root.selectedVm(), "providerId", "") === "virtualbox"
                                        text: newDiskBus.currentText === "ide" ? "IDE"
                                              : newDiskBus.currentText === "scsi" ? "SCSI"
                                              : newDiskBus.currentText === "nvme" ? "NVMe" : "SATA"
                                        placeholderText: root.tr("Exact VirtualBox controller name", "確切 VirtualBox controller 名稱")
                                        Accessible.name: root.tr("Provider storage controller name", "供應器儲存 controller 名稱")
                                        selectByMouse: true
                                    }
                                    SpinBox {
                                        id: newDiskSlot
                                        Layout.fillWidth: true
                                        from: 0; to: 29; value: 1
                                        Accessible.name: root.tr("Disk bus port", "磁碟 bus port")
                                    }
                                    TextField {
                                        id: newDiskPath
                                        Layout.fillWidth: true
                                        placeholderText: root.tr("Existing absolute .vdi/.vmdk path", "現有 .vdi/.vmdk 絕對路徑")
                                        Accessible.name: root.tr("Existing virtual disk path", "現有虛擬磁碟路徑")
                                        selectByMouse: true
                                    }
                                    WfButton {
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        compact: true
                                        variant: "outlined"
                                        text: root.tr("Attach disk", "掛載磁碟")
                                        Accessible.name: root.tr("Attach existing virtual disk", "掛載現有虛擬磁碟")
                                        enabled: root.isPoweredOff(root.selectedVm()) && !root.app.vmBusy
                                                 && newDiskPath.text.trim().length > 0
                                                 && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "media")
                                        onClicked: root.submitDeviceAction("addDisk", {
                                                                              vmId: root.selectedVmId(),
                                                                              bus: newDiskBus.currentText,
                                                                              controller: newDiskControllerIndex.value,
                                                                              controllerName: newDiskControllerName.enabled ? newDiskControllerName.text.trim() : "",
                                                                              port: newDiskSlot.value,
                                                                              device: 0,
                                                                              path: newDiskPath.text.trim()
                                                                          })
                                    }
                                }
                            }
                        }

                        VmSectionCard {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            enabled: root.selectedVm() !== null
                            titleText: root.tr("Optical drives and ISO media", "光碟機及 ISO 媒體")
                            ColumnLayout {
                                width: parent.width
                                Repeater {
                                    model: root.list(root.value(root.selectedVm(), "opticalDevices", []))
                                    delegate: WfCard {
                                        id: opticalCard
                                        required property var modelData
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        outlined: true
                                        surfaceLevel: "low"
                                        padding: DesignTokens.spacing8
                                        GridLayout {
                                            width: parent.width
                                            columns: width >= 420 ? 3 : 1
                                            Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4; color: DesignTokens.secondary(root.dark) }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                Label { Layout.fillWidth: true; text: root.value(opticalCard.modelData, "name", root.tr("Optical drive", "光碟機")); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                                                Label { Layout.fillWidth: true; text: root.value(opticalCard.modelData, "isoPath", root.tr("Empty", "空白")); font.pixelSize: 10; color: root.secondaryText; wrapMode: Text.WrapAnywhere }
                                            }
                                            WfButton {
                                                dark: root.dark
                                                compact: true
                                                variant: "text"
                                                text: root.tr("Eject", "退出")
                                                enabled: root.value(opticalCard.modelData, "isoPath", "").length > 0
                                                         && root.value(opticalCard.modelData, "bus", "").length > 0
                                                         && root.value(opticalCard.modelData, "port", -1) >= 0
                                                         && !root.app.vmBusy
                                                         && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "media")
                                                Accessible.name: root.tr("Eject ISO from optical drive", "從光碟機退出 ISO")
                                                ToolTip.visible: hovered
                                                ToolTip.text: Accessible.name
                                                onClicked: root.submitDeviceAction("ejectIso", {
                                                                                      vmId: root.selectedVmId(),
                                                                                      bus: root.value(opticalCard.modelData, "bus", ""),
                                                                                      controller: root.value(opticalCard.modelData, "controller", 0),
                                                                                      controllerName: root.value(opticalCard.modelData, "controllerName", ""),
                                                                                      port: root.value(opticalCard.modelData, "port", -1),
                                                                                      device: root.value(opticalCard.modelData, "device", 0),
                                                                                      path: root.value(opticalCard.modelData, "isoPath", "")
                                                                                  })
                                            }
                                        }
                                    }
                                }
                                TextField {
                                    id: attachIsoPath
                                    Layout.fillWidth: true
                                    text: root.app.currentOutput || ""
                                    placeholderText: root.tr("ISO path", "ISO 路徑")
                                    Accessible.name: root.tr("ISO path to attach", "要掛載的 ISO 路徑")
                                    selectByMouse: true
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    ComboBox {
                                        id: opticalBus
                                        Layout.fillWidth: true
                                        model: ["sata", "ide"]
                                        Accessible.name: root.tr("Optical storage bus", "光碟機儲存 bus")
                                    }
                                    TextField {
                                        id: opticalControllerName
                                        Layout.fillWidth: true
                                        enabled: root.value(root.selectedVm(), "providerId", "") === "virtualbox"
                                        text: opticalBus.currentText === "ide" ? "IDE" : "SATA"
                                        placeholderText: root.tr("Exact VirtualBox controller name", "確切 VirtualBox controller 名稱")
                                        Accessible.name: root.tr("Optical controller name", "光碟機 controller 名稱")
                                        selectByMouse: true
                                    }
                                    SpinBox {
                                        id: opticalController
                                        Layout.fillWidth: true
                                        from: 0; to: 3; value: 0
                                        Accessible.name: root.tr("Optical controller index", "光碟機 controller index")
                                    }
                                    SpinBox {
                                        id: opticalPort
                                        Layout.fillWidth: true
                                        from: 0; to: opticalBus.currentText === "ide" ? 1 : 29; value: 1
                                        Accessible.name: root.tr("Optical port", "光碟機 port")
                                    }
                                    SpinBox {
                                        id: opticalDevice
                                        Layout.fillWidth: true
                                        from: 0; to: opticalBus.currentText === "ide" ? 1 : 0; value: 0
                                        Accessible.name: root.tr("Optical device index", "光碟機 device index")
                                    }
                                    WfButton {
                                        dark: root.dark
                                        compact: true
                                        variant: "outlined"
                                        text: root.tr("Attach ISO", "掛載 ISO")
                                        Accessible.name: root.tr("Attach ISO to selected virtual machine", "掛載 ISO 到已選虛擬機")
                                        enabled: root.isPoweredOff(root.selectedVm())
                                                 && attachIsoPath.text.trim().length > 0 && !root.app.vmBusy
                                                 && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "media")
                                        onClicked: root.submitDeviceAction("attachIso", {
                                                                              vmId: root.selectedVmId(),
                                                                              bus: opticalBus.currentText,
                                                                              controller: opticalController.value,
                                                                              controllerName: opticalControllerName.enabled ? opticalControllerName.text.trim() : "",
                                                                              port: opticalPort.value,
                                                                              device: opticalDevice.value,
                                                                              isoPath: attachIsoPath.text.trim()
                                                                          })
                                    }
                                }
                            }
                        }

                        VmSectionCard {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            enabled: root.selectedVm() !== null
                            titleText: root.tr("Network topology", "網絡拓撲")
                            ColumnLayout {
                                width: parent.width
                                Repeater {
                                    model: root.list(root.value(root.selectedVm(), "networkDevices", []))
                                    delegate: WfCard {
                                        id: networkCard
                                        required property var modelData
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        outlined: true
                                        surfaceLevel: "low"
                                        padding: DesignTokens.spacing8
                                        GridLayout {
                                            width: parent.width
                                            columns: width >= 420 ? 3 : 1
                                            Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4; color: DesignTokens.secondary(root.dark) }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                Label { Layout.fillWidth: true; text: root.value(networkCard.modelData, "name", root.tr("Network adapter", "網絡介面卡")); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: root.value(networkCard.modelData, "mode", "nat") + "  ·  "
                                                          + root.value(networkCard.modelData, "model", "") + "  ·  "
                                                          + root.value(networkCard.modelData, "macAddress", "")
                                                    font.pixelSize: 10
                                                    color: root.secondaryText
                                                    wrapMode: Text.WrapAnywhere
                                                }
                                            }
                                            WfButton {
                                                dark: root.dark
                                                compact: true
                                                variant: "text"
                                                text: root.tr("Remove", "移除")
                                                enabled: root.isPoweredOff(root.selectedVm()) && !root.app.vmBusy
                                                         && root.value(networkCard.modelData, "slot", 0) > 0
                                                         && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "configure")
                                                Accessible.name: root.tr("Remove network adapter", "移除網絡介面卡")
                                                ToolTip.visible: hovered
                                                ToolTip.text: Accessible.name
                                                onClicked: root.requestConfirmation("device", "removeNetwork", root.tr("Remove network adapter?", "移除網絡介面卡？"),
                                                                                    root.tr("The guest loses this adapter and its provider-specific network mapping.", "客體系統會失去此介面卡及其供應器特定網絡對應。"),
                                                                                    { vmId: root.selectedVmId(), slot: root.value(networkCard.modelData, "slot", 0) })
                                            }
                                        }
                                    }
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: width >= 540 ? 4 : width >= 360 ? 2 : 1
                                    SpinBox {
                                        id: addNetworkSlot
                                        Layout.fillWidth: true
                                        from: 1
                                        to: root.value(root.selectedVm(), "providerId", "").indexOf("vmware-") === 0 ? 10 : 8
                                        value: 1
                                        Accessible.name: root.tr("Network adapter slot", "網絡介面卡插槽")
                                    }
                                    ComboBox {
                                        id: addNetworkMode
                                        Layout.fillWidth: true
                                        model: root.value(root.selectedVm(), "providerId", "").indexOf("vmware-") === 0
                                               ? ["nat", "bridged", "host-only", "disconnected"]
                                               : ["nat", "bridged", "host-only", "internal", "disconnected"]
                                        Accessible.name: root.tr("New network adapter mode", "新網絡介面卡模式")
                                    }
                                    TextField {
                                        id: addNetworkInterface
                                        Layout.fillWidth: true
                                        enabled: root.value(root.selectedVm(), "providerId", "") === "virtualbox"
                                                 && ["bridged", "host-only", "internal"].indexOf(addNetworkMode.currentText) >= 0
                                        placeholderText: root.tr("Host interface or internal network name", "主機介面或內部網絡名稱")
                                        Accessible.name: root.tr("Network interface or network name", "網絡介面或網絡名稱")
                                        selectByMouse: true
                                    }
                                    WfButton {
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        compact: true
                                        variant: "outlined"
                                        text: root.tr("Apply slot", "套用插槽")
                                        Accessible.name: root.tr("Configure the explicit network adapter slot", "設定明確網絡介面卡插槽")
                                        enabled: root.isPoweredOff(root.selectedVm()) && !root.app.vmBusy
                                                 && (!addNetworkInterface.enabled
                                                     || addNetworkInterface.text.trim().length > 0)
                                                 && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "configure")
                                        onClicked: root.submitDeviceAction("addNetwork", {
                                                                              vmId: root.selectedVmId(),
                                                                              slot: addNetworkSlot.value,
                                                                              mode: addNetworkMode.currentText,
                                                                              interfaceName: addNetworkInterface.text.trim()
                                                                          })
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item {
                GridLayout {
                    anchors.fill: parent
                    columns: root.compact ? 1 : 2
                    columnSpacing: DesignTokens.spacing16
                    rowSpacing: DesignTokens.spacing16

                    WfCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: 125
                        dark: root.dark
                        outlined: true
                        surfaceLevel: "lowest"
                        padding: DesignTokens.spacing12
                        ColumnLayout {
                            anchors.fill: parent
                            Label {
                                Layout.fillWidth: true
                                text: root.tr("Snapshots for %1", "%1 的快照").arg(root.value(root.selectedVm(), "name", root.tr("selected machine", "已選虛擬機")))
                                font.pixelSize: 18
                                font.weight: Font.Bold
                                wrapMode: Text.Wrap
                            }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: snapshotList.width >= 520 ? 3 : 1
                                TextField {
                                    id: snapshotName
                                    Layout.fillWidth: true
                                    placeholderText: root.tr("Snapshot name", "快照名稱")
                                    Accessible.name: root.tr("New snapshot name", "新快照名稱")
                                }
                                TextField {
                                    id: snapshotDescription
                                    Layout.fillWidth: true
                                    placeholderText: root.tr("Optional description", "選填說明")
                                    Accessible.name: root.tr("New snapshot description", "新快照說明")
                                }
                                WfButton {
                                    Layout.fillWidth: snapshotList.width < 520
                                    dark: root.dark
                                    variant: "filled"
                                    text: root.tr("Take snapshot", "建立快照")
                                    Accessible.name: root.tr("Take virtual machine snapshot", "建立虛擬機快照")
                                    enabled: root.selectedVm() !== null && snapshotName.text.trim().length > 0 && !root.app.vmBusy
                                             && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "snapshots")
                                    onClicked: root.submitSnapshotAction("take", {
                                                                              vmId: root.selectedVmId(),
                                                                              name: snapshotName.text.trim(),
                                                                              description: snapshotDescription.text.trim()
                                                                          })
                                }
                            }
                            ListView {
                                id: snapshotList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 80
                                clip: true
                                spacing: 6
                                model: root.app.vmSnapshots || []
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                delegate: ItemDelegate {
                                    id: snapshotDelegate
                                    required property var modelData
                                    width: snapshotList.width
                                    highlighted: root.value(snapshotDelegate.modelData, "id", "") === root.selectedSnapshotId
                                    Accessible.name: root.tr("Select snapshot %1", "選取快照 %1").arg(root.value(snapshotDelegate.modelData, "name", ""))
                                    onClicked: root.selectedSnapshotId = root.value(snapshotDelegate.modelData, "id", "")
                                    contentItem: RowLayout {
                                        Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4; color: root.value(snapshotDelegate.modelData, "current", false) ? root.successText : DesignTokens.outline(root.dark) }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Label { Layout.fillWidth: true; text: root.value(snapshotDelegate.modelData, "name", ""); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                                            Label { Layout.fillWidth: true; text: root.value(snapshotDelegate.modelData, "createdAt", "") + "  ·  " + root.value(snapshotDelegate.modelData, "description", ""); color: root.secondaryText; font.pixelSize: 10; wrapMode: Text.Wrap }
                                        }
                                    }
                                }
                                Label {
                                    anchors.centerIn: parent
                                    visible: snapshotList.count === 0
                                    width: Math.max(80, parent.width - 20)
                                    text: root.selectedVm() ? root.tr("No snapshots reported.", "未回報快照。") : root.tr("Select a machine first.", "請先選取虛擬機。")
                                    color: root.secondaryText
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }

                    WfCard {
                        Layout.fillWidth: root.compact
                        Layout.preferredWidth: root.compact ? -1 : 370
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: 125
                        dark: root.dark
                        outlined: true
                        surfaceLevel: "lowest"
                        padding: 0
                        ScrollView {
                            id: snapshotDetailScroll
                            anchors.fill: parent
                            anchors.margins: 14
                            contentWidth: availableWidth
                            ColumnLayout {
                                width: snapshotDetailScroll.availableWidth
                                Label { Layout.fillWidth: true; text: root.tr("Snapshot details", "快照詳情"); font.pixelSize: 19; font.weight: Font.Bold; wrapMode: Text.Wrap }
                                Label {
                                    Layout.fillWidth: true
                                    text: root.selectedSnapshot() ? root.value(root.selectedSnapshot(), "name", "") : root.tr("Choose a snapshot to inspect or restore.", "選取快照以檢查或還原。")
                                    font.weight: Font.DemiBold
                                    wrapMode: Text.Wrap
                                }
                                Label { Layout.fillWidth: true; visible: root.selectedSnapshot() !== null; text: root.value(root.selectedSnapshot(), "description", root.tr("No description", "冇說明")); wrapMode: Text.Wrap; color: root.secondaryText }
                                Label { Layout.fillWidth: true; visible: root.selectedSnapshot() !== null; text: root.value(root.selectedSnapshot(), "createdAt", "") + "\n" + root.value(root.selectedSnapshot(), "id", ""); wrapMode: Text.WrapAnywhere; font.family: DesignTokens.fontMono; font.pixelSize: 10 }
                                WfStatusChip { visible: root.selectedSnapshot() !== null && root.value(root.selectedSnapshot(), "current", false); dark: root.dark; tone: "success"; showDot: true; uppercase: false; text: root.tr("Current snapshot", "而家嘅快照") }
                                WfButton {
                                    Layout.fillWidth: true
                                    visible: root.selectedSnapshot() !== null
                                    dark: root.dark
                                    variant: "outlined"
                                    text: root.tr("Restore snapshot", "還原快照")
                                    Accessible.name: root.tr("Restore selected snapshot", "還原已選快照")
                                    enabled: !root.app.vmBusy
                                             && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "snapshots")
                                    onClicked: root.requestConfirmation("snapshot", "restore", root.tr("Restore snapshot?", "還原快照？"),
                                                                                root.tr("The machine state and virtual disks return to this checkpoint. Newer guest changes may be lost.", "虛擬機狀態同虛擬磁碟會返去呢個檢查點；之後嘅客體變更可能會冇咗。"),
                                                                                { vmId: root.selectedVmId(), snapshotId: root.selectedSnapshotId, expectedConfirmation: "DELETE " + root.value(root.selectedVm(), "name", "") })
                                }
                                WfButton {
                                    Layout.fillWidth: true
                                    visible: root.selectedSnapshot() !== null
                                    dark: root.dark
                                    variant: "destructive"
                                    text: root.tr("Delete snapshot", "刪除快照")
                                    Accessible.name: root.tr("Delete selected snapshot", "刪除已選快照")
                                    enabled: !root.app.vmBusy && !root.value(root.selectedSnapshot(), "current", false)
                                             && root.providerSupports(root.value(root.selectedVm(), "providerId", ""), "snapshots")
                                    onClicked: root.requestConfirmation("snapshot", "delete", root.tr("Delete snapshot?", "刪除快照？"),
                                                                                root.tr("The provider may merge snapshot storage. This can take time and cannot be undone.", "供應器可能合併快照儲存空間。呢個操作可能需時，而且復原唔到。"),
                                                                                { vmId: root.selectedVmId(), snapshotId: root.selectedSnapshotId, expectedConfirmation: "DELETE " + root.value(root.selectedVm(), "name", "") })
                                }
                            }
                        }
                    }
                }
            }

            Item {
                GridLayout {
                    anchors.fill: parent
                    columns: root.compact ? 1 : 2
                    columnSpacing: DesignTokens.spacing16
                    rowSpacing: DesignTokens.spacing16

                    WfCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: 135
                        dark: root.dark
                        outlined: true
                        surfaceLevel: "lowest"
                        padding: DesignTokens.spacing12
                        ColumnLayout {
                            anchors.fill: parent
                            Label { Layout.fillWidth: true; text: root.tr("Validation runs", "驗證執行"); font.pixelSize: 19; font.weight: Font.Bold; wrapMode: Text.Wrap }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: validationRunList.width >= 520 ? 3 : 1
                                TextField {
                                    id: validationName
                                    Layout.fillWidth: true
                                    placeholderText: root.tr("Run name", "執行名稱")
                                    text: root.value(root.selectedVm(), "name", "") + " smoke test"
                                    Accessible.name: root.tr("Validation run name", "驗證執行名稱")
                                }
                                ComboBox {
                                    id: validationProfile
                                    Layout.fillWidth: true
                                    model: ["installation", "first-boot", "upgrade", "customization", "full-smoke"]
                                    currentIndex: 4
                                    Accessible.name: root.tr("Validation profile", "驗證設定檔")
                                }
                                WfButton {
                                    Layout.fillWidth: validationRunList.width < 520
                                    dark: root.dark
                                    variant: "filled"
                                    text: root.tr("Start run", "開始執行")
                                    Accessible.name: root.tr("Start validation run", "開始驗證執行")
                                    enabled: root.selectedVm() !== null && !root.app.vmBusy
                                             && String(root.app.currentOutput || "").length > 0
                                    onClicked: root.submitValidation({
                                                                         vmId: root.selectedVmId(),
                                                                         providerId: root.value(root.selectedVm(), "providerId", ""),
                                                                         name: validationName.text.trim(),
                                                                         profile: validationProfile.currentText,
                                                                         isoPath: root.app.currentOutput,
                                                                         notes: validationStartNotes.text.trim()
                                                                     })
                                }
                            }
                            TextArea {
                                id: validationStartNotes
                                Layout.fillWidth: true
                                Layout.preferredHeight: 58
                                placeholderText: root.tr("Purpose, build number, expected result, or test environment notes…", "用途、版本號、預期結果或測試環境備註……")
                                Accessible.name: root.tr("Validation run notes", "驗證執行備註")
                                wrapMode: TextEdit.Wrap
                            }
                            ListView {
                                id: validationRunList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 75
                                clip: true
                                spacing: 6
                                model: root.app.vmValidationRuns || []
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                delegate: ItemDelegate {
                                    id: validationRunDelegate
                                    required property var modelData
                                    width: validationRunList.width
                                    highlighted: root.value(validationRunDelegate.modelData, "id", "") === root.selectedValidationRunId
                                    Accessible.name: root.tr("Select validation run %1", "選取驗證執行 %1").arg(root.value(validationRunDelegate.modelData, "name", ""))
                                    onClicked: root.selectedValidationRunId = root.value(validationRunDelegate.modelData, "id", "")
                                    contentItem: GridLayout {
                                        columns: validationRunList.width >= 460 ? 3 : 2
                                        Rectangle {
                                            Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                                            color: root.value(validationRunDelegate.modelData, "result", "") === "pass" ? root.successText
                                                 : root.value(validationRunDelegate.modelData, "result", "") === "fail" ? root.errorText : DesignTokens.secondary(root.dark)
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Label { Layout.fillWidth: true; text: root.value(validationRunDelegate.modelData, "name", root.value(validationRunDelegate.modelData, "id", "")); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                                            Label { Layout.fillWidth: true; text: root.value(validationRunDelegate.modelData, "profile", "") + "  ·  " + root.value(validationRunDelegate.modelData, "startedAt", ""); font.pixelSize: 10; color: root.secondaryText; wrapMode: Text.Wrap }
                                        }
                                        Label {
                                            Layout.columnSpan: validationRunList.width >= 460 ? 1 : 2
                                            text: String(root.value(validationRunDelegate.modelData, "result", root.value(validationRunDelegate.modelData, "status", "running"))).toUpperCase()
                                            font.bold: true
                                            font.pixelSize: 10
                                            horizontalAlignment: validationRunList.width >= 460 ? Text.AlignRight : Text.AlignLeft
                                        }
                                    }
                                }
                                Label { anchors.centerIn: parent; visible: validationRunList.count === 0; width: Math.max(80, parent.width - 20); text: root.tr("No validation evidence recorded yet.", "尚未記錄驗證證據。"); horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; color: root.secondaryText }
                            }
                        }
                    }

                    WfCard {
                        Layout.fillWidth: root.compact
                        Layout.preferredWidth: root.compact ? -1 : 430
                        Layout.fillHeight: true
                        Layout.minimumWidth: 0
                        Layout.minimumHeight: 135
                        dark: root.dark
                        outlined: true
                        surfaceLevel: "lowest"
                        padding: 0
                        ScrollView {
                            id: validationDetailScroll
                            anchors.fill: parent
                            anchors.margins: 12
                            contentWidth: availableWidth
                            ColumnLayout {
                                width: validationDetailScroll.availableWidth
                                spacing: 7
                                Label {
                                    Layout.fillWidth: true
                                    text: root.selectedValidationRun() ? root.value(root.selectedValidationRun(), "name", root.tr("Validation evidence", "驗證證據"))
                                                                            : root.tr("Validation evidence", "驗證證據")
                                    font.pixelSize: 19
                                    font.weight: Font.Bold
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRun() !== null
                                    text: root.value(root.selectedValidationRun(), "status", "") + "  ·  "
                                          + root.value(root.selectedValidationRun(), "startedAt", "") + "  →  "
                                          + root.value(root.selectedValidationRun(), "finishedAt", root.tr("in progress", "進行中"))
                                    wrapMode: Text.Wrap
                                    font.pixelSize: 10
                                    color: root.secondaryText
                                }
                                ProgressBar {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRun() !== null
                                    from: 0
                                    to: Math.max(1, root.value(root.selectedValidationRun(), "milestoneCount", 8))
                                    value: root.value(root.selectedValidationRun(), "completedMilestones", root.list(root.value(root.selectedValidationRun(), "milestones", [])).length)
                                    Accessible.name: root.tr("Validation milestone progress", "驗證里程碑進度")
                                }

                                Repeater {
                                    model: root.list(root.value(root.selectedValidationRun(), "milestones", []))
                                    delegate: WfCard {
                                        id: milestoneCard
                                        required property var modelData
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        outlined: true
                                        surfaceLevel: "low"
                                        padding: DesignTokens.spacing8
                                        ColumnLayout {
                                            width: parent.width
                                            GridLayout {
                                                Layout.fillWidth: true
                                                columns: 2
                                                Label { Layout.fillWidth: true; text: root.value(milestoneCard.modelData, "name", root.value(milestoneCard.modelData, "kind", "")); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                                                Label {
                                                    text: String(root.value(milestoneCard.modelData, "result", "pending")).toUpperCase()
                                                    color: root.value(milestoneCard.modelData, "result", "") === "pass" ? root.successText
                                                           : root.value(milestoneCard.modelData, "result", "") === "fail" ? root.errorText : root.warningText
                                                    font.bold: true
                                                    font.pixelSize: 10
                                                }
                                            }
                                            Label { Layout.fillWidth: true; text: root.value(milestoneCard.modelData, "evidence", ""); wrapMode: Text.WrapAnywhere; font.family: DesignTokens.fontMono; font.pixelSize: 10 }
                                            Label { Layout.fillWidth: true; text: root.value(milestoneCard.modelData, "notes", ""); wrapMode: Text.Wrap; color: root.secondaryText; font.pixelSize: 10 }
                                        }
                                    }
                                }

                                Label { Layout.fillWidth: true; visible: root.selectedValidationRun() !== null; text: root.tr("Record milestone", "記錄里程碑"); font.weight: Font.DemiBold }
                                GridLayout {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRun() !== null
                                    columns: width >= 390 ? 2 : 1
                                    ComboBox {
                                        id: milestoneKind
                                        Layout.fillWidth: true
                                        model: ["installation-boot", "disk-layout", "installation-complete", "first-boot", "drivers", "networking", "customizations", "smoke-test"]
                                        Accessible.name: root.tr("Validation milestone", "驗證里程碑")
                                    }
                                    ComboBox {
                                        id: milestoneResult
                                        Layout.fillWidth: true
                                        model: ["pass", "fail", "skip"]
                                        Accessible.name: root.tr("Milestone result", "里程碑結果")
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRun() !== null
                                    TextField {
                                        id: milestoneEvidence
                                        Layout.fillWidth: true
                                        placeholderText: root.tr("Evidence file, screenshot hash, build ID, or observed value", "證據檔、截圖雜湊、版本 ID 或觀察值")
                                        Accessible.name: root.tr("Milestone evidence", "里程碑證據")
                                        selectByMouse: true
                                    }
                                    ToolButton {
                                        text: "…"
                                        Accessible.name: root.tr("Choose an evidence file to hash and preserve", "選擇要雜湊及保存嘅證據檔")
                                        ToolTip.visible: hovered
                                        ToolTip.text: Accessible.name
                                        onClicked: evidenceFileDialog.open()
                                    }
                                }
                                TextArea {
                                    id: milestoneNotes
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 64
                                    visible: root.selectedValidationRun() !== null
                                    placeholderText: root.tr("Observed behavior and reviewer notes", "觀察到的行為及審查備註")
                                    Accessible.name: root.tr("Milestone notes", "里程碑備註")
                                    wrapMode: TextEdit.Wrap
                                }
                                WfButton {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRun() !== null
                                    dark: root.dark
                                    variant: "outlined"
                                    text: root.tr("Record milestone", "記錄里程碑")
                                    Accessible.name: root.tr("Record validation milestone", "記錄驗證里程碑")
                                    enabled: !root.app.vmBusy && root.selectedValidationRunning()
                                    onClicked: root.submitMilestone(root.selectedValidationRunId, {
                                                                       kind: milestoneKind.currentText,
                                                                       result: milestoneResult.currentText,
                                                                       evidence: milestoneEvidence.text.trim(),
                                                                       notes: milestoneNotes.text.trim()
                                                                   })
                                }

                                Label { Layout.fillWidth: true; visible: root.selectedValidationRun() !== null; text: root.tr("Provider and validation log", "供應器及驗證記錄"); font.weight: Font.DemiBold }
                                TextArea {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 105
                                    visible: root.selectedValidationRun() !== null
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.WrapAnywhere
                                    font.family: DesignTokens.fontMono
                                    font.pixelSize: 10
                                    text: root.value(root.selectedValidationRun(), "log", root.tr("No log entries.", "冇記錄。"))
                                    Accessible.name: root.tr("Validation run log", "驗證執行記錄")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRunning()
                                             && root.list(root.value(root.selectedValidationRun(), "passBlockers", [])).length > 0
                                    text: root.tr("Pass gates still required:", "仍需完成嘅通過關卡：") + "\n• "
                                          + root.list(root.value(root.selectedValidationRun(), "passBlockers", [])).join("\n• ")
                                    wrapMode: Text.Wrap
                                    color: root.warningText
                                    font.pixelSize: 10
                                    Accessible.name: text
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRun() !== null
                                    columns: width >= 390 ? 3 : 1
                                    WfButton {
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        variant: "filled"
                                        text: root.tr("Finish pass", "完成：通過")
                                        Accessible.name: root.tr("Finish validation as passed", "完成驗證並標記為通過")
                                        enabled: !root.app.vmBusy && root.selectedValidationRunning()
                                                 && Boolean(root.value(root.selectedValidationRun(), "canPass", false))
                                        onClicked: root.submitValidationResult(root.selectedValidationRunId, { result: "pass", notes: milestoneNotes.text.trim() })
                                    }
                                    WfButton {
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        variant: "destructive"
                                        text: root.tr("Finish fail", "完成：失敗")
                                        Accessible.name: root.tr("Finish validation as failed", "完成驗證並標記為失敗")
                                        enabled: !root.app.vmBusy && root.selectedValidationRunning()
                                                 && milestoneNotes.text.trim().length > 0
                                        onClicked: root.submitValidationResult(root.selectedValidationRunId, { result: "fail", notes: milestoneNotes.text.trim() })
                                    }
                                    WfButton {
                                        Layout.fillWidth: true
                                        dark: root.dark
                                        variant: "outlined"
                                        text: root.tr("Abort", "中止")
                                        Accessible.name: root.tr("Abort validation run", "中止驗證執行")
                                        enabled: !root.app.vmBusy && root.selectedValidationRunning()
                                                 && milestoneNotes.text.trim().length > 0
                                        onClicked: root.submitValidationResult(root.selectedValidationRunId, { result: "aborted", notes: milestoneNotes.text.trim() })
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: root.selectedValidationRun() === null
                                    text: root.tr("Start a run or choose existing evidence to record installation milestones, logs, notes, and the final pass/fail result.",
                                                  "開始執行或選取現有證據，以記錄安裝里程碑、記錄、備註及最終通過／失敗結果。")
                                    wrapMode: Text.Wrap
                                    horizontalAlignment: Text.AlignHCenter
                                    color: root.secondaryText
                                }
                            }
                        }
                    }
                }
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            outlined: true
            surfaceLevel: "low"
            padding: DesignTokens.spacing8
            GridLayout {
                width: parent.width
                columns: width >= 620 ? 3 : 1
                columnSpacing: 8
                rowSpacing: 4
                BusyIndicator {
                    running: root.app.vmBusy
                    visible: running
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Accessible.name: root.tr("Virtual machine operation in progress", "虛擬機操作進行中")
                }
                Label {
                    Layout.fillWidth: true
                    text: root.statusMessage()
                    wrapMode: Text.WrapAnywhere
                    font.pixelSize: 10
                    color: root.value(root.app.vmStatus, "tone", "") === "error" ? root.errorText : root.secondaryText
                }
                WfButton {
                    Layout.fillWidth: width < 620
                    visible: root.app.vmBusy
                    dark: root.dark
                    compact: true
                    variant: "outlined"
                    text: root.tr("Cancel operation", "取消操作")
                    Accessible.name: root.tr("Cancel current virtual machine operation", "取消而家個虛擬機操作")
                    onClicked: root.cancelCurrentAction()
                }
            }
        }
    }

    Dialog {
        id: confirmationDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(520, root.width - 40)
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose

        contentItem: ColumnLayout {
            spacing: 10
            Label {
                id: confirmationMessage
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            CheckBox {
                id: deleteFilesCheck
                Layout.fillWidth: true
                visible: root.pendingDispatchKind === "vm" && root.pendingAction === "delete"
                         && root.value(root.selectedVm(), "ownership", "") === "managed"
                text: root.tr("Permanently delete provider files after ownership, inventory, lock, path, and file-identity checks", "完成擁有權、清單、鎖定、路徑及檔案識別檢查後，永久刪除供應器檔案")
                Accessible.name: text
                contentItem: Label {
                    leftPadding: deleteFilesCheck.indicator.width + deleteFilesCheck.spacing
                    text: deleteFilesCheck.text
                    font: deleteFilesCheck.font
                    color: deleteFilesCheck.palette.windowText
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Label {
                Layout.fillWidth: true
                text: root.tr("Continue to an exact, immutable operation preview. Nothing executes from this screen.", "繼續前往完全相同且不可變更嘅操作預覽。呢個畫面唔會執行任何操作。")
                wrapMode: Text.Wrap
                color: root.warningText
                font.pixelSize: 10
            }
        }

        footer: DialogButtonBox {
            Button {
                text: root.tr("Cancel", "取消")
                Accessible.name: root.tr("Cancel destructive action", "取消破壞性操作")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                text: root.tr("Review exact operation", "審閱完整操作")
                Accessible.name: root.tr("Generate the exact virtual machine operation preview", "產生完整虛擬機操作預覽")
                highlighted: true
                enabled: !(root.pendingDispatchKind === "vm" && root.pendingAction === "delete"
                           && root.value(root.selectedVm(), "ownership", "") === "managed")
                         || deleteFilesCheck.checked
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }

        onAccepted: root.dispatchConfirmedAction()
    }

    Dialog {
        id: reviewDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(760, root.width - 32)
        height: Math.min(720, root.height - 32)
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        title: root.tr("Review exact provider operation", "審閱完整供應器操作")

        contentItem: ColumnLayout {
            spacing: 10

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 540 ? 2 : 1
                columnSpacing: 12
                rowSpacing: 3
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Action: %1", "操作：%1").arg(root.value(root.pendingPreview, "action", ""))
                    font.weight: Font.DemiBold
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Risk: %1", "風險：%1").arg(root.value(root.pendingPreview, "risk", ""))
                    color: root.value(root.pendingPreview, "risk", "") === "destructive"
                           ? root.errorText : root.warningText
                    font.weight: Font.DemiBold
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Target: %1 (%2)", "目標：%1（%2）")
                          .arg(root.value(root.value(root.pendingPreview, "target", {}), "name", ""))
                          .arg(root.value(root.value(root.pendingPreview, "target", {}), "id", ""))
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: root.tr("Provider: %1", "供應器：%1")
                          .arg(root.value(root.value(root.pendingPreview, "target", {}), "providerId", ""))
                    wrapMode: Text.WrapAnywhere
                }
            }

            ScrollView {
                id: operationScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ColumnLayout {
                    width: operationScroll.availableWidth
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Effects", "影響")
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: root.list(root.value(root.pendingPreview, "effects", []))
                        delegate: Label {
                            required property var modelData
                            Layout.fillWidth: true
                            text: "• " + String(modelData)
                            wrapMode: Text.Wrap
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.list(root.value(root.pendingPreview, "warnings", [])).length > 0
                        text: root.tr("Warnings", "警告")
                        color: root.warningText
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: root.list(root.value(root.pendingPreview, "warnings", []))
                        delegate: Label {
                            required property var modelData
                            Layout.fillWidth: true
                            text: String(modelData)
                            color: root.warningText
                            wrapMode: Text.Wrap
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Provider commands", "供應器命令")
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.list(root.value(root.pendingPreview, "commands", [])).length === 0
                        text: root.tr("No provider process will be started; this operation only updates WimForge state.", "唔會啟動供應器程序；呢個操作只會更新 WimForge 狀態。")
                        color: root.secondaryText
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: root.list(root.value(root.pendingPreview, "commands", []))
                        delegate: WfCard {
                            id: commandPane
                            required property var modelData
                            Layout.fillWidth: true
                            dark: root.dark
                            outlined: true
                            surfaceLevel: "low"
                            padding: DesignTokens.spacing8
                            contentItem: Label {
                                text: root.commandText(commandPane.modelData)
                                font.family: DesignTokens.fontMono
                                font.pixelSize: 10
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Preview ID: %1\nInventory revision: %2\nExpires: %3",
                                      "預覽 ID：%1\n清單修訂：%2\n到期：%3")
                              .arg(root.value(root.pendingPreview, "id", ""))
                              .arg(root.value(root.pendingPreview, "revision", ""))
                              .arg(root.value(root.pendingPreview, "expiresAt", ""))
                        color: root.secondaryText
                        font.family: DesignTokens.fontMono
                        font.pixelSize: 9
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.reviewNeedsToken
                text: root.tr("Type this exact token to authorize execution:\n%1",
                              "輸入以下完整字句以授權執行：\n%1")
                      .arg(root.value(root.pendingPreview, "confirmation", ""))
                font.weight: Font.DemiBold
                color: root.errorText
                wrapMode: Text.WrapAnywhere
            }
            TextField {
                id: reviewConfirmation
                Layout.fillWidth: true
                visible: root.reviewNeedsToken
                placeholderText: root.tr("Exact confirmation token", "完整確認字句")
                Accessible.name: root.tr("Exact reviewed operation confirmation token", "已審閱操作嘅完整確認字句")
                selectByMouse: true
            }
        }

        footer: RowLayout {
            spacing: 8
            Item { Layout.fillWidth: true }
            Button {
                text: root.tr("Discard", "放棄")
                Accessible.name: root.tr("Discard the reviewed virtual machine operation", "放棄已審閱嘅虛擬機操作")
                onClicked: {
                    root.app.discardPendingVmPreview()
                    reviewDialog.close()
                }
            }
            Button {
                text: root.tr("Execute exact preview", "執行完整預覽")
                Accessible.name: root.tr("Execute the exact reviewed virtual machine operation", "執行完整嘅已審閱虛擬機操作")
                highlighted: true
                enabled: !root.app.vmBusy
                         && root.value(root.pendingPreview, "id", "").length > 0
                         && (!root.reviewNeedsToken
                             || reviewConfirmation.text === root.value(root.pendingPreview, "confirmation", ""))
                onClicked: {
                    if (root.app.executePendingVmPreview(
                                root.value(root.pendingPreview, "id", ""),
                                reviewConfirmation.text))
                        reviewDialog.close()
                }
            }
        }
    }

    FileDialog {
        id: evidenceFileDialog
        title: root.tr("Choose validation evidence", "選擇驗證證據")
        fileMode: FileDialog.OpenFile
        onAccepted: milestoneEvidence.text = root.app.pathFromUrl(selectedFile)
    }
}
