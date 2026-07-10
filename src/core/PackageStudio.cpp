#include "PackageStudio.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <utility>

namespace wimforge {
namespace {

void setError(QString *target, const QString &message)
{
    if (target)
        *target = message;
}

QJsonArray stringArray(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.append(value);
    return result;
}

QStringList readStringArray(const QJsonObject &object,
                            const QString &key,
                            QStringList *errors)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined())
        return {};
    if (!value.isArray()) {
        errors->append(QStringLiteral("'%1' must be an array of strings.").arg(key));
        return {};
    }

    QStringList result;
    const QJsonArray array = value.toArray();
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (!array.at(index).isString()) {
            errors->append(QStringLiteral("'%1[%2]' must be a string.").arg(key).arg(index));
            continue;
        }
        result.append(array.at(index).toString());
    }
    return result;
}

QString readString(const QJsonObject &object,
                   const QString &key,
                   QStringList *errors,
                   bool required = false,
                   const QString &fallback = {})
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        if (required)
            errors->append(QStringLiteral("Missing '%1'.").arg(key));
        return fallback;
    }
    if (!value.isString()) {
        errors->append(QStringLiteral("'%1' must be a string.").arg(key));
        return fallback;
    }
    return value.toString();
}

bool readBool(const QJsonObject &object,
              const QString &key,
              QStringList *errors,
              bool fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined())
        return fallback;
    if (!value.isBool()) {
        errors->append(QStringLiteral("'%1' must be true or false.").arg(key));
        return fallback;
    }
    return value.toBool();
}

int readInt(const QJsonObject &object,
            const QString &key,
            QStringList *errors,
            int fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined())
        return fallback;
    if (!value.isDouble() || value.toDouble() != static_cast<double>(value.toInt())) {
        errors->append(QStringLiteral("'%1' must be an integer.").arg(key));
        return fallback;
    }
    return value.toInt();
}

std::optional<PackageProvider> parseProvider(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("winget"))
        return PackageProvider::Winget;
    if (normalized == QStringLiteral("npm"))
        return PackageProvider::Npm;
    if (normalized == QStringLiteral("pip"))
        return PackageProvider::Pip;
    if (normalized == QStringLiteral("direct-signed-installer"))
        return PackageProvider::DirectSignedInstaller;
    if (normalized == QStringLiteral("offline-payload"))
        return PackageProvider::OfflinePayload;
    if (normalized == QStringLiteral("custom-command"))
        return PackageProvider::CustomCommand;
    return std::nullopt;
}

std::optional<PackageArchitecture> parseArchitecture(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("any"))
        return PackageArchitecture::Any;
    if (normalized == QStringLiteral("x64"))
        return PackageArchitecture::X64;
    if (normalized == QStringLiteral("x86"))
        return PackageArchitecture::X86;
    if (normalized == QStringLiteral("arm64"))
        return PackageArchitecture::Arm64;
    return std::nullopt;
}

std::optional<PackageScope> parseScope(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("either"))
        return PackageScope::Either;
    if (normalized == QStringLiteral("user"))
        return PackageScope::CurrentUser;
    if (normalized == QStringLiteral("machine"))
        return PackageScope::AllUsers;
    return std::nullopt;
}

std::optional<PackageNetworkMode> parseNetworkMode(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("online"))
        return PackageNetworkMode::Online;
    if (normalized == QStringLiteral("prefer-offline"))
        return PackageNetworkMode::PreferOffline;
    if (normalized == QStringLiteral("offline"))
        return PackageNetworkMode::Offline;
    return std::nullopt;
}

QJsonObject commandJson(const PackageCommand &command)
{
    return QJsonObject{
        {QStringLiteral("executable"), command.executable},
        {QStringLiteral("arguments"), stringArray(command.arguments)},
    };
}

PackageCommand readCommand(const QJsonObject &object,
                           const QString &key,
                           QStringList *errors)
{
    PackageCommand command;
    const QJsonValue value = object.value(key);
    if (value.isUndefined())
        return command;
    if (!value.isObject()) {
        errors->append(QStringLiteral("'%1' must be a command object.").arg(key));
        return command;
    }

    const QJsonObject commandObject = value.toObject();
    command.executable = readString(commandObject,
                                    QStringLiteral("executable"),
                                    errors,
                                    true);
    command.arguments = readStringArray(commandObject, QStringLiteral("arguments"), errors);
    return command;
}

QJsonObject packageJson(const PackageEntry &package)
{
    QJsonObject result{
        {QStringLiteral("id"), package.id},
        {QStringLiteral("name"), package.displayName},
        {QStringLiteral("description"), package.description},
        {QStringLiteral("provider"), PackageStudio::providerName(package.provider)},
        {QStringLiteral("packageIdentifier"), package.packageIdentifier},
        {QStringLiteral("version"), package.version},
        {QStringLiteral("architecture"), PackageStudio::architectureName(package.architecture)},
        {QStringLiteral("scope"), PackageStudio::scopeName(package.scope)},
        {QStringLiteral("enabled"), package.enabled},
        {QStringLiteral("optional"), package.optional},
        {QStringLiteral("requiresNetwork"), package.requiresNetwork},
        {QStringLiteral("dependencies"), stringArray(package.dependencies)},
        {QStringLiteral("silentArguments"), stringArray(package.silentArguments)},
        {QStringLiteral("expectedSha256"), package.expectedSha256},
        {QStringLiteral("expectedPublisher"), package.expectedPublisher},
        {QStringLiteral("license"), package.license},
        {QStringLiteral("homepage"), package.homepage},
        {QStringLiteral("downloadUrl"), package.downloadUrl},
        {QStringLiteral("offlinePayload"), package.offlinePayload},
        {QStringLiteral("vendorReleasePage"), package.vendorReleasePage},
        {QStringLiteral("notes"), package.notes},
    };
    if (!package.installCommand.isEmpty())
        result.insert(QStringLiteral("installCommand"), commandJson(package.installCommand));
    if (!package.verifyCommand.isEmpty())
        result.insert(QStringLiteral("verifyCommand"), commandJson(package.verifyCommand));
    return result;
}

bool hasControlCharacter(const QString &value)
{
    return std::any_of(value.cbegin(), value.cend(), [](QChar character) {
        return character.unicode() == 0
            || character == QLatin1Char('\r')
            || character == QLatin1Char('\n')
            || character == QLatin1Char('\t');
    });
}

bool containsSecret(const QString &value)
{
    static const QRegularExpression assignment(
        QStringLiteral("(?i)(?:api[_-]?key|access[_-]?token|auth[_-]?token|password|passwd|client[_-]?secret)\\s*[:=]\\s*[^\\s]+"));
    static const QRegularExpression credential(
        QStringLiteral("(?i)(?:bearer\\s+[a-z0-9._~-]{8,}|sk-[a-z0-9_-]{12,}|ghp_[a-z0-9]{12,}|github_pat_[a-z0-9_]{12,})"));
    return assignment.match(value).hasMatch() || credential.match(value).hasMatch();
}

bool validHttpsUrl(const QString &value)
{
    if (value.trimmed().isEmpty())
        return true;
    const QUrl url(value);
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && !url.host().isEmpty() && url.userInfo().isEmpty();
}

bool validRelativePayload(const QString &value)
{
    if (value.trimmed().isEmpty() || QDir::isAbsolutePath(value))
        return false;
    const QString normalized = QDir::cleanPath(value);
    return normalized != QStringLiteral("..")
        && !normalized.startsWith(QStringLiteral("../"))
        && !normalized.startsWith(QStringLiteral("..\\"))
        && !normalized.contains(QLatin1Char(':'));
}

QString quoteVendorArgument(const QString &argument)
{
    if (!argument.contains(QLatin1Char(' ')) && !argument.contains(QLatin1Char('"')))
        return argument;
    QString escaped = argument;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString joinedVendorArguments(const QStringList &arguments)
{
    QStringList quoted;
    quoted.reserve(arguments.size());
    for (const QString &argument : arguments)
        quoted.append(quoteVendorArgument(argument));
    return quoted.join(QLatin1Char(' '));
}

QString versionedPackageIdentifier(const PackageEntry &package)
{
    const QString version = package.version.trimmed();
    if (version.isEmpty())
        return package.packageIdentifier;
    if (package.provider == PackageProvider::Npm) {
        if (version.compare(QStringLiteral("latest"), Qt::CaseInsensitive) == 0)
            return package.packageIdentifier + QStringLiteral("@latest");
        return package.packageIdentifier + QLatin1Char('@') + version;
    }
    if (package.provider == PackageProvider::Pip
        && version.compare(QStringLiteral("latest"), Qt::CaseInsensitive) != 0) {
        return package.packageIdentifier + QStringLiteral("==") + version;
    }
    return package.packageIdentifier;
}

QString powerShellLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString powerShellArray(const QStringList &values)
{
    QStringList quoted;
    quoted.reserve(values.size());
    for (const QString &value : values)
        quoted.append(powerShellLiteral(value));
    return QStringLiteral("[string[]]@(%1)").arg(quoted.join(QStringLiteral(", ")));
}

QString boolPowerShell(bool value)
{
    return value ? QStringLiteral("$true") : QStringLiteral("$false");
}

QString packageFingerprint(const PackageEntry &package)
{
    const QByteArray canonical = QJsonDocument(packageJson(package)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QString sha256Bytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool writeAtomicFile(const QString &path, const QByteArray &bytes, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("Could not create package staging directory: %1")
                            .arg(QFileInfo(path).absolutePath()));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size()
        || !file.commit()) {
        setError(error, QStringLiteral("Could not atomically write %1: %2")
                            .arg(path, file.errorString()));
        return false;
    }
    return true;
}

bool pathWithin(const QString &root, const QString &candidate)
{
    const QString cleanRoot = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(root).absoluteFilePath()));
    const QString cleanCandidate = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(candidate).absoluteFilePath()));
    if (cleanCandidate.compare(cleanRoot, Qt::CaseInsensitive) == 0)
        return true;
    return cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive);
}

