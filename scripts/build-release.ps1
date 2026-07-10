[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$')]
    [string] $Version,

    [string] $BuildDirectory = 'build/release',

    [string] $OutputDirectory = 'dist',

    [string] $InnoCompiler,

    [switch] $SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$versionParts = @($Version.Split('.') | ForEach-Object { [int] $_ })
if (@($versionParts | Where-Object { $_ -gt 65535 }).Count -ne 0) {
    throw "Each Windows version component must be between 0 and 65535: $Version"
}

function Resolve-RepositoryPath {
    param([Parameter(Mandatory)][string] $Path)

    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }

    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Assert-SafeOutputPath {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Label
    )

    $rootPrefix = $repoRoot.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
    if (-not $Path.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must be inside the repository: $Path"
    }
}

function Find-Application {
    param([Parameter(Mandatory)][string] $Name)

    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $command) {
        throw "Required tool was not found on PATH: $Name"
    }

    return $command.Source
}

function Initialize-MsvcEnvironment {
    if (Get-Command 'cl.exe' -CommandType Application -ErrorAction SilentlyContinue) {
        return
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'MSVC is not active and vswhere.exe was not found. Install Visual Studio 2022 C++ Build Tools.'
    }
    $installationPath = (& $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if (-not $installationPath) {
        throw 'Visual Studio 2022 with the x64 C++ toolchain was not found.'
    }
    $vcvars = Join-Path $installationPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
        throw "MSVC environment script was not found: $vcvars"
    }

    $environmentLines = & $env:ComSpec /d /s /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not initialize the Visual Studio x64 build environment.'
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 1)
            Set-Item -Path "Env:$name" -Value $value
        }
    }
    if (-not (Get-Command 'cl.exe' -CommandType Application -ErrorAction SilentlyContinue)) {
        throw 'The Visual Studio environment initialized without exposing cl.exe.'
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory)][string] $FilePath,
        [Parameter(Mandatory)][string[]] $ArgumentList
    )

    Write-Host "> $FilePath $($ArgumentList -join ' ')"
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

$buildPath = Resolve-RepositoryPath $BuildDirectory
$outputPath = Resolve-RepositoryPath $OutputDirectory
Assert-SafeOutputPath -Path $buildPath -Label 'BuildDirectory'
Assert-SafeOutputPath -Path $outputPath -Label 'OutputDirectory'

