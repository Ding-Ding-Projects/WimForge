#include "core/ProjectBundle.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

using namespace wimforge;

namespace {

class TestRun
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int result() const
    {
        if (m_failures == 0)
            QTextStream(stdout) << "project_bundle_tests: all checks passed\n";
        return m_failures == 0 ? 0 : 1;
    }

private:
    int m_failures = 0;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size()
        && file.commit();
}

bool runGit(const QString &repository,
            const QStringList &arguments,
            QByteArray *standardOutput = nullptr,
            QString *error = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(repository);
    process.setProgram(QStringLiteral("git"));
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000) || !process.waitForFinished(30000)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("git %1 failed: %2")
                         .arg(arguments.join(QLatin1Char(' ')),
                              QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        }
        return false;
    }
    if (standardOutput)
        *standardOutput = process.readAllStandardOutput();
    return true;
}

bool commitFile(const QString &repository,
                const QString &relativePath,
                const QByteArray &contents,
                const QString &message,
                QString *error)
{
    if (!writeFile(QDir(repository).filePath(relativePath), contents)) {
        if (error)
            *error = QStringLiteral("Could not write commit fixture.");
        return false;
    }
    return runGit(repository, {QStringLiteral("add"), QStringLiteral("--all")}, nullptr, error)
        && runGit(repository,
                  {QStringLiteral("commit"), QStringLiteral("-m"), message},
                  nullptr,
                  error);
}

bool createRepository(const QString &repository, const QString &identity, QString *error)
{
    if (!QDir().mkpath(repository)
        || !runGit(repository,
                   {QStringLiteral("init"), QStringLiteral("--initial-branch=main")},
                   nullptr,
                   error)
        || !runGit(repository,
                   {QStringLiteral("config"), QStringLiteral("user.name"),
                    QStringLiteral("WimForge Tests")},
                   nullptr,
                   error)
        || !runGit(repository,
                   {QStringLiteral("config"), QStringLiteral("user.email"),
                    QStringLiteral("tests@wimforge.invalid")},
                   nullptr,
                   error)
        || !runGit(repository,
                   {QStringLiteral("config"), QStringLiteral("core.autocrlf"),
                    QStringLiteral("false")},
                   nullptr,
                   error)
        || !commitFile(repository,
                       QStringLiteral("state.json"),
                       QStringLiteral("{\"role\":\"%1\",\"revision\":1}\n").arg(identity).toUtf8(),
                       QStringLiteral("Create %1 history").arg(identity),
                       error)
        || !commitFile(repository,
                       QStringLiteral("state.json"),
                       QStringLiteral("{\"role\":\"%1\",\"revision\":2}\n").arg(identity).toUtf8(),
                       QStringLiteral("Change %1 state").arg(identity),
                       error)
        || !runGit(repository,
                   {QStringLiteral("tag"), QStringLiteral("-a"), QStringLiteral("checkpoint-v2"),
                    QStringLiteral("-m"), QStringLiteral("Audited checkpoint")},
                   nullptr,
                   error)
        || !runGit(repository,
                   {QStringLiteral("checkout"), QStringLiteral("-b"),
                    QStringLiteral("experiments/alternate")},
                   nullptr,
                   error)
        || !commitFile(repository,
                       QStringLiteral("alternate.txt"),
                       QByteArrayLiteral("branch-only state\n"),
                       QStringLiteral("Record alternate branch"),
                       error)
        || !runGit(repository,
                   {QStringLiteral("checkout"), QStringLiteral("main")},
                   nullptr,
                   error)
        || !commitFile(repository,
                       QStringLiteral("state.json"),
                       QStringLiteral("{\"role\":\"%1\",\"revision\":3}\n").arg(identity).toUtf8(),
                       QStringLiteral("Apply undoable %1 action").arg(identity),
                       error)
        || !runGit(repository,
                   {QStringLiteral("revert"), QStringLiteral("--no-edit"), QStringLiteral("HEAD")},
                   nullptr,
                   error)
        || !runGit(repository,
                   {QStringLiteral("tag"), QStringLiteral("undo-commit")},
                   nullptr,
                   error)) {
        return false;
    }
    return true;
}

