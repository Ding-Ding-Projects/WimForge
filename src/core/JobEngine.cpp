#include "JobEngine.h"
#include "ProcessLaunch.h"
#include "StructuredLogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#endif

#include <algorithm>
#include <utility>
#include <vector>

namespace wimforge {
namespace {

void setError(QString *target, const QString &value)
{
    if (target)
        *target = value;
}

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

bool terminal(OperationState state)
{
    return state == OperationState::Succeeded || state == OperationState::Failed
        || state == OperationState::Skipped || state == OperationState::Blocked
        || state == OperationState::Cancelled;
}

QString operationGraphError(const QList<ServicingOperation> &operations)
{
    QHash<QString, int> indexById;
    for (qsizetype index = 0; index < operations.size(); ++index) {
        const QString &id = operations.at(index).id;
        if (id.trimmed().isEmpty())
            return QStringLiteral("Servicing operation %1 has an empty ID.").arg(index + 1);
        if (indexById.contains(id))
            return QStringLiteral("Servicing operation ID '%1' is duplicated.").arg(id);
        indexById.insert(id, static_cast<int>(index));
    }

    std::vector<int> inDegree(static_cast<std::size_t>(operations.size()), 0);
    std::vector<std::vector<int>> dependents(static_cast<std::size_t>(operations.size()));
    for (qsizetype index = 0; index < operations.size(); ++index) {
        const ServicingOperation &operation = operations.at(index);
        QSet<QString> seenDependencies;
        for (const QString &dependency : operation.dependsOn) {
            if (seenDependencies.contains(dependency))
                continue;
            seenDependencies.insert(dependency);
            const auto found = indexById.constFind(dependency);
            if (found == indexById.cend()) {
                return QStringLiteral("Servicing operation '%1' depends on missing operation '%2'.")
                    .arg(operation.id, dependency);
            }
            ++inDegree.at(static_cast<std::size_t>(index));
            dependents.at(static_cast<std::size_t>(*found)).push_back(static_cast<int>(index));
        }
    }

    QList<int> ready;
    for (qsizetype index = 0; index < operations.size(); ++index)
        if (inDegree.at(static_cast<std::size_t>(index)) == 0)
            ready.append(static_cast<int>(index));

    qsizetype orderedCount = 0;
    while (!ready.isEmpty()) {
        const int index = ready.takeFirst();
        ++orderedCount;
        for (const int dependent : dependents.at(static_cast<std::size_t>(index))) {
            int &degree = inDegree.at(static_cast<std::size_t>(dependent));
            --degree;
            if (degree != 0)
                continue;
            const auto position = std::lower_bound(ready.begin(), ready.end(), dependent);
            ready.insert(position, dependent);
        }
    }
    if (orderedCount != operations.size()) {
        QStringList unresolved;
        for (qsizetype index = 0; index < operations.size(); ++index)
            if (inDegree.at(static_cast<std::size_t>(index)) > 0)
                unresolved.append(operations.at(index).id);
        return QStringLiteral("Servicing operation dependencies contain a cycle involving: %1.")
            .arg(unresolved.join(QStringLiteral(", ")));
    }
    return {};
}

} // namespace

JobEngine::JobEngine(QObject *parent) : QObject(parent) {}

JobEngine::~JobEngine()
{
    StructuredLogger::instance().log(
        LogSeverity::Debug, QStringLiteral("jobs"),
        QStringLiteral("engine.destroying"),
        QStringLiteral("Job engine is shutting down. / 工作引擎而家關閉。"),
        QJsonObject{{QStringLiteral("running"), m_running},
                    {QStringLiteral("activeProcesses"), m_active.size()},
                    {QStringLiteral("runId"), m_runId}});
    cancel();
    for (const ActiveProcess &active : std::as_const(m_active))
        if (active.process)
            active.process->kill();
}

bool JobEngine::isRunning() const { return m_running; }
int JobEngine::runningCount() const { return m_active.size(); }

double JobEngine::progress() const
{
    return m_operations.isEmpty()
        ? 0.0
        : static_cast<double>(completedCount()) / static_cast<double>(m_operations.size());
}

QString JobEngine::statusText() const { return m_statusText; }
QString JobEngine::journalPath() const { return journalPathForProject(m_project.projectDirectory); }
QList<ServicingOperation> JobEngine::operations() const { return m_operations; }

QString JobEngine::defaultRecoveryRoot()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty())
        root = QDir::home().filePath(QStringLiteral(".wimforge"));
    return QDir(root).filePath(QStringLiteral("recovery"));
}

