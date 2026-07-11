#include "ServicingPlan.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <utility>
#include <vector>

namespace wimforge {
namespace {

struct StagedFile
{
    QString source;
    QString destination;
    QString scope;
    QString role;
    QString expectedSha256;
};

struct VerifiedInput
{
    QString path;
    QString expectedSha256;
    QString role;
};

enum class SourceKind { MediaDirectory, IsoFile, WimFile, EsdFile, SwmFile, Unknown };

QString extraString(const ProjectConfig &project, const QString &key, const QString &fallback = {})
{
    const QJsonValue value = project.options.extra.value(key);
    return value.isString() ? value.toString().trimmed() : fallback;
}

bool extraBool(const ProjectConfig &project, const QString &key, bool fallback = false)
{
    const QJsonValue value = project.options.extra.value(key);
    return value.isBool() ? value.toBool() : fallback;
}

int extraInt(const ProjectConfig &project, const QString &key, int fallback)
{
    const QJsonValue value = project.options.extra.value(key);
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

QString normalized(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    QFileInfo cursor(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    QStringList missingTail;
    while (!cursor.exists() && !cursor.isRoot()) {
        missingTail.prepend(cursor.fileName());
        const QString parent = cursor.absolutePath();
        if (parent == cursor.absoluteFilePath())
            break;
        cursor.setFile(parent);
    }
    QString resolved = cursor.canonicalFilePath();
    if (resolved.isEmpty())
        resolved = cursor.absoluteFilePath();
    for (const QString &segment : std::as_const(missingTail))
        resolved = QDir(resolved).filePath(segment);
    return QDir::fromNativeSeparators(QDir::cleanPath(resolved)).toCaseFolded();
}

bool isAncestorOrSame(const QString &candidate, const QString &child)
{
    const QString left = normalized(candidate);
    const QString right = normalized(child);
    if (left.isEmpty() || right.isEmpty())
        return false;
    return right == left || right.startsWith(left + QLatin1Char('/'));
}

bool pathsOverlap(const QString &left, const QString &right)
{
    return isAncestorOrSame(left, right) || isAncestorOrSame(right, left);
}

bool absoluteNonRoot(const QString &path)
{
    const QFileInfo info(path);
    return info.isAbsolute() && !info.isRoot();
}

QStringList unsafeRelativeReasons(const QString &path)
{
    QStringList reasons;
    const QString portable = QDir::fromNativeSeparators(path.trimmed());
    if (portable.isEmpty()) {
        reasons.append(QStringLiteral("is empty"));
        return reasons;
    }
    if (QDir::isAbsolutePath(portable)
        || portable.startsWith(QLatin1Char('/'))
        || portable.startsWith(QStringLiteral("//"))
        || QRegularExpression(QStringLiteral("^[A-Za-z]:")).match(portable).hasMatch()) {
        reasons.append(QStringLiteral("must be relative"));
    }
    if (portable.contains(QRegularExpression(QStringLiteral("[<>:\"|?*]"))))
        reasons.append(QStringLiteral("contains a Windows-unsafe character"));

    const QRegularExpression deviceName(
        QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QStringList parts = portable.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) {
            reasons.append(QStringLiteral("contains an empty, dot, or parent segment"));
            break;
        }
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) {
            reasons.append(QStringLiteral("contains a segment ending in a dot or space"));
            break;
        }
        if (deviceName.match(part).hasMatch()) {
            reasons.append(QStringLiteral("contains a reserved Windows device name"));
            break;
        }
    }
    return reasons;
}

QString safeRelative(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
}

SourceKind classifySource(const QString &sourcePath)
{
    const QFileInfo info(sourcePath);
    if (info.isDir())
        return SourceKind::MediaDirectory;
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("iso"))
        return SourceKind::IsoFile;
    if (suffix == QStringLiteral("wim"))
        return SourceKind::WimFile;
    if (suffix == QStringLiteral("esd"))
        return SourceKind::EsdFile;
    if (suffix == QStringLiteral("swm"))
        return SourceKind::SwmFile;
    return SourceKind::Unknown;
}

QString dismTarget(const ProjectConfig &project)
{
    return extraBool(project, QStringLiteral("targetOnline"))
        ? QStringLiteral("/Online")
        : QStringLiteral("/Image:%1").arg(project.mountPath);
}

QString nextId(int &sequence, const QString &prefix)
{
    return QStringLiteral("%1-%2").arg(prefix).arg(++sequence, 3, 10, QLatin1Char('0'));
}

ServicingOperation operation(int &sequence,
                             const QString &prefix,
                             OperationKind kind,
                             const QString &titleEn,
                             const QString &titleZh,
                             const QString &descriptionEn,
                             const QString &descriptionZh,
                             const QString &executable,
                             QStringList arguments,
                             bool admin,
                             bool destructive = false)
{
    ServicingOperation result;
    result.id = nextId(sequence, prefix);
    result.kind = kind;
    result.titleEn = titleEn;
    result.titleZh = titleZh;
    result.descriptionEn = descriptionEn;
    result.descriptionZh = descriptionZh;
    result.executable = executable;
    result.arguments = std::move(arguments);
    result.requiresAdministrator = admin;
    result.destructive = destructive;
    result.checkpointBefore = destructive;
    return result;
}

void addDependency(ServicingOperation &value, const QString &dependency)
{
    if (!dependency.isEmpty() && !value.dependsOn.contains(dependency))
        value.dependsOn.append(dependency);
}

void addDependencies(ServicingOperation &value, const QStringList &dependencies)
{
    for (const QString &dependency : dependencies)
        addDependency(value, dependency);
}

void chainImageWrite(QList<ServicingOperation> &operations,
                     ServicingOperation value,
                     QString &lastImageWrite)
{
    value.writesMountedImage = true;
    value.mayRunInParallel = false;
    addDependency(value, lastImageWrite);
    lastImageWrite = value.id;
    operations.append(std::move(value));
}

void chainMediaWrite(QList<ServicingOperation> &operations,
                     ServicingOperation value,
                     QString &lastMediaWrite)
{
    value.writesMediaWorkspace = true;
    value.mayRunInParallel = false;
    addDependency(value, lastMediaWrite);
    lastMediaWrite = value.id;
    operations.append(std::move(value));
}

QStringList powershellArguments(const QString &script)
{
    const QString hardenedPrelude = QStringLiteral(
        "$ErrorActionPreference='Stop'; "
        "$wimforgeSystem32=[Environment]::SystemDirectory; "
        "$wimforgePowerShell=[IO.Path]::Combine($wimforgeSystem32,'WindowsPowerShell','v1.0'); "
        "$env:PATH=$wimforgeSystem32+';'+$wimforgePowerShell; "
        "$env:PSModulePath=[IO.Path]::Combine($wimforgePowerShell,'Modules'); "
        "$wimforgeManagementModule=[IO.Path]::Combine($env:PSModulePath,'Microsoft.PowerShell.Management','Microsoft.PowerShell.Management.psd1'); "
        "$wimforgeUtilityModule=[IO.Path]::Combine($env:PSModulePath,'Microsoft.PowerShell.Utility','Microsoft.PowerShell.Utility.psd1'); "
        "Microsoft.PowerShell.Core\\Import-Module -Name $wimforgeManagementModule -Force -ErrorAction Stop; "
        "Microsoft.PowerShell.Core\\Import-Module -Name $wimforgeUtilityModule -Force -ErrorAction Stop; "
        "$PSModuleAutoLoadingPreference='None'; "
        "$wimforgeRobocopy=[IO.Path]::Combine($wimforgeSystem32,'robocopy.exe'); ");
    return {QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
            QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"), QStringLiteral("-Command"),
            hardenedPrelude + script};
}

QString storageModuleImportScript()
{
    return QStringLiteral(
        "$wimforgeStorageModule=[IO.Path]::Combine($env:PSModulePath,'Storage','Storage.psd1'); "
        "Microsoft.PowerShell.Core\\Import-Module -Name $wimforgeStorageModule -Force -ErrorAction Stop; ");
}

QString swapPartialScript()
{
    return QStringLiteral(
        "if ((-not (Test-Path -LiteralPath $destination)) -and (Test-Path -LiteralPath $backup)) "
        "{ Move-Item -LiteralPath $backup -Destination $destination -Force }; "
        "if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }; "
        "if (Test-Path -LiteralPath $destination) "
        "{ Move-Item -LiteralPath $destination -Destination $backup -Force }; "
        "try { "
        "Move-Item -LiteralPath $partial -Destination $destination -Force; "
        "if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force } "
        "} catch { "
        "if ((-not (Test-Path -LiteralPath $destination)) -and (Test-Path -LiteralPath $backup)) "
        "{ Move-Item -LiteralPath $backup -Destination $destination -Force }; throw "
        "}; ");
}

QString atomicCopyScript(const QString &source,
                         const QString &destination,
                         const QString &containmentRoot = {},
                         const QString &relativeDestination = {})
{
    QString script = QStringLiteral("$ErrorActionPreference='Stop'; $source=%1; $destination=%2; ")
                         .arg(ServicingPlan::quotePowerShellLiteral(source),
                              ServicingPlan::quotePowerShellLiteral(destination));
    if (!containmentRoot.isEmpty() && !relativeDestination.isEmpty()) {
        script += QStringLiteral("$containmentRoot=%1; $relative=%2; ")
                      .arg(ServicingPlan::quotePowerShellLiteral(containmentRoot),
                           ServicingPlan::quotePowerShellLiteral(relativeDestination));
        script += QStringLiteral(
            "$segments=@($relative -split '/'); $cursor=$containmentRoot; "
            "for($index=0; $index -lt ($segments.Count-1); $index++) { "
            "$cursor=Join-Path $cursor $segments[$index]; "
            "if (Test-Path -LiteralPath $cursor) { $existing=Get-Item -LiteralPath $cursor -Force; "
            "if (($existing.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) "
            "{ throw ('Staged destination crosses a reparse point: '+$cursor) } } }; ");
    }
    script += QStringLiteral(
        "if (-not (Test-Path -LiteralPath $source)) { throw ('Missing staged input: ' + $source) }; "
        "$parent=Split-Path -Parent $destination; "
        "New-Item -ItemType Directory -Force -Path $parent | Out-Null; "
        "$partial=$destination+'.wimforge-partial'; $backup=$destination+'.wimforge-backup'; "
        "if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }; "
        "$item=Get-Item -LiteralPath $source -Force; "
        "if ($item.PSIsContainer) { "
        "New-Item -ItemType Directory -Force -Path $partial | Out-Null; "
        "Get-ChildItem -LiteralPath $source -Force | "
        "Copy-Item -Destination $partial -Recurse -Force "
        "} else { Copy-Item -LiteralPath $source -Destination $partial -Force }; ");
    script += swapPartialScript();
    return script;
}

QString atomicDirectoryCloneScript(const QString &source, const QString &destination)
{
    QString script = QStringLiteral("$ErrorActionPreference='Stop'; $source=%1; $destination=%2; ")
                         .arg(ServicingPlan::quotePowerShellLiteral(source),
                              ServicingPlan::quotePowerShellLiteral(destination));
    script += QStringLiteral(
        "if (-not (Test-Path -LiteralPath $source -PathType Container)) "
        "{ throw ('Missing media folder: ' + $source) }; "
        "$parent=Split-Path -Parent $destination; New-Item -ItemType Directory -Force -Path $parent | Out-Null; "
        "$partial=$destination+'.wimforge-partial'; $backup=$destination+'.wimforge-backup'; "
        "if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }; "
        "New-Item -ItemType Directory -Force -Path $partial | Out-Null; "
        "& $wimforgeRobocopy $source $partial /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /XJ /NFL /NDL /NP; "
        "$copyCode=$LASTEXITCODE; if ($copyCode -gt 7) { throw ('robocopy failed: '+$copyCode) }; ");
    script += swapPartialScript();
    return script;
}

QString isoExtractScript(const QString &sourceIso, const QString &destination)
{
    QString script = storageModuleImportScript();
    script += QStringLiteral("$source=%1; $destination=%2; ")
                  .arg(ServicingPlan::quotePowerShellLiteral(sourceIso),
                       ServicingPlan::quotePowerShellLiteral(destination));
    script += QStringLiteral(
        "if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw ('Missing ISO: '+$source) }; "
        "$parent=Split-Path -Parent $destination; New-Item -ItemType Directory -Force -Path $parent | Out-Null; "
        "$partial=$destination+'.wimforge-partial'; $backup=$destination+'.wimforge-backup'; "
        "if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }; "
        "New-Item -ItemType Directory -Force -Path $partial | Out-Null; $mounted=$false; "
        "try { "
        "$disk=Storage\\Mount-DiskImage -ImagePath $source -Access ReadOnly -PassThru -ErrorAction Stop; $mounted=$true; "
        "$volume=$disk | Storage\\Get-Volume -ErrorAction Stop | Where-Object DriveLetter | Select-Object -First 1; "
        "if ($null -eq $volume) { throw 'Mounted ISO has no readable volume' }; "
        "$mediaRoot=($volume.DriveLetter+':\\'); "
        "& $wimforgeRobocopy $mediaRoot $partial /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /XJ /NFL /NDL /NP; "
        "$copyCode=$LASTEXITCODE; if ($copyCode -gt 7) { throw ('robocopy failed: '+$copyCode) } "
        "} finally { if ($mounted) { "
        "Storage\\Dismount-DiskImage -ImagePath $source -ErrorAction Stop | Out-Null; "
        "$detachDeadline=[DateTime]::UtcNow.AddSeconds(10); do { "
        "Microsoft.PowerShell.Utility\\Start-Sleep -Milliseconds 200; "
        "$stillAttached=(Storage\\Get-DiskImage -ImagePath $source -ErrorAction Stop).Attached "
        "} while ($stillAttached -and [DateTime]::UtcNow -lt $detachDeadline); "
        "if ($stillAttached) { throw 'Windows did not confirm that the ISO was detached' } } }; ");
    script += swapPartialScript();
    return script;
}

QString verifyHashScript(const QString &path, const QString &expectedSha256)
{
    QString script = QStringLiteral(
        "$ErrorActionPreference='Stop'; "
        "function Get-WimForgeSha256([string]$filePath) { "
        "$stream=[IO.File]::Open($filePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read); "
        "$algorithm=[Security.Cryptography.SHA256]::Create(); try { "
        "return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','') "
        "} finally { $algorithm.Dispose(); $stream.Dispose() } }; "
        "$path=%1; $expected=%2; ")
                         .arg(ServicingPlan::quotePowerShellLiteral(path),
                              ServicingPlan::quotePowerShellLiteral(expectedSha256.trimmed().toUpper()));
    script += QStringLiteral(
        "if (-not (Test-Path -LiteralPath $path)) { throw ('Missing input: '+$path) }; "
        "$item=Get-Item -LiteralPath $path -Force; "
        "if ($item.PSIsContainer) { "
        "$files=@(Get-ChildItem -LiteralPath $path -File -Recurse -Force | Sort-Object FullName); "
        "$rows=@(); foreach($file in $files) { "
        "$relative=$file.FullName.Substring($item.FullName.Length).TrimStart([char]'\\',[char]'/'); "
        "$hash=Get-WimForgeSha256 $file.FullName; "
        "$rows += ($relative.Replace('\\','/')+' '+$hash) }; "
        "$manifest=[Text.Encoding]::UTF8.GetBytes(($rows -join \"`n\")); "
        "$sha=[Security.Cryptography.SHA256]::Create(); "
        "try { $actual=([BitConverter]::ToString($sha.ComputeHash($manifest))).Replace('-','') } "
        "finally { $sha.Dispose() } "
        "} else { $actual=Get-WimForgeSha256 $path }; "
        "Write-Host ('SHA256 '+$actual+'  '+$path); "
        "if (($expected.Length -gt 0) -and ($actual -ne $expected)) "
        "{ throw ('SHA256 mismatch for '+$path+': expected '+$expected+', got '+$actual) }; ");
    return script;
}

QString prepareOutputScript(const QString &partialPath, bool directory)
{
    QString script = QStringLiteral("$ErrorActionPreference='Stop'; $partial=%1; ")
                         .arg(ServicingPlan::quotePowerShellLiteral(partialPath));
    script += QStringLiteral(
        "$parent=Split-Path -Parent $partial; New-Item -ItemType Directory -Force -Path $parent | Out-Null; "
        "if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }; ");
    if (directory)
        script += QStringLiteral("New-Item -ItemType Directory -Force -Path $partial | Out-Null; ");
    return script;
}

QString atomicMoveScript(const QString &partialPath, const QString &destination)
{
    QString script = QStringLiteral("$ErrorActionPreference='Stop'; $partial=%1; $destination=%2; ")
                         .arg(ServicingPlan::quotePowerShellLiteral(partialPath),
                              ServicingPlan::quotePowerShellLiteral(destination));
    script += QStringLiteral(
        "if (-not (Test-Path -LiteralPath $partial)) { throw ('Missing completed output: '+$partial) }; "
        "$parent=Split-Path -Parent $destination; New-Item -ItemType Directory -Force -Path $parent | Out-Null; "
        "$backup=$destination+'.wimforge-backup'; ");
    script += swapPartialScript();
    return script;
}

QString splitFinalizeScript(const QString &temporaryDirectory, const QString &destinationFirstPart)
{
    const QFileInfo destinationInfo(destinationFirstPart);
    const QString stem = destinationInfo.completeBaseName();
    const QString escapedStem = QRegularExpression::escape(stem);
    QString script = QStringLiteral(
        "$ErrorActionPreference='Stop'; $temporary=%1; $destinationDirectory=%2; "
        "$pattern=%3; $backup=Join-Path $destinationDirectory '.wimforge-swm-backup'; ")
                         .arg(ServicingPlan::quotePowerShellLiteral(temporaryDirectory),
                              ServicingPlan::quotePowerShellLiteral(destinationInfo.absolutePath()),
                              ServicingPlan::quotePowerShellLiteral(
                                  QStringLiteral("^%1[0-9]*\\.swm$").arg(escapedStem)));
    script += QStringLiteral(
        "$parts=@(Get-ChildItem -LiteralPath $temporary -File | Where-Object Name -Match $pattern | Sort-Object Name); "
        "if ($parts.Count -eq 0) { throw 'DISM did not produce any SWM parts' }; "
        "New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null; "
        "if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }; "
        "New-Item -ItemType Directory -Force -Path $backup | Out-Null; "
        "$existing=@(Get-ChildItem -LiteralPath $destinationDirectory -File | Where-Object Name -Match $pattern); "
        "foreach($file in $existing) { Move-Item -LiteralPath $file.FullName -Destination $backup -Force }; "
        "try { foreach($part in $parts) { Move-Item -LiteralPath $part.FullName -Destination $destinationDirectory -Force }; "
        "Remove-Item -LiteralPath $backup -Recurse -Force; Remove-Item -LiteralPath $temporary -Recurse -Force "
        "} catch { "
        "Get-ChildItem -LiteralPath $destinationDirectory -File | Where-Object Name -Match $pattern | "
        "Remove-Item -Force; Get-ChildItem -LiteralPath $backup -File | "
        "Move-Item -Destination $destinationDirectory -Force; throw }; ");
    return script;
}

QString partialOutputPath(const QString &outputPath)
{
    const QFileInfo info(outputPath);
    const QString suffix = info.suffix();
    const QString name = suffix.isEmpty()
        ? QStringLiteral(".%1.wimforge-partial").arg(info.fileName())
        : QStringLiteral(".%1.wimforge-partial.%2").arg(info.completeBaseName(), suffix);
    return QDir(info.absolutePath()).filePath(name);
}

QString expectedHash(const ProjectConfig &project, const QString &path)
{
    const QJsonObject hashes = project.options.extra.value(QStringLiteral("payloadHashes")).toObject();
    const QString direct = hashes.value(path).toString().trimmed();
    if (!direct.isEmpty())
        return direct;
    return hashes.value(QFileInfo(path).fileName()).toString().trimmed();
}

void addVerifiedInput(QList<VerifiedInput> &inputs,
                      const QString &path,
                      const QString &expected,
                      const QString &role,
                      QStringList &errors)
{
    if (path.trimmed().isEmpty())
        return;
    const QString key = normalized(path);
    for (VerifiedInput &input : inputs) {
        if (normalized(input.path) != key)
            continue;
        if (!expected.isEmpty() && !input.expectedSha256.isEmpty()
            && expected.compare(input.expectedSha256, Qt::CaseInsensitive) != 0) {
            errors.append(QStringLiteral("Conflicting SHA-256 values were configured for %1.").arg(path));
        } else if (input.expectedSha256.isEmpty()) {
            input.expectedSha256 = expected;
        }
        return;
    }
    inputs.append(VerifiedInput{path, expected, role});
}

QString registryKeyWithoutHivePrefix(const RegistryTweak &tweak)
{
    QString key = tweak.key.trimmed();
    while (key.startsWith(QLatin1Char('\\')))
        key.remove(0, 1);
    const QString hive = tweak.hive.trimmed().toUpper();
    QStringList prefixes;
    if (hive == QStringLiteral("HKCU") || hive == QStringLiteral("HKEY_CURRENT_USER"))
        prefixes = {QStringLiteral("HKCU"), QStringLiteral("HKEY_CURRENT_USER")};
    else if (hive == QStringLiteral("HKLM") || hive == QStringLiteral("HKEY_LOCAL_MACHINE"))
        prefixes = {QStringLiteral("HKLM"), QStringLiteral("HKEY_LOCAL_MACHINE")};
    for (const QString &prefix : prefixes) {
        if (key.compare(prefix, Qt::CaseInsensitive) == 0)
            return {};
        if (key.startsWith(prefix + QLatin1Char('\\'), Qt::CaseInsensitive))
            return key.mid(prefix.size() + 1);
    }
    return key;
}

QString registryHiveFile(const ProjectConfig &project, const RegistryTweak &tweak)
{
    const QString hive = tweak.hive.trimmed().toUpper();
    const QString config = QDir(project.mountPath).filePath(QStringLiteral("Windows/System32/config"));
    if (hive == QStringLiteral("HKCU") || hive == QStringLiteral("HKEY_CURRENT_USER"))
        return QDir(project.mountPath).filePath(QStringLiteral("Users/Default/NTUSER.DAT"));
    const QString key = registryKeyWithoutHivePrefix(tweak);
    if (key.compare(QStringLiteral("SYSTEM"), Qt::CaseInsensitive) == 0
        || key.startsWith(QStringLiteral("SYSTEM\\"), Qt::CaseInsensitive))
        return QDir(config).filePath(QStringLiteral("SYSTEM"));
    if (key.compare(QStringLiteral("SAM"), Qt::CaseInsensitive) == 0
        || key.startsWith(QStringLiteral("SAM\\"), Qt::CaseInsensitive))
        return QDir(config).filePath(QStringLiteral("SAM"));
    if (key.compare(QStringLiteral("SECURITY"), Qt::CaseInsensitive) == 0
        || key.startsWith(QStringLiteral("SECURITY\\"), Qt::CaseInsensitive))
        return QDir(config).filePath(QStringLiteral("SECURITY"));
    if (key.compare(QStringLiteral("DEFAULT"), Qt::CaseInsensitive) == 0
        || key.startsWith(QStringLiteral("DEFAULT\\"), Qt::CaseInsensitive))
        return QDir(config).filePath(QStringLiteral("DEFAULT"));
    return QDir(config).filePath(QStringLiteral("SOFTWARE"));
}

QString registryRelativeKey(const RegistryTweak &tweak)
{
    QString key = registryKeyWithoutHivePrefix(tweak);
    const QString hive = tweak.hive.trimmed().toUpper();
    // NTUSER.DAT is mounted at the HKCU root, so Software is part of the
    // actual key path and must never be discarded.
    if (hive == QStringLiteral("HKCU") || hive == QStringLiteral("HKEY_CURRENT_USER"))
        return key;

    static const QStringList hiveRoots{
        QStringLiteral("SOFTWARE"), QStringLiteral("SYSTEM"), QStringLiteral("SAM"),
        QStringLiteral("SECURITY"), QStringLiteral("DEFAULT")};
    for (const QString &root : hiveRoots) {
        if (key.compare(root, Qt::CaseInsensitive) == 0)
            return {};
        if (key.startsWith(root + QLatin1Char('\\'), Qt::CaseInsensitive))
            return key.mid(root.size() + 1);
    }
    return key;
}

QString registryCommandValue(const RegistryTweak &tweak)
{
    if (tweak.type.compare(QStringLiteral("REG_MULTI_SZ"), Qt::CaseInsensitive) != 0)
        return tweak.value;
    QString value = tweak.value;
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    value.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return value.split(QLatin1Char('\n'), Qt::SkipEmptyParts).join(QStringLiteral("\\0"));
}

struct SettingDefinition
{
    QString id;
    RegistryTweak tweak;
    int minimumBuild = 0;
    int maximumBuild = 0;
    QString compatibility;
};

SettingDefinition registrySetting(const QString &id,
                                  const QString &hive,
                                  const QString &key,
                                  const QString &name,
                                  const QString &value,
                                  int minimumBuild,
                                  int maximumBuild,
                                  const QString &compatibility)
{
    RegistryTweak tweak;
    tweak.hive = hive;
    tweak.key = key;
    tweak.valueName = name;
    tweak.type = QStringLiteral("REG_DWORD");
    tweak.value = value;
    tweak.ownerId = QStringLiteral("customize:%1").arg(id);
    return SettingDefinition{id, tweak, minimumBuild, maximumBuild, compatibility};
}

QList<SettingDefinition> settingDefinitions()
{
    QList<SettingDefinition> definitions{
        registrySetting(
            QStringLiteral("disableTelemetry"), QStringLiteral("HKLM"),
            QStringLiteral("SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection"),
            QStringLiteral("AllowTelemetry"), QStringLiteral("0"), 10240, 0,
            QStringLiteral("Policy support and the minimum accepted diagnostic level vary by Windows edition.")),
        registrySetting(
            QStringLiteral("localAccountOobe"), QStringLiteral("HKLM"),
            QStringLiteral("SYSTEM\\Setup\\LabConfig"), QStringLiteral("BypassNRO"),
            QStringLiteral("1"), 22000, 0,
            QStringLiteral("OOBE behavior is serviced by Windows and can change between Windows 11 builds.")),
        registrySetting(
            QStringLiteral("showFileExtensions"), QStringLiteral("HKCU"),
            QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"),
            QStringLiteral("HideFileExt"), QStringLiteral("0"), 7600, 0,
            QStringLiteral("Applied to the default-user profile; existing user profiles are not rewritten.")),
        registrySetting(
            QStringLiteral("classicContextMenu"), QStringLiteral("HKCU"),
            QStringLiteral("Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32"),
            QString(), QStringLiteral(""), 22000, 26100,
            QStringLiteral("The legacy context-menu compatibility registration is not guaranteed on newer Windows 11 builds.")),
        registrySetting(
            QStringLiteral("disableConsumerFeatures"), QStringLiteral("HKLM"),
            QStringLiteral("SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent"),
            QStringLiteral("DisableWindowsConsumerFeatures"), QStringLiteral("1"), 10240, 0,
            QStringLiteral("The policy is edition-dependent and may be ignored by editions that do not license it.")),
        registrySetting(
            QStringLiteral("enableLongPaths"), QStringLiteral("HKLM"),
            QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\FileSystem"),
            QStringLiteral("LongPathsEnabled"), QStringLiteral("1"), 14393, 0,
            QStringLiteral("Applications must also declare long-path awareness before this system setting has an effect.")),
        registrySetting(
            QStringLiteral("performanceVisuals"), QStringLiteral("HKCU"),
            QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects"),
            QStringLiteral("VisualFXSetting"), QStringLiteral("2"), 7600, 0,
            QStringLiteral("Applied to the default-user profile; per-user choices can override it later.")),
        registrySetting(
            QStringLiteral("disableRecall"), QStringLiteral("HKLM"),
            QStringLiteral("SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsAI"),
            QStringLiteral("DisableAIDataAnalysis"), QStringLiteral("1"), 26100, 0,
            QStringLiteral("Recall policy is applicable only to Windows builds and hardware that include Recall.")),
    };
    // The classic context-menu registration uses the default REG_SZ value;
    // every other built-in setting above is a DWORD policy/preference.
    definitions[3].tweak.type = QStringLiteral("REG_SZ");
    return definitions;
}

bool settingEnabled(const ProjectConfig &project, const QString &id)
{
    return project.customizeSettingEnabled(id);
}

QString settingCompatibilityNote(const SettingDefinition &definition, int targetBuild)
{
    QString range;
    if (definition.minimumBuild > 0 && definition.maximumBuild > 0) {
        range = QStringLiteral("Designed for Windows builds %1 through %2.")
                    .arg(definition.minimumBuild).arg(definition.maximumBuild);
    } else if (definition.minimumBuild > 0) {
        range = QStringLiteral("Requires Windows build %1 or later.").arg(definition.minimumBuild);
    }
    if (targetBuild == 0)
        return QStringLiteral("%1 Target build is unknown. %2").arg(range, definition.compatibility).trimmed();
    if ((definition.minimumBuild > 0 && targetBuild < definition.minimumBuild)
        || (definition.maximumBuild > 0 && targetBuild > definition.maximumBuild)) {
        return QStringLiteral("Target build %1 is outside the verified range. %2 %3")
            .arg(targetBuild).arg(range, definition.compatibility).trimmed();
    }
    return QStringLiteral("Target build %1 is within the verified range. %2")
        .arg(targetBuild).arg(definition.compatibility).trimmed();
}

QString scheduledTaskScript(const QString &tasksRoot,
                            const QString &relativeTaskPath,
                            ScheduledTaskDisposition disposition)
{
    const QString guard = QStringLiteral(
        "$root=%1; $relative=%2; $path=Join-Path $root $relative; $cursor=$root; "
        "if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) "
        "{ throw ('Scheduled-task root is a reparse point: '+$root) }; "
        "foreach($segment in @($relative -split '/')) { $cursor=Join-Path $cursor $segment; "
        "if (Test-Path -LiteralPath $cursor) { $item=Get-Item -LiteralPath $cursor -Force; "
        "if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) "
        "{ throw ('Scheduled-task path crosses a reparse point: '+$cursor) } } }; ")
        .arg(ServicingPlan::quotePowerShellLiteral(tasksRoot),
             ServicingPlan::quotePowerShellLiteral(relativeTaskPath));
    if (disposition == ScheduledTaskDisposition::Remove) {
        return QStringLiteral(
            "$ErrorActionPreference='Stop'; %1"
            "if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }")
            .arg(guard);
    }
    const QString enabled = disposition == ScheduledTaskDisposition::Enable
        ? QStringLiteral("true") : QStringLiteral("false");
    return QStringLiteral(
        "$ErrorActionPreference='Stop'; %1"
        "if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw ('Scheduled task not found: '+$path) }; "
        "[xml]$xml=Get-Content -LiteralPath $path -Raw; "
        "$ns=New-Object Xml.XmlNamespaceManager($xml.NameTable); "
        "$ns.AddNamespace('t',$xml.DocumentElement.NamespaceURI); "
        "$settings=$xml.SelectSingleNode('/t:Task/t:Settings',$ns); "
        "if ($null -eq $settings) { throw 'Scheduled task has no Settings element' }; "
        "$node=$settings.SelectSingleNode('t:Enabled',$ns); "
        "if ($null -eq $node) { $node=$xml.CreateElement('Enabled',$xml.DocumentElement.NamespaceURI); "
        "$settings.AppendChild($node) | Out-Null }; $node.InnerText='%2'; "
        "$partial=$path+'.wimforge-partial'; $backup=$path+'.wimforge-backup'; "
        "$xml.Save($partial); if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }; "
        "Move-Item -LiteralPath $path -Destination $backup -Force; try { "
        "Move-Item -LiteralPath $partial -Destination $path -Force; Remove-Item -LiteralPath $backup -Force "
        "} catch { if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $path -Force }; throw }")
        .arg(guard, enabled);
}

QString removeStagedPathScript(const QString &root, const QString &relativeDestination)
{
    return QStringLiteral(
        "$ErrorActionPreference='Stop'; $root=%1; $relative=%2; $path=Join-Path $root $relative; "
        "if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) "
        "{ throw ('Answer-file root is a reparse point: '+$root) }; "
        "$cursor=$root; foreach($segment in @($relative -split '/')) { $cursor=Join-Path $cursor $segment; "
        "if (Test-Path -LiteralPath $cursor) { $item=Get-Item -LiteralPath $cursor -Force; "
        "if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) "
        "{ throw ('Answer-file path crosses a reparse point: '+$cursor) } } }; "
        "if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }")
        .arg(ServicingPlan::quotePowerShellLiteral(root),
             ServicingPlan::quotePowerShellLiteral(relativeDestination));
}

QString appendPostSetupScript(const QString &setupDirectory,
                              const QString &command,
                              const QString &commandId)
{
    return QStringLiteral(
        "$ErrorActionPreference='Stop'; $directory=%1; $command=%2; $marker=%3; "
        "New-Item -ItemType Directory -Force -Path $directory | Out-Null; "
        "$path=Join-Path $directory 'SetupComplete.cmd'; "
        "$content=if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllText($path) } else { \"@echo off`r`n\" }; "
        "if ($content.IndexOf($marker,[StringComparison]::Ordinal) -lt 0) { "
        "if (($content.Length -gt 0) -and (-not $content.EndsWith(\"`n\"))) { $content += \"`r`n\" }; "
        "$content += $marker+\"`r`n\"+$command+\"`r`n\"; "
        "$partial=$path+'.wimforge-partial'; $backup=$path+'.wimforge-backup'; "
        "$utf8=New-Object Text.UTF8Encoding($false); [IO.File]::WriteAllText($partial,$content,$utf8); "
        "if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }; "
        "if (Test-Path -LiteralPath $path) { Move-Item -LiteralPath $path -Destination $backup -Force }; "
        "try { Move-Item -LiteralPath $partial -Destination $path -Force; "
        "if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force } "
        "} catch { if ((-not (Test-Path -LiteralPath $path)) -and (Test-Path -LiteralPath $backup)) "
        "{ Move-Item -LiteralPath $backup -Destination $path -Force }; throw } }")
        .arg(ServicingPlan::quotePowerShellLiteral(setupDirectory),
             ServicingPlan::quotePowerShellLiteral(command),
             ServicingPlan::quotePowerShellLiteral(QStringLiteral("REM WimForge:%1").arg(commandId)));
}

void finalizeOperationSemantics(QList<ServicingOperation> &operations)
{
    for (ServicingOperation &item : operations) {
        const QString metadataScope = item.metadata.value(QStringLiteral("writeScope")).toString();
        if (metadataScope == QStringLiteral("host"))
            item.writeScope = OperationWriteScope::Host;
        else if (metadataScope == QStringLiteral("image")
                 && item.kind == OperationKind::PrepareWorkspace)
            item.writeScope = OperationWriteScope::WorkingImage;
        else if (item.writesMountedImage)
            item.writeScope = OperationWriteScope::MountedImage;
        else if (item.writesMediaWorkspace)
            item.writeScope = OperationWriteScope::MediaWorkspace;
        else if (metadataScope == QStringLiteral("output"))
            item.writeScope = OperationWriteScope::Output;
        else if (metadataScope == QStringLiteral("media"))
            item.writeScope = OperationWriteScope::MediaWorkspace;
        else if (metadataScope == QStringLiteral("image"))
            item.writeScope = OperationWriteScope::MountedImage;
        else if (item.kind == OperationKind::PrepareWorkspace)
            item.writeScope = OperationWriteScope::ProjectWorkspace;

        switch (item.kind) {
        case OperationKind::Driver:
        case OperationKind::Update:
        case OperationKind::Package:
        case OperationKind::Feature:
        case OperationKind::Capability:
        case OperationKind::Appx:
        case OperationKind::Component:
        case OperationKind::ScheduledTask:
        case OperationKind::Unattended:
        case OperationKind::PostSetup:
        case OperationKind::Cleanup:
            item.skipConsequence = SkipConsequence::OmitsOptionalChange;
            break;
        case OperationKind::Registry:
            item.skipConsequence = item.id.startsWith(QStringLiteral("regedit"))
                ? SkipConsequence::OmitsOptionalChange : SkipConsequence::BlocksDependents;
            break;
        case OperationKind::Inspect:
        case OperationKind::VerifyPayload:
        case OperationKind::Mount:
        case OperationKind::Validate:
            item.skipConsequence = SkipConsequence::BlocksDependents;
            break;
        case OperationKind::PrepareWorkspace:
        case OperationKind::StageFile:
        case OperationKind::Commit:
        case OperationKind::Export:
        case OperationKind::Split:
        case OperationKind::CreateIso:
        case OperationKind::Recovery:
            item.skipConsequence = SkipConsequence::LeavesIncompleteOutput;
            break;
        }
        if (!item.metadata.contains(QStringLiteral("writeScope"))) {
            item.metadata.insert(QStringLiteral("writeScope"),
                                 ServicingPlan::writeScopeName(item.writeScope));
        }
        item.metadata.insert(QStringLiteral("checkpointRequired"), item.checkpointBefore);
        item.metadata.insert(QStringLiteral("parallelEligible"), item.mayRunInParallel);
        item.metadata.insert(QStringLiteral("skipConsequence"),
                             ServicingPlan::skipConsequenceName(item.skipConsequence));
        item.metadata.insert(QStringLiteral("reversible"), item.reversible);
    }
}

bool stableTopologicalOrder(const QList<ServicingOperation> &operations,
                            QList<int> *orderedIndices,
                            QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QHash<QString, int> indexById;
    for (qsizetype index = 0; index < operations.size(); ++index) {
        const QString &id = operations.at(index).id;
        if (id.trimmed().isEmpty())
            return fail(QStringLiteral("Servicing operation %1 has an empty ID.").arg(index + 1));
        if (indexById.contains(id))
            return fail(QStringLiteral("Servicing operation ID '%1' is duplicated.").arg(id));
        indexById.insert(id, static_cast<int>(index));
    }

    std::vector<int> inDegree(static_cast<std::size_t>(operations.size()), 0);
    std::vector<std::vector<int>> dependents(static_cast<std::size_t>(operations.size()));
    for (qsizetype index = 0; index < operations.size(); ++index) {
        const ServicingOperation &operation = operations.at(index);
        QSet<QString> seenDependencies;
        for (const QString &dependency : operation.dependsOn) {
            if (seenDependencies.contains(dependency))
                continue;
            seenDependencies.insert(dependency);
            const auto found = indexById.constFind(dependency);
            if (found == indexById.cend()) {
                return fail(QStringLiteral("Servicing operation '%1' depends on missing operation '%2'.")
                                .arg(operation.id, dependency));
            }
            ++inDegree.at(static_cast<std::size_t>(index));
            dependents.at(static_cast<std::size_t>(*found)).push_back(static_cast<int>(index));
        }
    }

    QList<int> ready;
    for (qsizetype index = 0; index < operations.size(); ++index)
        if (inDegree.at(static_cast<std::size_t>(index)) == 0)
            ready.append(static_cast<int>(index));

    QList<int> result;
    result.reserve(operations.size());
    while (!ready.isEmpty()) {
        const int index = ready.takeFirst();
        result.append(index);
        for (const int dependent : dependents.at(static_cast<std::size_t>(index))) {
            int &degree = inDegree.at(static_cast<std::size_t>(dependent));
            --degree;
            if (degree != 0)
                continue;
            const auto position = std::lower_bound(ready.begin(), ready.end(), dependent);
            ready.insert(position, dependent);
        }
    }

    if (result.size() != operations.size()) {
        QStringList unresolved;
        for (qsizetype index = 0; index < operations.size(); ++index)
            if (inDegree.at(static_cast<std::size_t>(index)) > 0)
                unresolved.append(operations.at(index).id);
        return fail(QStringLiteral("Servicing operation dependencies contain a cycle involving: %1.")
                        .arg(unresolved.join(QStringLiteral(", "))));
    }

    if (orderedIndices)
        *orderedIndices = std::move(result);
    if (error)
        error->clear();
    return true;
}

QString powerShellCommentText(QString value)
{
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return value;
}

QString boolText(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

} // namespace

QString ServicingOperation::previewCommand() const
{
    QStringList parts{ServicingPlan::quoteWindowsArgument(executable)};
    for (const QString &argument : arguments)
        parts.append(ServicingPlan::quoteWindowsArgument(argument));
    return parts.join(QLatin1Char(' '));
}

QJsonObject ServicingOperation::toJson() const
{
    QJsonArray dependencies;
    for (const QString &dependency : dependsOn)
        dependencies.append(dependency);
    QJsonArray notes;
    for (const QString &note : compatibilityNotes)
        notes.append(note);
    QJsonArray commandArguments;
    for (const QString &argument : arguments)
        commandArguments.append(argument);
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("kind"), ServicingPlan::operationKindName(kind)},
        {QStringLiteral("titleEn"), titleEn},
        {QStringLiteral("titleZh"), titleZh},
        {QStringLiteral("descriptionEn"), descriptionEn},
        {QStringLiteral("descriptionZh"), descriptionZh},
        {QStringLiteral("executable"), executable},
        {QStringLiteral("arguments"), commandArguments},
        {QStringLiteral("workingDirectory"), workingDirectory},
        {QStringLiteral("dependsOn"), dependencies},
        {QStringLiteral("requiresAdministrator"), requiresAdministrator},
        {QStringLiteral("destructive"), destructive},
        {QStringLiteral("rebootRequired"), rebootRequired},
        {QStringLiteral("parallelEligible"), mayRunInParallel},
        {QStringLiteral("checkpointRequired"), checkpointBefore},
        {QStringLiteral("reversible"), reversible},
        {QStringLiteral("writeScope"), ServicingPlan::writeScopeName(writeScope)},
        {QStringLiteral("skipConsequence"), ServicingPlan::skipConsequenceName(skipConsequence)},
        {QStringLiteral("compatibilityNotes"), notes},
        {QStringLiteral("state"), ServicingPlan::operationStateName(state)},
        {QStringLiteral("metadata"), metadata},
    };
}

