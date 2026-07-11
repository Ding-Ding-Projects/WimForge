pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "../components"

Item {
    id: root

    required property var app
    required property var terminal
    required property var tr

    required property bool dark
    Material.theme: dark ? Material.Dark : Material.Light
    readonly property color surfaceForeground: DesignTokens.onSurface(root.dark)
    readonly property color surfaceVariantForeground: DesignTokens.onSurfaceVariant(root.dark)
    readonly property color outlineVariant: DesignTokens.outlineVariant(root.dark)
    readonly property color success: DesignTokens.success(root.dark)
    readonly property color error: DesignTokens.error(root.dark)
    readonly property bool compactHeight: height < 560
    readonly property int terminalColumns: Math.max(40, Math.floor(Math.max(360, terminalOutput.width) / 8.2))
    readonly property int terminalRows: Math.max(12, Math.floor(Math.max(220, terminalOutput.height) / 18))

    property var commandHistory: []
    property int historyIndex: commandHistory.length

    function statusText() {
        if (root.terminal.running)
            return root.tr("Running inside WimForge", "喺 WimForge 入面執行緊")
        if (root.terminal.errorString.length > 0)
            return root.terminal.errorString
        if (root.terminal.exitStatus !== 0) {
            var outcome = root.terminal.exitStatus === 3
                        ? root.tr("Terminated", "已終止")
                        : root.terminal.exitStatus === 2
                          ? root.tr("Crashed", "已崩潰")
                          : root.tr("Exited", "已結束")
            return root.tr("%1 with code %2", "%1，代碼 %2")
                       .arg(outcome).arg(root.terminal.exitCode)
        }
        return root.tr("Ready", "準備好")
    }

    function startSession() {
        var directory = workingDirectory.text.trim()
        if (directory.length === 0 && root.app.projectLoaded)
            directory = root.app.projectRoot
        if (root.terminal.startShell(shellChoice.currentValue, directory,
                                     root.terminalColumns, root.terminalRows)) {
            terminalInput.forceActiveFocus()
        }
    }

    function sendInput() {
        var command = terminalInput.text
        if (command.length === 0 || !root.terminal.running)
            return
        if (root.terminal.writeInput(command + "\r\n")) {
            var nextHistory = root.commandHistory.slice(Math.max(0, root.commandHistory.length - 99))
            if (nextHistory.length === 0 || nextHistory[nextHistory.length - 1] !== command)
                nextHistory.push(command)
            root.commandHistory = nextHistory
            root.historyIndex = root.commandHistory.length
            terminalInput.clear()
        }
    }

    function showPreviousCommand() {
        if (root.commandHistory.length === 0)
            return
        root.historyIndex = Math.max(0, root.historyIndex - 1)
        terminalInput.text = root.commandHistory[root.historyIndex]
        terminalInput.cursorPosition = terminalInput.length
    }

    function showNextCommand() {
        if (root.commandHistory.length === 0)
            return
        root.historyIndex = Math.min(root.commandHistory.length, root.historyIndex + 1)
        terminalInput.text = root.historyIndex < root.commandHistory.length
                           ? root.commandHistory[root.historyIndex] : ""
        terminalInput.cursorPosition = terminalInput.length
    }

    Component.onCompleted: {
        if (root.app.projectLoaded)
            workingDirectory.text = root.app.projectRoot
    }

    Connections {
        target: root.app
        function onStateChanged() {
            if (root.app.projectLoaded && workingDirectory.text.trim().length === 0)
                workingDirectory.text = root.app.projectRoot
        }
    }

    Connections {
        target: root.terminal
        function onDisplayTranscriptChanged() {
            Qt.callLater(function() {
                terminalOutput.cursorPosition = terminalOutput.length
                outputScroll.ScrollBar.vertical.position = 1.0
            })
        }
        function onProcessExited() {
            terminalInput.clear()
        }
    }

    Timer {
        id: resizeTimer
        interval: 180
        repeat: false
        onTriggered: {
            if (root.terminal.running)
                root.terminal.resize(root.terminalColumns, root.terminalRows)
        }
    }

    onTerminalColumnsChanged: resizeTimer.restart()
    onTerminalRowsChanged: resizeTimer.restart()

    ColumnLayout {
        anchors.fill: parent
        spacing: root.compactHeight ? DesignTokens.spacing8 : DesignTokens.spacing12

        WfPageHeader {
            Layout.fillWidth: true
            dark: root.dark
            eyebrow: root.compactHeight ? "" : root.tr("Administrator session", "系統管理員工作階段")
            title: root.tr("Embedded terminal", "內嵌終端機")
            description: root.compactHeight ? "" : root.tr(
                             "Interactive PowerShell or Command Prompt hosted by ConPTY, without an external console window.",
                             "由 ConPTY 承載嘅互動式 PowerShell 或命令提示字元，唔會開外部主控台視窗。")
            WfStatusChip {
                dark: root.dark
                tone: root.terminal.running ? "info"
                      : root.terminal.errorString.length > 0 ? "error" : "neutral"
                uppercase: false
                text: root.statusText()
            }
        }

        WfCard {
            visible: !root.compactHeight || root.terminal.errorString.length > 0
                     || root.terminal.transcriptTruncated || root.terminal.droppedOutputBytes > 0
            Layout.fillWidth: true
            dark: root.dark
            fillColor: root.terminal.errorString.length > 0
                     ? DesignTokens.errorContainer(root.dark)
                     : root.terminal.running
                       ? DesignTokens.secondaryContainer(root.dark)
                       : DesignTokens.surfaceLow(root.dark)
            outlineColor: root.terminal.errorString.length > 0
                        ? DesignTokens.error(root.dark)
                        : root.terminal.running
                          ? DesignTokens.secondary(root.dark) : root.outlineVariant
            padding: DesignTokens.spacing12
            RowLayout {
                anchors.fill: parent
                spacing: DesignTokens.spacing12
                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: root.terminal.errorString.length > 0
                           ? DesignTokens.error(root.dark)
                           : root.terminal.running
                             ? DesignTokens.secondary(root.dark) : root.surfaceVariantForeground
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: DesignTokens.spacing4
                    Label {
                        Layout.fillWidth: true
                        text: root.statusText()
                        color: root.terminal.errorString.length > 0
                               ? DesignTokens.onErrorContainer(root.dark)
                               : root.terminal.running
                                 ? DesignTokens.onSecondaryContainer(root.dark) : root.surfaceForeground
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 13
                        font.weight: Font.Bold
                        wrapMode: Text.WrapAnywhere
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.terminal.transcriptTruncated || root.terminal.droppedOutputBytes > 0
                        text: root.tr("Bounded output protection discarded %1 byte(s) under backpressure.",
                                      "有界輸出保護因背壓捨棄咗 %1 bytes。")
                                  .arg(root.terminal.droppedOutputBytes)
                        color: root.error
                        font.family: DesignTokens.fontBody
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            surfaceLevel: "low"
            padding: DesignTokens.spacing12

            GridLayout {
                id: terminalControls
                anchors.fill: parent
                columns: root.width >= 1120 ? 5 : root.width >= 620 ? 2 : 1
                columnSpacing: DesignTokens.spacing8
                rowSpacing: DesignTokens.spacing8

                ComboBox {
                    id: shellChoice
                    Layout.fillWidth: true
                    Layout.preferredHeight: DesignTokens.controlHeight
                    enabled: !root.terminal.running
                    textRole: "label"
                    valueRole: "value"
                    model: [
                        { label: root.tr("Default (PowerShell)", "預設（PowerShell）"), value: "default" },
                        { label: root.tr("Windows PowerShell", "Windows PowerShell"), value: "powershell" },
                        { label: root.tr("Command Prompt", "命令提示字元"), value: "cmd" }
                    ]
                    Accessible.name: root.tr("Embedded terminal shell", "內嵌終端機 shell")
                }

                TextField {
                    id: workingDirectory
                    Layout.fillWidth: true
                    Layout.preferredHeight: DesignTokens.fieldHeight
                    enabled: !root.terminal.running
                    placeholderText: root.tr("Working directory (project when empty)", "工作目錄（留空時使用工程）")
                    Accessible.name: root.tr("Terminal working directory", "終端機工作目錄")
                    selectByMouse: true
                    font.family: DesignTokens.fontMono
                    font.pixelSize: 11
                }

                WfButton {
                    Layout.fillWidth: root.width < 1120
                    Layout.columnSpan: terminalControls.columns === 2 ? 2 : 1
                    dark: root.dark
                    variant: "filled"
                    text: root.tr("Start", "啟動")
                    enabled: !root.terminal.running
                    Accessible.name: root.tr("Start the embedded terminal", "啟動內嵌終端機")
                    onClicked: root.startSession()
                }
                WfButton {
                    Layout.fillWidth: root.width < 1120
                    dark: root.dark
                    variant: "outlined"
                    text: root.tr("Stop", "停止")
                    enabled: root.terminal.running
                    Accessible.name: root.tr("Gracefully stop the embedded terminal", "正常停止內嵌終端機")
                    onClicked: root.terminal.stopGracefully(3000)
                }
                WfButton {
                    Layout.fillWidth: root.width < 1120
                    dark: root.dark
                    variant: "destructive"
                    text: root.tr("Force stop", "強制停止")
                    enabled: root.terminal.running
                    Accessible.name: root.tr("Force stop the embedded terminal process tree", "強制停止內嵌終端機程序樹")
                    onClicked: forceStopDialog.open()
                }
            }
        }

        WfCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.compactHeight ? 170 : 260
            dark: true
            padding: 0
            fillColor: "#0B0F14"
            outlineColor: root.terminal.running ? DesignTokens.secondary(true) : "#3B424C"

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: DesignTokens.spacing12
                    Layout.rightMargin: DesignTokens.spacing8
                    Layout.topMargin: DesignTokens.spacing8
                    Layout.bottomMargin: DesignTokens.spacing8
                    spacing: DesignTokens.spacing8
                    Rectangle {
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        radius: 4
                        color: root.terminal.running ? DesignTokens.secondary(true) : "#7D8590"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.tr("Administrator shell · %1 columns × %2 rows",
                                      "系統管理員 shell · %1 欄 × %2 行")
                                  .arg(root.terminalColumns).arg(root.terminalRows)
                        color: "#9DA7B3"
                        font.family: DesignTokens.fontMono
                        font.pixelSize: 10
                    }
                    WfButton {
                        dark: true
                        compact: true
                        variant: "text"
                        text: root.tr("Copy", "複製")
                        enabled: terminalOutput.length > 0
                        Accessible.name: root.tr("Copy terminal output", "複製終端機輸出")
                        onClicked: {
                            terminalOutput.selectAll()
                            terminalOutput.copy()
                            terminalOutput.deselect()
                        }
                    }
                    WfButton {
                        dark: true
                        compact: true
                        variant: "text"
                        text: root.tr("Clear", "清除")
                        enabled: terminalOutput.length > 0
                        Accessible.name: root.tr("Clear retained terminal output", "清除保留嘅終端機輸出")
                        onClicked: root.terminal.clearTranscript()
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#30363D" }

                ScrollView {
                    id: outputScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    TextArea {
                        id: terminalOutput
                        width: outputScroll.availableWidth
                        text: root.terminal.displayTranscript
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        color: "#E6EDF3"
                        selectionColor: "#315FAD"
                        selectedTextColor: "#FFFFFF"
                        font.family: DesignTokens.fontMono
                        font.pixelSize: 12
                        background: null
                        leftPadding: DesignTokens.spacing12
                        rightPadding: DesignTokens.spacing12
                        topPadding: DesignTokens.spacing8
                        bottomPadding: DesignTokens.spacing8
                        Accessible.name: root.tr("Embedded terminal output", "內嵌終端機輸出")
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: "#30363D" }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.margins: DesignTokens.spacing8
                    columns: root.width >= 620 ? 4 : 2
                    columnSpacing: DesignTokens.spacing8
                    rowSpacing: DesignTokens.spacing8

                    Label {
                        text: ">"
                        color: root.terminal.running ? DesignTokens.secondary(true) : "#7D8590"
                        font.family: DesignTokens.fontMono
                        font.pixelSize: 13
                        font.bold: true
                    }
                    TextField {
                        id: terminalInput
                        Layout.fillWidth: true
                        enabled: root.terminal.running
                        placeholderText: root.terminal.running
                                         ? root.tr("Type a command and press Enter", "輸入命令再按 Enter")
                                         : root.tr("Start a session to enter commands", "啟動工作階段後先可以輸入命令")
                        Accessible.name: root.tr("Embedded terminal command input", "內嵌終端機命令輸入")
                        color: "#E6EDF3"
                        placeholderTextColor: "#7D8590"
                        selectionColor: "#315FAD"
                        selectedTextColor: "#FFFFFF"
                        font.family: DesignTokens.fontMono
                        font.pixelSize: 12
                        selectByMouse: true
                        onAccepted: root.sendInput()
                        Keys.onUpPressed: function(event) {
                            root.showPreviousCommand()
                            event.accepted = true
                        }
                        Keys.onDownPressed: function(event) {
                            root.showNextCommand()
                            event.accepted = true
                        }
                        background: Rectangle {
                            radius: DesignTokens.radiusControl
                            color: "#111820"
                            border.width: terminalInput.activeFocus ? 2 : 1
                            border.color: terminalInput.activeFocus ? "#79D6EC" : "#3B424C"
                        }
                    }
                    WfButton {
                        dark: true
                        compact: true
                        variant: "outlined"
                        text: "Ctrl+C"
                        enabled: root.terminal.running
                        Accessible.name: root.tr("Send interrupt to terminal", "傳送中斷到終端機")
                        onClicked: root.terminal.writeInput("\u0003")
                    }
                    WfButton {
                        dark: true
                        compact: true
                        variant: "tonal"
                        text: root.tr("Send", "傳送")
                        enabled: root.terminal.running && terminalInput.text.length > 0
                        Accessible.name: root.tr("Send command to embedded terminal", "傳送命令到內嵌終端機")
                        onClicked: root.sendInput()
                    }
                }
            }
        }

        WfCard {
            Layout.fillWidth: true
            dark: root.dark
            fillColor: DesignTokens.errorContainer(root.dark)
            outlineColor: DesignTokens.error(root.dark)
            padding: root.compactHeight ? DesignTokens.spacing8 : DesignTokens.spacing12
            RowLayout {
                anchors.fill: parent
                spacing: DesignTokens.spacing8
                Label {
                    text: root.tr("ADMIN", "管理員")
                    color: DesignTokens.onErrorContainer(root.dark)
                    font.family: DesignTokens.fontMono
                    font.pixelSize: 10
                    font.weight: Font.Bold
                }
                Label {
                    Layout.fillWidth: true
                    text: root.compactHeight
                          ? root.tr("Administrator mode: review every command before sending.",
                                    "管理員模式：傳送前請先檢查每個指令。")
                          : root.tr("WimForge is elevated by default, so every terminal command runs as administrator. Review the command before sending it. The backend uses the documented Windows ConPTY API and does not embed Windows Terminal source.",
                                    "WimForge 預設會提升權限，所以每個終端機命令都會用系統管理員身份執行。傳送前請先檢查指令。後端使用 Windows 文件記載嘅 ConPTY API，並冇嵌入 Windows Terminal 原始碼。")
                    color: DesignTokens.onErrorContainer(root.dark)
                    font.family: DesignTokens.fontBody
                    font.pixelSize: 10
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Dialog {
        id: forceStopDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(520, root.width - 40)
        modal: true
        focus: true
        title: root.tr("Force-stop terminal process tree?", "強制停止終端機程序樹？")
        standardButtons: Dialog.Cancel | Dialog.Ok

        Label {
            width: parent.width
            text: root.tr("This immediately terminates the shell and every process it started. Unsaved command work can be lost.",
                          "呢個操作會即時終止 shell 同由佢啟動嘅所有程序。未儲存嘅命令工作可能會遺失。")
            color: root.surfaceForeground
            font.family: DesignTokens.fontBody
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
        onAccepted: root.terminal.forceStop()
    }
}