bool linkedPathComponent(const QString &root, const QString &relative)
{
    QString current = QDir::cleanPath(root);
    const QStringList parts = QDir::fromNativeSeparators(relative).split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (info.exists() && info.isSymLink())
            return true;
    }
    return false;
}

bool copyVerifiedPayload(const QString &source,
                         const QString &destination,
                         const QString &expectedSha256,
                         QString *actualSha256,
                         QString *error)
{
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not read offline payload %1: %2")
                            .arg(source, input.errorString()));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        setError(error, QStringLiteral("Could not create payload destination: %1")
                            .arg(QFileInfo(destination).absolutePath()));
        return false;
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not stage offline payload %1: %2")
                            .arg(destination, output.errorString()));
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!input.atEnd()) {
        const QByteArray block = input.read(1024 * 1024);
        if (block.isEmpty() && input.error() != QFileDevice::NoError) {
            output.cancelWriting();
            setError(error, QStringLiteral("Could not finish reading payload %1: %2")
                                .arg(source, input.errorString()));
            return false;
        }
        hash.addData(block);
        if (output.write(block) != block.size()) {
            output.cancelWriting();
            setError(error, QStringLiteral("Could not finish staging payload %1: %2")
                                .arg(destination, output.errorString()));
            return false;
        }
    }
    const QString actual = QString::fromLatin1(hash.result().toHex());
    if (!expectedSha256.trimmed().isEmpty()
        && actual.compare(expectedSha256.trimmed(), Qt::CaseInsensitive) != 0) {
        output.cancelWriting();
        setError(error, QStringLiteral("Offline payload SHA-256 mismatch for %1.").arg(source));
        return false;
    }
    if (!output.commit()) {
        setError(error, QStringLiteral("Could not publish staged payload %1: %2")
                            .arg(destination, output.errorString()));
        return false;
    }
    if (actualSha256)
        *actualSha256 = actual;
    return true;
}

QByteArray firstLogonRegistrationScript()
{
    return QByteArrayLiteral(R"POWERSHELL(#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$taskName = 'WimForge Package Studio'
$scriptPath = Join-Path $env:ProgramData 'WimForge\PackageStudio\first-logon.ps1'
if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf)) {
    throw ('WimForge first-logon installer is missing: {0}' -f $scriptPath)
}
$arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $scriptPath + '"'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arguments
$trigger = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-544' -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Hours 12)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings -Force | Out-Null
)POWERSHELL");
}

PackageCommand wingetVerify(const QString &id)
{
    return PackageCommand{
        QStringLiteral("winget.exe"),
        {QStringLiteral("list"),
         QStringLiteral("--id"), id,
         QStringLiteral("-e"),
         QStringLiteral("--source"), QStringLiteral("winget"),
         QStringLiteral("--accept-source-agreements"),
         QStringLiteral("--disable-interactivity")},
    };
}

PackageEntry wingetPackage(const QString &id,
                           const QString &name,
                           const QString &packageIdentifier,
                           const QString &publisher,
                           const QString &license,
                           const QString &homepage)
{
    PackageEntry package;
    package.id = id;
    package.displayName = name;
    package.provider = PackageProvider::Winget;
    package.packageIdentifier = packageIdentifier;
    package.architecture = PackageArchitecture::X64;
    package.scope = PackageScope::AllUsers;
    package.expectedPublisher = publisher;
    package.license = license;
    package.homepage = homepage;
    package.verifyCommand = wingetVerify(packageIdentifier);
    return package;
}

} // namespace

QString PackageStudio::providerName(PackageProvider provider)
{
    switch (provider) {
    case PackageProvider::Winget: return QStringLiteral("winget");
    case PackageProvider::Npm: return QStringLiteral("npm");
    case PackageProvider::Pip: return QStringLiteral("pip");
    case PackageProvider::DirectSignedInstaller: return QStringLiteral("direct-signed-installer");
    case PackageProvider::OfflinePayload: return QStringLiteral("offline-payload");
    case PackageProvider::CustomCommand: return QStringLiteral("custom-command");
    }
    return {};
}

QString PackageStudio::architectureName(PackageArchitecture architecture)
{
    switch (architecture) {
    case PackageArchitecture::Any: return QStringLiteral("any");
    case PackageArchitecture::X64: return QStringLiteral("x64");
    case PackageArchitecture::X86: return QStringLiteral("x86");
    case PackageArchitecture::Arm64: return QStringLiteral("arm64");
    }
    return {};
}

QString PackageStudio::scopeName(PackageScope scope)
{
    switch (scope) {
    case PackageScope::Either: return QStringLiteral("either");
    case PackageScope::CurrentUser: return QStringLiteral("user");
    case PackageScope::AllUsers: return QStringLiteral("machine");
    }
    return {};
}

QString PackageStudio::networkModeName(PackageNetworkMode mode)
{
    switch (mode) {
    case PackageNetworkMode::Online: return QStringLiteral("online");
    case PackageNetworkMode::PreferOffline: return QStringLiteral("prefer-offline");
    case PackageNetworkMode::Offline: return QStringLiteral("offline");
    }
    return {};
}

bool PackageStudio::isCommandSafe(const PackageCommand &command, QString *reason)
{
    auto fail = [reason](const QString &message) {
        setError(reason, message);
        return false;
    };

    const QString executable = command.executable.trimmed();
    if (executable.isEmpty())
        return fail(QStringLiteral("Executable is empty."));
    if (hasControlCharacter(executable)
        || executable.contains(QRegularExpression(QStringLiteral("[`;&|<>]")))
        || executable.contains(QStringLiteral("$("))) {
        return fail(QStringLiteral("Executable contains shell syntax or control characters."));
    }

    QString executableName = QFileInfo(executable).fileName().toLower();
    if (executableName.endsWith(QStringLiteral(".exe"))
        || executableName.endsWith(QStringLiteral(".com"))
        || executableName.endsWith(QStringLiteral(".bat"))
        || executableName.endsWith(QStringLiteral(".cmd"))) {
        executableName.chop(4);
    }
    static const QSet<QString> shellHosts{
        QStringLiteral("cmd"), QStringLiteral("powershell"), QStringLiteral("pwsh"),
        QStringLiteral("wscript"), QStringLiteral("cscript"), QStringLiteral("mshta"),
        QStringLiteral("rundll32"), QStringLiteral("regsvr32"),
    };
    if (shellHosts.contains(executableName))
        return fail(QStringLiteral("Shell and script-host executables are not accepted; use a structured command."));

    static const QSet<QString> interpreterSwitches{
        QStringLiteral("/c"), QStringLiteral("/k"), QStringLiteral("-c"),
        QStringLiteral("-command"), QStringLiteral("-encodedcommand"),
        QStringLiteral("--eval"), QStringLiteral("-e"),
    };
    for (const QString &argument : command.arguments) {
        if (hasControlCharacter(argument)
            || argument.contains(QRegularExpression(QStringLiteral("[`;&|<>]")))
            || argument.contains(QStringLiteral("$("))) {
            return fail(QStringLiteral("An argument contains shell syntax or control characters."));
        }
        const QString normalizedArgument = argument.trimmed().toLower();
        if (interpreterSwitches.contains(normalizedArgument)
            && !(executableName == QStringLiteral("winget")
                 && normalizedArgument == QStringLiteral("-e")))
            return fail(QStringLiteral("An argument enables inline command interpretation."));
        if (containsSecret(argument))
            return fail(QStringLiteral("A command argument appears to contain an embedded secret."));
    }
    if (containsSecret(executable))
        return fail(QStringLiteral("The executable field appears to contain an embedded secret."));

    setError(reason, {});
    return true;
}

