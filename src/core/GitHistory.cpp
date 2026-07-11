#include "GitHistory.h"
#include "ProcessLaunch.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>

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

QString nullDevicePath()
{
#ifdef Q_OS_WIN
    return QStringLiteral("NUL");
#else
    return QStringLiteral("/dev/null");
#endif
}

QStringList hardenedArguments(const QString &workingDirectory,
                              const QStringList &arguments)
{
    const QString nullDevice = nullDevicePath();
    QStringList result{
        QStringLiteral("--no-pager"),
        QStringLiteral("-c"), QStringLiteral("core.hooksPath=%1").arg(nullDevice),
        QStringLiteral("-c"), QStringLiteral("core.fsmonitor=false"),
        QStringLiteral("-c"), QStringLiteral("core.attributesFile=%1").arg(nullDevice),
        QStringLiteral("-c"), QStringLiteral("core.bare=false"),
        QStringLiteral("-c"), QStringLiteral("core.worktree=%1").arg(
            QDir(workingDirectory).absolutePath()),
        QStringLiteral("-c"), QStringLiteral("commit.gpgSign=false"),
    };
    result.append(arguments);
    return result;
}

CommandResult runGit(const QString &workingDirectory,
                     const QStringList &arguments,
                     int timeoutMilliseconds = 30'000)
{
    QProcess process;
    configureProcessWithoutConsole(process);
    process.setWorkingDirectory(workingDirectory);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : environment.keys()) {
        if (name.startsWith(QStringLiteral("GIT_"), Qt::CaseInsensitive))
            environment.remove(name);
    }
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QString());
    // Project-local history is application state, not a general-purpose Git
    // execution surface. Ignore user/system configuration that can select
    // executable hooks or global attribute drivers. Known local executable
    // controls are overridden again on every command line below.
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_SYSTEM"), nullDevicePath());
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), nullDevicePath());
    environment.insert(QStringLiteral("GIT_ATTR_NOSYSTEM"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);

    process.start(resolveExecutableForLaunch(QStringLiteral("git")),
                  hardenedArguments(workingDirectory, arguments),
                  QIODevice::ReadOnly);

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
    return message.isEmpty() ? QStringLiteral("Save project / 儲存工程") : message;
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

bool ensureNoExecutableAttributes(const QString &repositoryPath,
                                  const QStringList &trackedFiles,
                                  QString *error)
{
    QStringList arguments{
        QStringLiteral("check-attr"), QStringLiteral("-z"),
        QStringLiteral("filter"), QStringLiteral("diff"),
        QStringLiteral("merge"), QStringLiteral("--"),
    };
    arguments.append(trackedFiles);
    const CommandResult result = runGit(repositoryPath, arguments);
    if (!result.ok()) {
        setError(error, QStringLiteral("Could not validate Git attributes for local history: %1")
                            .arg(result.detail()));
        return false;
    }

    const QStringList fields = result.standardOutput.split(QChar::Null,
                                                            Qt::SkipEmptyParts);
    if (fields.size() % 3 != 0) {
        setError(error, QStringLiteral("Git returned malformed attribute data for local history."));
        return false;
    }
    for (qsizetype index = 0; index < fields.size(); index += 3) {
        const QString value = fields.at(index + 2).trimmed().toLower();
        if (value == QStringLiteral("unspecified") || value == QStringLiteral("unset"))
            continue;
        setError(error,
                 QStringLiteral("Tracked state '%1' uses executable-capable Git attribute '%2=%3'. "
                                "Remove filter/diff/merge attributes from WimForge state files.")
                     .arg(fields.at(index), fields.at(index + 1), fields.at(index + 2)));
        return false;
    }
    return true;
}

bool stageTrackedFilesWithoutFilters(const QString &repositoryPath,
                                     const QStringList &trackedFiles,
                                     QString *error)
{
    static const QRegularExpression objectId(
        QStringLiteral("^(?:[0-9A-Fa-f]{40}|[0-9A-Fa-f]{64})$"));
    for (const QString &relativePath : trackedFiles) {
        const QFileInfo stateFile(QDir(repositoryPath).filePath(relativePath));
        if (stateFile.isSymLink()) {
            setError(error, QStringLiteral("Tracked state cannot be a symbolic link: %1")
                                .arg(relativePath));
            return false;
        }
#ifdef Q_OS_WIN
        if (stateFile.isJunction()) {
            setError(error, QStringLiteral("Tracked state cannot be a junction: %1")
                                .arg(relativePath));
            return false;
        }
#endif
        if (stateFile.exists() && !stateFile.isFile()) {
            setError(error, QStringLiteral("Tracked state must be an ordinary file: %1")
                                .arg(relativePath));
            return false;
        }

        if (!stateFile.exists()) {
            const CommandResult removed = runGit(
                repositoryPath,
                {QStringLiteral("update-index"), QStringLiteral("--force-remove"),
                 QStringLiteral("--"), relativePath});
            if (!removed.ok()) {
                setError(error, QStringLiteral("Could not stage removal of '%1': %2")
                                    .arg(relativePath, removed.detail()));
                return false;
            }
            continue;
        }

        const CommandResult hashed = runGit(
            repositoryPath,
            {QStringLiteral("hash-object"), QStringLiteral("-w"),
             QStringLiteral("--no-filters"), QStringLiteral("--"), relativePath});
        const QString hash = hashed.standardOutput.trimmed();
        if (!hashed.ok() || !objectId.match(hash).hasMatch()) {
            setError(error, QStringLiteral("Could not hash '%1' without Git filters: %2")
                                .arg(relativePath, hashed.ok()
                                                       ? QStringLiteral("invalid object id")
                                                       : hashed.detail()));
            return false;
        }
        const CommandResult staged = runGit(
            repositoryPath,
            {QStringLiteral("update-index"), QStringLiteral("--add"),
             QStringLiteral("--cacheinfo"), QStringLiteral("100644"), hash,
             relativePath});
        if (!staged.ok()) {
            setError(error, QStringLiteral("Could not stage raw state '%1': %2")
                                .arg(relativePath, staged.detail()));
            return false;
        }
    }
    return true;
}

QSet<QString> normalizedTrackedPaths(const QStringList &trackedFiles)
{
    QSet<QString> allowedPaths;
    for (const QString &path : trackedFiles)
        allowedPaths.insert(QDir::fromNativeSeparators(QDir::cleanPath(path)));
    return allowedPaths;
}

bool ensureTreeStateEntriesAreOrdinary(const QString &repositoryPath,
                                       const QStringList &trackedFiles,
                                       const QString &treeish,
                                       QString *error)
{
    const QSet<QString> allowedPaths = normalizedTrackedPaths(trackedFiles);
    QStringList arguments{
        QStringLiteral("ls-tree"), QStringLiteral("-r"), QStringLiteral("-z"),
        QStringLiteral("--full-tree"), treeish, QStringLiteral("--"),
    };
    arguments.append(trackedFiles);

    const CommandResult result = runGit(repositoryPath, arguments);
    if (!result.ok()) {
        setError(error, QStringLiteral("Could not validate state entries in local history tree %1: %2")
                            .arg(treeish, result.detail()));
        return false;
    }

    const QStringList records = result.standardOutput.split(QChar::Null,
                                                             Qt::SkipEmptyParts);
    for (const QString &record : records) {
        const qsizetype tab = record.indexOf(QLatin1Char('\t'));
        const QStringList metadata = tab < 0
            ? QStringList()
            : record.first(tab).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString path = tab < 0 ? QString() : record.sliced(tab + 1);
        if (metadata.size() != 3 || metadata.at(0) != QStringLiteral("100644")
            || metadata.at(1) != QStringLiteral("blob") || !allowedPaths.contains(path)) {
            setError(error,
                     QStringLiteral("Local history tree %1 contains a non-file state object: %2")
                         .arg(treeish, path.isEmpty() ? QStringLiteral("<malformed entry>") : path));
            return false;
        }
    }
    return true;
}

bool ensureLatestCommitTouchesOnlyTrackedFiles(const QString &repositoryPath,
                                               const QStringList &trackedFiles,
                                               QString *error)
{
    const QSet<QString> allowedPaths = normalizedTrackedPaths(trackedFiles);
    const CommandResult changed = runGit(
        repositoryPath,
        {QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"),
         QStringLiteral("--name-only"), QStringLiteral("--no-renames"),
         QStringLiteral("-r"), QStringLiteral("-z"), QStringLiteral("HEAD^"),
         QStringLiteral("HEAD"), QStringLiteral("--")});
    if (!changed.ok()) {
        setError(error, QStringLiteral("Could not validate the latest local-history change: %1")
                            .arg(changed.detail()));
        return false;
    }
    const QStringList changedPaths = changed.standardOutput.split(QChar::Null,
                                                                   Qt::SkipEmptyParts);
    for (const QString &path : changedPaths) {
        if (!allowedPaths.contains(path)) {
            setError(error,
                     QStringLiteral("The latest local-history commit changes unexpected path '%1'.")
                         .arg(path));
            return false;
        }
    }
    return ensureTreeStateEntriesAreOrdinary(repositoryPath, trackedFiles,
                                             QStringLiteral("HEAD"), error)
        && ensureTreeStateEntriesAreOrdinary(repositoryPath, trackedFiles,
                                             QStringLiteral("HEAD^"), error);
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
    const QFileInfo gitDirectory(
        QDir(m_repositoryPath).filePath(QStringLiteral(".git")));
    if (!gitDirectory.exists() || !gitDirectory.isDir() || gitDirectory.isSymLink())
        return false;
#ifdef Q_OS_WIN
    if (gitDirectory.isJunction())
        return false;
#endif
    return true;
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

    const QFileInfo gitPath(QDir(m_repositoryPath).filePath(QStringLiteral(".git")));
    if (gitPath.exists() && !isRepository()) {
        setError(error, QStringLiteral(
            "The local history .git path must be a real directory, not a file, link, or junction."));
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
    if (!ensureNoExecutableAttributes(m_repositoryPath, m_trackedFiles, error)
        || !stageTrackedFilesWithoutFilters(m_repositoryPath, m_trackedFiles, error)) {
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
    if (!ensureNoExecutableAttributes(m_repositoryPath, m_trackedFiles, error))
        return false;

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
    if (!ensureLatestCommitTouchesOnlyTrackedFiles(m_repositoryPath,
                                                   m_trackedFiles, error)) {
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
    const QString subject = QStringLiteral("Revert \"%1\" / 還原「%1」").arg(target.subject);
    const QString body = QStringLiteral("This reverts commit %1. / 呢次會還原 commit %1。")
                             .arg(target.hash);
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
