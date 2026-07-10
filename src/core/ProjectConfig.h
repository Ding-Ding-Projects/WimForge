#pragma once

#include "GitHistory.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace wimforge {

struct RegistryTweak
{
    QString hive;
    QString key;
    QString valueName;
    QString type = QStringLiteral("REG_SZ");
    QString value;
    bool deleteValue = false;
    // Delete every value directly under key while retaining the key and all
    // subkeys.  This models an ADMX list with additive="false" without using
    // the much more destructive `reg delete <key> /f` form.
    bool deleteAllValues = false;
    // Stable provenance for stateful producers such as Group Policy Studio.
    // It allows NotConfigured and later edits to remove only values that the
    // same policy previously owned.
    QString ownerId;
};

struct OperationOptions
{
    bool verifyPayloads = true;
    bool mountReadOnly = false;
    bool cleanupComponentStore = true;
    bool resetBase = false;
    bool optimizeImage = true;
    bool rebuildImage = true;
    bool createIso = false;
    bool keepMountOnFailure = false;
    bool dryRun = false;
    QString compression = QStringLiteral("max");
    QString scratchDirectory;
    int maximumParallelOperations = 0; // 0 = choose automatically
    QJsonObject extra;
};

struct ProjectValidation
{
    QStringList errors;

    [[nodiscard]] bool ok() const { return errors.isEmpty(); }
    [[nodiscard]] QString message() const { return errors.join(QLatin1Char('\n')); }
};

class ProjectConfig
{
public:
    static constexpr int CurrentSchemaVersion = 1;
    static constexpr auto FileName = "project.json";

    QString projectDirectory;
    QString projectName;
    QString description;

    QString sourcePath;
    QString imagePath;
    QString mountPath;
    QString outputPath;
    int selectedImageIndex = 1;
    QString outputFormat = QStringLiteral("wim");
    QString isoLabel = QStringLiteral("WIMFORGE");
    bool cloneSource = true;

    QStringList drivers;
    QStringList updates;
    QStringList packages;
    QStringList featuresToEnable;
    QStringList featuresToDisable;
    QStringList capabilitiesToAdd;
    QStringList capabilitiesToRemove;
    QStringList appxPackagesToRemove;
    QStringList appxPackagesToProvision;
    QStringList componentsToRemove;
    QString unattendedXmlPath;
    QStringList unattendedFiles;
    QStringList postSetupItems;
    QList<RegistryTweak> registryTweaks;
    QJsonObject settings;
    OperationOptions options;

    bool autoImport = false;
    bool autoExport = false;
    QString autoExportPath;

    [[nodiscard]] QString projectFilePath() const;
    // Draft validation allows payload paths to be empty or not-yet-created.
    [[nodiscard]] ProjectValidation validate() const;
    // Execution validation requires the source/image and every selected
    // payload to exist, while still allowing mount/output targets to be new.
    [[nodiscard]] ProjectValidation validateForExecution() const;
    [[nodiscard]] QJsonObject toJson() const;

    static std::optional<ProjectConfig> fromJson(const QJsonObject &json,
                                                 const QString &projectDirectory,
                                                 QString *error = nullptr);
    static std::optional<ProjectConfig> load(const QString &projectDirectory,
                                             QString *error = nullptr);

    // save() is the canonical write path: it writes project.json and creates a
    // commit in the project's own repository on every successful call.
    bool save(QString *error = nullptr, const QString &commitMessage = {}) const;
    bool exportJson(const QString &destinationFile, QString *error = nullptr) const;
    static std::optional<ProjectConfig> importJson(const QString &sourceFile,
                                                   const QString &destinationProjectDirectory,
                                                   QString *error = nullptr);

    [[nodiscard]] QList<GitCommit> history(int maximumCount = 100,
                                           QString *error = nullptr) const;
    bool revertLatest(QString *error = nullptr) const;
};

} // namespace wimforge