if ($buildPath.Equals($outputPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory and OutputDirectory must be different.'
}
$buildPrefix = $buildPath.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
$outputPrefix = $outputPath.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
if ($buildPath.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    $outputPath.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory and OutputDirectory must not contain one another.'
}

$requiredDocuments = @('README.md', 'LICENSE')
foreach ($documentName in $requiredDocuments) {
    $documentPath = Join-Path $repoRoot $documentName
    if (-not (Test-Path -LiteralPath $documentPath -PathType Leaf)) {
        throw "Required release document is missing: $documentPath"
    }
}

$cmake = Find-Application 'cmake.exe'
$null = Find-Application 'ninja.exe'
$git = Find-Application 'git.exe'
Initialize-MsvcEnvironment
if ($env:QT_ROOT_DIR) {
    $qtRoot = [IO.Path]::GetFullPath($env:QT_ROOT_DIR)
    $windeployqt = Join-Path $qtRoot 'bin/windeployqt.exe'
    if (-not (Test-Path -LiteralPath $windeployqt -PathType Leaf)) {
        throw "QT_ROOT_DIR does not contain windeployqt.exe: $qtRoot"
    }
} else {
    $qtRoot = $null
    $windeployqt = Find-Application 'windeployqt.exe'
}

if ($InnoCompiler) {
    $iscc = Resolve-RepositoryPath $InnoCompiler
    if (-not (Test-Path -LiteralPath $iscc -PathType Leaf)) {
        throw "Inno Setup compiler was not found: $iscc"
    }
} else {
    $isccCommand = Get-Command 'iscc.exe' -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($isccCommand) {
        $iscc = $isccCommand.Source
    } else {
        $isccCandidates = @(
            "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
        ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
        $iscc = $isccCandidates | Select-Object -First 1
    }

    if (-not $iscc) {
        throw 'Inno Setup 6 compiler (ISCC.exe) was not found.'
    }
}

foreach ($path in @($buildPath, $outputPath)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $path -Force | Out-Null
}

$configureArguments = @(
    '-S', $repoRoot,
    '-B', $buildPath,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DBUILD_TESTING=ON',
    "-DWIMFORGE_BUILD_VERSION=$Version"
)
if ($qtRoot) {
    $configureArguments += "-DCMAKE_PREFIX_PATH=$qtRoot"
}

Invoke-NativeCommand -FilePath $cmake -ArgumentList $configureArguments
Invoke-NativeCommand -FilePath $cmake -ArgumentList @(
    '--build', $buildPath,
    '--config', 'Release',
    '--parallel'
)

$ctestFile = Join-Path $buildPath 'CTestTestfile.cmake'
if (-not $SkipTests) {
    if (-not (Test-Path -LiteralPath $ctestFile -PathType Leaf)) {
        throw "CTest was enabled for a release build, but its test file is missing: $ctestFile"
    }
    $ctest = Find-Application 'ctest.exe'
    $testInventoryJson = (& $ctest --test-dir $buildPath --build-config Release --show-only=json-v1) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to enumerate registered release tests.'
    }
    $testInventory = $testInventoryJson | ConvertFrom-Json
    if (@($testInventory.tests).Count -eq 0) {
        throw 'The release build has no registered tests.'
    }
    Invoke-NativeCommand -FilePath $ctest -ArgumentList @(
        '--test-dir', $buildPath,
        '--build-config', 'Release',
        '--output-on-failure'
    )
}

$executableCandidates = @(
    Get-ChildItem -LiteralPath $buildPath -Recurse -File -Filter 'WimForge.exe' |
        Where-Object { $_.FullName -notmatch '[\\/]CMakeFiles[\\/]' }
)
if ($executableCandidates.Count -ne 1) {
    $found = ($executableCandidates.FullName -join ', ')
    throw "Expected exactly one built WimForge.exe, found $($executableCandidates.Count). $found"
}

$builtExecutable = $executableCandidates[0]
$builtCli = Join-Path $builtExecutable.DirectoryName 'WimForgeCli.exe'
if (-not (Test-Path -LiteralPath $builtCli -PathType Leaf)) {
    throw "The release build did not produce the companion CLI: $builtCli"
}
$portableName = "WimForge-portable-x64-$Version"
$portablePath = Join-Path $outputPath $portableName
New-Item -ItemType Directory -Path $portablePath -Force | Out-Null

# Copy only the two product executables. CTest binaries can share Ninja's root
# output directory, so copying every .exe would silently ship the test suite.
# windeployqt adds the complete Qt/MSVC runtime after this explicit allowlist.
Copy-Item -LiteralPath $builtExecutable.FullName -Destination $portablePath -Force
Copy-Item -LiteralPath $builtCli -Destination $portablePath -Force

$portableExecutable = Join-Path $portablePath 'WimForge.exe'
if (-not (Test-Path -LiteralPath $portableExecutable -PathType Leaf)) {
    throw "Portable staging failed to copy WimForge.exe: $portableExecutable"
}

$qmlSource = Join-Path $repoRoot 'qml'
if (-not (Test-Path -LiteralPath $qmlSource -PathType Container)) {
    $qmlSource = $repoRoot
}
Invoke-NativeCommand -FilePath $windeployqt -ArgumentList @(
    '--release',
    '--compiler-runtime',
    '--qmldir', $qmlSource,
    $portableExecutable
)

# A shared Qt build depends on the MSVC runtime as well as Qt. windeployqt
# normally deploys the compiler runtime, but copying the redistributable DLLs
# from the active VS toolchain is a deterministic fallback for the portable
# package (which cannot assume that an installer has run first).
$compilerRuntime = Join-Path $portablePath 'vcruntime140.dll'
if (-not (Test-Path -LiteralPath $compilerRuntime -PathType Leaf) -and $env:VCToolsRedistDir) {
    $redistDirectory = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
    if (Test-Path -LiteralPath $redistDirectory -PathType Container) {
        Get-ChildItem -LiteralPath $redistDirectory -File -Filter '*.dll' |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $portablePath -Force }
    }
}

$requiredRuntimeFiles = @(
    (Join-Path $portablePath 'Qt6Core.dll'),
    (Join-Path $portablePath 'Qt6Gui.dll'),
    (Join-Path $portablePath 'Qt6Qml.dll'),
    (Join-Path $portablePath 'Qt6Quick.dll'),
    (Join-Path $portablePath 'Qt6QuickControls2.dll'),
    $compilerRuntime,
    (Join-Path $portablePath 'platforms/qwindows.dll')
)
foreach ($runtimeFile in $requiredRuntimeFiles) {
    if (-not (Test-Path -LiteralPath $runtimeFile -PathType Leaf)) {
        throw "windeployqt did not produce required runtime file: $runtimeFile"
    }
}

