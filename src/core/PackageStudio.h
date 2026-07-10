#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace wimforge {

enum class PackageProvider
{
    Winget,
    Npm,
    Pip,
    DirectSignedInstaller,
    OfflinePayload,
    CustomCommand,
};

enum class PackageArchitecture
{
    Any,
    X64,
    X86,
    Arm64,
};

enum class PackageScope
{
    Either,
    CurrentUser,
    AllUsers,
};

enum class PackageNetworkMode
{
    Online,
    PreferOffline,
    Offline,
};

// Commands deliberately keep the executable and arguments separate.  The
// generated PowerShell never evaluates a command-line string through a shell.
struct PackageCommand
{
    QString executable;
    QStringList arguments;

    [[nodiscard]] bool isEmpty() const { return executable.trimmed().isEmpty(); }
};

struct PackageEntry
{
    // Stable profile-local ID used by dependencies, logs, and resume markers.
    QString id;
    QString displayName;
    QString description;
    PackageProvider provider = PackageProvider::Winget;

    // WinGet ID, npm name, or pip distribution name.  Direct/offline/custom
    // entries may leave this empty.
    QString packageIdentifier;
    QString version = QStringLiteral("latest");
    PackageArchitecture architecture = PackageArchitecture::Any;
    PackageScope scope = PackageScope::Either;

    bool enabled = true;
    bool optional = false;
    bool requiresNetwork = true;
    QStringList dependencies;

    // Vendor-specific unattended switches.  For WinGet they are passed as one
    // --override value; for direct/offline installers they are ordinary args.
    QStringList silentArguments;
    QString expectedSha256;
    QString expectedPublisher;
    QString license;
    QString homepage;

    // A direct installer may be downloaded from downloadUrl.  An offline
    // payload is relative to the generated first-logon script on the ISO.
    QString downloadUrl;
    QString offlinePayload;
    QString vendorReleasePage;

    // An explicit install command overrides the provider default.  The exact
    // token {payload} may be used as the executable or an argument by direct
    // and offline installers.
    PackageCommand installCommand;
    PackageCommand verifyCommand;
    QString notes;
};

struct PackageProfile
{
    QString name;
    QString description;
    PackageNetworkMode networkMode = PackageNetworkMode::Online;
    int retryCount = 3;
    int networkWaitSeconds = 600;
    QList<PackageEntry> packages;
};

struct PackageStudioValidation
{
    QStringList errors;

    [[nodiscard]] bool ok() const { return errors.isEmpty(); }
    [[nodiscard]] QString message() const { return errors.join(QLatin1Char('\n')); }
};

struct PackageStagedFile
{
    QString sourcePath;
    QString relativePath;
    QString sha256;
    QString role;
};

struct PackageStagingResult
{
    QString bundleDirectory;
    QList<PackageStagedFile> files;
};

class PackageStudio
{
public:
    static constexpr int CurrentSchemaVersion = 1;

    [[nodiscard]] static QString providerName(PackageProvider provider);
    [[nodiscard]] static QString architectureName(PackageArchitecture architecture);
    [[nodiscard]] static QString scopeName(PackageScope scope);
    [[nodiscard]] static QString networkModeName(PackageNetworkMode mode);

    [[nodiscard]] static PackageStudioValidation validate(const PackageProfile &profile);
    [[nodiscard]] static bool isCommandSafe(const PackageCommand &command,
                                            QString *reason = nullptr);
    [[nodiscard]] static std::optional<QList<PackageEntry>> dependencyOrder(
        const PackageProfile &profile,
        QString *error = nullptr);

    [[nodiscard]] static PackageCommand effectiveInstallCommand(const PackageEntry &package);
    [[nodiscard]] static QJsonObject toJson(const PackageProfile &profile);
    [[nodiscard]] static std::optional<PackageProfile> fromJson(const QJsonObject &json,
                                                                QString *error = nullptr);
    static bool exportJson(const PackageProfile &profile,
                           const QString &destinationFile,
                           QString *error = nullptr);
    [[nodiscard]] static std::optional<PackageProfile> importJson(const QString &sourceFile,
                                                                  QString *error = nullptr);

    // Returns an empty string/object and sets error when validation fails.
    [[nodiscard]] static QString generateFirstLogonPowerShell(const PackageProfile &profile,
                                                              QString *error = nullptr);
    [[nodiscard]] static QJsonObject generateIsoStagingManifest(const PackageProfile &profile,
                                                                QString *error = nullptr);
    // Materializes the complete, self-verifying installer bundle. Offline
    // payload paths are resolved beneath payloadSourceRoot and copied beneath
    // destinationDirectory without following links or escaping either root.
    [[nodiscard]] static std::optional<PackageStagingResult> materializeFirstLogonBundle(
        const PackageProfile &profile,
        const QString &payloadSourceRoot,
        const QString &destinationDirectory,
        QString *error = nullptr);

    [[nodiscard]] static PackageProfile fullAiDevelopmentTemplate();
    [[nodiscard]] static QList<PackageEntry> builtInCatalog();
};

} // namespace wimforge