QString JobEngine::journalPathForProject(const QString &projectDirectory)
{
    return QDir(projectDirectory).filePath(QStringLiteral(".wimforge/job-journal.json"));
}

bool JobEngine::hasInterruptedRun(const QString &projectDirectory, QJsonObject *journal, QString *error)
{
    const QString path = journalPathForProject(projectDirectory);
    if (!QFileInfo::exists(path)) {
        setError(error, {});
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("Recovery journal is invalid at byte %1: %2")
                            .arg(parseError.offset).arg(parseError.errorString()));
        return false;
    }
    if (journal)
        *journal = document.object();
    setError(error, {});
    return document.object().value(QStringLiteral("status")).toString() == QStringLiteral("running")
        || document.object().value(QStringLiteral("status")).toString() == QStringLiteral("cancelling");
}

bool JobEngine::isAdministrator()
{
#ifdef Q_OS_WIN
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!::AllocateAndInitializeSid(&authority, 2,
                                    SECURITY_BUILTIN_DOMAIN_RID,
                                    DOMAIN_ALIAS_RID_ADMINS,
                                    0, 0, 0, 0, 0, 0,
                                    &administrators)) {
        return false;
    }

    BOOL member = FALSE;
    const BOOL checked = ::CheckTokenMembership(nullptr, administrators, &member);
    ::FreeSid(administrators);
    return checked != FALSE && member != FALSE;
#else
    return true;
#endif
}

bool JobEngine::start(const ProjectConfig &project,
                      const QList<ServicingOperation> &operations,
                      int maximumParallel,
                      QString *error)
{
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("jobs"),
        QStringLiteral("run.start_requested"),
        QStringLiteral("A servicing run was requested. / 已經要求執行維護工作。"),
        QJsonObject{{QStringLiteral("projectDirectoryPresent"),
                     !project.projectDirectory.isEmpty()},
                    {QStringLiteral("operationCount"), operations.size()},
                    {QStringLiteral("maximumParallel"), maximumParallel}});
    const auto reject = [error, &operations](const QString &message) {
        setError(error, message);
        StructuredLogger::instance().log(
            LogSeverity::Warning, QStringLiteral("jobs"),
            QStringLiteral("run.rejected"),
            QStringLiteral("The servicing run was rejected; diagnostic contents were omitted from logging. / 維護工作要求被拒絕；記錄已經省略診斷內容。"),
            QJsonObject{{QStringLiteral("operationCount"), operations.size()},
                        {QStringLiteral("diagnosticPresent"), !message.isEmpty()}});
        return false;
    };
    if (m_running) {
        return reject(QStringLiteral("A servicing run is already active."));
    }
    if (operations.isEmpty()) {
        return reject(QStringLiteral("The servicing plan is empty."));
    }
    const ProjectValidation validation = project.validateForExecution();
    if (!validation.ok()) {
        return reject(validation.message());
    }
    const QString graphError = operationGraphError(operations);
    if (!graphError.isEmpty()) {
        return reject(graphError);
    }
    if (std::any_of(operations.cbegin(), operations.cend(), [](const ServicingOperation &item) {
            return item.requiresAdministrator;
        }) && !isAdministrator()) {
        return reject(QStringLiteral("This plan contains Administrator operations. Relaunch WimForge as Administrator before running it."));
    }

    m_project = project;
    m_operations = operations;
    m_indexById.clear();
    for (qsizetype index = 0; index < m_operations.size(); ++index) {
        if (m_operations[index].state != OperationState::Skipped)
            m_operations[index].state = OperationState::Queued;
        m_indexById.insert(m_operations.at(index).id, static_cast<int>(index));
    }
    m_active.clear();
    m_runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_startedAt = nowIso();
    m_maximumParallel = qBound(1, maximumParallel, 16);
    m_statusText = QStringLiteral("Preparing servicing jobs");
    m_running = true;
    m_cancelling = false;

    if (!QDir().mkpath(stateDirectory())) {
        m_running = false;
        return reject(QStringLiteral("Could not create the crash-recovery state folder."));
    }
    QString journalError;
    if (!writeJournal(&journalError)) {
        m_running = false;
        return reject(journalError);
    }
    setError(error, {});
    emit stateChanged();
    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("jobs"),
        QStringLiteral("run.started"),
        QStringLiteral("Servicing run entered the scheduler. / 維護工作已經進入排程器。"),
        QJsonObject{{QStringLiteral("runId"), m_runId},
                    {QStringLiteral("operationCount"), m_operations.size()},
                    {QStringLiteral("maximumParallel"), m_maximumParallel},
                    {QStringLiteral("recoveryJournalEnabled"), true}});
    QTimer::singleShot(0, this, &JobEngine::schedule);
    return true;
}

