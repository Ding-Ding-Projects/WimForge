#include "ImageSourceInspector.h"
#include "ProcessLaunch.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace wimforge {
namespace {

const QStringList kImageSuffixes{
    QStringLiteral("wim"),
    QStringLiteral("esd"),
    QStringLiteral("swm"),
};

QString discoverMediaImage(const QString &mediaPath)
{
    const QDir media(mediaPath);
    for (const QString &suffix : kImageSuffixes) {
        const QString candidate = media.filePath(
            QStringLiteral("sources/install.%1").arg(suffix));
        if (QFileInfo(candidate).isFile())
            return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

} // namespace

ImageInspectionCommand ImageSourceInspector::commandFor(
    const QString &sourcePath,
    const QString &configuredImagePath)
{
    ImageInspectionCommand command;
    const QFileInfo source(QDir::cleanPath(sourcePath.trimmed()));
    if (sourcePath.trimmed().isEmpty()) {
        command.error = QStringLiteral("Choose a Windows ISO, media folder, WIM, ESD, or SWM first.");
        return command;
    }
    if (!source.exists()) {
        command.error = QStringLiteral("Source does not exist: %1").arg(source.filePath());
        return command;
    }

    QString image;
    if (source.isFile() && kImageSuffixes.contains(source.suffix().toLower())) {
        image = source.absoluteFilePath();
    } else if (source.isDir()) {
        image = discoverMediaImage(source.absoluteFilePath());
        if (image.isEmpty()) {
            command.error = QStringLiteral(
                "The selected folder does not contain sources\\install.wim, install.esd, or install.swm.");
            return command;
        }
    } else if (source.isFile()
               && source.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) == 0) {
        command.program = resolveExecutableForLaunch(QStringLiteral("powershell.exe"));
        command.arguments = {
            QStringLiteral("-NoLogo"),
            QStringLiteral("-NoProfile"),
            QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            isoInspectionScript(),
        };
        command.environment = sanitizedPowerShellEnvironment();
        command.environment.insert(QStringLiteral("WIMFORGE_ISO_PATH"),
                                   source.absoluteFilePath());
        command.isoSource = true;
        command.utf8Output = true;
        return command;
    } else {
        const QFileInfo configured(QDir::cleanPath(configuredImagePath.trimmed()));
        if (!configuredImagePath.trimmed().isEmpty() && configured.isFile()
            && kImageSuffixes.contains(configured.suffix().toLower())) {
            image = configured.absoluteFilePath();
        } else {
            command.error = QStringLiteral(
                "Source must be an ISO, a Windows installation-media folder, or a WIM/ESD/SWM image.");
            return command;
        }
    }

    command.program = resolveExecutableForLaunch(QStringLiteral("dism.exe"));
    command.arguments = {
        QStringLiteral("/English"),
        QStringLiteral("/Get-WimInfo"),
        QStringLiteral("/WimFile:%1").arg(image),
    };
    command.environment = QProcessEnvironment::systemEnvironment();
    command.imagePath = image;
    return command;
}

ImageInspectionResult ImageSourceInspector::parseOutput(
    const QByteArray &bytes,
    bool isoSource,
    bool utf8Output)
{
    ImageInspectionResult result;
    result.output = utf8Output ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);

    if (isoSource) {
        const QRegularExpression mountedPath(
            QStringLiteral(R"((?:^|[\r\n])WIMFORGE_IMAGE_PATH::([^\r\n]+))"));
        const QRegularExpressionMatch pathMatch = mountedPath.match(result.output);
        if (pathMatch.hasMatch()) {
            QString normalizedPath = pathMatch.captured(1).trimmed();
            // The PowerShell producer always reports a Windows path, even when
            // this parser is compiled and tested on another host OS.
            normalizedPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
            result.mountedImagePath = QDir::cleanPath(normalizedPath);
            const QRegularExpression driveRoot(QStringLiteral(R"(^[A-Za-z]:[\\/](.+)$)"));
            const QRegularExpressionMatch relativeMatch = driveRoot.match(result.mountedImagePath);
            if (relativeMatch.hasMatch())
                result.relativeImagePath = QDir::cleanPath(relativeMatch.captured(1));
        }
    }