struct TreeSnapshot
{
    QMap<QString, QByteArray> files;
    QSet<QString> directories;
    QByteArray refs;
    QByteArray objectInventory;
};

TreeSnapshot snapshotRepository(const QString &repository, QString *error)
{
    TreeSnapshot snapshot;
    QDirIterator iterator(repository,
                          QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    const QDir root(repository);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        const QString relative = QDir::fromNativeSeparators(root.relativeFilePath(info.filePath()));
        if (info.isDir()) {
            snapshot.directories.insert(relative);
        } else if (info.isFile()) {
            QFile file(info.filePath());
            if (!file.open(QIODevice::ReadOnly)) {
                if (error)
                    *error = QStringLiteral("Could not snapshot %1").arg(relative);
                return {};
            }
            snapshot.files.insert(relative, file.readAll());
        }
    }
    if (!runGit(repository,
                {QStringLiteral("show-ref"), QStringLiteral("--head"),
                 QStringLiteral("--dereference")},
                &snapshot.refs,
                error)
        || !runGit(repository,
                   {QStringLiteral("rev-list"), QStringLiteral("--all"),
                    QStringLiteral("--objects")},
                   &snapshot.objectInventory,
                   error)) {
        return {};
    }
    return snapshot;
}

bool snapshotsEqual(const TreeSnapshot &expected,
                    const TreeSnapshot &actual,
                    QString *difference)
{
    if (expected.directories != actual.directories) {
        if (difference)
            *difference = QStringLiteral("directory inventory differs");
        return false;
    }
    if (expected.files.keys() != actual.files.keys()) {
        if (difference)
            *difference = QStringLiteral("file inventory differs");
        return false;
    }
    for (auto iterator = expected.files.cbegin(); iterator != expected.files.cend(); ++iterator) {
        if (actual.files.value(iterator.key()) != iterator.value()) {
            if (difference)
                *difference = QStringLiteral("bytes differ for %1").arg(iterator.key());
            return false;
        }
    }
    if (expected.refs != actual.refs || expected.objectInventory != actual.objectInventory) {
        if (difference)
            *difference = QStringLiteral("Git refs or reachable-object inventory differs");
        return false;
    }
    return true;
}

bool copyFile(const QString &source, const QString &destination)
{
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    return writeFile(destination, input.readAll());
}

