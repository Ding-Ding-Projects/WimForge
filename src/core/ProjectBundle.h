#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace wimforge {

// A repository is stored byte-for-byte below archivePath. This deliberately
// includes its hidden .git directory; Git bundles alone would not retain all
// local refs, reflogs, hooks, configuration, and undo commits.
struct ProjectBundleRepository
{
    QString role;
    QString sourceDirectory;
    QString archivePath;
};

struct ProjectBundleFile
{
    QString sourcePath;
    QString archivePath;
};

struct ProjectBundleImportOptions
{
    bool overwriteExisting = false;
    quint64 maximumEntries = 500000;
    quint64 maximumManifestBytes = 64 * 1024 * 1024;
    quint64 maximumFileBytes = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    quint64 maximumTotalBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
};

struct ProjectBundleImportResult
{
    int formatVersion = 0;
    QString destinationDirectory;
    QMap<QString, QString> repositoryPaths;
    QStringList standaloneFiles;
    QString manifestSha256;
    // Normally empty. If replacing an existing destination succeeded but its
    // backup could not be removed, the old data remains safely available here.
    QString retainedBackupPath;
    QJsonObject manifest;
};

class ProjectBundle
{
public:
    static constexpr int CurrentFormatVersion = 1;

    static inline const QString ProjectRepositoryRole = QStringLiteral("project");
    static inline const QString ActionHistoryRepositoryRole = QStringLiteral("action-history");
    static inline const QString NotificationRepositoryRole = QStringLiteral("notifications");

    // Writes one atomic, self-contained .wimforge file. Repository writers
    // should be paused for the duration so the on-disk Git trees form one
    // coherent snapshot. The exporter detects files that change while read.
    static bool exportToFile(const QString &bundleFile,
                             const QList<ProjectBundleRepository> &repositories,
                             const QList<ProjectBundleFile> &standaloneFiles = {},
                             QString *error = nullptr);

    // Always validates into a sibling staging directory before promotion.
    // Existing destinations are untouched unless overwriteExisting is true.
    static std::optional<ProjectBundleImportResult>
    importFromFile(const QString &bundleFile,
                   const QString &destinationDirectory,
                   const ProjectBundleImportOptions &options = {},
                   QString *error = nullptr);
};

} // namespace wimforge