PackageStudioValidation PackageStudio::validate(const PackageProfile &profile)
{
    PackageStudioValidation result;
    if (profile.name.trimmed().isEmpty())
        result.errors.append(QStringLiteral("Profile name is required."));
    if (profile.retryCount < 1 || profile.retryCount > 10)
        result.errors.append(QStringLiteral("Retry count must be between 1 and 10."));
    if (profile.networkWaitSeconds < 15 || profile.networkWaitSeconds > 3600)
        result.errors.append(QStringLiteral("Network wait must be between 15 and 3600 seconds."));
    if (profile.packages.isEmpty())
        result.errors.append(QStringLiteral("At least one package entry is required."));

    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    static const QRegularExpression shaPattern(QStringLiteral("^[A-Fa-f0-9]{64}$"));
    QSet<QString> identifiers;
    QSet<QString> enabledIdentifiers;
    for (qsizetype index = 0; index < profile.packages.size(); ++index) {
        const PackageEntry &package = profile.packages.at(index);
        const QString label = QStringLiteral("Package %1").arg(index);
        const QString normalizedId = package.id.trimmed().toLower();
        if (!idPattern.match(package.id).hasMatch())
            result.errors.append(QStringLiteral("%1 has an invalid stable ID: %2").arg(label, package.id));
        if (identifiers.contains(normalizedId))
            result.errors.append(QStringLiteral("Duplicate package ID: %1").arg(package.id));
        identifiers.insert(normalizedId);
        if (package.enabled)
            enabledIdentifiers.insert(normalizedId);
        if (package.displayName.trimmed().isEmpty())
            result.errors.append(QStringLiteral("%1 needs a display name.").arg(label));

        if ((package.provider == PackageProvider::Winget
             || package.provider == PackageProvider::Npm
             || package.provider == PackageProvider::Pip)
            && package.packageIdentifier.trimmed().isEmpty()) {
            result.errors.append(QStringLiteral("%1 needs a provider package identifier.").arg(label));
        }
        if (profile.networkMode == PackageNetworkMode::Offline
            && package.enabled && package.requiresNetwork) {
            result.errors.append(QStringLiteral("Offline profile enables network-only package '%1'.")
                                     .arg(package.id));
        }
        if (package.provider == PackageProvider::OfflinePayload && package.requiresNetwork)
            result.errors.append(QStringLiteral("Offline payload '%1' cannot require a network.").arg(package.id));

        const bool directOrOffline = package.provider == PackageProvider::DirectSignedInstaller
            || package.provider == PackageProvider::OfflinePayload;
        if (package.enabled && directOrOffline) {
            if (package.offlinePayload.trimmed().isEmpty()
                && (package.provider == PackageProvider::OfflinePayload
                    || package.downloadUrl.trimmed().isEmpty())) {
                result.errors.append(QStringLiteral("Installer '%1' needs an offline payload or HTTPS download URL.")
                                         .arg(package.id));
            }
            if (package.expectedSha256.trimmed().isEmpty())
                result.errors.append(QStringLiteral("Installer '%1' needs an expected SHA256.").arg(package.id));
            if (package.expectedPublisher.trimmed().isEmpty())
                result.errors.append(QStringLiteral("Installer '%1' needs an expected signer/publisher.").arg(package.id));
        }
        if (!package.expectedSha256.trimmed().isEmpty()
            && !shaPattern.match(package.expectedSha256.trimmed()).hasMatch()) {
            result.errors.append(QStringLiteral("Package '%1' has an invalid SHA256.").arg(package.id));
        }
        if (!package.offlinePayload.trimmed().isEmpty()
            && !validRelativePayload(package.offlinePayload)) {
            result.errors.append(QStringLiteral("Package '%1' payload must be a safe relative ISO path.")
                                     .arg(package.id));
        }
        if (!validHttpsUrl(package.homepage))
            result.errors.append(QStringLiteral("Package '%1' homepage must be an HTTPS URL.").arg(package.id));
        if (!validHttpsUrl(package.downloadUrl))
            result.errors.append(QStringLiteral("Package '%1' download URL must be HTTPS without credentials.")
                                     .arg(package.id));
        if (!validHttpsUrl(package.vendorReleasePage))
            result.errors.append(QStringLiteral("Package '%1' vendor release page must be an HTTPS URL.")
                                     .arg(package.id));

        const PackageCommand install = effectiveInstallCommand(package);
        QString commandReason;
        if (package.enabled && install.isEmpty())
            result.errors.append(QStringLiteral("Package '%1' has no install command.").arg(package.id));
        else if (!install.isEmpty() && !isCommandSafe(install, &commandReason))
            result.errors.append(QStringLiteral("Package '%1' install command is unsafe: %2")
                                     .arg(package.id, commandReason));

        if (package.enabled && package.verifyCommand.isEmpty())
            result.errors.append(QStringLiteral("Package '%1' needs a verify command for idempotent resume.")
                                     .arg(package.id));
        else if (!package.verifyCommand.isEmpty()
                 && !isCommandSafe(package.verifyCommand, &commandReason)) {
            result.errors.append(QStringLiteral("Package '%1' verify command is unsafe: %2")
                                     .arg(package.id, commandReason));
        }

        const QStringList fields{
            package.packageIdentifier, package.version, package.expectedSha256,
            package.expectedPublisher, package.homepage, package.downloadUrl,
            package.vendorReleasePage, package.notes,
        };
        for (const QString &field : fields) {
            if (containsSecret(field)) {
                result.errors.append(QStringLiteral("Package '%1' appears to embed a secret in its configuration.")
                                         .arg(package.id));
                break;
            }
        }
    }

    for (const PackageEntry &package : profile.packages) {
        if (!package.enabled)
            continue;
        QSet<QString> seenDependencies;
        for (const QString &dependency : package.dependencies) {
            const QString normalized = dependency.trimmed().toLower();
            if (normalized == package.id.trimmed().toLower())
                result.errors.append(QStringLiteral("Package '%1' cannot depend on itself.").arg(package.id));
            if (!identifiers.contains(normalized))
                result.errors.append(QStringLiteral("Package '%1' has unknown dependency '%2'.")
                                         .arg(package.id, dependency));
            else if (!enabledIdentifiers.contains(normalized))
                result.errors.append(QStringLiteral("Package '%1' depends on disabled package '%2'.")
                                         .arg(package.id, dependency));
            if (seenDependencies.contains(normalized))
                result.errors.append(QStringLiteral("Package '%1' repeats dependency '%2'.")
                                         .arg(package.id, dependency));
            seenDependencies.insert(normalized);
        }
    }

    // Validation must report cycles, not leave them solely to script generation.
    if (result.errors.isEmpty()) {
        QString orderingError;
        if (!dependencyOrder(profile, &orderingError))
            result.errors.append(orderingError);
    }
    result.errors.removeDuplicates();
    return result;
}

std::optional<QList<PackageEntry>> PackageStudio::dependencyOrder(const PackageProfile &profile,
                                                                  QString *error)
{
    QHash<QString, qsizetype> byId;
    for (qsizetype index = 0; index < profile.packages.size(); ++index) {
        if (profile.packages.at(index).enabled)
            byId.insert(profile.packages.at(index).id.trimmed().toLower(), index);
    }

    enum class Mark { Unseen, Visiting, Complete };
    QHash<QString, Mark> marks;
    QList<PackageEntry> ordered;
    QStringList stack;
    QString failure;

    std::function<bool(const QString &)> visit = [&](const QString &normalizedId) {
        const Mark mark = marks.value(normalizedId, Mark::Unseen);
        if (mark == Mark::Complete)
            return true;
        if (mark == Mark::Visiting) {
            const qsizetype cycleStart = stack.indexOf(normalizedId);
            QStringList cycle;
            const qsizetype start = cycleStart >= 0 ? cycleStart : 0;
            for (qsizetype index = start; index < stack.size(); ++index)
                cycle.append(profile.packages.at(byId.value(stack.at(index))).id);
            cycle.append(profile.packages.at(byId.value(normalizedId)).id);
            failure = QStringLiteral("Package dependency cycle: %1").arg(cycle.join(QStringLiteral(" -> ")));
            return false;
        }
        if (!byId.contains(normalizedId)) {
            failure = QStringLiteral("Unknown or disabled package dependency: %1").arg(normalizedId);
            return false;
        }

        marks.insert(normalizedId, Mark::Visiting);
        stack.append(normalizedId);
        const PackageEntry &package = profile.packages.at(byId.value(normalizedId));
        for (const QString &dependency : package.dependencies) {
            if (!visit(dependency.trimmed().toLower()))
                return false;
        }
        stack.removeLast();
        marks.insert(normalizedId, Mark::Complete);
        ordered.append(package);
        return true;
    };

    for (const PackageEntry &package : profile.packages) {
        if (package.enabled && !visit(package.id.trimmed().toLower())) {
            setError(error, failure);
            return std::nullopt;
        }
    }
    setError(error, {});
    return ordered;
}

