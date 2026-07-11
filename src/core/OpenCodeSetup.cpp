#include "OpenCodeSetup.h"
#include "ProcessLaunch.h"
#include "StructuredLogger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

namespace wimforge {
namespace {

QString environmentFile(const char *name, const QString &relative)
{
    const QString root = qEnvironmentVariable(name).trimmed();
    return root.isEmpty() ? QString() : QDir(root).filePath(relative);
}

QString findOpenCodeExecutable()
{
#ifdef Q_OS_WIN
    // npm also creates cmd and POSIX shims. Resolve the native executable so
    // prompts are always passed as structured CreateProcess arguments.
    const QStringList names{QStringLiteral("opencode.exe")};
#else
    const QStringList names{QStringLiteral("opencode")};
#endif
    for (const QString &name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return QDir::cleanPath(found);
    }

    const QStringList candidates{
        environmentFile("APPDATA", QStringLiteral("npm/opencode.exe")),
        environmentFile("LOCALAPPDATA", QStringLiteral("npm/opencode.exe")),
        environmentFile("APPDATA", QStringLiteral("npm/node_modules/opencode-ai/bin/opencode.exe")),
        environmentFile("LOCALAPPDATA", QStringLiteral("npm/node_modules/opencode-ai/bin/opencode.exe")),
        environmentFile("USERPROFILE", QStringLiteral(".opencode/bin/opencode.exe")),
        environmentFile("LOCALAPPDATA", QStringLiteral("opencode/bin/opencode.exe")),
    };
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile())
            return QDir::cleanPath(candidate);
    }
    return {};
}

OpenCodeToolCommand findNpmCommand()
{
#ifdef Q_OS_WIN
    QStringList nodeCandidates;
    const QString pathNode = QStandardPaths::findExecutable(QStringLiteral("node.exe"));
    if (!pathNode.isEmpty())
        nodeCandidates.append(pathNode);
    nodeCandidates.append(environmentFile("ProgramFiles", QStringLiteral("nodejs/node.exe")));
    nodeCandidates.append(environmentFile("ProgramW6432", QStringLiteral("nodejs/node.exe")));
    nodeCandidates.append(environmentFile("ProgramFiles(x86)", QStringLiteral("nodejs/node.exe")));

    QStringList npmRoots;
    const QString npmShim = QStandardPaths::findExecutable(QStringLiteral("npm.cmd"));
    if (!npmShim.isEmpty())
        npmRoots.append(QFileInfo(npmShim).absolutePath());
    for (const QString &node : std::as_const(nodeCandidates)) {
        if (node.isEmpty() || !QFileInfo(node).isFile())
            continue;
        QStringList roots = npmRoots;
        roots.prepend(QFileInfo(node).absolutePath());
        for (const QString &root : std::as_const(roots)) {
            const QString npmCli = QDir(root).filePath(
                QStringLiteral("node_modules/npm/bin/npm-cli.js"));
            if (QFileInfo(npmCli).isFile())
                return {QDir::cleanPath(node), {QDir::cleanPath(npmCli)}};
        }
    }
    return {};
#else
    const QString npm = QStandardPaths::findExecutable(QStringLiteral("npm"));
    return npm.isEmpty() ? OpenCodeToolCommand{} : OpenCodeToolCommand{npm, {}};
#endif
}

QString boundedOutput(const QByteArray &bytes)
{
    constexpr qsizetype maximumCharacters = 8'192;
    QString output = QString::fromLocal8Bit(bytes).trimmed();
    if (output.size() > maximumCharacters)
        output = QStringLiteral("…") + output.right(maximumCharacters);
    return output;
}

class SystemOpenCodeProcessRunner final : public OpenCodeProcessRunner
{
public:
    ~SystemOpenCodeProcessRunner() override { shutdown(); }