bool writeTraversalBundle(const QString &path)
{
    const QByteArray payload = QByteArrayLiteral("owned");
    QJsonObject entry;
    entry.insert(QStringLiteral("path"), QStringLiteral("../escaped.txt"));
    entry.insert(QStringLiteral("kind"), QStringLiteral("file"));
    entry.insert(QStringLiteral("permissions"), QStringLiteral("0"));
    entry.insert(QStringLiteral("modifiedMs"), QStringLiteral("0"));
    entry.insert(QStringLiteral("offset"), QStringLiteral("0"));
    entry.insert(QStringLiteral("size"), QString::number(payload.size()));
    entry.insert(QStringLiteral("sha256"),
                 QString::fromLatin1(
                     QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));

    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QStringLiteral("org.wimforge.project-bundle"));
    manifest.insert(QStringLiteral("formatVersion"), ProjectBundle::CurrentFormatVersion);
    manifest.insert(QStringLiteral("createdUtc"), QStringLiteral("2026-07-10T00:00:00.000Z"));
    manifest.insert(QStringLiteral("generator"), QStringLiteral("hostile-test"));
    manifest.insert(QStringLiteral("payloadBytes"), QString::number(payload.size()));
    manifest.insert(QStringLiteral("repositories"), QJsonArray());
    manifest.insert(QStringLiteral("standaloneFiles"), QJsonArray());
    manifest.insert(QStringLiteral("entries"), QJsonArray{entry});
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    const QByteArray digest =
        QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256);

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(QByteArrayLiteral("WIMFORGE-BUNDLE\x1a")) != 16) {
        return false;
    }
    QDataStream stream(&output);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(ProjectBundle::CurrentFormatVersion) << quint32(0)
           << quint64(manifestBytes.size()) << quint64(payload.size());
    if (stream.status() != QDataStream::Ok || output.write(digest) != digest.size()
        || output.write(manifestBytes) != manifestBytes.size()
        || output.write(payload) != payload.size()) {
        output.cancelWriting();
        return false;
    }
    return output.commit();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    TestRun test;
    QTemporaryDir temporary;
    test.check(temporary.isValid(), QStringLiteral("temporary directory is available"));
    if (!temporary.isValid())
        return test.result();

    QString error;
    const QString sourceRoot = QDir(temporary.path()).filePath(QStringLiteral("source"));
    const QString projectRepository = QDir(sourceRoot).filePath(QStringLiteral("project"));
    const QString actionRepository = QDir(sourceRoot).filePath(QStringLiteral("actions"));
    const QString notificationRepository =
        QDir(sourceRoot).filePath(QStringLiteral("notifications"));
    test.check(createRepository(projectRepository, QStringLiteral("project"), &error),
               QStringLiteral("project repository fixture is complete: %1").arg(error));
    test.check(createRepository(actionRepository, QStringLiteral("action-history"), &error),
               QStringLiteral("action-history repository fixture is complete: %1").arg(error));
    test.check(createRepository(notificationRepository, QStringLiteral("notifications"), &error),
               QStringLiteral("notification repository fixture is complete: %1").arg(error));

    const TreeSnapshot projectSnapshot = snapshotRepository(projectRepository, &error);
    const TreeSnapshot actionSnapshot = snapshotRepository(actionRepository, &error);
    const TreeSnapshot notificationSnapshot = snapshotRepository(notificationRepository, &error);
    test.check(!projectSnapshot.files.isEmpty() && error.isEmpty(),
               QStringLiteral("all repository bytes were snapshotted: %1").arg(error));

    const QString noteSource = QDir(sourceRoot).filePath(QStringLiteral("portable-note.txt"));
    const QByteArray noteContents = QByteArrayLiteral("WimForge portable project metadata\n");
    test.check(writeFile(noteSource, noteContents), QStringLiteral("standalone file fixture exists"));

    const QString bundle = QDir(temporary.path()).filePath(QStringLiteral("saves/demo.wimforge"));
    const QList<ProjectBundleRepository> repositories = {
        {ProjectBundle::ProjectRepositoryRole, projectRepository, QStringLiteral("repos/project")},
        {ProjectBundle::ActionHistoryRepositoryRole,
         actionRepository,
         QStringLiteral("repos/action-history")},
        {ProjectBundle::NotificationRepositoryRole,
         notificationRepository,
         QStringLiteral("repos/notifications")},
    };
    const QList<ProjectBundleFile> files = {
        {noteSource, QStringLiteral("attachments/portable-note.txt")},
    };
    const bool exported = ProjectBundle::exportToFile(bundle, repositories, files, &error);
    test.check(exported, QStringLiteral("bundle export succeeds: %1").arg(error));
    test.check(QFileInfo(bundle).size() > 72, QStringLiteral("bundle contains manifest and payload"));

    test.check(QDir(sourceRoot).removeRecursively(),
               QStringLiteral("original repositories can be removed before restore"));
    const QString restoredRoot = QDir(temporary.path()).filePath(QStringLiteral("restored"));
    const std::optional<ProjectBundleImportResult> imported =
        ProjectBundle::importFromFile(bundle, restoredRoot, {}, &error);
    test.check(imported.has_value(), QStringLiteral("bundle import succeeds: %1").arg(error));
    if (imported) {
        test.check(imported->formatVersion == ProjectBundle::CurrentFormatVersion,
                   QStringLiteral("format version is reported"));
        test.check(imported->repositoryPaths.size() == 3,
                   QStringLiteral("all three repository roles are restored"));
        test.check(imported->standaloneFiles.size() == 1
                       && QFile(imported->standaloneFiles.first()).open(QIODevice::ReadOnly),
                   QStringLiteral("standalone files are restored"));
        QFile note(imported->standaloneFiles.value(0));
        test.check(note.open(QIODevice::ReadOnly) && note.readAll() == noteContents,
                   QStringLiteral("standalone file bytes are exact"));

        const QList<QPair<QString, TreeSnapshot>> expected = {
            {ProjectBundle::ProjectRepositoryRole, projectSnapshot},
            {ProjectBundle::ActionHistoryRepositoryRole, actionSnapshot},
            {ProjectBundle::NotificationRepositoryRole, notificationSnapshot},
        };
        for (const auto &[role, snapshot] : expected) {
            QString snapshotError;
            const QString restoredRepository = imported->repositoryPaths.value(role);
            const TreeSnapshot actual = snapshotRepository(restoredRepository, &snapshotError);
            QString difference;
            test.check(snapshotError.isEmpty() && snapshotsEqual(snapshot, actual, &difference),
                       QStringLiteral("%1 repo preserves files, .git objects, refs, tags, branches, and undo commits: %2 %3")
                           .arg(role, snapshotError, difference));
            QByteArray ignored;
            test.check(runGit(restoredRepository,
                              {QStringLiteral("fsck"), QStringLiteral("--full"),
                               QStringLiteral("--strict")},
                              &ignored,
                              &snapshotError),
                       QStringLiteral("%1 repository passes git fsck: %2")
                           .arg(role, snapshotError));
        }
    }

    const QString existingDestination =
        QDir(temporary.path()).filePath(QStringLiteral("existing-destination"));
    const QString sentinel = QDir(existingDestination).filePath(QStringLiteral("keep.txt"));
    test.check(writeFile(sentinel, QByteArrayLiteral("do not touch")),
               QStringLiteral("existing destination fixture exists"));
    const auto refusedExisting =
        ProjectBundle::importFromFile(bundle, existingDestination, {}, &error);
    test.check(!refusedExisting && QFileInfo::exists(sentinel),
               QStringLiteral("default import never overwrites an existing destination"));

    ProjectBundleImportOptions overwriteOptions;
    overwriteOptions.overwriteExisting = true;
    const auto overwritten =
        ProjectBundle::importFromFile(bundle, existingDestination, overwriteOptions, &error);
    test.check(overwritten.has_value() && !QFileInfo::exists(sentinel),
               QStringLiteral("explicit overwrite promotes validated staging safely: %1").arg(error));

    const QString corruptBundle =
        QDir(temporary.path()).filePath(QStringLiteral("saves/corrupt.wimforge"));
    test.check(copyFile(bundle, corruptBundle), QStringLiteral("corruption fixture copied"));
    QFile corrupt(corruptBundle);
    if (corrupt.open(QIODevice::ReadWrite) && corrupt.size() > 0) {
        corrupt.seek(corrupt.size() - 1);
        QByteArray byte = corrupt.read(1);
        byte[0] = static_cast<char>(byte[0] ^ 0x55);
        corrupt.seek(corrupt.size() - 1);
        corrupt.write(byte);
        corrupt.close();
    }
    const QString corruptDestination =
        QDir(temporary.path()).filePath(QStringLiteral("corrupt-import"));
    const auto corruptResult =
        ProjectBundle::importFromFile(corruptBundle, corruptDestination, {}, &error);
    test.check(!corruptResult && !QFileInfo::exists(corruptDestination)
                   && error.contains(QStringLiteral("Checksum"), Qt::CaseInsensitive),
               QStringLiteral("corrupt payload is rejected before promotion: %1").arg(error));

    const QString truncatedBundle =
        QDir(temporary.path()).filePath(QStringLiteral("saves/truncated.wimforge"));
    QFile validBundle(bundle);
    test.check(validBundle.open(QIODevice::ReadOnly), QStringLiteral("valid bundle opens for truncation"));
    const QByteArray firstHalf = validBundle.read(validBundle.size() / 2);
    test.check(writeFile(truncatedBundle, firstHalf), QStringLiteral("truncated fixture written"));
    const auto truncatedResult = ProjectBundle::importFromFile(
        truncatedBundle,
        QDir(temporary.path()).filePath(QStringLiteral("truncated-import")),
        {},
        &error);
    test.check(!truncatedResult, QStringLiteral("truncated bundle is rejected: %1").arg(error));

    const QString traversalBundle =
        QDir(temporary.path()).filePath(QStringLiteral("saves/traversal.wimforge"));
    test.check(writeTraversalBundle(traversalBundle),
               QStringLiteral("validly checksummed hostile manifest was created"));
    const QString traversalRoot =
        QDir(temporary.path()).filePath(QStringLiteral("traversal-import"));
    const QString escapedPath = QDir(temporary.path()).filePath(QStringLiteral("escaped.txt"));
    const auto traversalResult =
        ProjectBundle::importFromFile(traversalBundle, traversalRoot, {}, &error);
    test.check(!traversalResult && !QFileInfo::exists(traversalRoot)
                   && !QFileInfo::exists(escapedPath)
                   && error.contains(QStringLiteral("Unsafe"), Qt::CaseInsensitive),
               QStringLiteral("checksummed path traversal is refused: %1").arg(error));