void JobEngine::cancel()
{
    if (!m_running || m_cancelling)
        return;
    StructuredLogger::instance().log(
        LogSeverity::Warning, QStringLiteral("jobs"),
        QStringLiteral("run.cancel_requested"),
        QStringLiteral("Safe cancellation was requested. / 已經要求安全取消。"),
        QJsonObject{{QStringLiteral("runId"), m_runId},
                    {QStringLiteral("activeProcesses"), m_active.size()}});
    m_cancelling = true;
    m_statusText = QStringLiteral("Cancelling safely");
    for (ServicingOperation &operation : m_operations)
        if (operation.state == OperationState::Queued)
            operation.state = OperationState::Cancelled;
    for (const ActiveProcess &active : std::as_const(m_active)) {
        if (!active.process)
            continue;
        active.process->terminate();
        QPointer<QProcess> guarded = active.process;
        QTimer::singleShot(8000, this, [guarded] {
            if (guarded && guarded->state() != QProcess::NotRunning)
                guarded->kill();
        });
    }
    writeJournal(nullptr);
    emit stateChanged();
    if (m_active.isEmpty())
        finishRun(false, QStringLiteral("Servicing run cancelled before launch."));
}

bool JobEngine::dependenciesDone(const ServicingOperation &operation) const
{
    for (const QString &dependency : operation.dependsOn)
        if (!dependencyChainDone(dependency))
            return false;
    return true;
}

bool JobEngine::dependencyFailed(const ServicingOperation &operation) const
{
    for (const QString &dependency : operation.dependsOn)
        if (dependencyChainFailed(dependency))
            return true;
    return false;
}

bool JobEngine::dependencyChainDone(const QString &operationId) const
{
    const auto found = m_indexById.constFind(operationId);
    if (found == m_indexById.cend())
        return false;
    const ServicingOperation &operation = m_operations.at(*found);
    if (operation.state != OperationState::Skipped)
        return terminal(operation.state);
    // An intentional skip is transparent, not a shortcut around the skipped
    // operation's own prerequisites.
    return dependenciesDone(operation);
}

bool JobEngine::dependencyChainFailed(const QString &operationId) const
{
    const auto found = m_indexById.constFind(operationId);
    if (found == m_indexById.cend())
        return true;
    const ServicingOperation &operation = m_operations.at(*found);
    if (operation.state == OperationState::Failed || operation.state == OperationState::Blocked
        || operation.state == OperationState::Cancelled) {
        return true;
    }
    // Preserve the user's explicit skip while still carrying any failure from
    // its prerequisite chain to consumers that cannot safely run.
    return operation.state == OperationState::Skipped && dependencyFailed(operation);
}

int JobEngine::completedCount() const
{
    return static_cast<int>(std::count_if(m_operations.cbegin(), m_operations.cend(),
                                          [](const ServicingOperation &item) { return terminal(item.state); }));
}