    quint64 start(const OpenCodeProcessCommand &command, Completion completed) override
    {
        const quint64 id = ++m_nextId;
        auto *process = new QProcess;
        configureProcessWithoutConsole(*process);
        process->setProgram(resolveExecutableForLaunch(command.program));
        process->setArguments(command.arguments);
        process->setProcessChannelMode(QProcess::MergedChannels);
        m_processes.insert(id, process);

        auto finish = [this, id, process, completed = std::move(completed)](
                          OpenCodeProcessResult result) mutable {
            if (process->property("wimforgeHandled").toBool())
                return;
            process->setProperty("wimforgeHandled", true);
            result.output = boundedOutput(process->readAll());
            m_processes.remove(id);
            process->deleteLater();
            completed(result);
        };

        QObject::connect(process, &QProcess::errorOccurred, process,
                         [process, finish](QProcess::ProcessError error) mutable {
            if (error != QProcess::FailedToStart)
                return;
            OpenCodeProcessResult result;
            result.outcome = OpenCodeProcessResult::Outcome::FailedToStart;
            result.error = process->errorString();
            finish(result);
        });
        QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
                         [process, finish](int exitCode, QProcess::ExitStatus exitStatus) mutable {
            OpenCodeProcessResult result;
            result.outcome = process->property("wimforgeTimedOut").toBool()
                ? OpenCodeProcessResult::Outcome::TimedOut
                : OpenCodeProcessResult::Outcome::Finished;
            result.exitCode = exitCode;
            result.normalExit = exitStatus == QProcess::NormalExit;
            result.error = process->errorString();
            finish(result);
        });

        if (command.timeoutMs > 0) {
            auto *timer = new QTimer(process);
            timer->setSingleShot(true);
            QObject::connect(timer, &QTimer::timeout, process, [process] {
                if (process->state() == QProcess::NotRunning)
                    return;
                process->setProperty("wimforgeTimedOut", true);
                process->terminate();
                QTimer::singleShot(1'500, process, [process] {
                    if (process->state() != QProcess::NotRunning)
                        process->kill();
                });
            });
            timer->start(command.timeoutMs);
        }

        process->start();
        return id;
    }

    void shutdown() override
    {
        const QList<QProcess *> processes = m_processes.values();
        m_processes.clear();
        for (QProcess *process : processes) {
            if (!process)
                continue;
            process->setProperty("wimforgeHandled", true);
            QObject::disconnect(process, nullptr, nullptr, nullptr);
            if (process->state() != QProcess::NotRunning) {
                process->terminate();
                if (!process->waitForFinished(1'500)) {
                    process->kill();
                    process->waitForFinished(1'500);
                }
            }
            delete process;
        }
    }

private:
    quint64 m_nextId = 0;
    QHash<quint64, QProcess *> m_processes;
};

QString firstOutputLine(const QString &output)
{
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.first().trimmed();
}

QString bilingualStatus(const QString &english, const QString &cantonese)
{
    return english + QStringLiteral(" / ") + cantonese;
}

QString processOutcomeName(OpenCodeProcessResult::Outcome outcome)
{
    switch (outcome) {
    case OpenCodeProcessResult::Outcome::Finished:
        return QStringLiteral("finished");
    case OpenCodeProcessResult::Outcome::FailedToStart:
        return QStringLiteral("failed-to-start");
    case OpenCodeProcessResult::Outcome::TimedOut:
        return QStringLiteral("timed-out");
    }
    return QStringLiteral("unknown");
}

void logOpenCodeProcessResult(const QString &operation,
                              const OpenCodeProcessResult &result)
{
    StructuredLogger::instance().log(
        result.succeeded() ? LogSeverity::Info : LogSeverity::Error,
        QStringLiteral("opencode"), QStringLiteral("opencode.action.completed"),
        result.succeeded()
            ? QStringLiteral("OpenCode setup action completed. / OpenCode 設定動作完成。")
            : QStringLiteral("OpenCode setup action failed. / OpenCode 設定動作失敗。"),
        QJsonObject{
            {QStringLiteral("operation"), operation},
            {QStringLiteral("outcome"), processOutcomeName(result.outcome)},
            {QStringLiteral("exitCode"), result.exitCode},
            {QStringLiteral("normalExit"), result.normalExit},
            {QStringLiteral("succeeded"), result.succeeded()},
            // Process output, errors, executable paths, and arguments are
            // intentionally never logged; they may contain credentials.
            {QStringLiteral("diagnosticOutputPresent"), !result.output.isEmpty()},
            {QStringLiteral("processErrorPresent"), !result.error.isEmpty()},
        });
}

} // namespace

OpenCodeSetupEnvironment systemOpenCodeEnvironment()
{
    return {
        [] { return findOpenCodeExecutable(); },
        [] { return findNpmCommand(); },
        [] { return QStandardPaths::findExecutable(QStringLiteral("winget.exe")); },
        [] { return QDateTime::currentMSecsSinceEpoch(); },
    };
}

std::unique_ptr<OpenCodeProcessRunner> createSystemOpenCodeProcessRunner()
{
    return std::make_unique<SystemOpenCodeProcessRunner>();
}

