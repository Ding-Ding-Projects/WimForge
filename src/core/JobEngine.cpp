#include "JobEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#endif

#include <algorithm>
#include <utility>

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
        || state == OperationState::Skipped || state == OperationState::Cancelled;
}

} // namespace

JobEngine::JobEngine(QObject *parent) : QObject(parent) {}

JobEngine::~JobEngine()
{
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
    if (m_running) {
        setError(error, QStringLiteral("A servicing run is already active."));
        return false;
    }
    if (operations.isEmpty()) {
        setError(error, QStringLiteral("The servicing plan is empty."));
        return false;
    }
    const ProjectValidation validation = project.validateForExecution();
    if (!validation.ok()) {
        setError(error, validation.message());
        return false;
    }
    if (std::any_of(operations.cbegin(), operations.cend(), [](const ServicingOperation &item) {
            return item.requiresAdministrator;
        }) && !isAdministrator()) {
        setError(error, QStringLiteral("This plan contains Administrator operations. Relaunch WimForge as Administrator before running it."));
        return false;
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
        setError(error, QStringLiteral("Could not create the crash-recovery state folder."));
        return false;
    }
    QString journalError;
    if (!writeJournal(&journalError)) {
        m_running = false;
        setError(error, journalError);
        return false;
    }
    setError(error, {});
    emit stateChanged();
    QTimer::singleShot(0, this, &JobEngine::schedule);
    return true;
}

void JobEngine::cancel()
{
    if (!m_running || m_cancelling)
        return;
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
    for (const QString &dependency : operation.dependsOn) {
        const auto it = m_indexById.constFind(dependency);
        if (it == m_indexById.cend() || !terminal(m_operations.at(*it).state))
            return false;
    }
    return true;
}

bool JobEngine::dependencyFailed(const ServicingOperation &operation) const
{
    for (const QString &dependency : operation.dependsOn) {
        const auto it = m_indexById.constFind(dependency);
        if (it != m_indexById.cend()) {
            const OperationState state = m_operations.at(*it).state;
            if (state == OperationState::Failed || state == OperationState::Cancelled
                || state == OperationState::Skipped)
                return true;
        }
    }
    return false;
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
    for (qsizetype index = 0; index < m_operations.size(); ++index) {
        ServicingOperation &operation = m_operations[index];
        if (operation.state == OperationState::Queued && dependenciesDone(operation)
            && dependencyFailed(operation)) {
            operation.state = OperationState::Skipped;
            changed = true;
            emit operationChanged(static_cast<int>(index), ServicingPlan::operationStateName(operation.state),
                                  QStringLiteral("Skipped because a dependency failed."));
        }
    }

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
        const bool success = std::none_of(m_operations.cbegin(), m_operations.cend(), [](const ServicingOperation &item) {
            return item.state == OperationState::Failed || item.state == OperationState::Cancelled;
        });
        finishRun(success, success ? QStringLiteral("All servicing operations completed.")
                                   : (m_cancelling ? QStringLiteral("Servicing run cancelled safely.")
                                                   : QStringLiteral("Servicing stopped after a failed operation.")));
    }
}

void JobEngine::launch(int index)
{
    ServicingOperation &operation = m_operations[index];
    operation.state = OperationState::Running;
    m_statusText = operation.titleEn;

    const QString logDirectory = QDir(stateDirectory()).filePath(QStringLiteral("logs/%1").arg(m_runId));
    QDir().mkpath(logDirectory);
    const QString logPath = QDir(logDirectory).filePath(operation.id + QStringLiteral(".log"));
    const QString header = QStringLiteral("[%1] %2\n$ %3\n\n")
                               .arg(nowIso(), operation.titleEn, operation.previewCommand());
    appendLog(logPath, header.toUtf8());

    auto *process = new QProcess(this);
    process->setProgram(operation.executable);
    process->setArguments(operation.arguments);
    process->setProcessChannelMode(QProcess::MergedChannels);
    if (!operation.workingDirectory.isEmpty())
        process->setWorkingDirectory(operation.workingDirectory);
    m_active.append(ActiveProcess{index, process, logPath});

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, index, logPath] {
        const QByteArray bytes = process->readAllStandardOutput();
        appendLog(logPath, bytes);
        emit outputReceived(index, QString::fromLocal8Bit(bytes));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, index](QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart)
            finishOperation(index, false, process->errorString());
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, index, logPath](int exitCode, QProcess::ExitStatus exitStatus) {
        const QByteArray trailing = process->readAllStandardOutput();
        if (!trailing.isEmpty()) {
            appendLog(logPath, trailing);
            emit outputReceived(index, QString::fromLocal8Bit(trailing));
        }
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
    writeJournal(nullptr);
    // Rewrite the final status after writeJournal's running/cancelling value.
    QFile file(journalPath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument document = QJsonDocument::fromJson(file.readAll());
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
            {QStringLiteral("command"), operation.previewCommand()},
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
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(bytes);
        file.flush();
    }
}

} // namespace wimforge
