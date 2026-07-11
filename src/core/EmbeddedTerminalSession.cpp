#include "EmbeddedTerminalSession.h"
#include "StructuredLogger.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
#endif

namespace wimforge {

namespace {

constexpr qsizetype kMinimumBufferBytes = 64;
constexpr qsizetype kMaximumBufferBytes = 64 * 1024 * 1024;

QString terminalStateName(EmbeddedTerminalSession::State state)
{
    using State = EmbeddedTerminalSession::State;
    switch (state) {
    case State::Idle: return QStringLiteral("idle");
    case State::Starting: return QStringLiteral("starting");
    case State::Running: return QStringLiteral("running");
    case State::Stopping: return QStringLiteral("stopping");
    case State::Exited: return QStringLiteral("exited");
    case State::Failed: return QStringLiteral("failed");
    case State::Unsupported: return QStringLiteral("unsupported");
    }
    return QStringLiteral("unknown");
}

QString terminalShellName(EmbeddedTerminalSession::Shell shell)
{
    using Shell = EmbeddedTerminalSession::Shell;
    switch (shell) {
    case Shell::DefaultShell: return QStringLiteral("default");
    case Shell::PowerShell: return QStringLiteral("powershell");
    case Shell::CommandPrompt: return QStringLiteral("command-prompt");
    }
    return QStringLiteral("unknown");
}

QString terminalExitStatusName(EmbeddedTerminalSession::ExitStatus status)
{
    using Status = EmbeddedTerminalSession::ExitStatus;
    switch (status) {
    case Status::NotExited: return QStringLiteral("not-exited");
    case Status::NormalExit: return QStringLiteral("normal-exit");
    case Status::Crashed: return QStringLiteral("crashed");
    case Status::Terminated: return QStringLiteral("terminated");
    }
    return QStringLiteral("unknown");
}

#ifdef Q_OS_WIN
constexpr int kMaximumTerminalDimension =
    static_cast<int>((std::numeric_limits<short>::max)());

bool containsNull(const QString &value)
{
    return value.contains(QChar(u'\0'));
}
#endif

qsizetype boundedBufferSize(qsizetype requested, qsizetype fallback)
{
    if (requested <= 0)
        return fallback;
    return std::clamp(requested, kMinimumBufferBytes, kMaximumBufferBytes);
}

bool isUtf8Continuation(char value)
{
    return (static_cast<unsigned char>(value) & 0xC0U) == 0x80U;
}

int expectedUtf8SequenceLength(unsigned char lead)
{
    if ((lead & 0x80U) == 0)
        return 1;
    if ((lead & 0xE0U) == 0xC0U)
        return 2;
    if ((lead & 0xF0U) == 0xE0U)
        return 3;
    if ((lead & 0xF8U) == 0xF0U)
        return 4;
    return 1;
}

qsizetype completeUtf8PrefixLength(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return 0;

    qsizetype leadIndex = bytes.size() - 1;
    while (leadIndex >= 0 && isUtf8Continuation(bytes.at(leadIndex)))
        --leadIndex;
    if (leadIndex < 0)
        return bytes.size();

    const int expected = expectedUtf8SequenceLength(
        static_cast<unsigned char>(bytes.at(leadIndex)));
    const qsizetype available = bytes.size() - leadIndex;
    return expected > available ? leadIndex : bytes.size();
}

#ifdef Q_OS_WIN

using CreatePseudoConsoleFunction = HRESULT(WINAPI *)(
    COORD, HANDLE, HANDLE, DWORD, HANDLE *);
using ResizePseudoConsoleFunction = HRESULT(WINAPI *)(HANDLE, COORD);
using ClosePseudoConsoleFunction = void(WINAPI *)(HANDLE);

struct ConPtyApi
{
    CreatePseudoConsoleFunction create = nullptr;
    ResizePseudoConsoleFunction resize = nullptr;
    ClosePseudoConsoleFunction close = nullptr;

    [[nodiscard]] bool available() const
    {
        return create != nullptr && resize != nullptr && close != nullptr;
    }
};

const ConPtyApi &conPtyApi()
{
    static const ConPtyApi api = [] {
        ConPtyApi loaded;
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel == nullptr)
            return loaded;
        loaded.create = reinterpret_cast<CreatePseudoConsoleFunction>(
            GetProcAddress(kernel, "CreatePseudoConsole"));
        loaded.resize = reinterpret_cast<ResizePseudoConsoleFunction>(
            GetProcAddress(kernel, "ResizePseudoConsole"));
        loaded.close = reinterpret_cast<ClosePseudoConsoleFunction>(
            GetProcAddress(kernel, "ClosePseudoConsole"));
        return loaded;
    }();
    return api;
}

QString windowsErrorMessage(DWORD errorCode)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t *>(&buffer),
        0,
        nullptr);
    QString message;
    if (length > 0 && buffer != nullptr)
        message = QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed();
    if (buffer != nullptr)
        LocalFree(buffer);
    if (message.isEmpty())
        message = QStringLiteral("Windows error %1").arg(errorCode);
    return message;
}

QString hresultMessage(HRESULT result)
{
    const quint32 value = static_cast<quint32>(result);
    return QStringLiteral("HRESULT 0x%1")
        .arg(value, 8, 16, QLatin1Char('0'));
}

QString normalizeFinalWindowsPath(QString path)
{
    path = QDir::fromNativeSeparators(path);
    if (path.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive))
        path = QStringLiteral("//") + path.mid(8);
    else if (path.startsWith(QStringLiteral("//?/"), Qt::CaseInsensitive))
        path = path.mid(4);
    return QDir::cleanPath(path);
}

QString finalPathForHandle(HANDLE handle, QString *error)
{
    const DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) {
        if (error != nullptr)
            *error = windowsErrorMessage(GetLastError());
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1U, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) {
        if (error != nullptr)
            *error = windowsErrorMessage(GetLastError());
        return {};
    }
    return normalizeFinalWindowsPath(
        QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length)));
}

QString systemDirectoryPath(QString *error)
{
    std::vector<wchar_t> buffer(32768U, L'\0');
    const UINT length = GetSystemDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        if (error != nullptr)
            *error = QStringLiteral("Unable to resolve the protected Windows system directory: %1")
                         .arg(windowsErrorMessage(GetLastError()));
        return {};
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(
        QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length))));
}