OpenCodeSetup::OpenCodeSetup(QObject *parent)
    : OpenCodeSetup(systemOpenCodeEnvironment(), createSystemOpenCodeProcessRunner(), parent)
{
}

OpenCodeSetup::OpenCodeSetup(OpenCodeSetupEnvironment environment,
                             std::unique_ptr<OpenCodeProcessRunner> runner,
                             QObject *parent)
    : QObject(parent),
      m_environment(std::move(environment)),
      m_runner(std::move(runner))
{
    if (!m_environment.openCodeExecutable)
        m_environment.openCodeExecutable = [] { return QString(); };
    if (!m_environment.npmCommand)
        m_environment.npmCommand = [] { return OpenCodeToolCommand{}; };
    if (!m_environment.wingetExecutable)
        m_environment.wingetExecutable = [] { return QString(); };
    if (!m_environment.nowMs)
        m_environment.nowMs = [] { return QDateTime::currentMSecsSinceEpoch(); };

    // The desktop process is elevated. Do not even resolve user-profile/PATH
    // developer tools until the operator selects the explicit setup action.
    m_status = bilingualStatus(
        QStringLiteral("OpenCode host integration is idle because WimForge is elevated. Select Verify / install now to approve host-tool discovery and administrator execution for this session."),
        QStringLiteral("WimForge 已提升權限，所以 OpenCode host 整合而家保持閒置。請撳 Verify / install now，批准今次工作階段搜尋 host 工具同用管理員權限執行。"));
    m_lastTransitionMs = m_environment.nowMs();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.lifecycle.created"),
        QStringLiteral("OpenCode setup is idle until explicitly authorized. / OpenCode 設定會保持閒置，直至你明確批准。"),
        QJsonObject{{QStringLiteral("state"), stateName()},
                    {QStringLiteral("hostDiscoveryDeferred"), true}});
}

OpenCodeSetup::~OpenCodeSetup()
{
    shutdown();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.lifecycle.destroyed"),
        QStringLiteral("OpenCode setup was destroyed. / OpenCode 設定已經結束。"),
        QJsonObject{{QStringLiteral("state"), stateName()}});
}

QString OpenCodeSetup::stateName() const
{
    switch (m_state) {
    case OpenCodeSetupState::Absent: return QStringLiteral("absent");
    case OpenCodeSetupState::Installing: return QStringLiteral("installing");
    case OpenCodeSetupState::Verifying: return QStringLiteral("verifying");
    case OpenCodeSetupState::Ready: return QStringLiteral("ready");
    case OpenCodeSetupState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

bool OpenCodeSetup::busy() const
{
    return m_state == OpenCodeSetupState::Installing
        || m_state == OpenCodeSetupState::Verifying;
}

bool OpenCodeSetup::installed() const
{
    return !m_executablePath.isEmpty();
}

bool OpenCodeSetup::canRetry() const
{
    return !m_shuttingDown && (m_state == OpenCodeSetupState::Absent
                               || m_state == OpenCodeSetupState::Failed);
}

void OpenCodeSetup::ensureReady(Completion completed)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.ensure.requested"),
        QStringLiteral("OpenCode readiness was requested. / 已經要求檢查 OpenCode 準備狀態。"),
        QJsonObject{{QStringLiteral("state"), stateName()},
                    {QStringLiteral("authorized"), m_explicitSetupAuthorized},
                    {QStringLiteral("busy"), busy()},
                    {QStringLiteral("pendingCompletionCount"),
                     static_cast<qint64>(m_pending.size())}});
    if (m_shuttingDown) {
        StructuredLogger::instance().log(
            LogSeverity::Warning, QStringLiteral("opencode"),
            QStringLiteral("opencode.ensure.rejected"),
            QStringLiteral("OpenCode readiness was rejected during shutdown. / OpenCode 正在關閉，所以唔接受準備狀態要求。"),
            QJsonObject{{QStringLiteral("reason"), QStringLiteral("shutting-down")}});
        if (completed)
            completed(false, bilingualStatus(
                QStringLiteral("OpenCode setup is shutting down."),
                QStringLiteral("OpenCode 設定正在關閉。")));
        return;
    }
    if (!m_explicitSetupAuthorized) {
        const QString message = bilingualStatus(
            QStringLiteral("OpenCode host integration requires explicit approval because WimForge is elevated. Open Package Studio and select Verify / install now before using an assisted action."),
            QStringLiteral("WimForge 已提升權限，所以 OpenCode host 整合要你明確批准。請先去 Package Studio 撳 Verify / install now，之後先用輔助動作。"));
        m_status = message;
        m_error.clear();
        if (completed)
            completed(false, message);
        emit changed();
        StructuredLogger::instance().log(
            LogSeverity::Warning, QStringLiteral("opencode"),
            QStringLiteral("opencode.ensure.rejected"),
            QStringLiteral("OpenCode host discovery requires explicit approval. / OpenCode 主機工具搜尋需要你明確批准。"),
            QJsonObject{{QStringLiteral("reason"),
                         QStringLiteral("explicit-authorization-required")}});
        return;
    }
    if (completed)
        m_pending.push_back(std::move(completed));

    if (m_state == OpenCodeSetupState::Ready) {
        const QString currentExecutable = m_environment.openCodeExecutable();
        if (!currentExecutable.isEmpty()) {
            m_executablePath = currentExecutable;
            StructuredLogger::instance().log(
                LogSeverity::Info, QStringLiteral("opencode"),
                QStringLiteral("opencode.ensure.reused"),
                QStringLiteral("The verified OpenCode installation is still available. / 已驗證嘅 OpenCode 安裝仍然可用。"),
                QJsonObject{{QStringLiteral("state"), stateName()}});
            completePending(true, {});
            return;
        }
        transition(OpenCodeSetupState::Absent, bilingualStatus(
            QStringLiteral("OpenCode is no longer available; setup will repair it."),
            QStringLiteral("而家搵唔到 OpenCode；設定程序會修復佢。")));
    }
    if (busy()) {
        StructuredLogger::instance().log(
            LogSeverity::Debug, QStringLiteral("opencode"),
            QStringLiteral("opencode.ensure.joined"),
            QStringLiteral("The readiness request joined the active setup attempt. / 準備狀態要求已經加入而家進行緊嘅設定嘗試。"),
            QJsonObject{{QStringLiteral("state"), stateName()},
                        {QStringLiteral("pendingCompletionCount"),
                         static_cast<qint64>(m_pending.size())}});
        return;
    }
    startAttempt();
}