QString JobEngine::stateDirectory() const
{
    return QDir(m_project.projectDirectory).filePath(QStringLiteral(".wimforge"));
}

void JobEngine::schedule()
{
    if (!m_running)
        return;

    bool changed = false;
    bool blockedOperation = false;
    do {
        blockedOperation = false;
        for (qsizetype index = 0; index < m_operations.size(); ++index) {
            ServicingOperation &operation = m_operations[index];
            if (operation.state == OperationState::Queued && dependenciesDone(operation)
                && dependencyFailed(operation)) {
                operation.state = OperationState::Blocked;
                StructuredLogger::instance().log(
                    LogSeverity::Warning, QStringLiteral("jobs"),
                    QStringLiteral("operation.blocked"),
                    QStringLiteral("Operation blocked because a dependency failed or was cancelled. / 因為相依操作失敗或取消，呢個操作已經被封鎖。"),
                    QJsonObject{{QStringLiteral("runId"), m_runId},
                                {QStringLiteral("operationIndex"),
                                 static_cast<int>(index)},
                                {QStringLiteral("operationKind"),
                                 ServicingPlan::operationKindName(operation.kind)},
                                {QStringLiteral("dependencyCount"),
                                 operation.dependsOn.size()}});
                changed = true;
                blockedOperation = true;
                emit operationChanged(static_cast<int>(index), ServicingPlan::operationStateName(operation.state),
                                      QStringLiteral("Blocked because a required dependency failed or was cancelled."));
            }
        }
    } while (blockedOperation);

    while (!m_cancelling && m_active.size() < m_maximumParallel) {
        int candidate = -1;
        for (qsizetype index = 0; index < m_operations.size(); ++index) {
            const ServicingOperation &operation = m_operations.at(index);
            if (operation.state != OperationState::Queued || !dependenciesDone(operation)
                || dependencyFailed(operation))
                continue;
            // Image writers are serialized even if a malformed imported plan forgot dependencies.
            if (operation.writesMountedImage
                && std::any_of(m_active.cbegin(), m_active.cend(), [this](const ActiveProcess &active) {
                    return active.index >= 0 && m_operations.at(active.index).writesMountedImage;
                }))
                continue;
            if (!operation.mayRunInParallel && !m_active.isEmpty())
                continue;
            candidate = static_cast<int>(index);
            break;
        }
        if (candidate < 0)
            break;
        launch(candidate);
        changed = true;
    }

    if (changed) {
        writeJournal(nullptr);
        emit stateChanged();
    }

    if (m_active.isEmpty() && completedCount() == m_operations.size()) {
        const bool success = std::all_of(m_operations.cbegin(), m_operations.cend(), [](const ServicingOperation &item) {
            return item.state == OperationState::Succeeded || item.state == OperationState::Skipped;
        });
        const int skippedCount = static_cast<int>(std::count_if(
            m_operations.cbegin(), m_operations.cend(), [](const ServicingOperation &item) {
                return item.state == OperationState::Skipped;
            }));
        const int failedCount = static_cast<int>(std::count_if(
            m_operations.cbegin(), m_operations.cend(), [](const ServicingOperation &item) {
                return item.state == OperationState::Failed;
            }));
        const int blockedCount = static_cast<int>(std::count_if(
            m_operations.cbegin(), m_operations.cend(), [](const ServicingOperation &item) {
                return item.state == OperationState::Blocked;
            }));
        QString message;
        if (success && skippedCount > 0) {
            message = QStringLiteral("All selected servicing operations completed; %1 intentionally skipped.")
                          .arg(skippedCount);
        } else if (success) {
            message = QStringLiteral("All servicing operations completed.");
        } else if (m_cancelling) {
            message = QStringLiteral("Servicing run cancelled safely.");
        } else {
            message = QStringLiteral("Servicing failed: %1 operation(s) failed and %2 blocked by dependencies.")
                          .arg(failedCount).arg(blockedCount);
        }
        finishRun(success, message);
    }
}