PackageCommand PackageStudio::effectiveInstallCommand(const PackageEntry &package)
{
    if (!package.installCommand.isEmpty())
        return package.installCommand;

    switch (package.provider) {
    case PackageProvider::Winget: {
        QStringList arguments{
            QStringLiteral("install"),
            QStringLiteral("--id"), package.packageIdentifier,
            QStringLiteral("-e"),
            QStringLiteral("--source"), QStringLiteral("winget"),
            QStringLiteral("--silent"),
            QStringLiteral("--accept-package-agreements"),
            QStringLiteral("--accept-source-agreements"),
            QStringLiteral("--disable-interactivity"),
        };
        if (!package.version.trimmed().isEmpty()
            && package.version.compare(QStringLiteral("latest"), Qt::CaseInsensitive) != 0) {
            arguments.append({QStringLiteral("--version"), package.version});
        }
        if (package.architecture != PackageArchitecture::Any)
            arguments.append({QStringLiteral("--architecture"), architectureName(package.architecture)});
        if (package.scope != PackageScope::Either)
            arguments.append({QStringLiteral("--scope"), scopeName(package.scope)});
        if (!package.silentArguments.isEmpty())
            arguments.append({QStringLiteral("--override"), joinedVendorArguments(package.silentArguments)});
        return PackageCommand{QStringLiteral("winget.exe"), arguments};
    }
    case PackageProvider::Npm:
        return PackageCommand{
            QStringLiteral("npm.cmd"),
            {QStringLiteral("install"), QStringLiteral("-g"), versionedPackageIdentifier(package)},
        };
    case PackageProvider::Pip: {
        QStringList arguments{
            QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
            versionedPackageIdentifier(package),
        };
        if (package.scope == PackageScope::CurrentUser)
            arguments.append(QStringLiteral("--user"));
        arguments.append(package.silentArguments);
        return PackageCommand{QStringLiteral("py.exe"), arguments};
    }
    case PackageProvider::DirectSignedInstaller:
    case PackageProvider::OfflinePayload:
        return PackageCommand{QStringLiteral("{payload}"), package.silentArguments};
    case PackageProvider::CustomCommand:
        return {};
    }
    return {};
}

QJsonObject PackageStudio::toJson(const PackageProfile &profile)
{
    QJsonArray packages;
    for (const PackageEntry &package : profile.packages)
        packages.append(packageJson(package));
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("wimforge.package-studio")},
        {QStringLiteral("version"), CurrentSchemaVersion},
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("description"), profile.description},
        {QStringLiteral("networkMode"), networkModeName(profile.networkMode)},
        {QStringLiteral("retryCount"), profile.retryCount},
        {QStringLiteral("networkWaitSeconds"), profile.networkWaitSeconds},
        {QStringLiteral("packages"), packages},
    };
}

std::optional<PackageProfile> PackageStudio::fromJson(const QJsonObject &json, QString *error)
{
    QStringList errors;
    if (readString(json, QStringLiteral("schema"), &errors, true)
        != QStringLiteral("wimforge.package-studio")) {
        errors.append(QStringLiteral("Unsupported package-studio schema."));
    }
    if (readInt(json, QStringLiteral("version"), &errors, 0) != CurrentSchemaVersion)
        errors.append(QStringLiteral("Unsupported package-studio schema version."));

    PackageProfile profile;
    profile.name = readString(json, QStringLiteral("name"), &errors, true);
    profile.description = readString(json, QStringLiteral("description"), &errors);
    const QString networkMode = readString(json,
                                           QStringLiteral("networkMode"),
                                           &errors,
                                           false,
                                           QStringLiteral("online"));
    const std::optional<PackageNetworkMode> parsedMode = parseNetworkMode(networkMode);
    if (!parsedMode)
        errors.append(QStringLiteral("'networkMode' must be online, prefer-offline, or offline."));
    else
        profile.networkMode = *parsedMode;
    profile.retryCount = readInt(json, QStringLiteral("retryCount"), &errors, 3);
    profile.networkWaitSeconds = readInt(json, QStringLiteral("networkWaitSeconds"), &errors, 600);

    const QJsonValue packagesValue = json.value(QStringLiteral("packages"));
    if (!packagesValue.isArray()) {
        errors.append(QStringLiteral("'packages' must be an array."));
    } else {
        const QJsonArray packages = packagesValue.toArray();
        for (qsizetype index = 0; index < packages.size(); ++index) {
            if (!packages.at(index).isObject()) {
                errors.append(QStringLiteral("'packages[%1]' must be an object.").arg(index));
                continue;
            }
            const QJsonObject object = packages.at(index).toObject();
            PackageEntry package;
            package.id = readString(object, QStringLiteral("id"), &errors, true);
            package.displayName = readString(object, QStringLiteral("name"), &errors, true);
            package.description = readString(object, QStringLiteral("description"), &errors);
            package.packageIdentifier = readString(object, QStringLiteral("packageIdentifier"), &errors);
            package.version = readString(object,
                                         QStringLiteral("version"),
                                         &errors,
                                         false,
                                         QStringLiteral("latest"));
            package.enabled = readBool(object, QStringLiteral("enabled"), &errors, true);
            package.optional = readBool(object, QStringLiteral("optional"), &errors, false);
            package.requiresNetwork = readBool(object, QStringLiteral("requiresNetwork"), &errors, true);
            package.dependencies = readStringArray(object, QStringLiteral("dependencies"), &errors);
            package.silentArguments = readStringArray(object, QStringLiteral("silentArguments"), &errors);
            package.expectedSha256 = readString(object, QStringLiteral("expectedSha256"), &errors);
            package.expectedPublisher = readString(object, QStringLiteral("expectedPublisher"), &errors);
            package.license = readString(object, QStringLiteral("license"), &errors);
            package.homepage = readString(object, QStringLiteral("homepage"), &errors);
            package.downloadUrl = readString(object, QStringLiteral("downloadUrl"), &errors);
            package.offlinePayload = readString(object, QStringLiteral("offlinePayload"), &errors);
            package.vendorReleasePage = readString(object, QStringLiteral("vendorReleasePage"), &errors);
            package.installCommand = readCommand(object, QStringLiteral("installCommand"), &errors);
            package.verifyCommand = readCommand(object, QStringLiteral("verifyCommand"), &errors);
            package.notes = readString(object, QStringLiteral("notes"), &errors);

            const QString provider = readString(object,
                                                QStringLiteral("provider"),
                                                &errors,
                                                true);
            const std::optional<PackageProvider> parsedProvider = parseProvider(provider);
            if (!parsedProvider)
                errors.append(QStringLiteral("Package '%1' has an unknown provider '%2'.")
                                  .arg(package.id, provider));
            else
                package.provider = *parsedProvider;

            const QString architecture = readString(object,
                                                    QStringLiteral("architecture"),
                                                    &errors,
                                                    false,
                                                    QStringLiteral("any"));
            const std::optional<PackageArchitecture> parsedArchitecture = parseArchitecture(architecture);
            if (!parsedArchitecture)
                errors.append(QStringLiteral("Package '%1' has an unknown architecture '%2'.")
                                  .arg(package.id, architecture));
            else
                package.architecture = *parsedArchitecture;

            const QString scope = readString(object,
                                             QStringLiteral("scope"),
                                             &errors,
                                             false,
                                             QStringLiteral("either"));
            const std::optional<PackageScope> parsedScope = parseScope(scope);
            if (!parsedScope)
                errors.append(QStringLiteral("Package '%1' has an unknown scope '%2'.")
                                  .arg(package.id, scope));
            else
                package.scope = *parsedScope;
            profile.packages.append(package);
        }
    }

    if (errors.isEmpty()) {
        const PackageStudioValidation validation = validate(profile);
        errors = validation.errors;
    }
    if (!errors.isEmpty()) {
        errors.removeDuplicates();
        setError(error, errors.join(QLatin1Char('\n')));
        return std::nullopt;
    }
    setError(error, {});
    return profile;
}

bool PackageStudio::exportJson(const PackageProfile &profile,
                               const QString &destinationFile,
                               QString *error)
{
    const PackageStudioValidation validation = validate(profile);
    if (!validation.ok()) {
        setError(error, QStringLiteral("Package profile cannot be exported:\n%1")
                            .arg(validation.message()));
        return false;
    }
    if (destinationFile.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Package profile export destination is empty."));
        return false;
    }
    const QFileInfo destination(destinationFile);
    if (!QDir().mkpath(destination.absolutePath())) {
        setError(error, QStringLiteral("Could not create folder: %1").arg(destination.absolutePath()));
        return false;
    }

    QSaveFile file(destination.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not open %1: %2")
                            .arg(destination.absoluteFilePath(), file.errorString()));
        return false;
    }
    const QByteArray data = QJsonDocument(toJson(profile)).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        setError(error, QStringLiteral("Could not write %1: %2")
                            .arg(destination.absoluteFilePath(), file.errorString()));
        return false;
    }
    setError(error, {});
    return true;
}