void OpenCodeSetup::retry(Completion completed)
{
    m_explicitSetupAuthorized = true;
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.authorization.granted"),
        QStringLiteral("OpenCode setup was explicitly authorized for this session. / 你已經明確批准今次工作階段設定 OpenCode。"));
    ensureReady(std::move(completed));
}

void OpenCodeSetup::shutdown()
{
    if (m_shuttingDown)
        return;
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.shutdown.requested"),
        QStringLiteral("OpenCode setup shutdown was requested. / 已經要求關閉 OpenCode 設定。"),
        QJsonObject{{QStringLiteral("state"), stateName()},
                    {QStringLiteral("busy"), busy()},
                    {QStringLiteral("pendingCompletionCount"),
                     static_cast<qint64>(m_pending.size())}});
    m_shuttingDown = true;
    ++m_generation;
    if (m_runner)
        m_runner->shutdown();
    completePending(false, bilingualStatus(
        QStringLiteral("OpenCode setup was cancelled during shutdown."),
        QStringLiteral("關閉期間已取消 OpenCode 設定。")));
}

void OpenCodeSetup::startAttempt()
{
    ++m_generation;
    m_installedDuringAttempt = false;
    m_version.clear();
    m_executablePath = m_environment.openCodeExecutable();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.attempt.started"),
        QStringLiteral("OpenCode setup attempt started. / OpenCode 設定嘗試已經開始。"),
        QJsonObject{{QStringLiteral("generation"),
                     static_cast<qint64>(m_generation)},
                    {QStringLiteral("existingExecutableFound"),
                     !m_executablePath.isEmpty()}});
    if (!m_executablePath.isEmpty()) {
        startVerification();
        return;
    }
    if (!m_environment.npmCommand().isEmpty()) {
        startNpmInstall();
        return;
    }
    if (!m_environment.wingetExecutable().trimmed().isEmpty()) {
        startWinGetInstall();
        return;
    }
    finishFailed(QStringLiteral(
        "OpenCode needs Node.js/npm, but neither npm nor WinGet is available."));
}