void JobEngine::launch(int index)
{
    ServicingOperation &operation = m_operations[index];
    const QString launchProgram = resolveExecutableForLaunch(operation.executable);
    operation.state = OperationState::Running;
    m_statusText = operation.titleEn;

    const QString logDirectory = QDir(stateDirectory()).filePath(QStringLiteral("logs/%1").arg(m_runId));
    QDir().mkpath(logDirectory);
    const QString logPath = QDir(logDirectory).filePath(operation.id + QStringLiteral(".log"));
    const QString header = QStringLiteral(
        "[%1] Servicing child process starting / 維護子程序而家啟動\n"
        "operation-index: %2\noperation-kind: %3\nargument-count: %4\n"
        "command-omitted: true\n\n")
                               .arg(nowIso())
                               .arg(index)
                               .arg(ServicingPlan::operationKindName(operation.kind))
                               .arg(operation.arguments.size());
    appendLog(logPath, header.toUtf8());

    StructuredLogger::instance().log(
        LogSeverity::Info, QStringLiteral("process"),
        QStringLiteral("operation.process_starting"),
        QStringLiteral("Starting a servicing child process. / 維護子程序而家啟動。"),
        QJsonObject{{QStringLiteral("runId"), m_runId},
                    {QStringLiteral("operationIndex"), index},
                    {QStringLiteral("operationKind"),
                     ServicingPlan::operationKindName(operation.kind)},
                    {QStringLiteral("argumentCount"), operation.arguments.size()},
                    {QStringLiteral("workingDirectoryProvided"),
                     !operation.workingDirectory.isEmpty()},
                    {QStringLiteral("executableResolved"),
                     !launchProgram.isEmpty()},
                    {QStringLiteral("executableResolutionChanged"),
                     launchProgram != operation.executable},
                    {QStringLiteral("requiresAdministrator"),
                     operation.requiresAdministrator},
                    {QStringLiteral("destructive"), operation.destructive},
                    {QStringLiteral("writesMountedImage"),
                     operation.writesMountedImage}});

    auto *process = new QProcess(this);
    configureProcessWithoutConsole(*process);
    process->setProgram(launchProgram);
    process->setArguments(operation.arguments);
    process->setProcessChannelMode(QProcess::MergedChannels);
    if (operation.executable.compare(QStringLiteral("powershell.exe"),
                                     Qt::CaseInsensitive) == 0) {
        process->setProcessEnvironment(sanitizedPowerShellEnvironment());
    }
    if (!operation.workingDirectory.isEmpty())
        process->setWorkingDirectory(operation.workingDirectory);
    m_active.append(ActiveProcess{index, process, logPath});

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, index, logPath] {
        const QByteArray bytes = process->readAllStandardOutput();
        appendLog(logPath,
                  QStringLiteral("[%1] output-bytes: %2; contents-omitted: true / 輸出 byte：%2；內容已經省略：true\n")
                      .arg(nowIso())
                      .arg(bytes.size())
                      .toUtf8());
        const QString output = QString::fromLocal8Bit(bytes);
        StructuredLogger::instance().log(
            LogSeverity::Debug, QStringLiteral("process"),
            QStringLiteral("operation.output"),
            QStringLiteral("Servicing child process produced output; contents were omitted from logging. / 維護子程序有輸出；記錄已經省略內容。"),
            QJsonObject{{QStringLiteral("runId"), m_runId},
                        {QStringLiteral("operationIndex"), index},
                        {QStringLiteral("bytes"), bytes.size()}});
        emit outputReceived(index, output);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, index](QProcess::ProcessError processError) {
        StructuredLogger::instance().log(
            LogSeverity::Error, QStringLiteral("process"),
            QStringLiteral("operation.process_error"),
            QStringLiteral("The servicing child process reported an error; diagnostic contents were omitted from logging. / 維護子程序報告錯誤；記錄已經省略診斷內容。"),
            QJsonObject{{QStringLiteral("runId"), m_runId},
                        {QStringLiteral("operationIndex"), index},
                        {QStringLiteral("errorStringPresent"),
                         !process->errorString().isEmpty()},
                        {QStringLiteral("processError"), static_cast<int>(processError)}});
        if (processError == QProcess::FailedToStart)
            finishOperation(index, false, process->errorString());
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, index, logPath](int exitCode, QProcess::ExitStatus exitStatus) {
        const QByteArray trailing = process->readAllStandardOutput();
        if (!trailing.isEmpty()) {
            appendLog(logPath,
                      QStringLiteral("[%1] trailing-output-bytes: %2; contents-omitted: true / 尾段輸出 byte：%2；內容已經省略：true\n")
                          .arg(nowIso())
                          .arg(trailing.size())
                          .toUtf8());
            emit outputReceived(index, QString::fromLocal8Bit(trailing));
        }
        appendLog(logPath,
                  QStringLiteral("[%1] exit-code: %2; exit-status: %3\n")
                      .arg(nowIso())
                      .arg(exitCode)
                      .arg(static_cast<int>(exitStatus))
                      .toUtf8());
        StructuredLogger::instance().log(
            exitStatus == QProcess::NormalExit && exitCode == 0
                ? LogSeverity::Info : LogSeverity::Error,
            QStringLiteral("process"),
            QStringLiteral("operation.process_finished"),
            QStringLiteral("Servicing child process finished. / 維護子程序已經完成。"),
            QJsonObject{{QStringLiteral("runId"), m_runId},
                        {QStringLiteral("operationIndex"), index},
                        {QStringLiteral("exitCode"), exitCode},
                        {QStringLiteral("exitStatus"), static_cast<int>(exitStatus)}});
        finishOperation(index, exitStatus == QProcess::NormalExit && exitCode == 0,
                        QStringLiteral("exit code %1").arg(exitCode));
    });

    writeJournal(nullptr);
    emit operationChanged(index, ServicingPlan::operationStateName(operation.state), QString());
    emit stateChanged();
    process->start();
}