int ServicingPlanResult::destructiveCount() const
{
    return static_cast<int>(std::count_if(operations.cbegin(), operations.cend(),
                                          [](const ServicingOperation &item) { return item.destructive; }));
}

QString ServicingPlan::quoteWindowsArgument(const QString &argument)
{
    if (argument.isEmpty())
        return QStringLiteral("\"\"");

    const bool needsQuotes = std::any_of(argument.cbegin(), argument.cend(), [](QChar character) {
        return character.isSpace() || character == QLatin1Char('"');
    });
    if (!needsQuotes)
        return argument;

    QString result = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar character : argument) {
        if (character == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (character == QLatin1Char('"')) {
            result += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            result += character;
            backslashes = 0;
            continue;
        }
        result += QString(backslashes, QLatin1Char('\\'));
        backslashes = 0;
        result += character;
    }
    result += QString(backslashes * 2, QLatin1Char('\\'));
    result += QLatin1Char('"');
    return result;
}

QString ServicingPlan::quotePowerShellLiteral(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'%1'").arg(escaped);
}

QString ServicingPlan::operationKindName(OperationKind kind)
{
    switch (kind) {
    case OperationKind::Inspect: return QStringLiteral("inspect");
    case OperationKind::PrepareWorkspace: return QStringLiteral("prepare");
    case OperationKind::VerifyPayload: return QStringLiteral("verify");
    case OperationKind::StageFile: return QStringLiteral("stage-file");
    case OperationKind::Mount: return QStringLiteral("mount");
    case OperationKind::Driver: return QStringLiteral("driver");
    case OperationKind::Update: return QStringLiteral("update");
    case OperationKind::Package: return QStringLiteral("package");
    case OperationKind::Feature: return QStringLiteral("feature");
    case OperationKind::Capability: return QStringLiteral("capability");
    case OperationKind::Appx: return QStringLiteral("appx");
    case OperationKind::Component: return QStringLiteral("component");
    case OperationKind::ScheduledTask: return QStringLiteral("scheduled-task");
    case OperationKind::Registry: return QStringLiteral("registry");
    case OperationKind::Unattended: return QStringLiteral("unattended");
    case OperationKind::PostSetup: return QStringLiteral("post-setup");
    case OperationKind::Cleanup: return QStringLiteral("cleanup");
    case OperationKind::Validate: return QStringLiteral("validate");
    case OperationKind::Commit: return QStringLiteral("commit");
    case OperationKind::Export: return QStringLiteral("export");
    case OperationKind::Split: return QStringLiteral("split");
    case OperationKind::CreateIso: return QStringLiteral("iso");
    case OperationKind::Recovery: return QStringLiteral("recovery");
    }
    return QStringLiteral("unknown");
}