    const QRegularExpression editionBlock(
        QStringLiteral(R"(Index\s*:\s*(\d+)[\s\S]*?Name\s*:\s*([^\r\n]+))"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = editionBlock.globalMatch(result.output);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result.editions.append(QStringLiteral("Index %1 — %2")
                                   .arg(match.captured(1), match.captured(2).trimmed()));
    }
    return result;
}

QString ImageSourceInspector::isoInspectionScript()
{
    return QStringLiteral(R"PS(
$ErrorActionPreference = 'Stop'
$wimforgeSystem32 = [Environment]::SystemDirectory
$wimforgePowerShell = [IO.Path]::Combine($wimforgeSystem32, 'WindowsPowerShell', 'v1.0')
$env:PATH = $wimforgeSystem32 + ';' + $wimforgePowerShell
$env:PSModulePath = [IO.Path]::Combine($wimforgePowerShell, 'Modules')
$wimforgeManagementModule = [IO.Path]::Combine($env:PSModulePath, 'Microsoft.PowerShell.Management', 'Microsoft.PowerShell.Management.psd1')
$wimforgeUtilityModule = [IO.Path]::Combine($env:PSModulePath, 'Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Utility.psd1')
Microsoft.PowerShell.Core\Import-Module -Name $wimforgeManagementModule -Force -ErrorAction Stop
Microsoft.PowerShell.Core\Import-Module -Name $wimforgeUtilityModule -Force -ErrorAction Stop
$PSModuleAutoLoadingPreference = 'None'
$wimforgeStorageModule = [IO.Path]::Combine($env:PSModulePath, 'Storage', 'Storage.psd1')
Microsoft.PowerShell.Core\Import-Module -Name $wimforgeStorageModule -Force -ErrorAction Stop
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$isoPath = [Environment]::GetEnvironmentVariable('WIMFORGE_ISO_PATH')
$mounted = $false
$exitCode = 0
try {
    if ([String]::IsNullOrWhiteSpace($isoPath)) { throw 'The ISO path was not supplied.' }
    $disk = Storage\Mount-DiskImage -ImagePath $isoPath -Access ReadOnly -PassThru -ErrorAction Stop
    $mounted = $true
    $imagePath = $null
    foreach ($volume in @($disk | Storage\Get-Volume -ErrorAction Stop)) {
        if (-not $volume.DriveLetter) { continue }
        $root = "$($volume.DriveLetter):\"
        foreach ($name in @('install.wim', 'install.esd', 'install.swm')) {
            $candidate = Join-Path $root ("sources\" + $name)
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $imagePath = $candidate
                break
            }
        }
        if ($imagePath) { break }
    }
    if (-not $imagePath) {
        throw 'The mounted ISO does not contain sources\install.wim, install.esd, or install.swm.'
    }
    [Console]::Out.WriteLine('WIMFORGE_IMAGE_PATH::' + $imagePath)
    & ([IO.Path]::Combine($wimforgeSystem32, 'dism.exe')) /English /Get-WimInfo "/WimFile:$imagePath"
    $exitCode = $LASTEXITCODE
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    $exitCode = 1
} finally {
    if ($mounted) {
        try {
            Storage\Dismount-DiskImage -ImagePath $isoPath -ErrorAction Stop | Out-Null
            $detachDeadline = [DateTime]::UtcNow.AddSeconds(10)
            do {
                Microsoft.PowerShell.Utility\Start-Sleep -Milliseconds 200
                $stillAttached = (Storage\Get-DiskImage -ImagePath $isoPath -ErrorAction Stop).Attached
            } while ($stillAttached -and [DateTime]::UtcNow -lt $detachDeadline)
            if ($stillAttached) { throw 'Windows did not confirm that the ISO was detached.' }
        } catch {
            [Console]::Error.WriteLine('The ISO was inspected, but could not be dismounted: ' + $_.Exception.Message)
            if ($exitCode -eq 0) { $exitCode = 1 }
        }
    }
}
exit $exitCode
)PS");
}

} // namespace wimforge