QString trustedSystemExecutable(const QString &relativePath, QString *error)
{
    QString systemError;
    const QString systemDirectory = systemDirectoryPath(&systemError);
    if (systemDirectory.isEmpty()) {
        if (error != nullptr)
            *error = systemError;
        return {};
    }

    const QString candidate = QDir(systemDirectory).absoluteFilePath(relativePath);
    const std::wstring systemNative = QDir::toNativeSeparators(systemDirectory).toStdWString();
    const std::wstring candidateNative = QDir::toNativeSeparators(candidate).toStdWString();

    HANDLE rootHandle = CreateFileW(
        systemNative.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (rootHandle == INVALID_HANDLE_VALUE) {
        if (error != nullptr)
            *error = QStringLiteral("Unable to validate the Windows system directory: %1")
                         .arg(windowsErrorMessage(GetLastError()));
        return {};
    }

    HANDLE executableHandle = CreateFileW(
        candidateNative.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (executableHandle == INVALID_HANDLE_VALUE) {
        const DWORD openError = GetLastError();
        CloseHandle(rootHandle);
        if (error != nullptr)
            *error = QStringLiteral("Trusted system shell is unavailable at %1: %2")
                         .arg(QDir::toNativeSeparators(candidate),
                              windowsErrorMessage(openError));
        return {};
    }

    FILE_ATTRIBUTE_TAG_INFO rootAttributes{};
    FILE_ATTRIBUTE_TAG_INFO executableAttributes{};
    const bool rootAttributesRead = GetFileInformationByHandleEx(
        rootHandle,
        FileAttributeTagInfo,
        &rootAttributes,
        sizeof(rootAttributes));
    const bool executableAttributesRead = GetFileInformationByHandleEx(
        executableHandle,
        FileAttributeTagInfo,
        &executableAttributes,
        sizeof(executableAttributes));

    QString rootFinalError;
    QString executableFinalError;
    const QString rootFinal = finalPathForHandle(rootHandle, &rootFinalError);
    const QString executableFinal = finalPathForHandle(
        executableHandle, &executableFinalError);
    CloseHandle(executableHandle);
    CloseHandle(rootHandle);

    if (!rootAttributesRead || !executableAttributesRead
        || (rootAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || (executableAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || (executableAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (error != nullptr)
            *error = QStringLiteral("The system shell path failed the no-reparse executable check.");
        return {};
    }
    if (rootFinal.isEmpty() || executableFinal.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to validate the final system shell path: %1")
                         .arg(rootFinal.isEmpty() ? rootFinalError : executableFinalError);
        }
        return {};
    }

    QString rootPrefix = rootFinal;
    if (!rootPrefix.endsWith(QLatin1Char('/')))
        rootPrefix.append(QLatin1Char('/'));
    if (!executableFinal.startsWith(rootPrefix, Qt::CaseInsensitive)) {
        if (error != nullptr)
            *error = QStringLiteral("The resolved shell escaped the protected Windows system directory.");
        return {};
    }
    return QDir::toNativeSeparators(executableFinal);
}

QString quoteWindowsArgument(const QString &argument)
{
    if (!argument.isEmpty()
        && !argument.contains(QLatin1Char(' '))
        && !argument.contains(QLatin1Char('\t'))
        && !argument.contains(QLatin1Char('"'))) {
        return argument;
    }

    QString quoted;
    quoted.reserve(argument.size() + 2);
    quoted.append(QLatin1Char('"'));
    qsizetype backslashes = 0;
    for (const QChar character : argument) {
        if (character == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (character == QLatin1Char('"')) {
            quoted.append(QString(backslashes * 2 + 1, QLatin1Char('\\')));
            quoted.append(character);
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            quoted.append(QString(backslashes, QLatin1Char('\\')));
            backslashes = 0;
        }
        quoted.append(character);
    }
    if (backslashes > 0)
        quoted.append(QString(backslashes * 2, QLatin1Char('\\')));
    quoted.append(QLatin1Char('"'));
    return quoted;
}

QString buildWindowsCommandLine(const QString &executable, const QStringList &arguments)
{
    QStringList tokens;
    tokens.reserve(arguments.size() + 1);
    tokens.append(quoteWindowsArgument(executable));
    for (const QString &argument : arguments)
        tokens.append(quoteWindowsArgument(argument));
    return tokens.join(QLatin1Char(' '));
}

#endif

} // namespace

struct EmbeddedTerminalSession::Impl
{
    std::atomic_bool destroying = false;
    std::atomic_bool suppressExitNotification = false;
    std::atomic_bool forced = false;

    std::mutex outputMutex;
    QByteArray pendingOutput;
    qsizetype maxPendingOutputBytes = 256 * 1024;
    quint64 pendingDiscardedBytes = 0;
    bool outputDrainScheduled = false;

#ifdef Q_OS_WIN
    std::mutex handleMutex;
    HANDLE pseudoConsole = nullptr;
    HANDLE inputWrite = nullptr;
    HANDLE outputRead = nullptr;
    HANDLE processHandle = nullptr;
    HANDLE jobHandle = nullptr;

    std::mutex inputMutex;
    std::condition_variable inputCondition;
    QByteArray pendingInput;
    qsizetype maxPendingInputBytes = 64 * 1024;
    bool stopWriter = false;
    bool closeInputWhenDrained = false;

    std::mutex readerMutex;
    std::condition_variable readerCondition;
    bool readerDone = true;

    std::thread readerThread;
    std::thread writerThread;
    std::thread waiterThread;
#endif
};

EmbeddedTerminalSession::EmbeddedTerminalSession(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("terminal"),
        QStringLiteral("terminal.lifecycle.created"),
        QStringLiteral("Embedded terminal session was created. / 內嵌終端機工作階段已經建立。"),
        QJsonObject{{QStringLiteral("state"), terminalStateName(m_state)},
                    {QStringLiteral("supported"), isSupported()}});
}

EmbeddedTerminalSession::~EmbeddedTerminalSession()
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("terminal"),
        QStringLiteral("terminal.lifecycle.stopping"),
        QStringLiteral("Embedded terminal session is stopping. / 內嵌終端機工作階段而家停止。"),
        QJsonObject{{QStringLiteral("state"), terminalStateName(m_state)},
                    {QStringLiteral("running"), running()},
                    {QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    m_impl->destroying.store(true);
    m_impl->suppressExitNotification.store(true);
#ifdef Q_OS_WIN
    m_impl->forced.store(true);
    {
        std::lock_guard lock(m_impl->inputMutex);
        m_impl->pendingInput.clear();
        m_impl->stopWriter = true;
    }
    m_impl->inputCondition.notify_all();

    {
        std::lock_guard lock(m_impl->handleMutex);
        if (m_impl->jobHandle != nullptr)
            TerminateJobObject(m_impl->jobHandle, static_cast<UINT>(0xC000013AU));
        else if (m_impl->processHandle != nullptr)
            TerminateProcess(m_impl->processHandle, static_cast<UINT>(0xC000013AU));
    }

    if (m_impl->waiterThread.joinable())
        m_impl->waiterThread.join();
    else
        cleanupNativeResources();

    if (m_impl->writerThread.joinable()) {
        CancelSynchronousIo(m_impl->writerThread.native_handle());
        m_impl->writerThread.join();
    }
    if (m_impl->readerThread.joinable()) {
        CancelSynchronousIo(m_impl->readerThread.native_handle());
        m_impl->readerThread.join();
    }
#endif
    cleanupNativeResources();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("terminal"),
        QStringLiteral("terminal.lifecycle.stopped"),
        QStringLiteral("Embedded terminal session stopped. / 內嵌終端機工作階段已經停止。"));
}

EmbeddedTerminalSession::State EmbeddedTerminalSession::state() const
{
    return m_state;
}

bool EmbeddedTerminalSession::running() const
{
    return m_state == State::Starting || m_state == State::Running
        || m_state == State::Stopping;
}

QString EmbeddedTerminalSession::transcript() const
{
    return m_transcript;
}

QString EmbeddedTerminalSession::displayTranscript() const
{
    return m_displayTranscript;
}

bool EmbeddedTerminalSession::transcriptTruncated() const
{
    return m_transcriptTruncated;
}

quint64 EmbeddedTerminalSession::droppedOutputBytes() const
{
    return m_droppedOutputBytes;
}

QString EmbeddedTerminalSession::errorString() const
{
    return m_errorString;
}

int EmbeddedTerminalSession::exitCode() const
{
    return m_exitCode;
}

EmbeddedTerminalSession::ExitStatus EmbeddedTerminalSession::exitStatus() const
{
    return m_exitStatus;
}

void EmbeddedTerminalSession::setState(State state)
{
    if (m_state == state)
        return;
    const State previous = m_state;
    m_state = state;
    StructuredLogger::instance().log(
        LogSeverity::Debug, QStringLiteral("terminal"),
        QStringLiteral("terminal.state.changed"),
        QStringLiteral("Embedded terminal state changed. / 內嵌終端機狀態已經改變。"),
        QJsonObject{{QStringLiteral("from"), terminalStateName(previous)},
                    {QStringLiteral("to"), terminalStateName(state)},
                    {QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    emit stateChanged();
}

void EmbeddedTerminalSession::setError(const QString &message, bool fatal)
{
    if (m_errorString != message) {
        m_errorString = message;
        emit errorChanged();
    }
    if (!message.isEmpty())
        emit errorOccurred(message);
    if (!message.isEmpty()) {
        StructuredLogger::instance().log(
            fatal ? LogSeverity::Error : LogSeverity::Warning,
            QStringLiteral("terminal"), QStringLiteral("terminal.failure"),
            QStringLiteral("An embedded terminal operation failed; diagnostic contents were omitted from logging. / 內嵌終端機操作失敗；記錄已經省略診斷內容。"),
            QJsonObject{{QStringLiteral("fatal"), fatal},
                        {QStringLiteral("state"), terminalStateName(m_state)},
                        {QStringLiteral("sessionGeneration"),
                         static_cast<qint64>(m_sessionGeneration)}});
    }
    if (fatal)
        setState(State::Failed);
}

bool EmbeddedTerminalSession::isSupported()
{
#ifdef Q_OS_WIN
    return conPtyApi().available();
#else
    return false;
#endif
}

bool EmbeddedTerminalSession::start()
{
    return start(StartOptions{});
}

QString EmbeddedTerminalSession::resolveShellExecutable(Shell shell, QString *error)
{
    if (error != nullptr)
        error->clear();
#ifdef Q_OS_WIN
    if (shell == Shell::DefaultShell || shell == Shell::PowerShell) {
        QString powershellError;
        const QString powershell = trustedSystemExecutable(
            QStringLiteral("WindowsPowerShell/v1.0/powershell.exe"),
            &powershellError);
        if (!powershell.isEmpty())
            return powershell;
        if (shell == Shell::PowerShell) {
            if (error != nullptr)
                *error = powershellError;
            return {};
        }
    }

    QString commandError;
    const QString commandPrompt = trustedSystemExecutable(
        QStringLiteral("cmd.exe"), &commandError);
    if (commandPrompt.isEmpty() && error != nullptr)
        *error = commandError;
    return commandPrompt;
#else
    Q_UNUSED(shell)
    if (error != nullptr)
        *error = QStringLiteral("Embedded ConPTY terminals are supported only on Windows.");
    return {};
#endif
}

bool EmbeddedTerminalSession::start(const StartOptions &options)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("terminal"),
        QStringLiteral("terminal.start.requested"),
        QStringLiteral("Embedded terminal startup was requested. / 已經要求啟動內嵌終端機。"),
        QJsonObject{
            // Never log argument values, executable paths, working-directory
            // paths, environment values, or terminal input/output contents.
            {QStringLiteral("shell"), terminalShellName(options.shell)},
            {QStringLiteral("argumentCount"), options.arguments.size()},
            {QStringLiteral("workingDirectoryProvided"),
             !options.workingDirectory.isEmpty()},
            {QStringLiteral("columns"), options.initialSize.width()},
            {QStringLiteral("rows"), options.initialSize.height()},
            {QStringLiteral("maxTranscriptBytes"),
             static_cast<qint64>(options.maxTranscriptBytes)},
            {QStringLiteral("maxPendingOutputBytes"),
             static_cast<qint64>(options.maxPendingOutputBytes)},
            {QStringLiteral("maxPendingInputBytes"),
             static_cast<qint64>(options.maxPendingInputBytes)},
        });
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                setError(QStringLiteral(
                             "Terminal sessions must be started from their owning thread."),
                         false);
            },
            Qt::QueuedConnection);
        return false;
    }
    if (running()) {
        setError(QStringLiteral("The embedded terminal is already running."), false);
        return false;
    }

    joinFinishedWorkers();
    cleanupNativeResources();

    m_impl->destroying.store(false);
    m_impl->suppressExitNotification.store(false);
    m_impl->forced.store(false);
    ++m_sessionGeneration;
    m_impl->maxPendingOutputBytes = boundedBufferSize(
        options.maxPendingOutputBytes, 256 * 1024);
    m_maxTranscriptBytes = boundedBufferSize(
        options.maxTranscriptBytes, 1024 * 1024);
    {
        std::lock_guard lock(m_impl->outputMutex);
        m_impl->pendingOutput.clear();
        m_impl->pendingDiscardedBytes = 0;
        m_impl->outputDrainScheduled = false;
    }
    m_utf8Carry.clear();
    m_transcript.clear();
    m_displayTranscript.clear();
    m_displayEscapeCarry.clear();
    m_transcriptTruncated = false;
    m_droppedOutputBytes = 0;
    m_exitCode = -1;
    m_exitStatus = ExitStatus::NotExited;
    m_errorString.clear();
    emit transcriptChanged();
    emit displayTranscriptChanged();
    emit transcriptTruncatedChanged();
    emit droppedOutputBytesChanged();
    emit exitChanged();
    emit errorChanged();
    setState(State::Starting);

#ifndef Q_OS_WIN
    Q_UNUSED(options)
    const QString unsupported =
        QStringLiteral("Embedded ConPTY terminals are supported only on Windows.");
    m_errorString = unsupported;
    emit errorChanged();
    emit errorOccurred(unsupported);
    setState(State::Unsupported);
    StructuredLogger::instance().log(
        LogSeverity::Warning, QStringLiteral("terminal"),
        QStringLiteral("terminal.start.unsupported"),
        QStringLiteral("Embedded terminal startup is unsupported on this platform. / 呢個平台唔支援啟動內嵌終端機。"));
    return false;
#else
    if (!conPtyApi().available()) {
        const QString unsupported = QStringLiteral(
            "The Windows ConPTY API is unavailable. Windows 10 version 1809 or newer is required.");
        m_errorString = unsupported;
        emit errorChanged();
        emit errorOccurred(unsupported);
        setState(State::Unsupported);
        StructuredLogger::instance().log(
            LogSeverity::Warning, QStringLiteral("terminal"),
            QStringLiteral("terminal.start.unsupported"),
            QStringLiteral("Windows ConPTY is unavailable. / Windows ConPTY 而家用唔到。"));
        return false;
    }

    if (options.initialSize.width() <= 0 || options.initialSize.height() <= 0
        || options.initialSize.width() > kMaximumTerminalDimension
        || options.initialSize.height() > kMaximumTerminalDimension) {
        setError(QStringLiteral("Terminal dimensions must be between 1 and 32767."), true);
        return false;
    }

    QString executableError;
    const QString executable = resolveShellExecutable(options.shell, &executableError);
    if (executable.isEmpty()) {
        setError(executableError.isEmpty()
                     ? QStringLiteral("No trusted Windows system shell is available.")
                     : executableError,
                 true);
        return false;
    }

    QStringList arguments = options.arguments;
    if (arguments.isEmpty()) {
        if (executable.endsWith(QStringLiteral("powershell.exe"), Qt::CaseInsensitive))
            arguments = {QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile")};
        else
            arguments = {QStringLiteral("/Q"), QStringLiteral("/D")};
    }
    if (std::any_of(arguments.cbegin(), arguments.cend(), containsNull)) {
        setError(QStringLiteral("Terminal arguments cannot contain null characters."), true);
        return false;
    }

    QString workingDirectory = options.workingDirectory;
    if (workingDirectory.isEmpty())
        workingDirectory = QDir::currentPath();
    if (containsNull(workingDirectory)) {
        setError(QStringLiteral("The terminal working directory contains a null character."), true);
        return false;
    }
    const QFileInfo workingInfo(workingDirectory);
    if (!workingInfo.isAbsolute() || !workingInfo.exists() || !workingInfo.isDir()) {
        setError(QStringLiteral("The terminal working directory must be an existing absolute directory."),
                 true);
        return false;
    }
    workingDirectory = workingInfo.canonicalFilePath();
    if (workingDirectory.isEmpty()) {
        setError(QStringLiteral("Unable to resolve the terminal working directory."), true);
        return false;
    }

    HANDLE pseudoInputRead = nullptr;
    HANDLE terminalInputWrite = nullptr;
    HANDLE terminalOutputRead = nullptr;
    HANDLE pseudoOutputWrite = nullptr;
    HANDLE pseudoConsole = nullptr;
    HANDLE jobHandle = nullptr;
    PROCESS_INFORMATION processInfo{};

    const auto closeIfValid = [](HANDLE &handle) {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        handle = nullptr;
    };
    const auto failStart = [this](const QString &message,
                                  HANDLE &inputRead,
                                  HANDLE &inputWrite,
                                  HANDLE &outputRead,
                                  HANDLE &outputWrite,
                                  HANDLE &console,
                                  HANDLE &job,
                                  PROCESS_INFORMATION &process) {
        if (process.hProcess != nullptr) {
            TerminateProcess(process.hProcess, static_cast<UINT>(0xC000013AU));
            WaitForSingleObject(process.hProcess, 5000);
        }
        if (process.hThread != nullptr)
            CloseHandle(process.hThread);
        if (process.hProcess != nullptr)
            CloseHandle(process.hProcess);
        process = {};
        if (job != nullptr)
            CloseHandle(job);
        job = nullptr;
        if (console != nullptr)
            conPtyApi().close(console);
        console = nullptr;
        for (HANDLE *handle : {&inputRead, &inputWrite, &outputRead, &outputWrite}) {
            if (*handle != nullptr && *handle != INVALID_HANDLE_VALUE)
                CloseHandle(*handle);
            *handle = nullptr;
        }
        setError(message, true);
        return false;
    };

    if (!CreatePipe(&pseudoInputRead, &terminalInputWrite, nullptr, 0)) {
        return failStart(
            QStringLiteral("Unable to create the ConPTY input pipe: %1")
                .arg(windowsErrorMessage(GetLastError())),
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }
    if (!CreatePipe(&terminalOutputRead, &pseudoOutputWrite, nullptr, 0)) {
        return failStart(
            QStringLiteral("Unable to create the ConPTY output pipe: %1")
                .arg(windowsErrorMessage(GetLastError())),
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }

    const COORD initialSize{
        static_cast<SHORT>(options.initialSize.width()),
        static_cast<SHORT>(options.initialSize.height())};
    const HRESULT createResult = conPtyApi().create(
        initialSize, pseudoInputRead, pseudoOutputWrite, 0, &pseudoConsole);
    if (FAILED(createResult)) {
        return failStart(
            QStringLiteral("Unable to create the Windows pseudoconsole: %1")
                .arg(hresultMessage(createResult)),
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributeStorage(attributeBytes);
    auto *attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) {
        return failStart(
            QStringLiteral("Unable to initialize the ConPTY process attributes: %1")
                .arg(windowsErrorMessage(GetLastError())),
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }

    if (!UpdateProcThreadAttribute(
            attributeList,
            0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            pseudoConsole,
            sizeof(pseudoConsole),
            nullptr,
            nullptr)) {
        const QString message =
            QStringLiteral("Unable to attach the ConPTY process attribute: %1")
                .arg(windowsErrorMessage(GetLastError()));
        DeleteProcThreadAttributeList(attributeList);
        return failStart(
            message,
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }

    jobHandle = CreateJobObjectW(nullptr, nullptr);
    if (jobHandle == nullptr) {
        const QString message = QStringLiteral("Unable to create the terminal process job: %1")
                                    .arg(windowsErrorMessage(GetLastError()));
        DeleteProcThreadAttributeList(attributeList);
        return failStart(
            message,
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            jobHandle,
            JobObjectExtendedLimitInformation,
            &jobLimits,
            sizeof(jobLimits))) {
        const QString message = QStringLiteral("Unable to secure the terminal process job: %1")
                                    .arg(windowsErrorMessage(GetLastError()));
        DeleteProcThreadAttributeList(attributeList);
        return failStart(
            message,
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Explicitly provide null standard handles. Without this flag Windows can
    // copy redirected parent stdio (for example under CTest) into the child,
    // bypassing ConPTY even though the pseudoconsole attribute is present.
    // Null handles are replaced with the new pseudoconsole's CONIN$/CONOUT$
    // handles as the console client attaches.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = nullptr;
    startup.StartupInfo.hStdError = nullptr;
    startup.lpAttributeList = attributeList;
    QString commandLine = buildWindowsCommandLine(executable, arguments);
    std::wstring mutableCommandLine = commandLine.toStdWString();
    const std::wstring executableNative = executable.toStdWString();
    const std::wstring workingNative = QDir::toNativeSeparators(workingDirectory).toStdWString();
    // The pseudoconsole process attribute is the supported no-window hosting
    // mechanism. CREATE_NO_WINDOW conflicts with console attachment and must
    // not be combined with a ConPTY client process.
    const DWORD creationFlags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    const BOOL created = CreateProcessW(
        executableNative.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        creationFlags,
        nullptr,
        workingNative.c_str(),
        &startup.StartupInfo,
        &processInfo);
    const DWORD createProcessError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributeList);
    if (!created) {
        return failStart(
            QStringLiteral("Unable to start the trusted system shell: %1")
                .arg(windowsErrorMessage(createProcessError)),
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }
    if (!AssignProcessToJobObject(jobHandle, processInfo.hProcess)) {
        return failStart(
            QStringLiteral("Unable to contain the terminal process in its job: %1")
                .arg(windowsErrorMessage(GetLastError())),
            pseudoInputRead,
            terminalInputWrite,
            terminalOutputRead,
            pseudoOutputWrite,
            pseudoConsole,
            jobHandle,
            processInfo);
    }

    {
        std::lock_guard lock(m_impl->inputMutex);
        m_impl->pendingInput.clear();
        m_impl->maxPendingInputBytes = boundedBufferSize(
            options.maxPendingInputBytes, 64 * 1024);
        m_impl->stopWriter = false;
        m_impl->closeInputWhenDrained = false;
    }
    {
        std::lock_guard lock(m_impl->readerMutex);
        m_impl->readerDone = false;
    }
    {
        std::lock_guard lock(m_impl->handleMutex);
        m_impl->pseudoConsole = pseudoConsole;
        m_impl->inputWrite = terminalInputWrite;
        m_impl->outputRead = terminalOutputRead;
        m_impl->processHandle = processInfo.hProcess;
        m_impl->jobHandle = jobHandle;
    }
    pseudoConsole = nullptr;
    terminalInputWrite = nullptr;
    terminalOutputRead = nullptr;
    jobHandle = nullptr;

    const HANDLE readerHandle = m_impl->outputRead;
    const HANDLE writerHandle = m_impl->inputWrite;
    const HANDLE processHandle = m_impl->processHandle;
    try {
        m_impl->readerThread = std::thread([this, readerHandle] {
            std::vector<char> buffer(16U * 1024U);
            for (;;) {
                DWORD bytesRead = 0;
                const BOOL read = ReadFile(
                    readerHandle,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &bytesRead,
                    nullptr);
                if (read && bytesRead > 0) {
                    enqueueOutputFromWorker(QByteArray(
                        buffer.data(), static_cast<qsizetype>(bytesRead)));
                    continue;
                }
                if (!read) {
                    const DWORD errorCode = GetLastError();
                    if (errorCode != ERROR_BROKEN_PIPE
                        && errorCode != ERROR_OPERATION_ABORTED
                        && !m_impl->destroying.load()) {
                        reportWorkerError(
                            QStringLiteral("ConPTY output failed: %1")
                                .arg(windowsErrorMessage(errorCode)));
                    }
                }
                break;
            }
            {
                std::lock_guard lock(m_impl->handleMutex);
                if (m_impl->outputRead == readerHandle)
                    m_impl->outputRead = nullptr;
            }
            CloseHandle(readerHandle);
            {
                std::lock_guard lock(m_impl->readerMutex);
                m_impl->readerDone = true;
            }
            m_impl->readerCondition.notify_all();
        });

        m_impl->writerThread = std::thread([this, writerHandle] {
            for (;;) {
                QByteArray chunk;
                bool closeAfterChunk = false;
                {
                    std::unique_lock lock(m_impl->inputMutex);
                    m_impl->inputCondition.wait(lock, [this] {
                        return m_impl->stopWriter
                            || !m_impl->pendingInput.isEmpty()
                            || m_impl->closeInputWhenDrained;
                    });
                    if (m_impl->stopWriter) {
                        m_impl->pendingInput.clear();
                        break;
                    }
                    const qsizetype chunkBytes = std::min<qsizetype>(
                        m_impl->pendingInput.size(), 16 * 1024);
                    chunk = m_impl->pendingInput.left(chunkBytes);
                    m_impl->pendingInput.remove(0, chunkBytes);
                    closeAfterChunk = m_impl->closeInputWhenDrained
                        && m_impl->pendingInput.isEmpty();
                    if (chunk.isEmpty() && m_impl->closeInputWhenDrained)
                        break;
                }

                qsizetype offset = 0;
                while (offset < chunk.size()) {
                    DWORD bytesWritten = 0;
                    const DWORD requestBytes = static_cast<DWORD>(
                        std::min<qsizetype>(chunk.size() - offset,
                                            (std::numeric_limits<DWORD>::max)()));
                    if (!WriteFile(
                            writerHandle,
                            chunk.constData() + offset,
                            requestBytes,
                            &bytesWritten,
                            nullptr)) {
                        const DWORD errorCode = GetLastError();
                        if (errorCode != ERROR_BROKEN_PIPE
                            && errorCode != ERROR_NO_DATA
                            && errorCode != ERROR_OPERATION_ABORTED
                            && !m_impl->destroying.load()) {
                            reportWorkerError(
                                QStringLiteral("ConPTY input failed: %1")
                                    .arg(windowsErrorMessage(errorCode)));
                        }
                        offset = chunk.size();
                        closeAfterChunk = true;
                        break;
                    }
                    if (bytesWritten == 0) {
                        closeAfterChunk = true;
                        break;
                    }
                    offset += static_cast<qsizetype>(bytesWritten);
                }
                if (closeAfterChunk)
                    break;
            }
            {
                std::lock_guard lock(m_impl->handleMutex);
                if (m_impl->inputWrite == writerHandle)
                    m_impl->inputWrite = nullptr;
            }
            CloseHandle(writerHandle);
        });

        m_impl->waiterThread = std::thread([this, processHandle] {
            WaitForSingleObject(processHandle, INFINITE);
            DWORD nativeExitCode = static_cast<DWORD>(-1);
            GetExitCodeProcess(processHandle, &nativeExitCode);

            {
                std::lock_guard lock(m_impl->inputMutex);
                m_impl->pendingInput.clear();
                m_impl->stopWriter = true;
            }
            m_impl->inputCondition.notify_all();

            HANDLE job = nullptr;
            HANDLE console = nullptr;
            {
                std::lock_guard lock(m_impl->handleMutex);
                job = m_impl->jobHandle;
                m_impl->jobHandle = nullptr;
                console = m_impl->pseudoConsole;
                m_impl->pseudoConsole = nullptr;
                if (m_impl->processHandle == processHandle)
                    m_impl->processHandle = nullptr;
            }
            if (job != nullptr)
                CloseHandle(job);
            if (console != nullptr)
                conPtyApi().close(console);

            {
                std::unique_lock lock(m_impl->readerMutex);
                m_impl->readerCondition.wait(lock, [this] {
                    return m_impl->readerDone;
                });
            }
            CloseHandle(processHandle);

            if (!m_impl->destroying.load()
                && !m_impl->suppressExitNotification.load()) {
                const bool wasForced = m_impl->forced.load();
                QMetaObject::invokeMethod(
                    this,
                    [this, nativeExitCode, wasForced] {
                        handleNativeExit(nativeExitCode, wasForced);
                    },
                    Qt::QueuedConnection);
            }
        });
    } catch (const std::system_error &exception) {
        m_impl->suppressExitNotification.store(true);
        {
            std::lock_guard lock(m_impl->handleMutex);
            if (m_impl->jobHandle != nullptr)
                TerminateJobObject(m_impl->jobHandle, static_cast<UINT>(0xC000013AU));
            else if (m_impl->processHandle != nullptr)
                TerminateProcess(m_impl->processHandle, static_cast<UINT>(0xC000013AU));
        }
        {
            std::lock_guard lock(m_impl->inputMutex);
            m_impl->stopWriter = true;
            m_impl->pendingInput.clear();
        }
        m_impl->inputCondition.notify_all();
        if (!m_impl->waiterThread.joinable()) {
            HANDLE console = nullptr;
            {
                std::lock_guard lock(m_impl->handleMutex);
                console = m_impl->pseudoConsole;
                m_impl->pseudoConsole = nullptr;
            }
            if (console != nullptr)
                conPtyApi().close(console);
        }
        joinFinishedWorkers();
        cleanupNativeResources();
        closeIfValid(pseudoInputRead);
        closeIfValid(pseudoOutputWrite);
        setError(QStringLiteral("Unable to create terminal worker threads: %1")
                     .arg(QString::fromLocal8Bit(exception.what())),
                 true);
        closeIfValid(processInfo.hThread);
        return false;
    }

    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        const QString resumeError = windowsErrorMessage(GetLastError());
        m_impl->suppressExitNotification.store(true);
        {
            std::lock_guard lock(m_impl->handleMutex);
            if (m_impl->jobHandle != nullptr)
                TerminateJobObject(m_impl->jobHandle, static_cast<UINT>(0xC000013AU));
            else if (m_impl->processHandle != nullptr)
                TerminateProcess(m_impl->processHandle, static_cast<UINT>(0xC000013AU));
        }
        closeIfValid(processInfo.hThread);
        joinFinishedWorkers();
        cleanupNativeResources();
        closeIfValid(pseudoInputRead);
        closeIfValid(pseudoOutputWrite);
        setError(QStringLiteral("Unable to resume the terminal process: %1").arg(resumeError),
                 true);
        return false;
    }
    closeIfValid(processInfo.hThread);
    // The channel ends supplied to CreatePseudoConsole remain open through
    // hosted process creation, as required by the ConPTY lifecycle contract.
    closeIfValid(pseudoInputRead);
    closeIfValid(pseudoOutputWrite);
    setState(State::Running);
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("terminal"),
        QStringLiteral("terminal.started"),
        QStringLiteral("Embedded terminal started. / 內嵌終端機已經啟動。"),
        QJsonObject{{QStringLiteral("shell"), terminalShellName(options.shell)},
                    {QStringLiteral("columns"), options.initialSize.width()},
                    {QStringLiteral("rows"), options.initialSize.height()},
                    {QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    return true;
#endif
}

bool EmbeddedTerminalSession::startShell(const QString &shellName,
                                         const QString &workingDirectory,
                                         int columns,
                                         int rows)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                setError(QStringLiteral(
                             "Terminal sessions must be started from their owning thread."),
                         false);
            },
            Qt::QueuedConnection);
        return false;
    }
    const QString normalized = shellName.trimmed().toLower();
    Shell shell = Shell::DefaultShell;
    if (normalized.isEmpty() || normalized == QStringLiteral("default")) {
        shell = Shell::DefaultShell;
    } else if (normalized == QStringLiteral("powershell")
               || normalized == QStringLiteral("windows-powershell")) {
        shell = Shell::PowerShell;
    } else if (normalized == QStringLiteral("cmd")
               || normalized == QStringLiteral("command-prompt")) {
        shell = Shell::CommandPrompt;
    } else {
        setError(QStringLiteral("Unknown terminal shell '%1'. Use default, powershell, or cmd.")
                     .arg(shellName),
                 false);
        return false;
    }

    StartOptions options;
    options.shell = shell;
    options.workingDirectory = workingDirectory;
    options.initialSize = QSize(columns, rows);
    return start(options);
}

bool EmbeddedTerminalSession::writeInput(const QString &text)
{
    StructuredLogger::instance().log(
        LogSeverity::Trace, QStringLiteral("terminal"),
        QStringLiteral("terminal.input.requested"),
        QStringLiteral("Embedded terminal input metadata was received. / 已經收到內嵌終端機輸入嘅中繼資料。"),
        QJsonObject{{QStringLiteral("characterCount"), text.size()},
                    {QStringLiteral("state"), terminalStateName(m_state)},
                    {QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                setError(QStringLiteral(
                             "Terminal input must be submitted from the owning thread."),
                         false);
            },
            Qt::QueuedConnection);
        return false;
    }
#ifndef Q_OS_WIN
    Q_UNUSED(text)
    setError(QStringLiteral("Embedded terminal input is unsupported on this platform."), false);
    return false;
#else
    if (m_state != State::Running) {
        setError(QStringLiteral("Terminal input is accepted only while the session is running."),
                 false);
        return false;
    }
    if (text.size() > m_impl->maxPendingInputBytes) {
        setError(QStringLiteral("The bounded terminal input queue is full."), false);
        return false;
    }
    const QByteArray bytes = text.toUtf8();
    if (bytes.isEmpty()) {
        StructuredLogger::instance().log(
            LogSeverity::Trace, QStringLiteral("terminal"),
            QStringLiteral("terminal.input.queued"),
            QStringLiteral("Empty terminal input required no queue write. / 空白終端機輸入唔需要寫入佇列。"),
            QJsonObject{{QStringLiteral("utf8Bytes"), 0}});
        return true;
    }

    QString rejection;
    {
        std::lock_guard lock(m_impl->inputMutex);
        if (m_impl->stopWriter || m_impl->closeInputWhenDrained) {
            rejection = QStringLiteral("The terminal input stream is closing.");
        } else if (bytes.size() > m_impl->maxPendingInputBytes
                   || m_impl->pendingInput.size()
                           > m_impl->maxPendingInputBytes - bytes.size()) {
            rejection = QStringLiteral("The bounded terminal input queue is full.");
        } else {
            m_impl->pendingInput.append(bytes);
        }
    }
    if (!rejection.isEmpty()) {
        setError(rejection, false);
        return false;
    }
    m_impl->inputCondition.notify_one();
    StructuredLogger::instance().log(
        LogSeverity::Trace, QStringLiteral("terminal"),
        QStringLiteral("terminal.input.queued"),
        QStringLiteral("Terminal input was queued without logging its contents. / 終端機輸入已經加入佇列，記錄冇保存內容。"),
        QJsonObject{{QStringLiteral("characterCount"), text.size()},
                    {QStringLiteral("utf8Bytes"), bytes.size()},
                    {QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    return true;
#endif
}

bool EmbeddedTerminalSession::resize(int columns, int rows)
{
    StructuredLogger::instance().log(
        LogSeverity::Debug, QStringLiteral("terminal"),
        QStringLiteral("terminal.resize.requested"),
        QStringLiteral("Embedded terminal resize was requested. / 已經要求調整內嵌終端機大小。"),
        QJsonObject{{QStringLiteral("columns"), columns},
                    {QStringLiteral("rows"), rows},
                    {QStringLiteral("state"), terminalStateName(m_state)}});
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                setError(QStringLiteral(
                             "Terminal resize must be requested from the owning thread."),
                         false);
            },
            Qt::QueuedConnection);
        return false;
    }
#ifndef Q_OS_WIN
    Q_UNUSED(columns)
    Q_UNUSED(rows)
    setError(QStringLiteral("Embedded terminal resizing is unsupported on this platform."), false);
    return false;
#else
    if (!running()) {
        setError(QStringLiteral("The terminal must be running before it can be resized."), false);
        return false;
    }
    if (columns <= 0 || rows <= 0
        || columns > kMaximumTerminalDimension
        || rows > kMaximumTerminalDimension) {
        setError(QStringLiteral("Terminal dimensions must be between 1 and 32767."), false);
        return false;
    }

    HRESULT result = E_HANDLE;
    {
        std::lock_guard lock(m_impl->handleMutex);
        if (m_impl->pseudoConsole != nullptr) {
            result = conPtyApi().resize(
                m_impl->pseudoConsole,
                COORD{static_cast<SHORT>(columns), static_cast<SHORT>(rows)});
        }
    }
    if (FAILED(result)) {
        setError(QStringLiteral("Unable to resize the Windows pseudoconsole: %1")
                     .arg(hresultMessage(result)),
                 false);
        return false;
    }
    StructuredLogger::instance().log(
        LogSeverity::Debug, QStringLiteral("terminal"),
        QStringLiteral("terminal.resize.completed"),
        QStringLiteral("Embedded terminal resize completed. / 內嵌終端機大小調整完成。"),
        QJsonObject{{QStringLiteral("columns"), columns},
                    {QStringLiteral("rows"), rows}});
    return true;
#endif
}

void EmbeddedTerminalSession::stopGracefully(int forceAfterMs)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, forceAfterMs] { stopGracefully(forceAfterMs); },
            Qt::QueuedConnection);
        return;
    }
#ifdef Q_OS_WIN
    if (m_state != State::Running)
        return;
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("terminal"),
        QStringLiteral("terminal.stop.requested"),
        QStringLiteral("Graceful terminal stop was requested. / 已經要求正常停止終端機。"),
        QJsonObject{{QStringLiteral("forceAfterMs"),
                     std::clamp(forceAfterMs, 0, 60'000)},
                    {QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    setState(State::Stopping);
    const QByteArray exitCommand("exit\r\n");
    {
        std::lock_guard lock(m_impl->inputMutex);
        if (!m_impl->stopWriter
            && exitCommand.size() <= m_impl->maxPendingInputBytes
            && m_impl->pendingInput.size()
                    <= m_impl->maxPendingInputBytes - exitCommand.size()) {
            m_impl->pendingInput.append(exitCommand);
        }
        // Keep the ConPTY input channel alive while the shell consumes the
        // command. Closing it immediately can make a just-started console
        // client terminate with an NTSTATUS instead of performing a normal
        // shell exit. The bounded timer below remains the hard fallback.
        m_impl->closeInputWhenDrained = false;
    }
    m_impl->inputCondition.notify_one();

    const int boundedTimeout = std::clamp(forceAfterMs, 0, 60'000);
    const quint64 generation = m_sessionGeneration;
    QTimer::singleShot(boundedTimeout, this, [this, generation] {
        if (generation == m_sessionGeneration && m_state == State::Stopping)
            forceStop();
    });
#else
    Q_UNUSED(forceAfterMs)
#endif
}

void EmbeddedTerminalSession::forceStop()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] { forceStop(); },
            Qt::QueuedConnection);
        return;
    }
#ifdef Q_OS_WIN
    if (!running())
        return;
    StructuredLogger::instance().log(
        LogSeverity::Warning, QStringLiteral("terminal"),
        QStringLiteral("terminal.force-stop.requested"),
        QStringLiteral("Forced terminal stop was requested. / 已經要求強制停止終端機。"),
        QJsonObject{{QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    m_impl->forced.store(true);
    setState(State::Stopping);

    BOOL terminated = FALSE;
    DWORD errorCode = ERROR_SUCCESS;
    {
        std::lock_guard lock(m_impl->handleMutex);
        if (m_impl->jobHandle != nullptr)
            terminated = TerminateJobObject(
                m_impl->jobHandle, static_cast<UINT>(0xC000013AU));
        else if (m_impl->processHandle != nullptr)
            terminated = TerminateProcess(
                m_impl->processHandle, static_cast<UINT>(0xC000013AU));
        if (!terminated)
            errorCode = GetLastError();
    }
    if (!terminated && errorCode != ERROR_ACCESS_DENIED
        && errorCode != ERROR_INVALID_HANDLE) {
        setError(QStringLiteral("Unable to terminate the embedded terminal: %1")
                     .arg(windowsErrorMessage(errorCode)),
                 false);
    }
#endif
}

void EmbeddedTerminalSession::clearTranscript()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] { clearTranscript(); },
            Qt::QueuedConnection);
        return;
    }
    const bool hadTranscript = !m_transcript.isEmpty();
    const bool hadDisplayTranscript = !m_displayTranscript.isEmpty();
    const bool wasTruncated = m_transcriptTruncated;
    const qsizetype storedCharacters = m_transcript.size();
    m_transcript.clear();
    m_displayTranscript.clear();
    m_displayEscapeCarry.clear();
    m_transcriptTruncated = false;
    if (hadTranscript)
        emit transcriptChanged();
    if (hadDisplayTranscript)
        emit displayTranscriptChanged();
    if (wasTruncated)
        emit transcriptTruncatedChanged();
    StructuredLogger::instance().log(
        LogSeverity::Debug, QStringLiteral("terminal"),
        QStringLiteral("terminal.transcript.cleared"),
        QStringLiteral("The embedded terminal transcript was cleared. / 內嵌終端機記錄已經清除。"),
        QJsonObject{{QStringLiteral("storedCharacterCount"), storedCharacters},
                    {QStringLiteral("wasTruncated"), wasTruncated}});
}

void EmbeddedTerminalSession::enqueueOutputFromWorker(const QByteArray &bytes)
{
    if (bytes.isEmpty() || m_impl->destroying.load())
        return;

    bool scheduleDrain = false;
    {
        std::lock_guard lock(m_impl->outputMutex);
        m_impl->pendingOutput.append(bytes);
        if (m_impl->pendingOutput.size() > m_impl->maxPendingOutputBytes) {
            qsizetype removeBytes =
                m_impl->pendingOutput.size() - m_impl->maxPendingOutputBytes;
            m_impl->pendingOutput.remove(0, removeBytes);
            while (!m_impl->pendingOutput.isEmpty()
                   && isUtf8Continuation(m_impl->pendingOutput.front())) {
                m_impl->pendingOutput.remove(0, 1);
                ++removeBytes;
            }
            m_impl->pendingDiscardedBytes += static_cast<quint64>(removeBytes);
        }
        if (!m_impl->outputDrainScheduled) {
            m_impl->outputDrainScheduled = true;
            scheduleDrain = true;
        }
    }

    if (scheduleDrain) {
        QMetaObject::invokeMethod(
            this,
            [this] { drainOutput(); },
            Qt::QueuedConnection);
    }
}

void EmbeddedTerminalSession::reportWorkerError(const QString &message)
{
    if (m_impl->destroying.load())
        return;
    QMetaObject::invokeMethod(
        this,
        [this, message] { setError(message, false); },
        Qt::QueuedConnection);
}

void EmbeddedTerminalSession::drainOutput()
{
    QByteArray bytes;
    quint64 discarded = 0;
    {
        std::lock_guard lock(m_impl->outputMutex);
        bytes.swap(m_impl->pendingOutput);
        discarded = std::exchange(m_impl->pendingDiscardedBytes, 0);
        m_impl->outputDrainScheduled = false;
    }

    if (discarded > 0) {
        m_utf8Carry.clear();
        m_displayEscapeCarry.clear();
        m_droppedOutputBytes += discarded;
        emit droppedOutputBytesChanged();
        emit outputDiscarded(m_droppedOutputBytes);
        StructuredLogger::instance().log(
            LogSeverity::Warning, QStringLiteral("terminal"),
            QStringLiteral("terminal.output.discarded"),
            QStringLiteral("Bounded terminal output was discarded. / 有界終端機輸出有部分已經丟棄。"),
            QJsonObject{{QStringLiteral("discardedBytes"),
                         static_cast<qint64>(discarded)},
                        {QStringLiteral("totalDiscardedBytes"),
                         static_cast<qint64>(m_droppedOutputBytes)}});
    }
    if (bytes.isEmpty())
        return;

    bytes.prepend(m_utf8Carry);
    const qsizetype prefixBytes = completeUtf8PrefixLength(bytes);
    m_utf8Carry = bytes.mid(prefixBytes);
    if (prefixBytes <= 0)
        return;

    const QString output = QString::fromUtf8(bytes.constData(), prefixBytes);
    if (output.isEmpty())
        return;
    emit outputReceived(output);
    appendTranscript(output);
    appendDisplayOutput(output);
}

void EmbeddedTerminalSession::appendTranscript(const QString &text)
{
    QByteArray transcriptBytes = (m_transcript + text).toUtf8();
    bool newlyTruncated = false;
    if (transcriptBytes.size() > m_maxTranscriptBytes) {
        qsizetype removeBytes = transcriptBytes.size() - m_maxTranscriptBytes;
        transcriptBytes.remove(0, removeBytes);
        while (!transcriptBytes.isEmpty()
               && isUtf8Continuation(transcriptBytes.front())) {
            transcriptBytes.remove(0, 1);
        }
        newlyTruncated = true;
    }
    m_transcript = QString::fromUtf8(transcriptBytes);
    emit transcriptChanged();
    if (newlyTruncated && !m_transcriptTruncated) {
        m_transcriptTruncated = true;
        emit transcriptTruncatedChanged();
    }
}

void EmbeddedTerminalSession::appendDisplayOutput(const QString &utf8Vt)
{
    // This is a deliberately small, streaming VT-to-plain-text projection for
    // basic TextArea use. The raw output signal remains authoritative for a
    // real terminal renderer. Incomplete escape strings are carried across
    // reads and capped so malformed output cannot grow memory without bound.
    QString input = m_displayEscapeCarry + utf8Vt;
    m_displayEscapeCarry.clear();
    QString plain;
    plain.reserve(input.size());
    bool storedDisplayChanged = false;

    qsizetype index = 0;
    while (index < input.size()) {
        const QChar character = input.at(index);
        if (character == QChar(0x009B)) {
            qsizetype cursor = index + 1;
            while (cursor < input.size()) {
                const ushort code = input.at(cursor).unicode();
                if (code >= 0x40U && code <= 0x7EU)
                    break;
                ++cursor;
            }
            if (cursor >= input.size()) {
                m_displayEscapeCarry = input.mid(index).left(256);
                break;
            }
            index = cursor + 1;
            continue;
        }
        if (character == QChar(0x009D)) {
            qsizetype cursor = index + 1;
            bool complete = false;
            while (cursor < input.size()) {
                if (input.at(cursor) == QChar(0x07)
                    || input.at(cursor) == QChar(0x009C)) {
                    ++cursor;
                    complete = true;
                    break;
                }
                if (input.at(cursor) == QChar(0x1B)
                    && cursor + 1 < input.size()
                    && input.at(cursor + 1) == QLatin1Char('\\')) {
                    cursor += 2;
                    complete = true;
                    break;
                }
                ++cursor;
            }
            if (!complete) {
                m_displayEscapeCarry = input.mid(index).left(256);
                break;
            }
            index = cursor;
            continue;
        }
        if (character == QChar(0x0090)
            || character == QChar(0x0098)
            || character == QChar(0x009E)
            || character == QChar(0x009F)) {
            qsizetype cursor = index + 1;
            bool complete = false;
            while (cursor < input.size()) {
                if (input.at(cursor) == QChar(0x009C)) {
                    ++cursor;
                    complete = true;
                    break;
                }
                if (input.at(cursor) == QChar(0x1B)
                    && cursor + 1 < input.size()
                    && input.at(cursor + 1) == QLatin1Char('\\')) {
                    cursor += 2;
                    complete = true;
                    break;
                }
                ++cursor;
            }
            if (!complete) {
                m_displayEscapeCarry = input.mid(index).left(256);
                break;
            }
            index = cursor;
            continue;
        }
        if (character == QChar(0x1B)) {
            if (index + 1 >= input.size()) {
                m_displayEscapeCarry = input.mid(index);
                break;
            }
            const QChar introducer = input.at(index + 1);
            if (introducer == QLatin1Char('[')) {
                qsizetype cursor = index + 2;
                while (cursor < input.size()) {
                    const ushort code = input.at(cursor).unicode();
                    if (code >= 0x40U && code <= 0x7EU)
                        break;
                    ++cursor;
                }
                if (cursor >= input.size()) {
                    m_displayEscapeCarry = input.mid(index).left(256);
                    break;
                }
                index = cursor + 1;
                continue;
            }
            if (introducer == QLatin1Char(']')) {
                qsizetype cursor = index + 2;
                bool complete = false;
                while (cursor < input.size()) {
                    if (input.at(cursor) == QChar(0x07)
                        || input.at(cursor) == QChar(0x009C)) {
                        ++cursor;
                        complete = true;
                        break;
                    }
                    if (input.at(cursor) == QChar(0x1B)
                        && cursor + 1 < input.size()
                        && input.at(cursor + 1) == QLatin1Char('\\')) {
                        cursor += 2;
                        complete = true;
                        break;
                    }
                    ++cursor;
                }
                if (!complete) {
                    m_displayEscapeCarry = input.mid(index).left(256);
                    break;
                }
                index = cursor;
                continue;
            }
            if (introducer == QLatin1Char('P')
                || introducer == QLatin1Char('X')
                || introducer == QLatin1Char('^')
                || introducer == QLatin1Char('_')) {
                qsizetype cursor = index + 2;
                bool complete = false;
                while (cursor < input.size()) {
                    if (input.at(cursor) == QChar(0x009C)) {
                        ++cursor;
                        complete = true;
                        break;
                    }
                    if (input.at(cursor) == QChar(0x1B)
                        && cursor + 1 < input.size()
                        && input.at(cursor + 1) == QLatin1Char('\\')) {
                        cursor += 2;
                        complete = true;
                        break;
                    }
                    ++cursor;
                }
                if (!complete) {
                    m_displayEscapeCarry = input.mid(index).left(256);
                    break;
                }
                index = cursor;
                continue;
            }
            // Two-byte Fe escape sequences carry no printable text.
            index += 2;
            continue;
        }

        if (character == QChar(0x08)) {
            if (!plain.isEmpty() && plain.back() != QLatin1Char('\n'))
                plain.chop(1);
            else if (!m_displayTranscript.isEmpty()
                     && m_displayTranscript.back() != QLatin1Char('\n'))
            {
                m_displayTranscript.chop(1);
                storedDisplayChanged = true;
            }
            ++index;
            continue;
        }
        if (character == QChar(0x0D)) {
            if (index + 1 < input.size() && input.at(index + 1) == QChar(0x0A)) {
                plain.append(QLatin1Char('\n'));
                index += 2;
                continue;
            }
            // A bare carriage return replaces the current display line.
            const qsizetype pendingLine = plain.lastIndexOf(QLatin1Char('\n'));
            if (pendingLine >= 0) {
                plain.truncate(pendingLine + 1);
            } else {
                const qsizetype storedLine = m_displayTranscript.lastIndexOf(QLatin1Char('\n'));
                const qsizetype newSize = storedLine < 0 ? 0 : storedLine + 1;
                if (newSize != m_displayTranscript.size()) {
                    m_displayTranscript.truncate(newSize);
                    storedDisplayChanged = true;
                }
            }
            ++index;
            continue;
        }
        const ushort code = character.unicode();
        if ((code < 0x20U && character != QChar(0x0A)
             && character != QChar(0x09))
            || code == 0x7FU
            || (code >= 0x80U && code <= 0x9FU)) {
            ++index;
            continue;
        }
        plain.append(character);
        ++index;
    }

    if (plain.isEmpty()) {
        if (storedDisplayChanged)
            emit displayTranscriptChanged();
        return;
    }
    QByteArray displayBytes = (m_displayTranscript + plain).toUtf8();
    if (displayBytes.size() > m_maxTranscriptBytes) {
        qsizetype removeBytes = displayBytes.size() - m_maxTranscriptBytes;
        displayBytes.remove(0, removeBytes);
        while (!displayBytes.isEmpty() && isUtf8Continuation(displayBytes.front()))
            displayBytes.remove(0, 1);
    }
    m_displayTranscript = QString::fromUtf8(displayBytes);
    emit displayOutputReceived(plain);
    emit displayTranscriptChanged();
}

void EmbeddedTerminalSession::handleNativeExit(quint32 nativeExitCode, bool forced)
{
    drainOutput();
    if (!m_utf8Carry.isEmpty()) {
        const QString finalText = QString::fromUtf8(m_utf8Carry);
        m_utf8Carry.clear();
        if (!finalText.isEmpty()) {
            emit outputReceived(finalText);
            appendTranscript(finalText);
            appendDisplayOutput(finalText);
        }
    }

    m_exitCode = static_cast<int>(nativeExitCode);
    if (forced)
        m_exitStatus = ExitStatus::Terminated;
    else if (nativeExitCode >= 0xC0000000U)
        m_exitStatus = ExitStatus::Crashed;
    else
        m_exitStatus = ExitStatus::NormalExit;
    emit exitChanged();
    setState(State::Exited);
    StructuredLogger::instance().log(
        m_exitStatus == ExitStatus::NormalExit ? LogSeverity::Info
                                               : LogSeverity::Warning,
        QStringLiteral("terminal"), QStringLiteral("terminal.exited"),
        m_exitStatus == ExitStatus::NormalExit
            ? QStringLiteral("Embedded terminal exited normally. / 內嵌終端機已經正常結束。")
            : QStringLiteral("Embedded terminal exited abnormally. / 內嵌終端機異常結束。"),
        QJsonObject{{QStringLiteral("exitCode"), m_exitCode},
                    {QStringLiteral("exitStatus"),
                     terminalExitStatusName(m_exitStatus)},
                    {QStringLiteral("forced"), forced},
                    {QStringLiteral("transcriptTruncated"),
                     m_transcriptTruncated},
                    {QStringLiteral("droppedOutputBytes"),
                     static_cast<qint64>(m_droppedOutputBytes)},
                    {QStringLiteral("sessionGeneration"),
                     static_cast<qint64>(m_sessionGeneration)}});
    emit processExited(m_exitCode, m_exitStatus);
}

void EmbeddedTerminalSession::joinFinishedWorkers()
{
#ifdef Q_OS_WIN
    if (m_impl->waiterThread.joinable())
        m_impl->waiterThread.join();
    if (m_impl->writerThread.joinable())
        m_impl->writerThread.join();
    if (m_impl->readerThread.joinable())
        m_impl->readerThread.join();
#endif
}

void EmbeddedTerminalSession::cleanupNativeResources()
{
#ifdef Q_OS_WIN
    HANDLE console = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    {
        std::lock_guard lock(m_impl->handleMutex);
        console = std::exchange(m_impl->pseudoConsole, nullptr);
        input = std::exchange(m_impl->inputWrite, nullptr);
        output = std::exchange(m_impl->outputRead, nullptr);
        process = std::exchange(m_impl->processHandle, nullptr);
        job = std::exchange(m_impl->jobHandle, nullptr);
    }
    if (job != nullptr)
        CloseHandle(job);
    if (process != nullptr)
        CloseHandle(process);
    if (console != nullptr)
        conPtyApi().close(console);
    if (input != nullptr)
        CloseHandle(input);
    if (output != nullptr)
        CloseHandle(output);
#endif
}

} // namespace wimforge