std::optional<PackageProfile> PackageStudio::importJson(const QString &sourceFile, QString *error)
{
    QFile file(sourceFile);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open %1: %2").arg(sourceFile, file.errorString()));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, QStringLiteral("Invalid JSON in %1 at offset %2: %3")
                            .arg(sourceFile)
                            .arg(parseError.offset)
                            .arg(parseError.errorString()));
        return std::nullopt;
    }
    if (!document.isObject()) {
        setError(error, QStringLiteral("%1 must contain one JSON object.").arg(sourceFile));
        return std::nullopt;
    }
    return fromJson(document.object(), error);
}

QString PackageStudio::generateFirstLogonPowerShell(const PackageProfile &profile, QString *error)
{
    const PackageStudioValidation validation = validate(profile);
    if (!validation.ok()) {
        setError(error, QStringLiteral("Cannot generate package installer:\n%1")
                            .arg(validation.message()));
        return {};
    }
    QString orderingError;
    const std::optional<QList<PackageEntry>> ordered = dependencyOrder(profile, &orderingError);
    if (!ordered) {
        setError(error, orderingError);
        return {};
    }

    QString script;
    QTextStream output(&script);
    output << "#requires -Version 5.1\n"
              "<# WimForge Package Studio first-logon installer.\n"
              "   Generated commands contain no credentials or API keys.  Applications request\n"
              "   their own authentication after Windows setup.  Safe to run repeatedly. #>\n"
              "param(\n"
              "    [string]$StateRoot = (Join-Path $env:ProgramData 'WimForge\\PackageStudio\\state'),\n"
              "    [string]$LogRoot = (Join-Path $env:ProgramData 'WimForge\\PackageStudio\\logs')\n"
              ")\n\n"
              "Set-StrictMode -Version 2.0\n"
              "$ErrorActionPreference = 'Stop'\n"
           << "$NetworkMode = " << powerShellLiteral(networkModeName(profile.networkMode)) << "\n"
           << "$NetworkWaitSeconds = " << profile.networkWaitSeconds << "\n"
           << "$RetryCount = " << profile.retryCount << "\n"
              "$CacheRoot = Join-Path $env:ProgramData 'WimForge\\PackageStudio\\cache'\n"
              "$TaskName = 'WimForge Package Studio'\n"
              "$null = New-Item -ItemType Directory -Force -Path $StateRoot, $LogRoot, $CacheRoot\n"
              "$RunMutex = [Threading.Mutex]::new($false, 'Global\\WimForge.PackageStudio')\n"
              "if (-not $RunMutex.WaitOne(0)) { $RunMutex.Dispose(); exit 0 }\n"
              "try {\n\n"
              "$Packages = @(\n";

    for (const PackageEntry &package : *ordered) {
        const PackageCommand install = effectiveInstallCommand(package);
        output << "    [pscustomobject]@{\n"
               << "        Id = " << powerShellLiteral(package.id) << "\n"
               << "        Name = " << powerShellLiteral(package.displayName) << "\n"
               << "        Provider = " << powerShellLiteral(providerName(package.provider)) << "\n"
               << "        RequiresNetwork = " << boolPowerShell(package.requiresNetwork) << "\n"
               << "        PayloadPath = " << powerShellLiteral(package.offlinePayload) << "\n"
               << "        DownloadUrl = " << powerShellLiteral(package.downloadUrl) << "\n"
               << "        ExpectedSha256 = " << powerShellLiteral(package.expectedSha256.toLower()) << "\n"
               << "        ExpectedPublisher = " << powerShellLiteral(package.expectedPublisher) << "\n"
               << "        Fingerprint = " << powerShellLiteral(packageFingerprint(package)) << "\n"
               << "        Install = [pscustomobject]@{ Executable = "
               << powerShellLiteral(install.executable)
               << "; Arguments = " << powerShellArray(install.arguments) << " }\n"
               << "        Verify = [pscustomobject]@{ Executable = "
               << powerShellLiteral(package.verifyCommand.executable)
               << "; Arguments = " << powerShellArray(package.verifyCommand.arguments) << " }\n"
                  "    }\n";
    }
    output << ")\n\n";

    output << QStringLiteral(R"POWERSHELL(function Write-WimForgeLog {
    param([string]$Path, [string]$Message)
    $line = ('{0:o} {1}' -f [DateTime]::UtcNow, $Message)
    Add-Content -LiteralPath $Path -Value $line -Encoding UTF8
}

function Read-WimForgeState {
    param([string]$Path, [string]$LogPath)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try {
        return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
    } catch {
        $corrupt = '{0}.corrupt.{1}' -f $Path, [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
        Move-Item -LiteralPath $Path -Destination $corrupt -Force
        Write-WimForgeLog -Path $LogPath -Message ('Archived unreadable resume marker: {0}' -f $corrupt)
        return $null
    }
}

function Write-WimForgeState {
    param(
        [string]$Path,
        [pscustomobject]$Package,
        [string]$Status,
        [int]$Attempt,
        [string]$Message
    )
    $record = [ordered]@{
        schema = 'wimforge.package-state'
        version = 1
        packageId = $Package.Id
        fingerprint = $Package.Fingerprint
        status = $Status
        attempt = $Attempt
        timestampUtc = [DateTime]::UtcNow.ToString('o')
        message = $Message
    }
    $temporary = '{0}.{1}.tmp' -f $Path, $PID
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Update-WimForgeProcessPath {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = @($machinePath, $userPath) -join ';'
}

function Wait-WimForgeNetwork {
    if ($script:WimForgeNetworkReady) { return }
    if ($NetworkMode -eq 'offline') { throw 'Network access is disabled by this profile.' }
    $deadline = [DateTime]::UtcNow.AddSeconds($NetworkWaitSeconds)
    do {
        try {
            if (Test-NetConnection -ComputerName 'cdn.winget.microsoft.com' -Port 443 `
                    -InformationLevel Quiet -WarningAction SilentlyContinue) {
                $script:WimForgeNetworkReady = $true
                return
            }
        } catch { }
        Start-Sleep -Seconds 5
    } while ([DateTime]::UtcNow -lt $deadline)
    throw ('Network did not become available within {0} seconds.' -f $NetworkWaitSeconds)
}

function Resolve-WimForgeApplication {
    param([string]$Executable)
    if ([IO.Path]::IsPathRooted($Executable)) {
        if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
            throw ('Executable does not exist: {0}' -f $Executable)
        }
        return (Resolve-Path -LiteralPath $Executable).Path
    }
    $application = Get-Command -Name $Executable -CommandType Application -ErrorAction Stop |
        Select-Object -First 1
    return $application.Source
}

function Invoke-WimForgeCommand {
    param(
        [pscustomobject]$Command,
        [string]$LogPath,
        [string]$Payload = ''
    )
    $executable = [string]$Command.Executable
    if ($executable -eq '{payload}') { $executable = $Payload }
    $arguments = @($Command.Arguments | ForEach-Object {
        if ([string]$_ -eq '{payload}') { $Payload } else { [string]$_ }
    })
    $resolved = Resolve-WimForgeApplication -Executable $executable
    Write-WimForgeLog -Path $LogPath -Message ('Running {0} with {1} structured argument(s).' -f $resolved, $arguments.Count)
    & $resolved @arguments 2>&1 | ForEach-Object {
        Add-Content -LiteralPath $LogPath -Value ([string]$_) -Encoding UTF8
    }
    $exitCode = $LASTEXITCODE
    if ($null -eq $exitCode) { $exitCode = 0 }
    if ($exitCode -ne 0) { throw ('Process exited with code {0}.' -f $exitCode) }
}

function Test-WimForgeInstalled {
    param([pscustomobject]$Package, [string]$LogPath)
    try {
        Invoke-WimForgeCommand -Command $Package.Verify -LogPath $LogPath
        return $true
    } catch {
        Write-WimForgeLog -Path $LogPath -Message ('Verification reports missing: {0}' -f $_.Exception.Message)
        return $false
    }
}

function Get-WimForgePayload {
    param([pscustomobject]$Package, [string]$LogPath)
    $payload = ''
    if (-not [string]::IsNullOrWhiteSpace([string]$Package.PayloadPath)) {
        $payload = Join-Path $PSScriptRoot ([string]$Package.PayloadPath)
    }
    if ([string]::IsNullOrWhiteSpace($payload) -or -not (Test-Path -LiteralPath $payload -PathType Leaf)) {
        if ([string]::IsNullOrWhiteSpace([string]$Package.DownloadUrl)) {
            throw ('Required offline payload is missing for {0}: {1}' -f $Package.Id, $payload)
        }
        Wait-WimForgeNetwork
        $uri = [Uri]$Package.DownloadUrl
        $extension = [IO.Path]::GetExtension($uri.AbsolutePath)
        if ([string]::IsNullOrWhiteSpace($extension)) { $extension = '.bin' }
        $payload = Join-Path $CacheRoot ($Package.Id + $extension)
        if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
            $temporary = $payload + '.download'
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
            Invoke-WebRequest -Uri $uri -OutFile $temporary -UseBasicParsing
            Move-Item -LiteralPath $temporary -Destination $payload -Force
        }
    }

    $actualHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne ([string]$Package.ExpectedSha256).ToLowerInvariant()) {
        throw ('SHA256 mismatch for {0}; refusing to execute payload.' -f $Package.Id)
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $payload
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw ('Authenticode signature is not valid for {0}: {1}' -f $Package.Id, $signature.Status)
    }
    $subject = [string]$signature.SignerCertificate.Subject
    if ($subject.IndexOf([string]$Package.ExpectedPublisher,
                         [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw ('Publisher mismatch for {0}; refusing to execute payload.' -f $Package.Id)
    }
    Write-WimForgeLog -Path $LogPath -Message ('Verified staged payload SHA256 and publisher: {0}' -f $payload)
    return $payload
}

$script:WimForgeNetworkReady = $false
$runLog = Join-Path $LogRoot ('first-logon-{0}.log' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
Write-WimForgeLog -Path $runLog -Message 'Starting resumable Package Studio run.'

foreach ($package in $Packages) {
    $packageLog = Join-Path $LogRoot ($package.Id + '.log')
    $stateFile = Join-Path $StateRoot ($package.Id + '.json')
    $prior = Read-WimForgeState -Path $stateFile -LogPath $packageLog
    $priorAttempt = 0
    if ($null -ne $prior -and $null -ne $prior.attempt) { $priorAttempt = [int]$prior.attempt }

    # Verification is authoritative.  It makes the script install-if-missing
    # even when a state marker was lost, while stale markers cannot hide an
    # uninstalled application.  This is what auto-installs OpenCode safely.
    Update-WimForgeProcessPath
    if (Test-WimForgeInstalled -Package $package -LogPath $packageLog) {
        $message = if ($null -ne $prior -and $prior.status -eq 'installed' -and
                          $prior.fingerprint -eq $package.Fingerprint) {
            'Resumed: prior installed marker and live verification agree.'
        } else {
            'Detected an existing installation; no install was needed.'
        }
        Write-WimForgeState -Path $stateFile -Package $package -Status 'installed' `
            -Attempt $priorAttempt -Message $message
        Write-WimForgeLog -Path $runLog -Message ('Skipped installed package: {0}' -f $package.Id)
        continue
    }

    $payload = ''
    if ($package.Provider -eq 'direct-signed-installer' -or
        $package.Provider -eq 'offline-payload') {
        $payload = Get-WimForgePayload -Package $package -LogPath $packageLog
    } elseif ($package.RequiresNetwork) {
        Wait-WimForgeNetwork
    }

    $installed = $false
    for ($attempt = 1; $attempt -le $RetryCount; $attempt++) {
        Write-WimForgeState -Path $stateFile -Package $package -Status 'installing' `
            -Attempt $attempt -Message 'Install started; an interrupted run may safely resume.'
        try {
            Invoke-WimForgeCommand -Command $package.Install -LogPath $packageLog -Payload $payload
            Update-WimForgeProcessPath
            if (-not (Test-WimForgeInstalled -Package $package -LogPath $packageLog)) {
                throw 'The install command returned success, but verification failed.'
            }
            Write-WimForgeState -Path $stateFile -Package $package -Status 'installed' `
                -Attempt $attempt -Message 'Install and verification completed.'
            Write-WimForgeLog -Path $runLog -Message ('Installed package: {0}' -f $package.Id)
            $installed = $true
            break
        } catch {
            $failure = $_.Exception.Message
            Write-WimForgeState -Path $stateFile -Package $package -Status 'failed' `
                -Attempt $attempt -Message $failure
            Write-WimForgeLog -Path $packageLog -Message ('Attempt {0} failed: {1}' -f $attempt, $failure)
            if ($attempt -lt $RetryCount) {
                Start-Sleep -Seconds ([Math]::Min(60, [Math]::Pow(2, $attempt) * 5))
            }
        }
    }
    if (-not $installed) {
        throw ('Package {0} failed after {1} attempt(s). Re-run this script to resume.' -f
            $package.Id, $RetryCount)
    }
}

Write-WimForgeLog -Path $runLog -Message 'Package Studio run completed successfully.'
Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
} finally {
    try { $RunMutex.ReleaseMutex() } catch { }
    $RunMutex.Dispose()
}
)POWERSHELL");

    setError(error, {});
    return script;
}

QJsonObject PackageStudio::generateIsoStagingManifest(const PackageProfile &profile, QString *error)
{
    const PackageStudioValidation validation = validate(profile);
    if (!validation.ok()) {
        setError(error, QStringLiteral("Cannot generate ISO staging manifest:\n%1")
                            .arg(validation.message()));
        return {};
    }

    QJsonArray payloads;
    QJsonArray onlinePackages;
    for (const PackageEntry &package : profile.packages) {
        if (!package.offlinePayload.trimmed().isEmpty()) {
            payloads.append(QJsonObject{
                {QStringLiteral("packageId"), package.id},
                {QStringLiteral("selected"), package.enabled},
                {QStringLiteral("required"), package.enabled && !package.optional},
                {QStringLiteral("sourcePath"), package.offlinePayload},
                {QStringLiteral("isoRelativePath"), package.offlinePayload},
                {QStringLiteral("expectedSha256"), package.expectedSha256.toLower()},
                {QStringLiteral("expectedPublisher"), package.expectedPublisher},
            });
        }
        if (package.enabled && package.requiresNetwork)
            onlinePackages.append(package.id);
    }

    setError(error, {});
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("wimforge.package-staging")},
        {QStringLiteral("version"), CurrentSchemaVersion},
        {QStringLiteral("profile"), profile.name},
        {QStringLiteral("networkMode"), networkModeName(profile.networkMode)},
        {QStringLiteral("firstLogonScript"), QStringLiteral("WimForge/PackageStudio/first-logon.ps1")},
        {QStringLiteral("stateRoot"), QStringLiteral("%ProgramData%/WimForge/PackageStudio/state")},
        {QStringLiteral("logRoot"), QStringLiteral("%ProgramData%/WimForge/PackageStudio/logs")},
        {QStringLiteral("generatedFiles"), QJsonArray{
             QStringLiteral("WimForge/PackageStudio/first-logon.ps1"),
             QStringLiteral("WimForge/PackageStudio/register-first-logon.ps1"),
             QStringLiteral("WimForge/PackageStudio/package-profile.json"),
             QStringLiteral("WimForge/PackageStudio/staging-manifest.json"),
         }},
        {QStringLiteral("payloads"), payloads},
        {QStringLiteral("onlinePackages"), onlinePackages},
    };
}

std::optional<PackageStagingResult> PackageStudio::materializeFirstLogonBundle(
    const PackageProfile &profile,
    const QString &payloadSourceRoot,
    const QString &destinationDirectory,
    QString *error)
{
    const PackageStudioValidation validation = validate(profile);
    if (!validation.ok()) {
        setError(error, QStringLiteral("Cannot stage Package Studio bundle:\n%1")
                            .arg(validation.message()));
        return std::nullopt;
    }
    const QString sourceRoot = QDir::cleanPath(QFileInfo(payloadSourceRoot).absoluteFilePath());
    const QString bundleRoot = QDir::cleanPath(QFileInfo(destinationDirectory).absoluteFilePath());
    if (!QFileInfo(sourceRoot).isDir()) {
        setError(error, QStringLiteral("Package payload source root is not a directory: %1")
                            .arg(sourceRoot));
        return std::nullopt;
    }
    if (bundleRoot.isEmpty() || sourceRoot.compare(bundleRoot, Qt::CaseInsensitive) == 0) {
        setError(error, QStringLiteral("Package bundle destination must differ from its payload source root."));
        return std::nullopt;
    }
    if (!QDir().mkpath(bundleRoot)) {
        setError(error, QStringLiteral("Could not create Package Studio bundle: %1").arg(bundleRoot));
        return std::nullopt;
    }

    PackageStagingResult result;
    result.bundleDirectory = bundleRoot;
    auto addGenerated = [&](const QString &relative, const QByteArray &bytes,
                            const QString &role) -> bool {
        const QString destination = QDir(bundleRoot).filePath(relative);
        if (!pathWithin(bundleRoot, destination)
            || !writeAtomicFile(destination, bytes, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("Generated package path escaped the bundle: %1").arg(relative);
            return false;
        }
        result.files.append(PackageStagedFile{
            destination, QDir::fromNativeSeparators(relative), sha256Bytes(bytes), role});
        return true;
    };

    QString generationError;
    const QString firstLogon = generateFirstLogonPowerShell(profile, &generationError);
    const QJsonObject manifest = generateIsoStagingManifest(profile, &generationError);
    if (firstLogon.isEmpty() || manifest.isEmpty()) {
        setError(error, generationError);
        return std::nullopt;
    }
    const QByteArray firstLogonBytes = QString(QChar(0xFEFF)).toUtf8() + firstLogon.toUtf8();
    const QByteArray registrationBytes = QString(QChar(0xFEFF)).toUtf8()
        + firstLogonRegistrationScript();
    const QByteArray profileBytes = QJsonDocument(toJson(profile)).toJson(QJsonDocument::Indented);
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (!addGenerated(QStringLiteral("first-logon.ps1"), firstLogonBytes,
                      QStringLiteral("package-first-logon"))
        || !addGenerated(QStringLiteral("register-first-logon.ps1"), registrationBytes,
                         QStringLiteral("package-registration"))
        || !addGenerated(QStringLiteral("package-profile.json"), profileBytes,
                         QStringLiteral("package-profile"))
        || !addGenerated(QStringLiteral("staging-manifest.json"), manifestBytes,
                         QStringLiteral("package-manifest"))) {
        return std::nullopt;
    }

    for (const PackageEntry &package : profile.packages) {
        if (!package.enabled || package.offlinePayload.trimmed().isEmpty())
            continue;
        const QString relative = QDir::cleanPath(QDir::fromNativeSeparators(
            package.offlinePayload.trimmed()));
        if (!validRelativePayload(relative)
            || linkedPathComponent(sourceRoot, relative)) {
            setError(error, QStringLiteral("Unsafe or linked offline payload path for %1: %2")
                                .arg(package.id, package.offlinePayload));
            return std::nullopt;
        }
        const QString source = QDir(sourceRoot).filePath(relative);
        const QFileInfo sourceInfo(source);
        const bool payloadRequired = package.provider == PackageProvider::OfflinePayload
            || profile.networkMode == PackageNetworkMode::Offline
            || package.downloadUrl.trimmed().isEmpty();
        if (!sourceInfo.exists()) {
            if (payloadRequired) {
                setError(error, QStringLiteral("Enabled package %1 is missing offline payload: %2")
                                    .arg(package.id, source));
                return std::nullopt;
            }
            continue;
        }
        if (!sourceInfo.isFile() || sourceInfo.isSymLink() || !pathWithin(sourceRoot, source)) {
            setError(error, QStringLiteral("Offline payload for %1 is not a safe regular file: %2")
                                .arg(package.id, source));
            return std::nullopt;
        }
        const QString destination = QDir(bundleRoot).filePath(relative);
        if (!pathWithin(bundleRoot, destination)
            || linkedPathComponent(bundleRoot, relative)) {
            setError(error, QStringLiteral("Offline payload destination escaped the bundle: %1")
                                .arg(relative));
            return std::nullopt;
        }
        QString actualHash;
        if (!copyVerifiedPayload(source, destination, package.expectedSha256,
                                 &actualHash, error)) {
            return std::nullopt;
        }
        result.files.append(PackageStagedFile{
            destination, QDir::fromNativeSeparators(relative), actualHash,
            QStringLiteral("package-payload:%1").arg(package.id)});
    }
    setError(error, {});
    return result;
}

PackageProfile PackageStudio::fullAiDevelopmentTemplate()
{
    PackageProfile profile;
    profile.name = QStringLiteral("Full AI Development");
    profile.description = QStringLiteral(
        "Windows AI-development workstation with compilers, runtimes, containers, editors, and AI coding tools.");
    profile.networkMode = PackageNetworkMode::Online;
    profile.retryCount = 4;
    profile.networkWaitSeconds = 900;

    PackageEntry git = wingetPackage(
        QStringLiteral("git"), QStringLiteral("Git for Windows"), QStringLiteral("Git.Git"),
        QStringLiteral("The Git Development Community"), QStringLiteral("GPL-2.0"),
        QStringLiteral("https://gitforwindows.org/"));
    git.description = QStringLiteral("Version control and Git Bash for Windows development tools.");

    PackageEntry node = wingetPackage(
        QStringLiteral("nodejs-lts"), QStringLiteral("Node.js LTS and npm"),
        QStringLiteral("OpenJS.NodeJS.LTS"), QStringLiteral("Node.js Foundation"),
        QStringLiteral("MIT"), QStringLiteral("https://nodejs.org/"));
    node.description = QStringLiteral("Long-term-support Node.js runtime, including npm.");

    PackageEntry python = wingetPackage(
        QStringLiteral("python-3.13"), QStringLiteral("Python 3.13"),
        QStringLiteral("Python.Python.3.13"), QStringLiteral("Python Software Foundation"),
        QStringLiteral("PSF-2.0"), QStringLiteral("https://www.python.org/"));
    python.description = QStringLiteral("Python 3.13 runtime, launcher, and pip.");

    PackageEntry cmake = wingetPackage(
        QStringLiteral("cmake"), QStringLiteral("CMake"), QStringLiteral("Kitware.CMake"),
        QStringLiteral("Kitware"), QStringLiteral("BSD-3-Clause"), QStringLiteral("https://cmake.org/"));
    cmake.description = QStringLiteral("Cross-platform native build generator.");

    PackageEntry java = wingetPackage(
        QStringLiteral("temurin-jdk-21"), QStringLiteral("Eclipse Temurin JDK 21"),
        QStringLiteral("EclipseAdoptium.Temurin.21.JDK"), QStringLiteral("Eclipse Adoptium"),
        QStringLiteral("GPL-2.0 with Classpath Exception"), QStringLiteral("https://adoptium.net/"));
    java.description = QStringLiteral("Long-term-support OpenJDK distribution.");

    PackageEntry buildTools = wingetPackage(
        QStringLiteral("vs-build-tools-2022"), QStringLiteral("Visual Studio 2022 Build Tools"),
        QStringLiteral("Microsoft.VisualStudio.2022.BuildTools"), QStringLiteral("Microsoft Corporation"),
        QStringLiteral("Proprietary"), QStringLiteral("https://visualstudio.microsoft.com/"));
    buildTools.description = QStringLiteral("MSVC, Windows SDK, CMake integration, and recommended C++ build dependencies.");
    buildTools.silentArguments = {
        QStringLiteral("--wait"), QStringLiteral("--passive"), QStringLiteral("--norestart"),
        QStringLiteral("--add"), QStringLiteral("Microsoft.VisualStudio.Workload.VCTools"),
        QStringLiteral("--includeRecommended"),
    };
    buildTools.dependencies = {QStringLiteral("cmake")};

    PackageEntry vscode = wingetPackage(
        QStringLiteral("vscode"), QStringLiteral("Visual Studio Code"),
        QStringLiteral("Microsoft.VisualStudioCode"), QStringLiteral("Microsoft Corporation"),
        QStringLiteral("Microsoft Software License"), QStringLiteral("https://code.visualstudio.com/"));
    vscode.description = QStringLiteral("Desktop code editor for the prepared toolchains.");

    PackageEntry dotnet = wingetPackage(
        QStringLiteral("dotnet-sdk-10"), QStringLiteral(".NET SDK 10"),
        QStringLiteral("Microsoft.DotNet.SDK.10"), QStringLiteral("Microsoft Corporation"),
        QStringLiteral("MIT"), QStringLiteral("https://dotnet.microsoft.com/"));
    dotnet.description = QStringLiteral("Current .NET SDK and command-line build toolchain.");

    PackageEntry go = wingetPackage(
        QStringLiteral("go"), QStringLiteral("Go toolchain"),
        QStringLiteral("GoLang.Go"), QStringLiteral("The Go Authors"),
        QStringLiteral("BSD-3-Clause"), QStringLiteral("https://go.dev/"));
    go.description = QStringLiteral("Go compiler, module tooling, formatter, and standard library.");

    PackageEntry rust = wingetPackage(
        QStringLiteral("rustup"), QStringLiteral("Rustup and Rust toolchain"),
        QStringLiteral("Rustlang.Rustup"), QStringLiteral("The Rust Project Developers"),
        QStringLiteral("Apache-2.0 OR MIT"), QStringLiteral("https://rustup.rs/"));
    rust.description = QStringLiteral("Rust toolchain manager used by native AI and systems projects.");

    PackageEntry llvm = wingetPackage(
        QStringLiteral("llvm"), QStringLiteral("LLVM and Clang"),
        QStringLiteral("LLVM.LLVM"), QStringLiteral("LLVM Project"),
        QStringLiteral("Apache-2.0 with LLVM exceptions"), QStringLiteral("https://llvm.org/"));
    llvm.description = QStringLiteral("Clang/LLVM compiler suite and native developer utilities.");

    PackageEntry ninja = wingetPackage(
        QStringLiteral("ninja"), QStringLiteral("Ninja build"),
        QStringLiteral("Ninja-build.Ninja"), QStringLiteral("Ninja contributors"),
        QStringLiteral("Apache-2.0"), QStringLiteral("https://ninja-build.org/"));
    ninja.description = QStringLiteral("Fast build executor used by CMake and native toolchains.");

    PackageEntry sevenZip = wingetPackage(
        QStringLiteral("7zip"), QStringLiteral("7-Zip"),
        QStringLiteral("7zip.7zip"), QStringLiteral("Igor Pavlov"),
        QStringLiteral("LGPL-2.1-or-later"), QStringLiteral("https://www.7-zip.org/"));
    sevenZip.description = QStringLiteral("Common archive formats and command-line extraction support.");

    PackageEntry powershell = wingetPackage(
        QStringLiteral("powershell"), QStringLiteral("PowerShell 7"),
        QStringLiteral("Microsoft.PowerShell"), QStringLiteral("Microsoft Corporation"),
        QStringLiteral("MIT"), QStringLiteral("https://github.com/PowerShell/PowerShell"));
    powershell.description = QStringLiteral("Modern cross-platform automation shell.");

    PackageEntry gitLfs = wingetPackage(
        QStringLiteral("git-lfs"), QStringLiteral("Git LFS"),
        QStringLiteral("GitHub.GitLFS"), QStringLiteral("GitHub, Inc."),
        QStringLiteral("MIT"), QStringLiteral("https://git-lfs.com/"));
    gitLfs.description = QStringLiteral("Large-file support for model, asset, and dataset repositories.");
    gitLfs.dependencies = {QStringLiteral("git")};

    PackageEntry docker = wingetPackage(
        QStringLiteral("docker-desktop"), QStringLiteral("Docker Desktop"),
        QStringLiteral("Docker.DockerDesktop"), QStringLiteral("Docker Inc."),
        QStringLiteral("Docker Subscription Service Agreement"),
        QStringLiteral("https://www.docker.com/products/docker-desktop/"));
    docker.description = QStringLiteral("Desktop container engine; Windows virtualization requirements still apply.");

    PackageEntry opencode;
    opencode.id = QStringLiteral("opencode-cli");
    opencode.displayName = QStringLiteral("OpenCode CLI");
    opencode.description = QStringLiteral("Open-source AI coding agent for the terminal.");
    opencode.provider = PackageProvider::Npm;
    opencode.packageIdentifier = QStringLiteral("opencode-ai");
    opencode.architecture = PackageArchitecture::Any;
    opencode.scope = PackageScope::AllUsers;
    opencode.dependencies = {QStringLiteral("nodejs-lts")};
    opencode.installCommand = PackageCommand{
        QStringLiteral("npm.cmd"),
        {QStringLiteral("install"), QStringLiteral("-g"), QStringLiteral("opencode-ai@latest")},
    };
    opencode.verifyCommand = PackageCommand{
        QStringLiteral("opencode"), {QStringLiteral("--version")},
    };
    opencode.expectedPublisher = QStringLiteral("Anomaly");
    opencode.license = QStringLiteral("MIT");
    opencode.homepage = QStringLiteral("https://opencode.ai/");
    opencode.notes = QStringLiteral("Install-if-missing verification runs before npm on every resumable pass.");

    PackageEntry opencodeDesktop;
    opencodeDesktop.id = QStringLiteral("opencode-desktop");
    opencodeDesktop.displayName = QStringLiteral("OpenCode Desktop (optional offline payload)");
    opencodeDesktop.description = QStringLiteral("Optional vendor desktop release supplied by the ISO author.");
    opencodeDesktop.provider = PackageProvider::OfflinePayload;
    opencodeDesktop.architecture = PackageArchitecture::X64;
    opencodeDesktop.scope = PackageScope::CurrentUser;
    opencodeDesktop.enabled = false;
    opencodeDesktop.optional = true;
    opencodeDesktop.requiresNetwork = false;
    opencodeDesktop.offlinePayload = QStringLiteral("payloads/opencode-desktop/OpenCode-Desktop.exe");
    opencodeDesktop.license = QStringLiteral("MIT");
    opencodeDesktop.homepage = QStringLiteral("https://opencode.ai/download");
    opencodeDesktop.vendorReleasePage = QStringLiteral("https://github.com/anomalyco/opencode/releases/latest");
    opencodeDesktop.notes = QStringLiteral(
        "Disabled until the user selects a current signed vendor asset and records its SHA256 and signer.");

    PackageEntry codex;
    codex.id = QStringLiteral("codex-cli");
    codex.displayName = QStringLiteral("OpenAI Codex CLI");
    codex.description = QStringLiteral("OpenAI coding agent command-line client.");
    codex.provider = PackageProvider::Npm;
    codex.packageIdentifier = QStringLiteral("@openai/codex");
    codex.scope = PackageScope::AllUsers;
    codex.dependencies = {QStringLiteral("nodejs-lts")};
    codex.installCommand = PackageCommand{
        QStringLiteral("npm.cmd"),
        {QStringLiteral("install"), QStringLiteral("-g"), QStringLiteral("@openai/codex")},
    };
    codex.verifyCommand = PackageCommand{QStringLiteral("codex"), {QStringLiteral("--version")}};
    codex.expectedPublisher = QStringLiteral("OpenAI");
    codex.license = QStringLiteral("Apache-2.0");
    codex.homepage = QStringLiteral("https://github.com/openai/codex");

    PackageEntry codexDesktop;
    codexDesktop.id = QStringLiteral("codex-desktop");
    codexDesktop.displayName = QStringLiteral("OpenAI Codex app (optional official payload)");
    codexDesktop.description = QStringLiteral("Optional official Codex desktop app supplied by the ISO author.");
    codexDesktop.provider = PackageProvider::OfflinePayload;
    codexDesktop.architecture = PackageArchitecture::X64;
    codexDesktop.scope = PackageScope::CurrentUser;
    codexDesktop.enabled = false;
    codexDesktop.optional = true;
    codexDesktop.requiresNetwork = false;
    codexDesktop.offlinePayload = QStringLiteral("payloads/codex-desktop/Codex-Setup.exe");
    codexDesktop.license = QStringLiteral("Proprietary");
    codexDesktop.homepage = QStringLiteral("https://developers.openai.com/codex/app");
    codexDesktop.vendorReleasePage = QStringLiteral("https://github.com/openai/codex/releases/latest");
    codexDesktop.notes = QStringLiteral(
        "Disabled until the user supplies a current official Windows asset, SHA256, signer, and reviewed silent-install switches; no download URL is guessed.");

    PackageEntry claudeCode = wingetPackage(
        QStringLiteral("claude-code"), QStringLiteral("Claude Code"),
        QStringLiteral("Anthropic.ClaudeCode"), QStringLiteral("Anthropic PBC"),
        QStringLiteral("Proprietary"), QStringLiteral("https://www.anthropic.com/claude-code"));
    claudeCode.description = QStringLiteral("Anthropic's native Windows agentic coding CLI.");
    claudeCode.dependencies = {QStringLiteral("git")};
    claudeCode.verifyCommand = PackageCommand{QStringLiteral("claude"), {QStringLiteral("--version")}};

    PackageEntry claudeDesktop = wingetPackage(
        QStringLiteral("claude-desktop"), QStringLiteral("Claude Desktop"),
        QStringLiteral("Anthropic.Claude"), QStringLiteral("Anthropic, PBC"),
        QStringLiteral("Proprietary"), QStringLiteral("https://claude.ai/download"));
    claudeDesktop.description = QStringLiteral("Anthropic's Windows desktop client.");
    claudeDesktop.scope = PackageScope::CurrentUser;

    PackageEntry chatgptDesktop;
    chatgptDesktop.id = QStringLiteral("chatgpt-desktop");
    chatgptDesktop.displayName = QStringLiteral("ChatGPT Desktop (optional Store/vendor payload)");
    chatgptDesktop.description = QStringLiteral("Optional official ChatGPT Windows app; no unverified WinGet ID is used.");
    chatgptDesktop.provider = PackageProvider::OfflinePayload;
    chatgptDesktop.architecture = PackageArchitecture::X64;
    chatgptDesktop.scope = PackageScope::CurrentUser;
    chatgptDesktop.enabled = false;
    chatgptDesktop.optional = true;
    chatgptDesktop.requiresNetwork = false;
    chatgptDesktop.offlinePayload = QStringLiteral("payloads/chatgpt-desktop/ChatGPT.msixbundle");
    chatgptDesktop.license = QStringLiteral("Proprietary");
    chatgptDesktop.homepage = QStringLiteral("https://openai.com/chatgpt/desktop/");
    chatgptDesktop.vendorReleasePage = QStringLiteral("https://apps.microsoft.com/detail/9nt1r1c2hh7j");
    chatgptDesktop.notes = QStringLiteral(
        "Disabled until an official Microsoft Store or OpenAI payload, SHA256, signer, and silent install command are supplied.");

    profile.packages = {
        git, gitLfs, node, python, dotnet, java, go, rust, llvm, cmake, ninja,
        buildTools, vscode, powershell, sevenZip, docker,
        opencode, opencodeDesktop, codex, codexDesktop, claudeCode, claudeDesktop, chatgptDesktop,
    };
    return profile;
}

QList<PackageEntry> PackageStudio::builtInCatalog()
{
    return fullAiDevelopmentTemplate().packages;
}

} // namespace wimforge
