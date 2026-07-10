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

enum class PayloadScope { Image, Media };

struct StagedPayload
{
    QString sourcePath;
    QString destinationPath;
    PayloadScope scope = PayloadScope::Image;
    QString role = QStringLiteral("payload");
    QString expectedSha256;
};

enum class ScheduledTaskDisposition { Disable, Enable, Remove };

struct ScheduledTaskChange
{
    // A path relative to Windows/System32/Tasks. Absolute paths and parent
    // traversal are rejected before a plan can be built.
    QString taskPath;
    ScheduledTaskDisposition disposition = ScheduledTaskDisposition::Disable;
    bool compatibilityOverride = false;
};

enum class AnswerFileMode { Apply, Place, Remove };

struct AnswerFileAction
{
    AnswerFileMode mode = AnswerFileMode::Apply;
    QString sourcePath;
    // Place/remove destinations are relative to the selected scope. Apply
    // actions ignore this value and pass sourcePath to DISM.
    QString destinationPath = QStringLiteral("Windows/Panther/unattend.xml");
    PayloadScope scope = PayloadScope::Image;
};

struct PostSetupCommand
{
    // Kept as a literal SetupComplete.cmd line. It is never tokenized or
    // reassembled, so quoting authored by the user remains intact.
    QString command;
    QString label;
};

struct CustomizeSettings
{
    bool disableTelemetry = false;
    bool localAccountOobe = false;
    bool showFileExtensions = false;
    bool classicContextMenu = false;
    bool disableConsumerFeatures = false;
    bool enableLongPaths = false;
    bool performanceVisuals = false;
    bool disableRecall = false;

    [[nodiscard]] bool value(const QString &id) const;
    bool setValue(const QString &id, bool enabled);
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
    int splitSizeMb = 3800;
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
    // 0 means unknown. Compatibility-sensitive plan entries retain their
    // minimum-build note when this value is not known.
    int targetBuildNumber = 0;

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
    QList<ScheduledTaskChange> scheduledTaskChanges;
    QString unattendedXmlPath;
    QStringList unattendedFiles;
    QList<AnswerFileAction> answerFileActions;
    QStringList postSetupItems;
    QList<PostSetupCommand> postSetupCommands;
    QList<StagedPayload> stagedPayloads;
    QList<RegistryTweak> registryTweaks;
    CustomizeSettings customize;
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
    [[nodiscard]] bool customizeSettingEnabled(const QString &id) const;
    // Canonical mutation path keeps the typed model and the schema-v1
    // settings object synchronized for older callers.
    bool setCustomizeSetting(const QString &id, bool enabled);

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