# windeployqt may add the interactive VC redistributable installer even after
# deploying the app-local CRT DLLs. A portable bundle must not launch or ship a
# second installer, so remove it only after the required DLL check succeeds.
$bundledRedistributable = Join-Path $portablePath 'vc_redist.x64.exe'
if (Test-Path -LiteralPath $bundledRedistributable -PathType Leaf) {
    Remove-Item -LiteralPath $bundledRedistributable -Force
}

$unexpectedTopLevelExecutables = @(Get-ChildItem -LiteralPath $portablePath -File -Filter '*.exe' |
    Where-Object { $_.Name -notin @('WimForge.exe', 'WimForgeCli.exe') })
if ($unexpectedTopLevelExecutables.Count -ne 0) {
    throw "Portable staging contains unexpected executables: $($unexpectedTopLevelExecutables.Name -join ', ')"
}

$deployedQmlPlugins = @(Get-ChildItem -LiteralPath (Join-Path $portablePath 'qml') `
    -Recurse -File -Filter '*.dll' -ErrorAction SilentlyContinue)
if ($deployedQmlPlugins.Count -eq 0) {
    throw 'windeployqt did not deploy any QML module plugins.'
}

foreach ($documentName in $requiredDocuments) {
    $documentPath = Join-Path $repoRoot $documentName
    Copy-Item -LiteralPath $documentPath -Destination $portablePath -Force
}

$embeddedVersion = (Get-Item -LiteralPath $portableExecutable).VersionInfo.ProductVersion.Trim()
if ($embeddedVersion -notin @($Version, "$Version.0")) {
    throw "WimForge.exe version stamp is '$embeddedVersion'; expected '$Version'."
}

$commit = (& $git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to identify the source commit.'
}
$buildInformation = [ordered]@{
    product = 'WimForge'
    version = $Version
    architecture = 'x64'
    configuration = 'Release'
    commit = $commit
}
$buildInformation | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $portablePath 'build-info.json') -Encoding utf8

$portableArchive = Join-Path $outputPath "$portableName.zip"
Compress-Archive -Path (Join-Path $portablePath '*') -DestinationPath $portableArchive -CompressionLevel Optimal -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($portableArchive)
try {
    $zipEntries = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    $requiredZipEntries = @(
        'WimForge.exe',
        'WimForgeCli.exe',
        'README.md',
        'LICENSE',
        'build-info.json',
        'Qt6Core.dll',
        'Qt6Gui.dll',
        'Qt6Qml.dll',
        'Qt6Quick.dll',
        'Qt6QuickControls2.dll',
        'platforms/qwindows.dll'
    )
    foreach ($entryName in $requiredZipEntries) {
        if ($entryName -notin $zipEntries) {
            throw "Portable archive is missing required entry: $entryName"
        }
    }
    $topLevelExecutables = @($zipEntries | Where-Object {
        $_ -notmatch '/' -and $_.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)
    })
    if (@($topLevelExecutables | Where-Object {
        $_ -notin @('WimForge.exe', 'WimForgeCli.exe')
    }).Count -ne 0) {
        throw "Portable archive contains unexpected executables: $($topLevelExecutables -join ', ')"
    }
} finally {
    $zip.Dispose()
}

$installerScript = Join-Path $repoRoot 'installer/WimForge.iss'
Invoke-NativeCommand -FilePath $iscc -ArgumentList @(
    "/DMyAppVersion=$Version",
    "/DMySourceDir=$portablePath",
    "/DMyOutputDir=$outputPath",
    $installerScript
)

$installerPath = Join-Path $outputPath "WimForge-Setup-x64-$Version.exe"
foreach ($releaseFile in @($portableArchive, $installerPath)) {
    if (-not (Test-Path -LiteralPath $releaseFile -PathType Leaf)) {
        throw "Release file was not produced: $releaseFile"
    }
    if ((Get-Item -LiteralPath $releaseFile).Length -le 0) {
        throw "Release file is empty: $releaseFile"
    }
}

$installerVersion = (Get-Item -LiteralPath $installerPath).VersionInfo.ProductVersion.Trim()
if ($installerVersion -notin @($Version, "$Version.0")) {
    throw "Installer version stamp is '$installerVersion'; expected '$Version'."
}

$expectedOutputFiles = @(
    "WimForge-Setup-x64-$Version.exe",
    "WimForge-portable-x64-$Version.zip"
)
$unexpectedOutputFiles = @(Get-ChildItem -LiteralPath $outputPath -File |
    Where-Object { $_.Name -notin $expectedOutputFiles })
if ($unexpectedOutputFiles.Count -ne 0) {
    throw "Unexpected top-level release files were produced: $($unexpectedOutputFiles.Name -join ', ')"
}

Write-Host 'Release package complete:'
Write-Host "  $installerPath"
Write-Host "  $portableArchive"