void JobEngine::finishOperation(int index, bool success, const QString &detail)
{
    if (index < 0 || index >= m_operations.size())
        return;
    ServicingOperation &operation = m_operations[index];
    if (terminal(operation.state))
        return;
    operation.state = m_cancelling ? OperationState::Cancelled
                    : success ? OperationState::Succeeded : OperationState::Failed;

    StructuredLogger::instance().log(
        success ? LogSeverity::Info : LogSeverity::Error,
        QStringLiteral("jobs"), QStringLiteral("operation.finished"),
        success
            ? QStringLiteral("The servicing operation completed. / 維護操作已經完成。")
            : QStringLiteral("The servicing operation did not complete successfully; diagnostic contents were omitted from logging. / 維護操作未能成功完成；記錄已經省略診斷內容。"),
        QJsonObject{{QStringLiteral("runId"), m_runId},
                    {QStringLiteral("operationIndex"), index},
                    {QStringLiteral("operationKind"),
                     ServicingPlan::operationKindName(operation.kind)},
                    {QStringLiteral("diagnosticPresent"), !detail.isEmpty()},
                    {QStringLiteral("state"),
                     ServicingPlan::operationStateName(operation.state)}});

    for (qsizetype activeIndex = m_active.size() - 1; activeIndex >= 0; --activeIndex) {
        if (m_active.at(activeIndex).index != index)
            continue;
        if (m_active.at(activeIndex).process)
            m_active.at(activeIndex).process->deleteLater();
        m_active.removeAt(activeIndex);
    }
    m_statusText = success ? QStringLiteral("Completed %1").arg(operation.titleEn)
                           : QStringLiteral("Stopped at %1").arg(operation.titleEn);
    writeJournal(nullptr);
    emit operationChanged(index, ServicingPlan::operationStateName(operation.state), detail);
    emit stateChanged();
    QTimer::singleShot(0, this, &JobEngine::schedule);
}