void OpenCodeSetup::startWinGetInstall()
{
    const QString winget = m_environment.wingetExecutable().trimmed();
    if (winget.isEmpty()) {
        finishFailed(QStringLiteral("WinGet disappeared before Node.js installation could start."));
        return;
    }
    transition(OpenCodeSetupState::Installing, bilingualStatus(
        QStringLiteral("Installing Node.js LTS with WinGet…"),
        QStringLiteral("正用 WinGet 安裝 Node.js LTS…")));
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.action.started"),
        QStringLiteral("Node.js installation started for OpenCode. / 為 OpenCode 安裝 Node.js 嘅動作已經開始。"),
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("install-node")},
                    {QStringLiteral("timeoutMs"), WinGetInstallTimeoutMs}});
    const quint64 generation = m_generation;
    m_runner->start(
        {winget,
         {QStringLiteral("install"), QStringLiteral("--id"),
          QStringLiteral("OpenJS.NodeJS.LTS"), QStringLiteral("--exact"),
          QStringLiteral("--silent"), QStringLiteral("--accept-package-agreements"),
          QStringLiteral("--accept-source-agreements"),
          QStringLiteral("--disable-interactivity")},
         WinGetInstallTimeoutMs},
        [this, generation](const OpenCodeProcessResult &result) {
            handleWinGetResult(generation, result);
        });
}

void OpenCodeSetup::startNpmInstall()
{
    const OpenCodeToolCommand npm = m_environment.npmCommand();
    if (npm.isEmpty()) {
        finishFailed(QStringLiteral(
            "Node.js installation completed, but npm could not be found. Restart WimForge or retry after Node.js finishes updating PATH."));
        return;
    }
    transition(OpenCodeSetupState::Installing, bilingualStatus(
        QStringLiteral("Installing OpenCode automatically with npm…"),
        QStringLiteral("正用 npm 自動安裝 OpenCode…")));
    QStringList arguments = npm.prefixArguments;
    arguments.append({QStringLiteral("install"), QStringLiteral("-g"),
                      QStringLiteral("opencode-ai@latest")});
    const quint64 generation = m_generation;
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.action.started"),
        QStringLiteral("OpenCode npm installation started. / OpenCode npm 安裝已經開始。"),
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("install-opencode")},
                    {QStringLiteral("timeoutMs"), NpmInstallTimeoutMs},
                    {QStringLiteral("argumentCount"), arguments.size()}});
    m_runner->start(
        {npm.program, arguments, NpmInstallTimeoutMs},
        [this, generation](const OpenCodeProcessResult &result) {
            handleNpmResult(generation, result);
        });
}

void OpenCodeSetup::startVerification()
{
    m_executablePath = m_environment.openCodeExecutable();
    if (m_executablePath.isEmpty()) {
        finishFailed(QStringLiteral(
            "OpenCode installation returned success, but no native executable was found in PATH or the npm prefix."));
        return;
    }
    transition(OpenCodeSetupState::Verifying, bilingualStatus(
        QStringLiteral("Live-verifying OpenCode…"),
        QStringLiteral("正即時驗證 OpenCode…")));
    const quint64 generation = m_generation;
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.action.started"),
        QStringLiteral("OpenCode live verification started. / OpenCode 即時驗證已經開始。"),
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("verify")},
                    {QStringLiteral("timeoutMs"), VerifyTimeoutMs},
                    {QStringLiteral("argumentCount"), 1}});
    m_runner->start(
        {m_executablePath, {QStringLiteral("--version")}, VerifyTimeoutMs},
        [this, generation](const OpenCodeProcessResult &result) {
            handleVerificationResult(generation, result);
        });
}

void OpenCodeSetup::handleWinGetResult(quint64 generation,
                                       const OpenCodeProcessResult &result)
{
    if (m_shuttingDown || generation != m_generation)
        return;
    logOpenCodeProcessResult(QStringLiteral("install-node"), result);
    if (!result.succeeded()) {
        finishFailed(processFailure(QStringLiteral("Node.js installation"), result));
        return;
    }
    startNpmInstall();
}

void OpenCodeSetup::handleNpmResult(quint64 generation,
                                    const OpenCodeProcessResult &result)
{
    if (m_shuttingDown || generation != m_generation)
        return;
    logOpenCodeProcessResult(QStringLiteral("install-opencode"), result);
    if (!result.succeeded()) {
        finishFailed(processFailure(QStringLiteral("OpenCode npm installation"), result));
        return;
    }
    m_installedDuringAttempt = true;
    startVerification();
}

