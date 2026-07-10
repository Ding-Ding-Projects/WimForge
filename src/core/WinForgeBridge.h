#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace wimforge {

enum class WinForgeActionKind
{
    Module,
    Page,
    Tweak,
    Command,
    Registry,
    Copy,
};

enum class WinForgeActionPhase
{
    Machine,
    User,
};

// One approved action in a WinForge recipe. The fields are deliberately typed:
// a command is an executable plus an argument array, never a shell command line;
// registry and copy operations never masquerade as commands.
struct WinForgeAction
{
    QString id;
    QString idempotencyKey;
    WinForgeActionKind kind = WinForgeActionKind::Page;
    WinForgeActionPhase phase = WinForgeActionPhase::User;
    bool enabled = true;

    // Module, page, and tweak identifier. Tweak values remain JSON data and
    // are passed only to a runtime contract that explicitly supports them.
    QString target;
    QJsonValue value;

    // Direct process action. Shell/interpreter executables and script files
    // are rejected. Arguments stay as distinct tokens through serialization.
    QString executable;
    QStringList arguments;
    QString workingDirectory;
    QList<int> successExitCodes = {0};

    // Typed registry action.
    QString registryHive;
    QString registryPath;
    QString registryValueName;
    QString registryValueType;
    QJsonValue registryValue;

    // Copy source is relative to the supplied payload directory. A SHA-256 is
    // mandatory so changed or substituted offline files fail closed.
    QString sourceRelative;
    QString destination;
    QString sha256;
    bool overwrite = false;
};

struct WinForgeRecipe
{
    QString id;
    QString name;
    QString description;
    QString createdUtc;
    int requiredContractVersion = 0;
    QString minimumRuntimeVersion;
    QList<WinForgeAction> actions;
};

struct WinForgeRuntimeContract
{
    bool runtimeFound = false;
    bool declaredContract = false;
    int contractVersion = 0;
    QString runtimeVersion;
    QString executableRelativePath;
    QString detectionSource;
    QStringList capabilities;
    // capability -> exact argument tokens. Supported placeholders occupy a
    // complete token and are substituted without evaluation.
    QMap<QString, QStringList> invocations;
};

struct WinForgeBridgeValidation
{
    QStringList errors;
    QStringList warnings;

    [[nodiscard]] bool ok() const { return errors.isEmpty(); }
    [[nodiscard]] QString message() const { return errors.join(QLatin1Char('\n')); }
};

struct WinForgeStageOptions
{
    bool includeRuntime = true;
    bool writeSetupCompleteHook = true;
    bool overwriteExisting = false;
    QString payloadDirectory;
    quint64 maximumFiles = 200000;
    quint64 maximumTotalBytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
};

struct WinForgeStageResult
{
    QString bundleDirectory;
    QString manifestPath;
    QString recipePath;
    QString bootstrapPath;
    QString setupCompletePath;
    QString manifestSha256;
    quint64 fileCount = 0;
    quint64 totalBytes = 0;
    bool setupCompleteNeedsMerge = false;
    WinForgeRuntimeContract runtimeContract;
};

class WinForgeBridge
{
public:
    static constexpr int CurrentRecipeVersion = 1;
    static constexpr int CurrentStageManifestVersion = 1;

    [[nodiscard]] static QString actionKindName(WinForgeActionKind kind);
    [[nodiscard]] static QString actionPhaseName(WinForgeActionPhase phase);

    [[nodiscard]] static WinForgeBridgeValidation validateRecipe(const WinForgeRecipe &recipe);
    [[nodiscard]] static WinForgeBridgeValidation validateAgainstRuntime(
        const WinForgeRecipe &recipe,
        const WinForgeRuntimeContract &contract);

    [[nodiscard]] static QJsonObject toJson(const WinForgeRecipe &recipe);
    [[nodiscard]] static std::optional<WinForgeRecipe> fromJson(const QJsonObject &json,
                                                                QString *error = nullptr);
    static bool exportJson(const WinForgeRecipe &recipe,
                           const QString &destinationFile,
                           QString *error = nullptr);
    [[nodiscard]] static std::optional<WinForgeRecipe> importJson(const QString &sourceFile,
                                                                  QString *error = nullptr);

    // A declared winforge-contract.json is authoritative. For today's adjacent
    // WinForge builds without a contract, detection reports only the observed
    // and source-verified "--page <alias>" capability; no headless tweak or
    // recipe command is invented.
    [[nodiscard]] static WinForgeRuntimeContract detectRuntimeContract(
        const QString &runtimeDirectory,
        QString *error = nullptr);

    [[nodiscard]] static QString generateBootstrapPowerShell(
        const WinForgeRecipe &recipe,
        const WinForgeRuntimeContract &contract,
        QString *error = nullptr);

    // Stages into the conventional Windows media OEM tree. The self-contained
    // bundle lands at C:\ProgramData\WimForge\WinForgeBridge\<recipe-id> after
    // setup. No source link/reparse point is followed.
    [[nodiscard]] static std::optional<WinForgeStageResult> stageForIso(
        const WinForgeRecipe &recipe,
        const QString &runtimeDirectory,
        const QString &isoStagingDirectory,
        const WinForgeStageOptions &options = {},
        QString *error = nullptr);
};

} // namespace wimforge