QString ServicingPlan::writeScopeName(OperationWriteScope scope)
{
    switch (scope) {
    case OperationWriteScope::None: return QStringLiteral("none");
    case OperationWriteScope::ProjectWorkspace: return QStringLiteral("project-workspace");
    case OperationWriteScope::WorkingImage: return QStringLiteral("working-image");
    case OperationWriteScope::MountedImage: return QStringLiteral("mounted-image");
    case OperationWriteScope::MediaWorkspace: return QStringLiteral("media-workspace");
    case OperationWriteScope::Output: return QStringLiteral("output");
    case OperationWriteScope::Host: return QStringLiteral("host");
    }
    return QStringLiteral("none");
}

QString ServicingPlan::skipConsequenceName(SkipConsequence consequence)
{
    switch (consequence) {
    case SkipConsequence::None: return QStringLiteral("none");
    case SkipConsequence::OmitsOptionalChange: return QStringLiteral("omits-optional-change");
    case SkipConsequence::BlocksDependents: return QStringLiteral("blocks-dependents");
    case SkipConsequence::LeavesIncompleteOutput: return QStringLiteral("leaves-incomplete-output");
    }
    return QStringLiteral("none");
}

QString ServicingPlan::operationStateName(OperationState state)
{
    switch (state) {
    case OperationState::Queued: return QStringLiteral("queued");
    case OperationState::Running: return QStringLiteral("running");
    case OperationState::Succeeded: return QStringLiteral("done");
    case OperationState::Failed: return QStringLiteral("failed");
    case OperationState::Skipped: return QStringLiteral("skipped");
    case OperationState::Blocked: return QStringLiteral("blocked");
    case OperationState::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("unknown");
}

ServicingPlanResult ServicingPlan::build(const ProjectConfig &project)
{
    ServicingPlanResult result;
    int sequence = 0;
    const bool online = extraBool(project, QStringLiteral("targetOnline"));
    const bool discard = extraBool(project, QStringLiteral("discardChanges"));
    const bool allowInPlace = extraBool(project, QStringLiteral("allowInPlaceSourceModification"));
    const QString format = (project.outputFormat.trimmed().isEmpty()
                                ? QFileInfo(project.outputPath).suffix()
                                : project.outputFormat).trimmed().toLower();
    const QString isoLabel = project.isoLabel.trimmed().isEmpty()
        ? QStringLiteral("WIMFORGE") : project.isoLabel.trimmed();
    const QString workingRoot = extraString(
        project, QStringLiteral("workingRoot"),
        QDir(project.projectDirectory).filePath(QStringLiteral(".wimforge/work")));
    const QString configuredRelative = extraString(
        project, QStringLiteral("imageRelativePath"));
    const QString workspace = extraString(
        project, QStringLiteral("mediaWorkspace"),
        QDir(workingRoot).filePath(QStringLiteral("media")));
    result.mediaWorkspace = workspace;

    if (project.projectDirectory.trimmed().isEmpty()
        || !QFileInfo(project.projectDirectory).isAbsolute()) {
        result.errors.append(QStringLiteral("Project folder must be an absolute path."));
    }
    if (!absoluteNonRoot(workingRoot))
        result.errors.append(QStringLiteral("Working root must be an absolute non-root folder."));
    if (!absoluteNonRoot(workspace))
        result.errors.append(QStringLiteral("Media workspace must be an absolute non-root folder."));
    if (!online && (project.sourcePath.trimmed().isEmpty()
                    || !QFileInfo(project.sourcePath).isAbsolute())) {
        result.errors.append(QStringLiteral("Choose an absolute source ISO, image, or media folder."));
    } else if (!online && !QFileInfo::exists(project.sourcePath)) {
        result.errors.append(QStringLiteral("Source does not exist: %1").arg(project.sourcePath));
    }
    const QFileInfo configuredSource(project.sourcePath);
    const bool sourceMayContainRelativeImage = configuredSource.isDir()
        || configuredSource.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) == 0;
    if (!online && (!sourceMayContainRelativeImage || configuredRelative.isEmpty())
        && (project.imagePath.trimmed().isEmpty()
            || !QFileInfo(project.imagePath).isAbsolute())) {
        result.errors.append(QStringLiteral("Choose an absolute WIM/ESD/SWM image path."));
    }
    if (!online && (project.mountPath.trimmed().isEmpty()
                    || !absoluteNonRoot(project.mountPath))) {
        result.errors.append(QStringLiteral("Mount folder must be an absolute non-root folder."));
    }
    if (!online && project.selectedImageIndex < 1)
        result.errors.append(QStringLiteral("Image index must be at least 1."));
    if (!project.outputPath.trimmed().isEmpty() && !QFileInfo(project.outputPath).isAbsolute())
        result.errors.append(QStringLiteral("Output path must be absolute."));
    if (!online && !project.outputPath.isEmpty()
        && normalized(project.sourcePath) == normalized(project.outputPath))
        result.errors.append(QStringLiteral("Output must not overwrite the source."));
    if (!online && !project.outputPath.isEmpty()
        && normalized(project.imagePath) == normalized(project.outputPath))
        result.errors.append(QStringLiteral("Output must not overwrite the selected source image."));
    if (!online && (pathsOverlap(project.mountPath, project.sourcePath)
                    || pathsOverlap(project.mountPath, project.outputPath)
                    || pathsOverlap(project.mountPath, workspace))) {
        result.errors.append(QStringLiteral("Mount folder must not overlap the source, output, or media workspace."));
    }
    if (!online && pathsOverlap(workspace, project.sourcePath))
        result.errors.append(QStringLiteral("Media workspace must not overlap the immutable source."));
    if (!online && !project.outputPath.isEmpty()
        && isAncestorOrSame(project.sourcePath, project.outputPath))
        result.errors.append(QStringLiteral("Output must not be created inside the source media folder."));
    if (!online && !project.outputPath.isEmpty()
        && isAncestorOrSame(workspace, project.outputPath))
        result.errors.append(QStringLiteral("Output must not be created inside the media workspace."));
    if (isoLabel.size() > 32)
        result.errors.append(QStringLiteral("ISO label cannot exceed 32 characters."));

    const SourceKind sourceKind = online ? SourceKind::Unknown : classifySource(project.sourcePath);
    const bool mediaSource = sourceKind == SourceKind::MediaDirectory
        || sourceKind == SourceKind::IsoFile;
    if (!online && sourceKind == SourceKind::Unknown)
        result.errors.append(QStringLiteral("Source must be an ISO, WIM, ESD, SWM, or installation-media folder."));
    if (!online && !project.cloneSource && !allowInPlace)
        result.errors.append(QStringLiteral(
            "Source cloning is disabled. Set allowInPlaceSourceModification=true explicitly or enable cloning."));
    if (!online && mediaSource && !project.cloneSource)
        result.errors.append(QStringLiteral("In-place servicing of ISO/media sources is not supported; enable source cloning."));
    if (!online && (format == QStringLiteral("iso") || project.options.createIso) && !mediaSource)
        result.errors.append(QStringLiteral("ISO output requires a source ISO or Windows installation-media folder."));
    if (online && (format == QStringLiteral("iso") || project.options.createIso))
        result.errors.append(QStringLiteral("ISO creation requires an offline ISO or media-folder source."));

    QString imageRelativePath;
    if (mediaSource) {
        if (!configuredRelative.isEmpty()) {
            imageRelativePath = safeRelative(configuredRelative);
        } else if (sourceKind == SourceKind::MediaDirectory
                   && isAncestorOrSame(project.sourcePath, project.imagePath)) {
            imageRelativePath = safeRelative(QDir(project.sourcePath).relativeFilePath(project.imagePath));
        } else {
            imageRelativePath = QStringLiteral("sources/%1").arg(QFileInfo(project.imagePath).fileName());
        }
        const QStringList reasons = unsafeRelativeReasons(imageRelativePath);
        if (!reasons.isEmpty()) {
            result.errors.append(QStringLiteral("Unsafe imageRelativePath '%1': %2.")
                                     .arg(imageRelativePath, reasons.join(QStringLiteral(", "))));
        }
    }

    // ISO inspection deliberately persists only the stable media-relative image
    // path.  Derive the effective container type from that path when there is no
    // external image so ESD/SWM media is converted before DISM tries to mount it.
    const QString imageDescriptor = project.imagePath.trimmed().isEmpty()
        ? configuredRelative : project.imagePath;
    const QString imageSuffix = QFileInfo(imageDescriptor).suffix().toLower();
    const bool splitInput = imageSuffix == QStringLiteral("swm")
        || sourceKind == SourceKind::SwmFile;
    const bool esdInput = imageSuffix == QStringLiteral("esd")
        || sourceKind == SourceKind::EsdFile;
    const bool convertedInput = splitInput || esdInput;
    if (!online && convertedInput && !project.cloneSource) {
        result.errors.append(QStringLiteral(
            "ESD and SWM inputs must be cloned and converted to a serviceable WIM."));
    }
    const bool imageIsIncludedInMedia = sourceKind == SourceKind::MediaDirectory
        && isAncestorOrSame(project.sourcePath, project.imagePath);
    const bool imageExistsOutsideMedia = QFileInfo::exists(project.imagePath)
        && !imageIsIncludedInMedia;
    if (!online && mediaSource && !imageIsIncludedInMedia && !imageExistsOutsideMedia
        && configuredRelative.isEmpty()) {
        result.errors.append(QStringLiteral(
            "The image is neither inside the media folder nor an existing external file; set imageRelativePath for an ISO-contained image."));
    }
    if (!online && !mediaSource && !QFileInfo::exists(project.imagePath))
        result.errors.append(QStringLiteral("Image does not exist: %1").arg(project.imagePath));

    if (!online) {
        if (!project.cloneSource && allowInPlace) {
            result.workingImagePath = project.imagePath;
            result.warnings.append(QStringLiteral(
                "Explicit in-place servicing is enabled; the source image can be changed."));
        } else if (convertedInput) {
            result.workingImagePath = extraString(
                project, QStringLiteral("workingImagePath"),
                QDir(workingRoot).filePath(QStringLiteral("images/install-working.wim")));
        } else if (mediaSource) {
            result.workingImagePath = QDir(workspace).filePath(imageRelativePath);
        } else {
            const QFileInfo input(project.imagePath);
            result.workingImagePath = extraString(
                project, QStringLiteral("workingImagePath"),
                QDir(workingRoot).filePath(
                    QStringLiteral("images/%1-working.%2")
                        .arg(input.completeBaseName(), input.suffix().toLower())));
        }
        if (!QFileInfo(result.workingImagePath).isAbsolute())
            result.errors.append(QStringLiteral("Working image path must be absolute."));
        if (project.cloneSource && normalized(result.workingImagePath) == normalized(project.imagePath))
            result.errors.append(QStringLiteral("Working image must be separate from the source image."));
        if (!project.outputPath.isEmpty()
            && normalized(result.workingImagePath) == normalized(project.outputPath))
            result.errors.append(QStringLiteral("Working image must be separate from the final output."));
        if (pathsOverlap(project.mountPath, result.workingImagePath))
            result.errors.append(QStringLiteral("Mount folder must not overlap the working image path."));
        if (project.cloneSource && pathsOverlap(result.workingImagePath, project.sourcePath)
            && !isAncestorOrSame(workspace, result.workingImagePath)) {
            result.errors.append(QStringLiteral("Working image must be outside the immutable source."));
        }
    }

    QList<StagedFile> stagedFiles;
    for (const QString &file : project.unattendedFiles) {
        stagedFiles.append(StagedFile{
            file,
            QStringLiteral("Windows/Setup/Scripts/WimForge/%1").arg(QFileInfo(file).fileName()),
            QStringLiteral("image"), QStringLiteral("unattended-file"), {}});
    }
    for (const StagedPayload &payload : project.stagedPayloads) {
        stagedFiles.append(StagedFile{
            payload.sourcePath, safeRelative(payload.destinationPath),
            payload.scope == PayloadScope::Media ? QStringLiteral("media") : QStringLiteral("image"),
            payload.role.trimmed().isEmpty() ? QStringLiteral("payload") : payload.role.trimmed(),
            payload.expectedSha256.trimmed()});
    }
    for (const AnswerFileAction &answerFile : project.answerFileActions) {
        if (answerFile.mode != AnswerFileMode::Place)
            continue;
        stagedFiles.append(StagedFile{
            answerFile.sourcePath, safeRelative(answerFile.destinationPath),
            answerFile.scope == PayloadScope::Media ? QStringLiteral("media") : QStringLiteral("image"),
            QStringLiteral("answer-file"), {}});
    }
    const QJsonValue stagedValue = project.options.extra.value(QStringLiteral("stagedFiles"));
    if (!stagedValue.isUndefined() && !stagedValue.isArray())
        result.errors.append(QStringLiteral("options.extra.stagedFiles must be an array."));
    const QJsonArray stagedArray = stagedValue.toArray();
    for (qsizetype index = 0; index < stagedArray.size(); ++index) {
        if (!stagedArray.at(index).isObject()) {
            result.errors.append(QStringLiteral("Staged file %1 must be an object.").arg(index + 1));
            continue;
        }
        const QJsonObject object = stagedArray.at(index).toObject();
        StagedFile staged{object.value(QStringLiteral("source")).toString().trimmed(),
                          safeRelative(object.value(QStringLiteral("destination")).toString()),
                          object.value(QStringLiteral("scope")).toString().trimmed().toLower(),
                          object.value(QStringLiteral("role")).toString().trimmed(),
                          object.value(QStringLiteral("sha256")).toString().trimmed()};
        stagedFiles.append(staged);
    }

    QSet<QString> stagedDestinations;
    for (qsizetype index = 0; index < stagedFiles.size(); ++index) {
        const StagedFile &staged = stagedFiles.at(index);
        const QString label = QStringLiteral("Staged payload %1").arg(index + 1);
        if (staged.source.isEmpty() || !QFileInfo(staged.source).isAbsolute())
            result.errors.append(label + QStringLiteral(" source must be absolute."));
        else if (!QFileInfo::exists(staged.source))
            result.errors.append(label + QStringLiteral(" source does not exist: ") + staged.source);
        if (staged.role.isEmpty())
            result.errors.append(label + QStringLiteral(" requires a role."));
        if (staged.scope != QStringLiteral("image") && staged.scope != QStringLiteral("media"))
            result.errors.append(label + QStringLiteral(" scope must be image or media."));
        const QStringList reasons = unsafeRelativeReasons(staged.destination);
        if (!reasons.isEmpty()) {
            result.errors.append(QStringLiteral("%1 destination '%2' is unsafe: %3.")
                                     .arg(label, staged.destination, reasons.join(QStringLiteral(", "))));
        }
        if (staged.scope == QStringLiteral("image") && online)
            result.errors.append(label + QStringLiteral(" cannot target image scope during online servicing."));
        const QString destinationKey = staged.scope + QLatin1Char(':')
            + safeRelative(staged.destination).toCaseFolded();
        if (stagedDestinations.contains(destinationKey)) {
            result.errors.append(QStringLiteral("Multiple staged payloads target %1:%2.")
                                     .arg(staged.scope, staged.destination));
        }
        stagedDestinations.insert(destinationKey);
    }
    for (qsizetype index = 0; index < project.answerFileActions.size(); ++index) {
        const AnswerFileAction &answerFile = project.answerFileActions.at(index);
        if (answerFile.mode == AnswerFileMode::Apply)
            continue;
        const QStringList reasons = unsafeRelativeReasons(answerFile.destinationPath);
        if (!reasons.isEmpty()) {
            result.errors.append(QStringLiteral("Answer file %1 destination '%2' is unsafe: %3.")
                                     .arg(index + 1).arg(answerFile.destinationPath,
                                         reasons.join(QStringLiteral(", "))));
        }
        if (online && answerFile.scope == PayloadScope::Image)
            result.errors.append(QStringLiteral("Answer file %1 cannot target image scope during online servicing.")
                                     .arg(index + 1));
    }
    for (qsizetype index = 0; index < project.scheduledTaskChanges.size(); ++index) {
        const ScheduledTaskChange &change = project.scheduledTaskChanges.at(index);
        const QStringList reasons = unsafeRelativeReasons(change.taskPath);
        if (!reasons.isEmpty()) {
            result.errors.append(QStringLiteral("Scheduled task %1 path '%2' is unsafe: %3.")
                                     .arg(index + 1).arg(project.scheduledTaskChanges.at(index).taskPath,
                                         reasons.join(QStringLiteral(", "))));
        }
        if (online)
            result.errors.append(QStringLiteral("Scheduled task changes require an offline image."));
        if (change.disposition == ScheduledTaskDisposition::Remove
            && !change.compatibilityOverride) {
            result.errors.append(QStringLiteral(
                "Scheduled task %1 removal requires an explicit compatibility override.").arg(index + 1));
        }
    }

    QStringList selectedPayloads = project.updates + project.packages + project.drivers
        + project.appxPackagesToProvision;
    if (!project.unattendedXmlPath.trimmed().isEmpty())
        selectedPayloads.append(project.unattendedXmlPath);
    selectedPayloads.append(project.unattendedFiles);
    for (const AnswerFileAction &answerFile : project.answerFileActions) {
        if (answerFile.mode == AnswerFileMode::Apply)
            selectedPayloads.append(answerFile.sourcePath);
    }
    for (const QString &payload : selectedPayloads) {
        if (!QFileInfo(payload).isAbsolute())
            result.errors.append(QStringLiteral("Payload path must be absolute: %1").arg(payload));
        else if (!QFileInfo::exists(payload))
            result.errors.append(QStringLiteral("Payload does not exist: %1").arg(payload));
    }

    if (!result.errors.isEmpty())
        return result;

    QList<VerifiedInput> verifiedInputs;
    if (!online)
        addVerifiedInput(verifiedInputs, project.sourcePath, expectedHash(project, project.sourcePath),
                         QStringLiteral("source"), result.errors);
    if (!online && (!imageIsIncludedInMedia || sourceKind == SourceKind::IsoFile)
        && QFileInfo::exists(project.imagePath)) {
        addVerifiedInput(verifiedInputs, project.imagePath, expectedHash(project, project.imagePath),
                         QStringLiteral("image"), result.errors);
    }
    for (const QString &payload : selectedPayloads)
        addVerifiedInput(verifiedInputs, payload, expectedHash(project, payload),
                         QStringLiteral("payload"), result.errors);
    for (const StagedFile &staged : std::as_const(stagedFiles)) {
        addVerifiedInput(verifiedInputs, staged.source,
                         staged.expectedSha256.isEmpty() ? expectedHash(project, staged.source)
                                                         : staged.expectedSha256,
                         staged.role, result.errors);
    }
    if (!result.errors.isEmpty())
        return result;

    QStringList verificationIds;
    if (project.options.verifyPayloads) {
        for (const VerifiedInput &input : std::as_const(verifiedInputs)) {
            ServicingOperation verify = operation(
                sequence, QStringLiteral("hash"), OperationKind::VerifyPayload,
                QStringLiteral("Verify %1: %2").arg(input.role, QFileInfo(input.path).fileName()),
                QStringLiteral("驗證%1：%2").arg(input.role, QFileInfo(input.path).fileName()),
                QStringLiteral("Compute a deterministic SHA-256 before any working-image or media write is allowed."),
                QStringLiteral("所有工作映像或媒體寫入之前，先計算確定性 SHA-256。"),
                QStringLiteral("powershell.exe"), powershellArguments(
                    verifyHashScript(input.path, input.expectedSha256)), false);
            verify.mayRunInParallel = true;
            verify.metadata = QJsonObject{{QStringLiteral("verifiedPath"), input.path},
                                          {QStringLiteral("expectedSha256"), input.expectedSha256},
                                          {QStringLiteral("role"), input.role}};
            verificationIds.append(verify.id);
            result.operations.append(std::move(verify));
        }
    } else {
        result.warnings.append(QStringLiteral(
            "Payload hashing is disabled; enable it to gate all writes on SHA-256 verification."));
    }

    QString mediaPreparation;
    QString imagePreparation;
    QString lastMediaWrite;
    QString lastImageWrite;

    if (!online && project.cloneSource && mediaSource) {
        const bool iso = sourceKind == SourceKind::IsoFile;
        ServicingOperation prepare = operation(
            sequence, QStringLiteral("media"), OperationKind::PrepareWorkspace,
            iso ? QStringLiteral("Extract ISO into isolated workspace")
                : QStringLiteral("Clone installation media into isolated workspace"),
            iso ? QStringLiteral("將 ISO 解壓去隔離工作區") : QStringLiteral("複製安裝媒體去隔離工作區"),
            QStringLiteral("Build a crash-recoverable project-owned media tree; the source remains read-only."),
            QStringLiteral("建立可從中斷恢復、工程專用嘅媒體樹；來源保持唯讀。"),
            QStringLiteral("powershell.exe"),
            powershellArguments(iso ? isoExtractScript(project.sourcePath, workspace)
                                    : atomicDirectoryCloneScript(project.sourcePath, workspace)),
            iso);
        addDependencies(prepare, verificationIds);
        prepare.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("media")},
                                       {QStringLiteral("source"), project.sourcePath},
                                       {QStringLiteral("destination"), workspace},
                                       {QStringLiteral("sourceImmutable"), true},
                                       {QStringLiteral("crashSafe"), true}};
        chainMediaWrite(result.operations, std::move(prepare), lastMediaWrite);
        mediaPreparation = lastMediaWrite;
    }

    if (!online && project.cloneSource && !convertedInput) {
        bool needsImageCopy = !mediaSource || imageExistsOutsideMedia;
        if (needsImageCopy) {
            ServicingOperation prepare = operation(
                sequence, QStringLiteral("image"), OperationKind::PrepareWorkspace,
                QStringLiteral("Clone image into project workspace"), QStringLiteral("複製映像去工程工作區"),
                QStringLiteral("Copy through a recoverable partial file so the original image is never mounted for write."),
                QStringLiteral("先寫入可恢復暫存檔，確保原本映像永遠唔會以寫入模式掛載。"),
                QStringLiteral("powershell.exe"),
                powershellArguments(atomicCopyScript(project.imagePath, result.workingImagePath)), false);
            addDependencies(prepare, verificationIds);
            addDependency(prepare, mediaPreparation);
            prepare.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("image")},
                                           {QStringLiteral("source"), project.imagePath},
                                           {QStringLiteral("destination"), result.workingImagePath},
                                           {QStringLiteral("sourceImmutable"), true},
                                           {QStringLiteral("crashSafe"), true}};
            if (isAncestorOrSame(workspace, result.workingImagePath))
                chainMediaWrite(result.operations, std::move(prepare), lastMediaWrite);
            else {
                prepare.writesMountedImage = true;
                result.operations.append(std::move(prepare));
            }
            imagePreparation = result.operations.constLast().id;
        } else {
            imagePreparation = mediaPreparation;
        }
    }

    const bool inputWasConverted = !online && project.cloneSource && convertedInput;
    if (inputWasConverted) {
        const QString conversionSource = mediaSource && !imageExistsOutsideMedia
            ? QDir(workspace).filePath(imageRelativePath) : project.imagePath;
        const QFileInfo conversionInfo(conversionSource);
        const QString splitWildcard = QDir(conversionInfo.absolutePath()).filePath(
            conversionInfo.completeBaseName() + QStringLiteral("*.swm"));
        const QString partial = partialOutputPath(result.workingImagePath);

        ServicingOperation clear = operation(
            sequence, QStringLiteral("image"), OperationKind::PrepareWorkspace,
            splitInput ? QStringLiteral("Prepare split-image conversion")
                       : QStringLiteral("Prepare ESD-to-WIM conversion"),
            splitInput ? QStringLiteral("準備分割映像轉換")
                       : QStringLiteral("準備 ESD 至 WIM 轉換"),
            QStringLiteral("Clear only the project-owned partial WIM left by a prior interrupted run."),
            QStringLiteral("只清理上次中斷留低、屬於工程嘅 WIM 暫存檔。"),
            QStringLiteral("powershell.exe"), powershellArguments(prepareOutputScript(partial, false)), false);
        addDependencies(clear, verificationIds);
        addDependency(clear, mediaPreparation);
        clear.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("image")},
                                     {QStringLiteral("destination"), partial}};
        result.operations.append(std::move(clear));

        QStringList conversionArguments{
            QStringLiteral("/English"), QStringLiteral("/Export-Image"),
            QStringLiteral("/SourceImageFile:%1").arg(conversionSource),
        };
        if (splitInput)
            conversionArguments.append(QStringLiteral("/SWMFile:%1").arg(splitWildcard));
        conversionArguments.append({
            QStringLiteral("/SourceIndex:%1").arg(project.selectedImageIndex),
            QStringLiteral("/DestinationImageFile:%1").arg(partial),
            QStringLiteral("/Compress:max"), QStringLiteral("/CheckIntegrity"),
        });

        ServicingOperation convert = operation(
            sequence, QStringLiteral("image"), OperationKind::PrepareWorkspace,
            splitInput ? QStringLiteral("Convert split SWM set to working WIM")
                       : QStringLiteral("Convert ESD edition to working WIM"),
            splitInput ? QStringLiteral("將 SWM 分卷轉成工作 WIM")
                       : QStringLiteral("將 ESD 版本轉成工作 WIM"),
            splitInput
                ? QStringLiteral("Export the selected index from every matching SWM part into a serviceable WIM.")
                : QStringLiteral("Export the selected ESD index into a serviceable project-owned WIM."),
            splitInput
                ? QStringLiteral("由所有相符 SWM 分卷匯出已揀索引，變成可維護 WIM。")
                : QStringLiteral("由 ESD 匯出已揀索引，變成工程專用可維護 WIM。"),
            QStringLiteral("dism.exe"), conversionArguments, true);
        addDependency(convert, result.operations.constLast().id);
        convert.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("image")},
                                       {QStringLiteral("source"), conversionSource},
                                       {QStringLiteral("destination"), partial},
                                       {QStringLiteral("sourceFormat"), splitInput
                                            ? QStringLiteral("swm") : QStringLiteral("esd")},
                                       {QStringLiteral("sourceImmutable"), true}};
        result.operations.append(std::move(convert));

        ServicingOperation publish = operation(
            sequence, QStringLiteral("image"), OperationKind::PrepareWorkspace,
            QStringLiteral("Publish working WIM atomically"), QStringLiteral("原子發佈工作 WIM"),
            QStringLiteral("Replace only the project working image and retain rollback data until success."),
            QStringLiteral("只取代工程工作映像，成功前保留回復資料。"),
            QStringLiteral("powershell.exe"),
            powershellArguments(atomicMoveScript(partial, result.workingImagePath)), false);
        addDependency(publish, result.operations.constLast().id);
        publish.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("image")},
                                       {QStringLiteral("destination"), result.workingImagePath},
                                       {QStringLiteral("crashSafe"), true}};
        result.operations.append(std::move(publish));
        imagePreparation = result.operations.constLast().id;
    }

    const int workingImageIndex = inputWasConverted ? 1 : project.selectedImageIndex;
    if (!online) {
        ServicingOperation inspect = operation(
            sequence, QStringLiteral("inspect"), OperationKind::Inspect,
            QStringLiteral("Inspect working image"), QStringLiteral("檢查工作映像"),
            QStringLiteral("Read indexes and architecture from the isolated working image."),
            QStringLiteral("由隔離工作映像讀取索引同架構。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), QStringLiteral("/Get-WimInfo"),
             QStringLiteral("/WimFile:%1").arg(result.workingImagePath)}, false);
        addDependencies(inspect, verificationIds);
        addDependency(inspect, imagePreparation);
        inspect.metadata = QJsonObject{{QStringLiteral("workingImagePath"), result.workingImagePath},
                                       {QStringLiteral("sourceImmutable"), true}};
        result.operations.append(inspect);

        ServicingOperation mount = operation(
            sequence, QStringLiteral("mount"), OperationKind::Mount,
            QStringLiteral("Mount selected edition from working image"), QStringLiteral("由工作映像掛載已揀版本"),
            QStringLiteral("Mount only the project-owned working image into the selected directory."),
            QStringLiteral("只將工程專用工作映像掛載去已揀資料夾。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), QStringLiteral("/Mount-Image"),
             QStringLiteral("/ImageFile:%1").arg(result.workingImagePath),
             QStringLiteral("/Index:%1").arg(workingImageIndex),
             QStringLiteral("/MountDir:%1").arg(project.mountPath)}, true);
        if (project.options.mountReadOnly)
            mount.arguments.append(QStringLiteral("/ReadOnly"));
        addDependency(mount, inspect.id);
        addDependencies(mount, verificationIds);
        mount.checkpointBefore = true;
        chainImageWrite(result.operations, std::move(mount), lastImageWrite);
    }

    for (const QString &driver : project.drivers) {
        QStringList arguments{QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Add-Driver"),
                              QStringLiteral("/Driver:%1").arg(driver)};
        if (QFileInfo(driver).isDir())
            arguments.append(QStringLiteral("/Recurse"));
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("driver"), OperationKind::Driver,
            QStringLiteral("Integrate driver: %1").arg(QFileInfo(driver).fileName()),
            QStringLiteral("整合驅動：%1").arg(QFileInfo(driver).fileName()),
            QStringLiteral("Stage an INF driver and its payload into the target image."),
            QStringLiteral("將 INF 驅動同相關檔案預先放入目標映像。"),
            QStringLiteral("dism.exe"), arguments, true), lastImageWrite);
    }

    for (const QString &update : project.updates) {
        ServicingOperation integrate = operation(
            sequence, QStringLiteral("update"), OperationKind::Update,
            QStringLiteral("Integrate update: %1").arg(QFileInfo(update).fileName()),
            QStringLiteral("整合更新：%1").arg(QFileInfo(update).fileName()),
            QStringLiteral("Add a CAB/MSU servicing-stack, cumulative, enablement, or language update."),
            QStringLiteral("加入 CAB/MSU 維護堆疊、累積、啟用或語言更新。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Add-Package"),
             QStringLiteral("/PackagePath:%1").arg(update), QStringLiteral("/PreventPending")}, true);
        integrate.metadata.insert(QStringLiteral("packageClass"), QStringLiteral("update"));
        integrate.metadata.insert(QStringLiteral("sourcePath"), update);
        chainImageWrite(result.operations, std::move(integrate), lastImageWrite);
    }
    for (const QString &package : project.packages) {
        ServicingOperation integrate = operation(
            sequence, QStringLiteral("package"), OperationKind::Package,
            QStringLiteral("Integrate package: %1").arg(QFileInfo(package).fileName()),
            QStringLiteral("整合套件：%1").arg(QFileInfo(package).fileName()),
            QStringLiteral("Add a language pack, Feature on Demand, or other standalone package."),
            QStringLiteral("加入語言包、按需功能或其他獨立套件。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Add-Package"),
             QStringLiteral("/PackagePath:%1").arg(package), QStringLiteral("/PreventPending")}, true);
        integrate.metadata.insert(QStringLiteral("packageClass"), QStringLiteral("package"));
        integrate.metadata.insert(QStringLiteral("sourcePath"), package);
        chainImageWrite(result.operations, std::move(integrate), lastImageWrite);
    }

    for (const QString &feature : project.featuresToEnable) {
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("feature"), OperationKind::Feature,
            QStringLiteral("Enable feature: %1").arg(feature), QStringLiteral("啟用功能：%1").arg(feature),
            QStringLiteral("Enable an optional Windows feature with its parent dependencies."),
            QStringLiteral("連父層依賴一齊啟用 Windows 選用功能。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Enable-Feature"),
             QStringLiteral("/FeatureName:%1").arg(feature), QStringLiteral("/All"),
             QStringLiteral("/NoRestart")}, true), lastImageWrite);
    }
    for (const QString &feature : project.featuresToDisable) {
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("feature"), OperationKind::Feature,
            QStringLiteral("Disable feature: %1").arg(feature), QStringLiteral("停用功能：%1").arg(feature),
            QStringLiteral("Disable an optional feature while preserving its payload."),
            QStringLiteral("停用選用功能，但保留功能檔案。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Disable-Feature"),
             QStringLiteral("/FeatureName:%1").arg(feature), QStringLiteral("/NoRestart")}, true),
            lastImageWrite);
    }
    for (const QString &capability : project.capabilitiesToAdd) {
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("capability"), OperationKind::Capability,
            QStringLiteral("Add capability: %1").arg(capability), QStringLiteral("加入能力：%1").arg(capability),
            QStringLiteral("Add a Windows capability or Feature on Demand."),
            QStringLiteral("加入 Windows 能力或者按需功能。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Add-Capability"),
             QStringLiteral("/CapabilityName:%1").arg(capability), QStringLiteral("/NoRestart")}, true),
            lastImageWrite);
    }
    for (const QString &capability : project.capabilitiesToRemove) {
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("capability"), OperationKind::Capability,
            QStringLiteral("Remove capability: %1").arg(capability), QStringLiteral("移除能力：%1").arg(capability),
            QStringLiteral("Remove an installed Windows capability payload."),
            QStringLiteral("移除已安裝 Windows 能力檔案。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Remove-Capability"),
             QStringLiteral("/CapabilityName:%1").arg(capability), QStringLiteral("/NoRestart")}, true, true),
            lastImageWrite);
    }
    for (const QString &packageName : project.appxPackagesToRemove) {
        ServicingOperation remove = operation(
            sequence, QStringLiteral("appx"), OperationKind::Appx,
            QStringLiteral("Remove provisioned app: %1").arg(packageName),
            QStringLiteral("移除預載 App：%1").arg(packageName),
            QStringLiteral("Remove an Appx/MSIX package from provisioning for new users."),
            QStringLiteral("由新用戶預載清單移除 Appx/MSIX 套件。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project),
             QStringLiteral("/Remove-ProvisionedAppxPackage"),
             QStringLiteral("/PackageName:%1").arg(packageName)}, true, true);
        remove.metadata.insert(QStringLiteral("appxAction"), QStringLiteral("remove"));
        remove.metadata.insert(QStringLiteral("packageName"), packageName);
        chainImageWrite(result.operations, std::move(remove), lastImageWrite);
    }
    for (const QString &packagePath : project.appxPackagesToProvision) {
        ServicingOperation provision = operation(
            sequence, QStringLiteral("appx"), OperationKind::Appx,
            QStringLiteral("Provision app: %1").arg(QFileInfo(packagePath).fileName()),
            QStringLiteral("預載 App：%1").arg(QFileInfo(packagePath).fileName()),
            QStringLiteral("Add a signed Appx/MSIX bundle for every new user."),
            QStringLiteral("為每個新用戶加入已簽署 Appx/MSIX bundle。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project),
             QStringLiteral("/Add-ProvisionedAppxPackage"),
             QStringLiteral("/PackagePath:%1").arg(packagePath), QStringLiteral("/SkipLicense")}, true);
        provision.metadata.insert(QStringLiteral("appxAction"), QStringLiteral("provision"));
        provision.metadata.insert(QStringLiteral("packagePath"), packagePath);
        chainImageWrite(result.operations, std::move(provision), lastImageWrite);
    }
    for (const QString &component : project.componentsToRemove) {
        ServicingOperation remove = operation(
            sequence, QStringLiteral("component"), OperationKind::Component,
            QStringLiteral("Remove component package: %1").arg(component),
            QStringLiteral("移除元件套件：%1").arg(component),
            QStringLiteral("Remove a package identity after compatibility checks; serviceability can be reduced."),
            QStringLiteral("相容性檢查後移除套件 identity；可能會降低日後維護能力。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Remove-Package"),
             QStringLiteral("/PackageName:%1").arg(component), QStringLiteral("/NoRestart")}, true, true);
        remove.metadata.insert(QStringLiteral("componentAction"), QStringLiteral("remove-package"));
        remove.metadata.insert(QStringLiteral("packageIdentity"), component);
        remove.compatibilityNotes.append(QStringLiteral(
            "Package identities are build-specific; confirm the identity exists in the selected image."));
        chainImageWrite(result.operations, std::move(remove), lastImageWrite);
    }
    for (const ScheduledTaskChange &change : project.scheduledTaskChanges) {
        const QString relative = safeRelative(change.taskPath);
        const QString tasksRoot = QDir(project.mountPath).filePath(QStringLiteral("Windows/System32/Tasks"));
        const QString taskFile = QDir(tasksRoot).filePath(relative);
        const bool removing = change.disposition == ScheduledTaskDisposition::Remove;
        const QString action = change.disposition == ScheduledTaskDisposition::Enable
            ? QStringLiteral("Enable")
            : removing ? QStringLiteral("Remove") : QStringLiteral("Disable");
        ServicingOperation task = operation(
            sequence, QStringLiteral("task"), OperationKind::ScheduledTask,
            QStringLiteral("%1 scheduled task: %2").arg(action, relative),
            QStringLiteral("%1排程工作：%2").arg(action, relative),
            removing
                ? QStringLiteral("Remove the offline task definition after an explicit compatibility override.")
                : QStringLiteral("Atomically update the Enabled flag in the offline task definition."),
            removing
                ? QStringLiteral("明確相容性解鎖後移除離線工作定義。")
                : QStringLiteral("原子更新離線工作定義嘅 Enabled 標記。"),
            QStringLiteral("powershell.exe"),
            powershellArguments(scheduledTaskScript(tasksRoot, relative, change.disposition)), true, removing);
        task.reversible = !removing;
        task.checkpointBefore = removing || !change.compatibilityOverride;
        task.metadata = QJsonObject{
            {QStringLiteral("taskAction"), action.toLower()},
            {QStringLiteral("taskPath"), relative},
            {QStringLiteral("taskFile"), taskFile},
            {QStringLiteral("compatibilityOverride"), change.compatibilityOverride},
        };
        task.compatibilityNotes.append(QStringLiteral(
            "Scheduled-task names and XML schemas are build-specific; validate the task on the selected image."));
        chainImageWrite(result.operations, std::move(task), lastImageWrite);
    }

    for (const StagedFile &staged : std::as_const(stagedFiles)) {
        const QString root = staged.scope == QStringLiteral("image") ? project.mountPath : workspace;
        const QString destination = QDir(root).filePath(staged.destination);
        ServicingOperation stage = operation(
            sequence, QStringLiteral("stage"), OperationKind::StageFile,
            QStringLiteral("Stage %1: %2").arg(staged.role, QFileInfo(staged.source).fileName()),
            QStringLiteral("加入%1：%2").arg(staged.role, QFileInfo(staged.source).fileName()),
            QStringLiteral("Copy a file or recursive runtime directory through a recoverable partial destination."),
            QStringLiteral("經可恢復暫存目的地複製檔案或完整 runtime 資料夾。"),
            QStringLiteral("powershell.exe"),
            powershellArguments(atomicCopyScript(staged.source, destination, root,
                                                 staged.destination)), true);
        stage.metadata = QJsonObject{{QStringLiteral("writeScope"), staged.scope},
                                     {QStringLiteral("role"), staged.role},
                                     {QStringLiteral("source"), staged.source},
                                     {QStringLiteral("destination"), destination},
                                     {QStringLiteral("relativeDestination"), staged.destination},
                                     {QStringLiteral("recursive"), QFileInfo(staged.source).isDir()},
                                     {QStringLiteral("crashSafe"), true}};
        addDependencies(stage, verificationIds);
        if (staged.scope == QStringLiteral("image"))
            chainImageWrite(result.operations, std::move(stage), lastImageWrite);
        else {
            addDependency(stage, mediaPreparation);
            chainMediaWrite(result.operations, std::move(stage), lastMediaWrite);
        }
    }

    QList<RegistryTweak> effectiveRegistryTweaks = project.registryTweaks;
    QHash<QString, QString> settingNotes;
    for (const SettingDefinition &definition : settingDefinitions()) {
        if (!settingEnabled(project, definition.id))
            continue;
        effectiveRegistryTweaks.append(definition.tweak);
        const QString note = settingCompatibilityNote(definition, project.targetBuildNumber);
        settingNotes.insert(definition.tweak.ownerId, note);
        if (note.contains(QStringLiteral("outside the verified range")))
            result.warnings.append(QStringLiteral("%1: %2").arg(definition.id, note));
    }

    for (qsizetype index = 0; index < effectiveRegistryTweaks.size(); ++index) {
        const RegistryTweak &tweak = effectiveRegistryTweaks.at(index);
        if (online) {
            QString hive = tweak.hive.trimmed().toUpper();
            if (hive == QStringLiteral("HKEY_LOCAL_MACHINE")) hive = QStringLiteral("HKLM");
            else if (hive == QStringLiteral("HKEY_CURRENT_USER")) hive = QStringLiteral("HKCU");
            else if (hive == QStringLiteral("HKEY_CLASSES_ROOT")) hive = QStringLiteral("HKCR");
            else if (hive == QStringLiteral("HKEY_USERS")) hive = QStringLiteral("HKU");
            else if (hive == QStringLiteral("HKEY_CURRENT_CONFIG")) hive = QStringLiteral("HKCC");
            const QString relativeKey = registryKeyWithoutHivePrefix(tweak);
            const QString targetKey = relativeKey.isEmpty() ? hive : hive + QLatin1Char('\\') + relativeKey;
            const bool deleting = tweak.deleteValue || tweak.deleteAllValues;
            QStringList arguments{deleting ? QStringLiteral("delete") : QStringLiteral("add"), targetKey};
            if (tweak.deleteAllValues)
                arguments << QStringLiteral("/va");
            else if (!tweak.valueName.isEmpty())
                arguments << QStringLiteral("/v") << tweak.valueName;
            else
                arguments << QStringLiteral("/ve");
            if (deleting)
                arguments << QStringLiteral("/f");
            else
                arguments << QStringLiteral("/t") << tweak.type << QStringLiteral("/d")
                          << registryCommandValue(tweak) << QStringLiteral("/f");
            ServicingOperation registryEdit = operation(
                sequence, QStringLiteral("regedit"), OperationKind::Registry,
                tweak.deleteAllValues ? QStringLiteral("Clear live registry key values")
                                      : QStringLiteral("Apply live registry setting"),
                tweak.deleteAllValues ? QStringLiteral("清除即時登錄 key 值") : QStringLiteral("套用即時登錄設定"),
                QStringLiteral("Apply the typed registry edit to the explicitly selected live target."),
                QStringLiteral("將已定型登錄修改套用去明確揀選嘅即時目標。"),
                QStringLiteral("reg.exe"), arguments, true, deleting);
            registryEdit.writeScope = OperationWriteScope::Host;
            registryEdit.metadata = QJsonObject{
                {QStringLiteral("writeScope"), QStringLiteral("host")},
                {QStringLiteral("registryKey"), relativeKey},
                {QStringLiteral("registryType"), tweak.type},
                {QStringLiteral("deleteAllValues"), tweak.deleteAllValues},
                {QStringLiteral("owner"), tweak.ownerId},
            };
            const auto settingNote = settingNotes.constFind(tweak.ownerId);
            if (settingNote != settingNotes.cend()) {
                const QString settingId = tweak.ownerId.mid(QStringLiteral("customize:").size());
                registryEdit.reversible = true;
                registryEdit.compatibilityNotes.append(*settingNote);
                registryEdit.metadata.insert(QStringLiteral("customizeSetting"), settingId);
                registryEdit.metadata.insert(QStringLiteral("reversal"), QJsonObject{
                    {QStringLiteral("action"), QStringLiteral("delete-value")},
                    {QStringLiteral("hive"), tweak.hive},
                    {QStringLiteral("key"), tweak.key},
                    {QStringLiteral("name"), tweak.valueName},
                });
            }
            addDependency(registryEdit, lastImageWrite);
            lastImageWrite = registryEdit.id;
            result.operations.append(std::move(registryEdit));
            continue;
        }
        const QString mountKey = QStringLiteral("HKLM\\WimForgeOffline_%1").arg(index);
        const QString hiveFile = registryHiveFile(project, tweak);
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("regload"), OperationKind::Registry,
            QStringLiteral("Load offline registry hive"), QStringLiteral("載入離線登錄 hive"),
            QStringLiteral("Temporarily attach the target hive under a unique key."),
            QStringLiteral("暫時將目標 hive 掛去獨立 key。"),
            QStringLiteral("reg.exe"), {QStringLiteral("load"), mountKey, hiveFile}, true), lastImageWrite);

        const QString key = registryRelativeKey(tweak);
        const QString targetKey = key.isEmpty() ? mountKey : mountKey + QLatin1Char('\\') + key;
        const bool deleting = tweak.deleteValue || tweak.deleteAllValues;
        QStringList arguments{deleting ? QStringLiteral("delete") : QStringLiteral("add"),
                              targetKey};
        if (tweak.deleteAllValues)
            arguments << QStringLiteral("/va");
        else if (!tweak.valueName.isEmpty())
            arguments << QStringLiteral("/v") << tweak.valueName;
        else
            arguments << QStringLiteral("/ve");
        if (deleting)
            arguments << QStringLiteral("/f");
        else
            arguments << QStringLiteral("/t") << tweak.type << QStringLiteral("/d")
                      << registryCommandValue(tweak) << QStringLiteral("/f");
        ServicingOperation registryEdit = operation(
            sequence, QStringLiteral("regedit"), OperationKind::Registry,
            tweak.deleteAllValues ? QStringLiteral("Clear registry key values")
                                  : QStringLiteral("Apply registry setting"),
            tweak.deleteAllValues ? QStringLiteral("清除登錄 key 值") : QStringLiteral("套用登錄設定"),
            tweak.deleteAllValues
                ? QStringLiteral("Delete values directly under the target key while retaining the key and its subkeys.")
                : QStringLiteral("Write the configured value into the offline hive."),
            tweak.deleteAllValues
                ? QStringLiteral("只刪除目標 key 直接包含嘅值，保留 key 同所有 subkey。")
                : QStringLiteral("將設定值寫入離線 hive。"),
            QStringLiteral("reg.exe"), arguments, true, deleting);
        registryEdit.metadata = QJsonObject{
            {QStringLiteral("registryHiveFile"), hiveFile},
            {QStringLiteral("registryKey"), key},
            {QStringLiteral("registryType"), tweak.type},
            {QStringLiteral("deleteAllValues"), tweak.deleteAllValues},
            {QStringLiteral("owner"), tweak.ownerId},
        };
        const auto settingNote = settingNotes.constFind(tweak.ownerId);
        if (settingNote != settingNotes.cend()) {
            const QString settingId = tweak.ownerId.mid(QStringLiteral("customize:").size());
            registryEdit.reversible = true;
            registryEdit.compatibilityNotes.append(*settingNote);
            registryEdit.metadata.insert(QStringLiteral("customizeSetting"), settingId);
            registryEdit.metadata.insert(QStringLiteral("reversal"), QJsonObject{
                {QStringLiteral("action"), QStringLiteral("delete-value")},
                {QStringLiteral("hive"), tweak.hive},
                {QStringLiteral("key"), tweak.key},
                {QStringLiteral("name"), tweak.valueName},
            });
        }
        chainImageWrite(result.operations, std::move(registryEdit), lastImageWrite);
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("regunload"), OperationKind::Registry,
            QStringLiteral("Unload offline registry hive"), QStringLiteral("卸載離線登錄 hive"),
            QStringLiteral("Flush and detach the temporary registry mount."),
            QStringLiteral("寫清同拆走暫時登錄掛載。"), QStringLiteral("reg.exe"),
            {QStringLiteral("unload"), mountKey}, true), lastImageWrite);
    }

    if (!project.unattendedXmlPath.trimmed().isEmpty()) {
        ServicingOperation apply = operation(
            sequence, QStringLiteral("unattend"), OperationKind::Unattended,
            QStringLiteral("Apply unattended settings"), QStringLiteral("套用無人值守設定"),
            QStringLiteral("Apply the answer file to the offline image with Windows servicing."),
            QStringLiteral("用 Windows 維護引擎將答案檔套入離線映像。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), dismTarget(project),
             QStringLiteral("/Apply-Unattend:%1").arg(project.unattendedXmlPath)}, true);
        apply.metadata.insert(QStringLiteral("answerFileAction"), QStringLiteral("apply"));
        apply.metadata.insert(QStringLiteral("sourcePath"), project.unattendedXmlPath);
        chainImageWrite(result.operations, std::move(apply), lastImageWrite);
    }

    for (const AnswerFileAction &answerFile : project.answerFileActions) {
        if (answerFile.mode == AnswerFileMode::Place)
            continue; // Placement is represented by the typed StageFile operation above.
        if (answerFile.mode == AnswerFileMode::Apply) {
            ServicingOperation apply = operation(
                sequence, QStringLiteral("unattend"), OperationKind::Unattended,
                QStringLiteral("Apply answer file: %1").arg(QFileInfo(answerFile.sourcePath).fileName()),
                QStringLiteral("套用答案檔：%1").arg(QFileInfo(answerFile.sourcePath).fileName()),
                QStringLiteral("Apply the selected answer file to the offline image with DISM."),
                QStringLiteral("用 DISM 將已揀答案檔套用去離線映像。"),
                QStringLiteral("dism.exe"),
                {QStringLiteral("/English"), dismTarget(project),
                 QStringLiteral("/Apply-Unattend:%1").arg(answerFile.sourcePath)}, true);
            apply.metadata.insert(QStringLiteral("answerFileAction"), QStringLiteral("apply"));
            apply.metadata.insert(QStringLiteral("sourcePath"), answerFile.sourcePath);
            chainImageWrite(result.operations, std::move(apply), lastImageWrite);
            continue;
        }

        const QString root = answerFile.scope == PayloadScope::Media ? workspace : project.mountPath;
        const QString destination = QDir(root).filePath(safeRelative(answerFile.destinationPath));
        ServicingOperation remove = operation(
            sequence, QStringLiteral("unattend"), OperationKind::Unattended,
            QStringLiteral("Remove placed answer file: %1").arg(answerFile.destinationPath),
            QStringLiteral("移除已放置答案檔：%1").arg(answerFile.destinationPath),
            QStringLiteral("Remove only the explicitly selected answer-file destination."),
            QStringLiteral("只移除明確揀選嘅答案檔目的地。"),
            QStringLiteral("powershell.exe"),
            powershellArguments(removeStagedPathScript(root, safeRelative(answerFile.destinationPath))), true, true);
        remove.metadata = QJsonObject{
            {QStringLiteral("answerFileAction"), QStringLiteral("remove")},
            {QStringLiteral("destination"), destination},
            {QStringLiteral("relativeDestination"), answerFile.destinationPath},
        };
        if (answerFile.scope == PayloadScope::Media) {
            addDependency(remove, mediaPreparation);
            chainMediaWrite(result.operations, std::move(remove), lastMediaWrite);
        } else {
            chainImageWrite(result.operations, std::move(remove), lastImageWrite);
        }
    }

    QList<PostSetupCommand> commands = project.postSetupCommands;
    for (const QString &entry : project.postSetupItems)
        commands.append(PostSetupCommand{entry, QStringLiteral("Legacy post-setup command")});
    for (const PostSetupCommand &command : std::as_const(commands)) {
        const QString setupDir = QDir(project.mountPath).filePath(QStringLiteral("Windows/Setup/Scripts"));
        const QString commandId = QString::fromLatin1(
            QCryptographicHash::hash(command.command.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        const QString script = appendPostSetupScript(setupDir, command.command, commandId);
        ServicingOperation append = operation(
            sequence, QStringLiteral("postsetup"), OperationKind::PostSetup,
            command.label.trimmed().isEmpty() ? QStringLiteral("Stage post-setup command") : command.label,
            QStringLiteral("加入裝完後指令"),
            QStringLiteral("Append a transparent command to SetupComplete.cmd in the target."),
            QStringLiteral("將清楚可見嘅指令加入目標 SetupComplete.cmd。"),
            QStringLiteral("powershell.exe"), powershellArguments(script), true);
        append.metadata.insert(QStringLiteral("literalCommand"), command.command);
        append.metadata.insert(QStringLiteral("commandLabel"), command.label);
        append.metadata.insert(QStringLiteral("commandId"), commandId);
        append.metadata.insert(QStringLiteral("idempotent"), true);
        chainImageWrite(result.operations, std::move(append), lastImageWrite);
    }

    if (project.options.cleanupComponentStore) {
        QStringList arguments{QStringLiteral("/English"), dismTarget(project),
                              QStringLiteral("/Cleanup-Image"), QStringLiteral("/StartComponentCleanup")};
        if (project.options.resetBase)
            arguments.append(QStringLiteral("/ResetBase"));
        chainImageWrite(result.operations, operation(
            sequence, QStringLiteral("cleanup"), OperationKind::Cleanup,
            project.options.resetBase ? QStringLiteral("Clean component store and reset base")
                                      : QStringLiteral("Clean component store"),
            project.options.resetBase ? QStringLiteral("清理元件庫兼重設基底") : QStringLiteral("清理元件庫"),
            project.options.resetBase
                ? QStringLiteral("Permanently remove superseded components; installed updates can no longer be uninstalled.")
                : QStringLiteral("Remove superseded payloads using the supported DISM path."),
            project.options.resetBase
                ? QStringLiteral("永久移除被取代元件；已裝更新之後唔可以解除安裝。")
                : QStringLiteral("用支援嘅 DISM 方法清理被取代元件。"),
            QStringLiteral("dism.exe"), arguments, true, project.options.resetBase), lastImageWrite);
    }

    chainImageWrite(result.operations, operation(
        sequence, QStringLiteral("validate"), OperationKind::Validate,
        QStringLiteral("Scan image health"), QStringLiteral("掃描映像健康"),
        QStringLiteral("Ask the component store to detect corruption before commit."),
        QStringLiteral("commit 之前叫元件庫檢查有冇損壞。"),
        QStringLiteral("dism.exe"),
        {QStringLiteral("/English"), dismTarget(project), QStringLiteral("/Cleanup-Image"),
         QStringLiteral("/ScanHealth")}, true), lastImageWrite);

    if (!online) {
        ServicingOperation unmount = operation(
            sequence, QStringLiteral("unmount"), OperationKind::Commit,
            discard ? QStringLiteral("Discard and unmount working image")
                    : QStringLiteral("Commit and unmount working image"),
            discard ? QStringLiteral("放棄改動並卸載工作映像") : QStringLiteral("提交改動並卸載工作映像"),
            discard ? QStringLiteral("Release the mount without changing even the working image container.")
                    : QStringLiteral("Flush all changes only to the isolated working image."),
            discard ? QStringLiteral("釋放掛載，連工作映像都唔會改。")
                    : QStringLiteral("只將所有改動寫入隔離工作映像。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), QStringLiteral("/Unmount-Image"),
             QStringLiteral("/MountDir:%1").arg(project.mountPath),
             discard ? QStringLiteral("/Discard") : QStringLiteral("/Commit")}, true, !discard);
        addDependency(unmount, lastImageWrite);
        unmount.checkpointBefore = true;
        chainImageWrite(result.operations, std::move(unmount), lastImageWrite);
    }

    QString lastOutputWrite = lastImageWrite;
    if (!online && !discard && !project.outputPath.trimmed().isEmpty()
        && (format == QStringLiteral("wim") || format == QStringLiteral("esd"))) {
        const QString partial = partialOutputPath(project.outputPath);
        ServicingOperation clear = operation(
            sequence, QStringLiteral("export"), OperationKind::PrepareWorkspace,
            QStringLiteral("Prepare atomic image export"), QStringLiteral("準備原子映像匯出"),
            QStringLiteral("Remove only a stale partial output from an interrupted export."),
            QStringLiteral("只移除中斷匯出留低嘅暫存輸出。"),
            QStringLiteral("powershell.exe"), powershellArguments(prepareOutputScript(partial, false)), false);
        addDependency(clear, lastImageWrite);
        result.operations.append(std::move(clear));

        const QString compression = format == QStringLiteral("esd")
            ? QStringLiteral("recovery") : project.options.compression;
        ServicingOperation exportImage = operation(
            sequence, QStringLiteral("export"), OperationKind::Export,
            QStringLiteral("Export %1 image").arg(format.toUpper()),
            QStringLiteral("匯出 %1 映像").arg(format.toUpper()),
            QStringLiteral("Export the selected working edition into a checked partial container."),
            QStringLiteral("將已揀工作版本匯出去經檢查暫存容器。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), QStringLiteral("/Export-Image"),
             QStringLiteral("/SourceImageFile:%1").arg(result.workingImagePath),
             QStringLiteral("/SourceIndex:%1").arg(workingImageIndex),
             QStringLiteral("/DestinationImageFile:%1").arg(partial),
             QStringLiteral("/Compress:%1").arg(compression), QStringLiteral("/CheckIntegrity")}, true);
        addDependency(exportImage, result.operations.constLast().id);
        result.operations.append(std::move(exportImage));

        ServicingOperation publish = operation(
            sequence, QStringLiteral("export"), OperationKind::Export,
            QStringLiteral("Publish image output atomically"), QStringLiteral("原子發佈映像輸出"),
            QStringLiteral("Replace the requested output only after DISM finishes successfully."),
            QStringLiteral("只喺 DISM 成功完成後先取代指定輸出。"),
            QStringLiteral("powershell.exe"), powershellArguments(
                atomicMoveScript(partial, project.outputPath)), false, QFileInfo::exists(project.outputPath));
        addDependency(publish, result.operations.constLast().id);
        publish.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("output")},
                                       {QStringLiteral("destination"), project.outputPath},
                                       {QStringLiteral("crashSafe"), true}};
        result.operations.append(std::move(publish));
        lastOutputWrite = result.operations.constLast().id;
    }

    auto appendSplit = [&](const QString &sourceImage,
                           const QString &destinationFirstPart,
                           const QString &dependency,
                           bool mediaWrite) -> QString {
        const QString temporary = QDir(workingRoot).filePath(
            QStringLiteral("split/%1-%2")
                .arg(QFileInfo(destinationFirstPart).completeBaseName())
                .arg(sequence + 1));
        ServicingOperation clear = operation(
            sequence, QStringLiteral("split"), OperationKind::PrepareWorkspace,
            QStringLiteral("Prepare SWM output staging"), QStringLiteral("準備 SWM 輸出暫存"),
            QStringLiteral("Create a project-owned temporary directory for every split part."),
            QStringLiteral("為所有分卷建立工程專用暫存資料夾。"),
            QStringLiteral("powershell.exe"), powershellArguments(prepareOutputScript(temporary, true)), false);
        addDependency(clear, dependency);
        if (mediaWrite)
            addDependency(clear, lastMediaWrite);
        result.operations.append(std::move(clear));

        const int splitSize = qBound(
            100, extraInt(project, QStringLiteral("splitSizeMb"), project.options.splitSizeMb), 4095);
        const QString temporaryFirstPart = QDir(temporary).filePath(QFileInfo(destinationFirstPart).fileName());
        ServicingOperation split = operation(
            sequence, QStringLiteral("split"), OperationKind::Split,
            QStringLiteral("Split working WIM for FAT32 media"), QStringLiteral("為 FAT32 分割工作 WIM"),
            QStringLiteral("Split the isolated WIM into checked SWM parts below the FAT32 limit."),
            QStringLiteral("將隔離 WIM 分成細過 FAT32 上限並經檢查嘅 SWM。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), QStringLiteral("/Split-Image"),
             QStringLiteral("/ImageFile:%1").arg(sourceImage),
             QStringLiteral("/SWMFile:%1").arg(temporaryFirstPart),
             QStringLiteral("/FileSize:%1").arg(splitSize), QStringLiteral("/CheckIntegrity")}, true);
        addDependency(split, result.operations.constLast().id);
        result.operations.append(std::move(split));

        ServicingOperation publish = operation(
            sequence, QStringLiteral("split"), OperationKind::Split,
            QStringLiteral("Publish SWM parts atomically"), QStringLiteral("原子發佈 SWM 分卷"),
            QStringLiteral("Swap the complete split set into place and restore the old set if publication fails."),
            QStringLiteral("一次過換入完整分卷；發佈失敗就還原舊分卷。"),
            QStringLiteral("powershell.exe"), powershellArguments(
                splitFinalizeScript(temporary, destinationFirstPart)), false, true);
        addDependency(publish, result.operations.constLast().id);
        publish.writesMediaWorkspace = mediaWrite;
        publish.metadata = QJsonObject{{QStringLiteral("writeScope"), mediaWrite
                                           ? QStringLiteral("media") : QStringLiteral("output")},
                                       {QStringLiteral("destination"), destinationFirstPart},
                                       {QStringLiteral("crashSafe"), true}};
        result.operations.append(std::move(publish));
        return result.operations.constLast().id;
    };

    if (!online && !discard && format == QStringLiteral("swm")
        && !project.outputPath.trimmed().isEmpty()) {
        QString sourceForSplit = result.workingImagePath;
        QString dependency = lastImageWrite;
        if (QFileInfo(sourceForSplit).suffix().compare(QStringLiteral("wim"), Qt::CaseInsensitive) != 0) {
            const QString intermediary = QDir(workingRoot).filePath(QStringLiteral("exports/install-for-split.wim"));
            const QString partial = partialOutputPath(intermediary);
            ServicingOperation clear = operation(
                sequence, QStringLiteral("split"), OperationKind::PrepareWorkspace,
                QStringLiteral("Prepare WIM for splitting"), QStringLiteral("準備用作分割嘅 WIM"),
                QStringLiteral("Clear only the prior project-owned intermediary."),
                QStringLiteral("只清理上次工程專用中介檔。"), QStringLiteral("powershell.exe"),
                powershellArguments(prepareOutputScript(partial, false)), false);
            addDependency(clear, dependency);
            result.operations.append(std::move(clear));
            ServicingOperation convert = operation(
                sequence, QStringLiteral("split"), OperationKind::Export,
                QStringLiteral("Export serviceable WIM for splitting"), QStringLiteral("匯出可分割 WIM"),
                QStringLiteral("Convert the committed working edition to WIM before splitting."),
                QStringLiteral("分割前將已提交工作版本轉成 WIM。"), QStringLiteral("dism.exe"),
                {QStringLiteral("/English"), QStringLiteral("/Export-Image"),
                 QStringLiteral("/SourceImageFile:%1").arg(result.workingImagePath),
                 QStringLiteral("/SourceIndex:%1").arg(workingImageIndex),
                 QStringLiteral("/DestinationImageFile:%1").arg(partial),
                 QStringLiteral("/Compress:max"), QStringLiteral("/CheckIntegrity")}, true);
            addDependency(convert, result.operations.constLast().id);
            result.operations.append(std::move(convert));
            ServicingOperation publish = operation(
                sequence, QStringLiteral("split"), OperationKind::Export,
                QStringLiteral("Publish intermediary WIM"), QStringLiteral("發佈中介 WIM"),
                QStringLiteral("Make the completed WIM visible only after conversion succeeds."),
                QStringLiteral("轉換成功後先令完整 WIM 可見。"), QStringLiteral("powershell.exe"),
                powershellArguments(atomicMoveScript(partial, intermediary)), false);
            addDependency(publish, result.operations.constLast().id);
            result.operations.append(std::move(publish));
            sourceForSplit = intermediary;
            dependency = result.operations.constLast().id;
        }
        lastOutputWrite = appendSplit(sourceForSplit, project.outputPath, dependency, false);
    }

    const bool creatingIso = format == QStringLiteral("iso") || project.options.createIso;
    if (!online && !discard && esdInput && mediaSource && creatingIso) {
        const QString destinationEsd = QDir(workspace).filePath(imageRelativePath);
        const QString partial = partialOutputPath(destinationEsd);
        ServicingOperation clear = operation(
            sequence, QStringLiteral("media-image"), OperationKind::PrepareWorkspace,
            QStringLiteral("Prepare ESD media replacement"), QStringLiteral("準備 ESD 媒體替換"),
            QStringLiteral("Clear only the project-owned partial ESD left by a prior interrupted export."),
            QStringLiteral("只清理上次中斷留低、屬於工程嘅 ESD 暫存檔。"),
            QStringLiteral("powershell.exe"),
            powershellArguments(prepareOutputScript(partial, false)), false);
        addDependency(clear, lastImageWrite);
        clear.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("media")},
                                     {QStringLiteral("destination"), partial}};
        chainMediaWrite(result.operations, std::move(clear), lastMediaWrite);

        ServicingOperation exportEsd = operation(
            sequence, QStringLiteral("media-image"), OperationKind::Export,
            QStringLiteral("Export serviced edition back to install.esd"),
            QStringLiteral("將已維護版本匯出返 install.esd"),
            QStringLiteral("Rebuild the ISO media's recovery-compressed ESD from the serviceable working WIM."),
            QStringLiteral("由可維護工作 WIM 重建 ISO 媒體嘅 recovery 壓縮 ESD。"),
            QStringLiteral("dism.exe"),
            {QStringLiteral("/English"), QStringLiteral("/Export-Image"),
             QStringLiteral("/SourceImageFile:%1").arg(result.workingImagePath),
             QStringLiteral("/SourceIndex:%1").arg(workingImageIndex),
             QStringLiteral("/DestinationImageFile:%1").arg(partial),
             QStringLiteral("/Compress:recovery"), QStringLiteral("/CheckIntegrity")}, true);
        exportEsd.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("media")},
                                         {QStringLiteral("source"), result.workingImagePath},
                                         {QStringLiteral("destination"), partial},
                                         {QStringLiteral("destinationFormat"), QStringLiteral("esd")}};
        chainMediaWrite(result.operations, std::move(exportEsd), lastMediaWrite);

        ServicingOperation publishEsd = operation(
            sequence, QStringLiteral("media-image"), OperationKind::Export,
            QStringLiteral("Publish rebuilt install.esd atomically"),
            QStringLiteral("原子發佈重建 install.esd"),
            QStringLiteral("Replace the media ESD only after DISM completes the checked export."),
            QStringLiteral("DISM 完成並檢查匯出後，先替換媒體 ESD。"),
            QStringLiteral("powershell.exe"),
            powershellArguments(atomicMoveScript(partial, destinationEsd)), false, true);
        publishEsd.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("media")},
                                          {QStringLiteral("destination"), destinationEsd},
                                          {QStringLiteral("crashSafe"), true}};
        chainMediaWrite(result.operations, std::move(publishEsd), lastMediaWrite);
    }

    if (!online && !discard && splitInput && mediaSource && creatingIso) {
        const QString destinationSplit = QDir(workspace).filePath(imageRelativePath);
        lastMediaWrite = appendSplit(result.workingImagePath, destinationSplit, lastImageWrite, true);
    }

    if (!online && !discard && creatingIso) {
        const QString isoOutput = format == QStringLiteral("iso") ? project.outputPath
            : extraString(project, QStringLiteral("isoOutputPath"));
        if (isoOutput.trimmed().isEmpty() || !QFileInfo(isoOutput).isAbsolute()) {
            result.errors.append(QStringLiteral("ISO creation requires an absolute output path."));
        } else if (normalized(isoOutput) == normalized(project.sourcePath)
                   || isAncestorOrSame(project.sourcePath, isoOutput)) {
            result.errors.append(QStringLiteral("ISO output must not overwrite or be placed inside the source."));
        } else if (isAncestorOrSame(workspace, isoOutput)) {
            result.errors.append(QStringLiteral("ISO output must not be created inside its media workspace."));
        } else {
            const QString partial = partialOutputPath(isoOutput);
            ServicingOperation clear = operation(
                sequence, QStringLiteral("iso"), OperationKind::PrepareWorkspace,
                QStringLiteral("Prepare atomic ISO output"), QStringLiteral("準備原子 ISO 輸出"),
                QStringLiteral("Clear only a stale project partial ISO from an interrupted build."),
                QStringLiteral("只清理中斷建立留低嘅工程 ISO 暫存檔。"),
                QStringLiteral("powershell.exe"), powershellArguments(prepareOutputScript(partial, false)), false);
            addDependency(clear, lastOutputWrite);
            addDependency(clear, lastMediaWrite);
            result.operations.append(std::move(clear));

            const QString oscdimg = extraString(project, QStringLiteral("oscdimgPath"),
                                                QStringLiteral("oscdimg.exe"));
            const QString biosBoot = QDir(workspace).filePath(QStringLiteral("boot/etfsboot.com"));
            const QString uefiBoot = QDir(workspace).filePath(
                QStringLiteral("efi/microsoft/boot/efisys.bin"));
            ServicingOperation iso = operation(
                sequence, QStringLiteral("iso"), OperationKind::CreateIso,
                QStringLiteral("Create bootable ISO"), QStringLiteral("建立可啟動 ISO"),
                QStringLiteral("Build a checked partial dual BIOS/UEFI ISO only after every image and media write."),
                QStringLiteral("所有映像同媒體寫入完成後，先建立 BIOS/UEFI 雙啟動暫存 ISO。"),
                oscdimg,
                {QStringLiteral("-m"), QStringLiteral("-o"), QStringLiteral("-u2"),
                 QStringLiteral("-udfver102"), QStringLiteral("-l%1").arg(isoLabel),
                 QStringLiteral("-bootdata:2#p0,e,b%1#pEF,e,b%2").arg(biosBoot, uefiBoot),
                 workspace, partial}, true);
            // Deliberately use a direct barrier, not only the last chained write.
            // Imported/reordered plans therefore cannot let oscdimg race a media copy.
            for (const ServicingOperation &candidate : std::as_const(result.operations)) {
                if (candidate.writesMountedImage || candidate.writesMediaWorkspace)
                    addDependency(iso, candidate.id);
            }
            addDependency(iso, result.operations.constLast().id);
            iso.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("output")},
                                       {QStringLiteral("workspace"), workspace},
                                       {QStringLiteral("partialOutput"), partial},
                                       {QStringLiteral("finalOutput"), isoOutput}};
            result.operations.append(std::move(iso));

            ServicingOperation publish = operation(
                sequence, QStringLiteral("iso"), OperationKind::CreateIso,
                QStringLiteral("Publish ISO atomically"), QStringLiteral("原子發佈 ISO"),
                QStringLiteral("Replace the final ISO only after oscdimg exits successfully."),
                QStringLiteral("只喺 oscdimg 成功完成後先取代最終 ISO。"),
                QStringLiteral("powershell.exe"), powershellArguments(
                    atomicMoveScript(partial, isoOutput)), false, QFileInfo::exists(isoOutput));
            addDependency(publish, result.operations.constLast().id);
            publish.metadata = QJsonObject{{QStringLiteral("writeScope"), QStringLiteral("output")},
                                           {QStringLiteral("destination"), isoOutput},
                                           {QStringLiteral("crashSafe"), true}};
            result.operations.append(std::move(publish));
        }
    }

    if (project.options.resetBase)
        result.warnings.append(QStringLiteral(
            "ResetBase is irreversible and prevents uninstalling integrated updates."));
    if (!project.componentsToRemove.isEmpty())
        result.warnings.append(QStringLiteral(
            "Component package removal can reduce Windows serviceability; test the ISO in a VM."));
    if (online)
        result.warnings.append(QStringLiteral(
            "Live-install servicing changes the running operating system; an offline image is safer."));
    if (project.options.dryRun) {
        result.warnings.append(QStringLiteral(
            "Dry-run is enabled; callers must preview this plan without launching its commands."));
        for (ServicingOperation &item : result.operations)
            item.metadata.insert(QStringLiteral("previewOnly"), true);
    }
    finalizeOperationSemantics(result.operations);
    return result;
}