void OpenCodeSetup::handleVerificationResult(quint64 generation,
                                             const OpenCodeProcessResult &result)
{
    if (m_shuttingDown || generation != m_generation)
        return;
    logOpenCodeProcessResult(QStringLiteral("verify"), result);
    if (!result.succeeded()) {
        finishFailed(processFailure(QStringLiteral("OpenCode live verification"), result));
        return;
    }
    const QString version = firstOutputLine(result.output);
    if (version.isEmpty()) {
        finishFailed(QStringLiteral(
            "OpenCode live verification returned no version text."));
        return;
    }
    finishReady(version);
}

void OpenCodeSetup::transition(OpenCodeSetupState state,
                               const QString &status,
                               const QString &error)
{
    const QString previousState = stateName();
    m_state = state;
    m_status = status;
    m_error = error;
    m_lastTransitionMs = m_environment.nowMs();
    StructuredLogger::instance().log(
        error.isEmpty() ? LogSeverity::Info : LogSeverity::Warning,
        QStringLiteral("opencode"), QStringLiteral("opencode.state.changed"),
        QStringLiteral("OpenCode setup state changed. / OpenCode 設定狀態已經改變。"),
        QJsonObject{{QStringLiteral("from"), previousState},
                    {QStringLiteral("to"), stateName()},
                    {QStringLiteral("errorPresent"), !error.isEmpty()}});
    emit changed();
}

void OpenCodeSetup::finishReady(const QString &version)
{
    m_version = version;
    const bool installedDuringAttempt = m_installedDuringAttempt;
    transition(OpenCodeSetupState::Ready, bilingualStatus(
        QStringLiteral("OpenCode is installed and live-verified: %1").arg(version),
        QStringLiteral("OpenCode 已安裝兼即時驗證：%1").arg(version)));
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("opencode"),
        QStringLiteral("opencode.attempt.succeeded"),
        QStringLiteral("OpenCode setup completed and was live-verified. / OpenCode 設定完成，亦已經即時驗證。"),
        QJsonObject{{QStringLiteral("installedDuringAttempt"),
                     installedDuringAttempt},
                    {QStringLiteral("versionTextPresent"), !version.isEmpty()}});
    emit becameReady(installedDuringAttempt);
    completePending(true, {});
}

void OpenCodeSetup::finishFailed(const QString &message)
{
    m_executablePath.clear();
    const QString displayMessage = message.contains(QStringLiteral(" / "))
        ? message
        : bilingualStatus(message,
                           QStringLiteral("OpenCode 設定失敗；請參考前面嘅英文診斷。"));
    transition(OpenCodeSetupState::Failed, displayMessage, displayMessage);
    StructuredLogger::instance().log(
        LogSeverity::Error, QStringLiteral("opencode"),
        QStringLiteral("opencode.attempt.failed"),
        QStringLiteral("OpenCode setup failed; diagnostic contents were omitted from logging. / OpenCode 設定失敗；記錄已經省略診斷內容。"),
        QJsonObject{{QStringLiteral("diagnosticPresent"), !message.isEmpty()}});
    emit failed(displayMessage);
    completePending(false, message);
}

void OpenCodeSetup::completePending(bool ready, const QString &error)
{
    std::vector<Completion> pending;
    pending.swap(m_pending);
    StructuredLogger::instance().log(
        pending.empty() ? LogSeverity::Debug
                        : (ready ? LogSeverity::Debug : LogSeverity::Warning),
        QStringLiteral("opencode"), QStringLiteral("opencode.callbacks.completed"),
        QStringLiteral("OpenCode setup callbacks were completed. / OpenCode 設定回呼已經完成。"),
        QJsonObject{{QStringLiteral("ready"), ready},
                    {QStringLiteral("errorPresent"), !error.isEmpty()},
                    {QStringLiteral("completionCount"),
                     static_cast<qint64>(pending.size())}});
    for (Completion &completed : pending) {
        if (completed)
            completed(ready, error);
    }
}

QString OpenCodeSetup::processFailure(const QString &operation,
                                      const OpenCodeProcessResult &result) const
{
    if (result.outcome == OpenCodeProcessResult::Outcome::TimedOut)
        return QStringLiteral("%1 timed out.").arg(operation);
    if (result.outcome == OpenCodeProcessResult::Outcome::FailedToStart)
        return QStringLiteral("%1 could not start: %2").arg(operation, result.error);
    const QString detail = !result.output.isEmpty() ? result.output : result.error;
    return QStringLiteral("%1 failed (exit %2): %3")
        .arg(operation)
        .arg(result.exitCode)
        .arg(detail.isEmpty() ? QStringLiteral("No diagnostic output was returned.") : detail);
}

} // namespace wimforge