void JobEngine::finishRun(bool success, const QString &message)
{
    if (!m_running)
        return;
    m_running = false;
    m_statusText = message;
    StructuredLogger::instance().log(
        success ? LogSeverity::Info : LogSeverity::Error,
        QStringLiteral("jobs"), QStringLiteral("run.finished"),
        success
            ? QStringLiteral("The servicing run completed. / 維護工作已經完成。")
            : QStringLiteral("The servicing run stopped without success. / 維護工作未成功完成就停止咗。"),
        QJsonObject{{QStringLiteral("runId"), m_runId},
                    {QStringLiteral("success"), success},
                    {QStringLiteral("cancelled"), m_cancelling},
                    {QStringLiteral("summaryPresent"), !message.isEmpty()},
                    {QStringLiteral("completedOperations"), completedCount()},
                    {QStringLiteral("operationCount"), m_operations.size()}});
    writeJournal(nullptr);
    // Rewrite the final status after writeJournal's running/cancelling value.
    QFile file(journalPath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        file.close();
        QJsonObject object = document.object();
        object.insert(QStringLiteral("status"), success ? QStringLiteral("succeeded")
                                                        : (m_cancelling ? QStringLiteral("cancelled")
                                                                        : QStringLiteral("failed")));
        object.insert(QStringLiteral("finishedAt"), nowIso());
        QSaveFile output(journalPath());
        if (output.open(QIODevice::WriteOnly)) {
            output.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
            output.commit();
        }
    }
    emit stateChanged();
    emit finished(success, message);
}

bool JobEngine::writeJournal(QString *error) const
{
    if (m_project.projectDirectory.isEmpty()) {
        setError(error, QStringLiteral("No project is loaded for the recovery journal."));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(journalPath()).absolutePath())) {
        setError(error, QStringLiteral("Could not create recovery journal folder."));
        return false;
    }
    QJsonArray operations;
    for (const ServicingOperation &operation : m_operations) {
        operations.append(QJsonObject{
            {QStringLiteral("id"), operation.id},
            {QStringLiteral("kind"), ServicingPlan::operationKindName(operation.kind)},
            {QStringLiteral("title"), operation.titleEn},
            {QStringLiteral("state"), ServicingPlan::operationStateName(operation.state)},
            {QStringLiteral("argumentCount"), operation.arguments.size()},
            {QStringLiteral("commandOmitted"), true},
            {QStringLiteral("destructive"), operation.destructive},
            {QStringLiteral("checkpointBefore"), operation.checkpointBefore},
            {QStringLiteral("dependsOn"), QJsonArray::fromStringList(operation.dependsOn)},
        });
    }
    const QJsonObject journal{
        {QStringLiteral("schema"), QStringLiteral("wimforge.job-journal")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("runId"), m_runId},
        {QStringLiteral("project"), m_project.projectDirectory},
        {QStringLiteral("projectConfig"), m_project.toJson()},
        {QStringLiteral("sourcePath"), m_project.sourcePath},
        {QStringLiteral("imagePath"), m_project.imagePath},
        {QStringLiteral("mountPath"), m_project.mountPath},
        {QStringLiteral("startedAt"), m_startedAt},
        {QStringLiteral("updatedAt"), nowIso()},
        {QStringLiteral("status"), m_running ? (m_cancelling ? QStringLiteral("cancelling")
                                                              : QStringLiteral("running"))
                                             : QStringLiteral("finished")},
        {QStringLiteral("maximumParallel"), m_maximumParallel},
        {QStringLiteral("operations"), operations},
    };
    QSaveFile file(journalPath());
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    const QByteArray bytes = QJsonDocument(journal).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        setError(error, file.errorString());
        file.cancelWriting();
        return false;
    }
#ifdef Q_OS_WIN
    // QSaveFile is atomic; explicitly flush the temporary file to disk before rename.
    if (file.handle() != -1)
        ::FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())));
#endif
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    setError(error, {});
    return true;
}

void JobEngine::appendLog(const QString &path, const QByteArray &bytes) const
{
    QString decoded = QString::fromUtf8(bytes);
    if (decoded.contains(QChar::ReplacementCharacter))
        decoded = QString::fromLocal8Bit(bytes);
    const QByteArray safeBytes = StructuredLogger::redactText(decoded).toUtf8();
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(safeBytes);
        file.flush();
    }
}

} // namespace wimforge