bool ServicingPlan::exportPowerShell(const ProjectConfig &project,
                                     const QList<ServicingOperation> &operations,
                                     const QString &destination,
                                     QString *error)
{
    if (destination.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Choose a PowerShell script destination.");
        return false;
    }
    QList<int> orderedIndices;
    if (!stableTopologicalOrder(operations, &orderedIndices, error))
        return false;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create the script folder.");
        return false;
    }

    QString script;
    script += QStringLiteral("# WimForge reproducible servicing plan\r\n");
    script += QStringLiteral("# Project: %1\r\n").arg(project.projectName);
    script += QStringLiteral("# Direct executable/argument invocation; review before running as Administrator.\r\n");
    script += QStringLiteral("[CmdletBinding()] param([switch]$WhatIfPlan)\r\n");
    script += QStringLiteral("$ErrorActionPreference = 'Stop'\r\n");
    script += QStringLiteral("$ProgressPreference = 'Continue'\r\n");
    script += QStringLiteral(R"PS($wimforgeSystem32 = [Environment]::SystemDirectory
$wimforgePowerShell = [IO.Path]::Combine($wimforgeSystem32, 'WindowsPowerShell', 'v1.0')
foreach ($keyObject in @([Environment]::GetEnvironmentVariables().Keys)) {
  $key = [string]$keyObject
  $upper = $key.ToUpperInvariant()
  if ($upper.StartsWith('COR_') -or $upper.StartsWith('COMPLUS_') -or $upper.StartsWith('DOTNET_')) {
    [Environment]::SetEnvironmentVariable($key, $null, [EnvironmentVariableTarget]::Process)
  }
}
$env:PATH = $wimforgeSystem32 + ';' + $wimforgePowerShell
$env:PSModulePath = [IO.Path]::Combine($wimforgePowerShell, 'Modules')
$wimforgeManagementModule = [IO.Path]::Combine($env:PSModulePath, 'Microsoft.PowerShell.Management', 'Microsoft.PowerShell.Management.psd1')
$wimforgeUtilityModule = [IO.Path]::Combine($env:PSModulePath, 'Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Utility.psd1')
Microsoft.PowerShell.Core\Import-Module -Name $wimforgeManagementModule -Force -ErrorAction Stop
Microsoft.PowerShell.Core\Import-Module -Name $wimforgeUtilityModule -Force -ErrorAction Stop
$PSModuleAutoLoadingPreference = 'None'

function Test-WimForgeProtectedFile([string]$Path, [string]$Root) {
  if ([String]::IsNullOrWhiteSpace($Path) -or [String]::IsNullOrWhiteSpace($Root)) { return $false }
  $full = [IO.Path]::GetFullPath($Path)
  $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char]'\', [char]'/')
  if (-not $full.StartsWith($rootFull + '\', [StringComparison]::OrdinalIgnoreCase)) { return $false }
  if (-not [IO.File]::Exists($full)) { return $false }
  $cursor = $full
  while ($true) {
    if (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $false }
    if ($cursor.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase)) { break }
    $parent = [IO.Directory]::GetParent($cursor)
    if ($null -eq $parent) { return $false }
    $cursor = $parent.FullName
  }
  return $true
}

function Resolve-WimForgeExecutable([string]$Exe) {
  if ([IO.Path]::IsPathRooted($Exe)) {
    $absolute = [IO.Path]::GetFullPath($Exe)
    if (-not [IO.File]::Exists($absolute)) { throw "Reviewed executable does not exist: $absolute" }
    return $absolute
  }
  if ([String]::IsNullOrWhiteSpace($Exe) -or $Exe.IndexOfAny([char[]]@('\','/')) -ge 0) {
    throw "Relative executable paths are forbidden: $Exe"
  }
  $name = $Exe.ToLowerInvariant()
  if (-not $name.EndsWith('.exe')) { $name += '.exe' }
  if ($name -eq 'powershell.exe') {
    return [IO.Path]::Combine($wimforgePowerShell, 'powershell.exe')
  }
  if ($name -in @('dism.exe','reg.exe','robocopy.exe')) {
    return [IO.Path]::Combine($wimforgeSystem32, $name)
  }
  if ($name -eq 'oscdimg.exe') {
    $relative = [IO.Path]::Combine('Windows Kits','10','Assessment and Deployment Kit','Deployment Tools','amd64','Oscdimg','oscdimg.exe')
    $roots = @(
      [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86),
      [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
    )
    foreach ($root in $roots) {
      if ([String]::IsNullOrWhiteSpace($root)) { continue }
      $candidate = [IO.Path]::Combine($root, $relative)
      if (Test-WimForgeProtectedFile $candidate $root) { return [IO.Path]::GetFullPath($candidate) }
    }
    throw 'Oscdimg was not found in the protected Windows ADK amd64 deployment-tools location.'
  }
  throw "Untrusted relative executable in servicing plan: $Exe"
}

)PS");
    script += QStringLiteral("function Invoke-WimForgeStep([string]$Id,[string]$Exe,[string[]]$Args) {\r\n");
    script += QStringLiteral("  Write-Host ('[{0}] {1} {2}' -f $Id,$Exe,($Args -join ' ')) -ForegroundColor Cyan\r\n");
    script += QStringLiteral("  $resolvedExe = Resolve-WimForgeExecutable $Exe\r\n");
    script += QStringLiteral("  if ($WhatIfPlan) { return }\r\n");
    script += QStringLiteral("  & $resolvedExe @Args\r\n");
    script += QStringLiteral("  if ($LASTEXITCODE -ne 0) { throw \"Step $Id failed with exit code $LASTEXITCODE\" }\r\n");
    script += QStringLiteral("}\r\n\r\n");

    for (const int index : std::as_const(orderedIndices)) {
        const ServicingOperation &item = operations.at(index);
        if (item.state == OperationState::Skipped)
            continue;
        script += QStringLiteral("# %1 | %2\r\n")
                      .arg(powerShellCommentText(item.titleEn), powerShellCommentText(item.titleZh));
        script += QStringLiteral("# ReviewState: %1\r\n").arg(operationStateName(item.state));
        script += QStringLiteral("# DependsOn: %1\r\n")
                      .arg(powerShellCommentText(item.dependsOn.join(QStringLiteral(", "))));
        script += QStringLiteral("# Safety: requiresAdministrator=%1; destructive=%2; checkpointBefore=%3; "
                                 "writesMountedImage=%4; writesMediaWorkspace=%5; mayRunInParallel=%6\r\n")
                      .arg(boolText(item.requiresAdministrator), boolText(item.destructive),
                           boolText(item.checkpointBefore), boolText(item.writesMountedImage),
                           boolText(item.writesMediaWorkspace), boolText(item.mayRunInParallel));
        if (!item.metadata.isEmpty()) {
            script += QStringLiteral("# Metadata: %1\r\n")
                          .arg(powerShellCommentText(QString::fromUtf8(
                              QJsonDocument(item.metadata).toJson(QJsonDocument::Compact))));
        }
        QStringList quoted;
        for (const QString &argument : item.arguments)
            quoted.append(quotePowerShellLiteral(argument));
        script += QStringLiteral("Invoke-WimForgeStep %1 %2 @(%3)\r\n\r\n")
                      .arg(quotePowerShellLiteral(item.id), quotePowerShellLiteral(item.executable),
                           quoted.join(QStringLiteral(", ")));
    }

    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QString(QChar(0xFEFF)).toUtf8() + script.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (error)
        error->clear();
    return true;
}

} // namespace wimforge