#ifdef Q_OS_WIN
    const QString linkedRepository =
        QDir(temporary.path()).filePath(QStringLiteral("linked-source/project"));
    const QString junctionTarget =
        QDir(temporary.path()).filePath(QStringLiteral("junction-target"));
    test.check(createRepository(linkedRepository, QStringLiteral("linked"), &error)
                   && QDir().mkpath(junctionTarget)
                   && writeFile(QDir(junctionTarget).filePath(QStringLiteral("outside.txt")),
                                QByteArrayLiteral("must not be followed")),
               QStringLiteral("reparse-point fixture is ready: %1").arg(error));
    const QString junction = QDir(linkedRepository).filePath(QStringLiteral("outside-junction"));
    QProcess junctionProcess;
    junctionProcess.setProgram(QStringLiteral("cmd.exe"));
    junctionProcess.setArguments({QStringLiteral("/d"), QStringLiteral("/c"),
                                  QStringLiteral("mklink"), QStringLiteral("/J"),
                                  QDir::toNativeSeparators(junction),
                                  QDir::toNativeSeparators(junctionTarget)});
    junctionProcess.start();
    const bool junctionCreated = junctionProcess.waitForFinished(10000)
        && junctionProcess.exitStatus() == QProcess::NormalExit
        && junctionProcess.exitCode() == 0 && QFileInfo(junction).isJunction();
    test.check(junctionCreated, QStringLiteral("NTFS junction fixture was created"));
    if (junctionCreated) {
        const bool linkedExport = ProjectBundle::exportToFile(
            QDir(temporary.path()).filePath(QStringLiteral("saves/linked.wimforge")),
            {{ProjectBundle::ProjectRepositoryRole,
              linkedRepository,
              QStringLiteral("repos/project")}},
            {},
            &error);
        test.check(!linkedExport
                       && (error.contains(QStringLiteral("reparse"), Qt::CaseInsensitive)
                           || error.contains(QStringLiteral("Symbolic"), Qt::CaseInsensitive)),
                   QStringLiteral("junctions are refused instead of traversed: %1").arg(error));
        test.check(QDir(linkedRepository).rmdir(QStringLiteral("outside-junction")),
                   QStringLiteral("junction fixture is unlinked without touching its target"));
        test.check(QFileInfo::exists(QDir(junctionTarget).filePath(QStringLiteral("outside.txt"))),
                   QStringLiteral("refused junction target remains untouched"));
    }
#endif

    return test.result();
}
