#include "GitHistory.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <utility>

namespace wimforge {
namespace {

struct CommandResult
{
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QString standardOutput;
    QString standardError;

    [[nodiscard]] bool ok() const
    {
        return started && finished && exitCode == 0;
    }

    [[nodiscard]] QString detail() const
    {
        const QString stderrText = standardError.trimmed();
        const QString stdoutText = standardOutput.trimmed();
        if (!stderrText.isEmpty())
            return stderrText;
        if (!stdoutText.isEmpty())
            return stdoutText;
        if (!started)
            return QStringLiteral("Git could not be started. Install Git and make sure git.exe is on PATH.");
        if (!finished)
            return QStringLiteral("Git did not finish before the timeout.");
        return QStringLiteral("Git exited with code %1.").arg(exitCode);
    }
};

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

CommandResult runGit(const QString &workingDirectory,
                     const QStringList &arguments,
                     int timeoutMilliseconds = 30'000)
{
    QProcess process;
    process.setWorkingDirectory(workingDirectory);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    process.setProcessEnvironment(environment);

    process.start(QStringLiteral("git"), arguments, QIODevice::ReadOnly);

    CommandResult result;
    result.started = process.waitForStarted(10'000);
    if (!result.started) {
        result.standardError = process.errorString();
        return result;
    }

    result.finished = process.waitForFinished(timeoutMilliseconds);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(2'000);
    }

    result.exitCode = process.exitCode();
    result.standardOutput = QString::fromUtf8(process.readAllStandardOutput());
    result.standardError = QString::fromUtf8(process.readAllStandardError());
    return result;
}

QString cleanCommitMessage(QString message)
{
    message.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
    message = message.simplified();
    return message.isEmpty() ? QStringLiteral("Save project") : message;
}

bool isSafeRelativePath(const QString &path)
{
    if (path.trimmed().isEmpty() || QFileInfo(path).isAbsolute())
        return false;

    const QString clean = QDir::cleanPath(path);
    return clean != QStringLiteral("..")
        && !clean.startsWith(QStringLiteral("../"))
        && !clean.startsWith(QStringLiteral("..\\"));
}

QStringList pathspecArguments(const QStringList &trackedFiles)
{
    QStringList result{QStringLiteral("--")};
    result.append(trackedFiles);
    return result;
}

} // namespace

GitHistory::GitHistory(QString repositoryPath, QStringList trackedFiles)
    : m_trackedFiles(std::move(trackedFiles))
{
    m_repositoryPath = repositoryPath.trimmed().isEmpty()
        ? QString()
        : QDir(repositoryPath).absolutePath();
    m_trackedFiles.removeDuplicates();
}

QString GitHistory::repositoryPath() const
{
    return m_repositoryPath;
}

QStringList GitHistory::trackedFiles() const
{
    return m_trackedFiles;
}

bool GitHistory::isRepository() const
{
    return QFileInfo::exists(QDir(m_repositoryPath).filePath(QStringLiteral(".git")));
}

bool GitHistory::gitAvailable(QString *error)
{
    const CommandResult result = runGit(QDir::currentPath(), {QStringLiteral("--version")}, 10'000);
    if (!result.ok()) {
        setError(error, result.detail());
        return false;
    }
    setError(error, {});
    return true;
}

bool GitHistory::initialize(QString *error) const
{
    if (m_repositoryPath.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Repository path is empty."));
        return false;
    }
    if (m_trackedFiles.isEmpty()) {
        setError(error, QStringLiteral("No state files were selected for Git history."));
        return false;
    }
    for (const QString &file : m_trackedFiles) {
        if (!isSafeRelativePath(file)) {
            setError(error, QStringLiteral("Tracked path '%1' must stay inside the repository.").arg(file));
            return false;
        }
    }

    const QFileInfo repositoryInfo(m_repositoryPath);
    if (repositoryInfo.exists() && !repositoryInfo.isDir()) {
        setError(error, QStringLiteral("Repository path exists but is not a folder: %1").arg(m_repositoryPath));
        return false;
    }
    if (!QDir().mkpath(m_repositoryPath)) {
        setError(error, QStringLiteral("Could not create repository folder: %1").arg(m_repositoryPath));
        return false;
    }

    if (!isRepository()) {
        const CommandResult init = runGit(m_repositoryPath, {QStringLiteral("init")});
        if (!init.ok()) {
            setError(error, QStringLiteral("Could not initialize local Git history: %1").arg(init.detail()));
            return false;
        }
    }

    const QList<QStringList> configuration{
        {QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("user.name"), QStringLiteral("WimForge")},
        {QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("user.email"), QStringLiteral("wimforge@localhost")},
        {QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("commit.gpgSign"), QStringLiteral("false")},
        {QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("core.autocrlf"), QStringLiteral("false")},
    };
    for (const QStringList &arguments : configuration) {
        const CommandResult configured = runGit(m_repositoryPath, arguments);
        if (!configured.ok()) {
            setError(error, QStringLiteral("Could not configure local Git history: %1").arg(configured.detail()));
            return false;
        }
    }

    setError(error, {});
    return true;
}

bool GitHistory::commit(const QString &message, QString *error) const
{
    if (!initialize(error))
        return false;

    QStringList addArguments{QStringLiteral("add"), QStringLiteral("-A")};
    addArguments.append(pathspecArguments(m_trackedFiles));
    const CommandResult staged = runGit(m_repositoryPath, addArguments);
    if (!staged.ok()) {
        setError(error, QStringLiteral("Could not stage state for local history: %1").arg(staged.detail()));
        return false;
    }

    // --allow-empty is intentional: a user-visible Save is still an auditable
    // event even when the serialized project happens to be byte-identical.
    const CommandResult committed = runGit(
        m_repositoryPath,
        {QStringLiteral("commit"), QStringLiteral("--allow-empty"), QStringLiteral("--no-verify"),
         QStringLiteral("--no-gpg-sign"), QStringLiteral("-m"), cleanCommitMessage(message)});
    if (!committed.ok()) {
        setError(error, QStringLiteral("Could not commit local history: %1").arg(committed.detail()));
        return false;
    }

    setError(error, {});
    return true;
}

QList<GitCommit> GitHistory::history(int maximumCount, QString *error) const
{
    QList<GitCommit> commits;
    if (!isRepository()) {
        setError(error, {});
        return commits;
    }

    maximumCount = qBound(1, maximumCount, 10'000);
    const QString format = QStringLiteral("%H%x1f%h%x1f%aI%x1f%s%x1e");
    const CommandResult result = runGit(
        m_repositoryPath,
        {QStringLiteral("log"), QStringLiteral("--no-decorate"), QStringLiteral("--encoding=UTF-8"),
         QStringLiteral("--max-count=%1").arg(maximumCount), QStringLiteral("--pretty=format:%1").arg(format)});

    // A freshly initialized repository legitimately has no HEAD yet.
    if (!result.ok()) {
        const CommandResult head = runGit(
            m_repositoryPath, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("HEAD")});
        if (!head.ok()) {
            setError(error, {});
            return commits;
        }
        setError(error, QStringLiteral("Could not read local history: %1").arg(result.detail()));
        return commits;
    }

    const QChar recordSeparator(0x1e);
    const QChar fieldSeparator(0x1f);
    const QStringList records = result.standardOutput.split(recordSeparator, Qt::SkipEmptyParts);
    for (QString record : records) {
        record = record.trimmed();
        const QStringList fields = record.split(fieldSeparator, Qt::KeepEmptyParts);
        if (fields.size() < 4)
            continue;
        commits.append(GitCommit{
            fields.at(0).trimmed(),
            fields.at(1).trimmed(),
            QDateTime::fromString(fields.at(2).trimmed(), Qt::ISODate),
            fields.mid(3).join(QString(fieldSeparator)).trimmed(),
        });
    }

    setError(error, {});
    return commits;
}

bool GitHistory::revertLatest(QString *error) const
{
    if (!isRepository()) {
        setError(error, QStringLiteral("This folder does not have local Git history yet."));
        return false;
    }

    QStringList statusArguments{QStringLiteral("status"), QStringLiteral("--porcelain=v1")};
    statusArguments.append(pathspecArguments(m_trackedFiles));
    const CommandResult status = runGit(m_repositoryPath, statusArguments);
    if (!status.ok()) {
        setError(error, QStringLiteral("Could not inspect local history: %1").arg(status.detail()));
        return false;
    }
    if (!status.standardOutput.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Save the current state before reverting history."));
        return false;
    }

    const CommandResult countResult = runGit(
        m_repositoryPath,
        {QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("HEAD")});
    bool countOk = false;
    const int count = countResult.standardOutput.trimmed().toInt(&countOk);
    if (!countResult.ok() || !countOk) {
        setError(error, QStringLiteral("Could not count local history commits: %1").arg(countResult.detail()));
        return false;
    }
    if (count < 2) {
        setError(error, QStringLiteral("There is no earlier saved state to restore."));
        return false;
    }

    const QList<GitCommit> latest = history(1, error);
    if (latest.isEmpty()) {
        if (!error || error->isEmpty())
            setError(error, QStringLiteral("There is no saved state to revert."));
        return false;
    }

    // Apply the inverse without committing first. We then create the commit
    // ourselves with --allow-empty, which also handles an audited no-op Save.
    const CommandResult reverted = runGit(
        m_repositoryPath,
        {QStringLiteral("revert"), QStringLiteral("--no-commit"), QStringLiteral("HEAD")});
    if (!reverted.ok()) {
        runGit(m_repositoryPath, {QStringLiteral("revert"), QStringLiteral("--abort")});
        setError(error, QStringLiteral("Could not revert the latest saved state: %1").arg(reverted.detail()));
        return false;
    }

    const GitCommit &target = latest.first();
    const QString subject = QStringLiteral("Revert \"%1\"").arg(target.subject);
    const QString body = QStringLiteral("This reverts commit %1.").arg(target.hash);
    const CommandResult committed = runGit(
        m_repositoryPath,
        {QStringLiteral("commit"), QStringLiteral("--allow-empty"), QStringLiteral("--no-verify"),
         QStringLiteral("--no-gpg-sign"), QStringLiteral("-m"), subject, QStringLiteral("-m"), body});
    if (!committed.ok()) {
        runGit(m_repositoryPath, {QStringLiteral("revert"), QStringLiteral("--abort")});
        setError(error, QStringLiteral("The state was reverted but the revert commit failed: %1")
                            .arg(committed.detail()));
        return false;
    }

    setError(error, {});
    return true;
}

} // namespace wimforge
