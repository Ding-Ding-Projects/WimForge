[CmdletBinding()]
param(
    [string] $BuildDirectory = 'build-capture/Debug',
    [string] $OutputDirectory = 'docs/screenshots',
    [ValidateSet('en', 'zh-HK', 'bilingual')]
    [string] $Language = 'bilingual'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDirectory))
$outputPath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputDirectory))
$executable = Join-Path $buildPath 'WimForge.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build WimForge first. The desktop executable was not found at $executable"
}

# A developer build intentionally does not copy the Qt runtime beside the
# executable. Resolve the Qt kit recorded by that build so Debug captures can
# find Qt6Guid.dll and Release captures can find Qt6Gui.dll on a clean shell.
$cmakeBuildPath = Split-Path -Parent $buildPath
$cachePath = Join-Path $cmakeBuildPath 'CMakeCache.txt'
$qtRoot = $env:QT_ROOT_DIR
if ([string]::IsNullOrWhiteSpace($qtRoot) -and (Test-Path -LiteralPath $cachePath)) {
    $qtDirectoryLine = Get-Content -LiteralPath $cachePath |
        Where-Object { $_ -match '^Qt6_DIR:[^=]*=' } |
        Select-Object -First 1
    if ($qtDirectoryLine) {
        $qtDirectory = ($qtDirectoryLine -split '=', 2)[1]
        $qtRoot = [System.IO.Path]::GetFullPath((Join-Path $qtDirectory '../../..'))
    }
}
$qtBin = if ([string]::IsNullOrWhiteSpace($qtRoot)) { $null } else { Join-Path $qtRoot 'bin' }
if (-not $qtBin -or -not (Test-Path -LiteralPath (Join-Path $qtBin 'Qt6Core.dll') -PathType Leaf)) {
    throw 'Unable to locate the Qt runtime. Configure CMake first or set QT_ROOT_DIR.'
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

$pages = [ordered] @{
    'project-start' = 'project-start.png'
    overview = 'overview.png'; source = 'source.png'; customize = 'customize.png'
    gpo = 'group-policy.png'; unattended = 'unattended.png'; packages = 'package-studio.png'
    winforge = 'winforge-bridge.png'; vmlab = 'virtual-machine-lab.png'
    plan = 'review-run.png'; history = 'history.png'; settings = 'settings.png'
    terminal = 'embedded-terminal.png'
}
$originalEnvironment = @{
    TEMP = $env:TEMP; TMP = $env:TMP; QT_SCALE_FACTOR = $env:QT_SCALE_FACTOR
    QT_SCREEN_SCALE_FACTORS = $env:QT_SCREEN_SCALE_FACTORS
    QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM
    QT_ENABLE_HIGHDPI_SCALING = $env:QT_ENABLE_HIGHDPI_SCALING
    QT_AUTO_SCREEN_SCALE_FACTOR = $env:QT_AUTO_SCREEN_SCALE_FACTOR
    LOCALAPPDATA = $env:LOCALAPPDATA; APPDATA = $env:APPDATA
    WIMFORGE_NOTIFICATION_STORE = $env:WIMFORGE_NOTIFICATION_STORE
    PATH = $env:PATH
}

try {
    $publicRoot = if ([string]::IsNullOrWhiteSpace($env:PUBLIC)) {
        [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonDocuments)
    } else { Join-Path $env:PUBLIC 'Documents' }
    $neutralRoot = [System.IO.Path]::GetFullPath((Join-Path $publicRoot 'WimForge-Screenshot'))
    New-Item -ItemType Directory -Path $neutralRoot -Force | Out-Null
    # The desktop manifest is PerMonitorV2-aware. On a 150% monitor Windows can
    # otherwise constrain the hidden 1,440x900 logical window to a 960x600
    # work area and leave the captured PNG padded. Disable platform DPI
    # virtualization for this documentation-only process so the requested
    # logical viewport maps one-to-one to the PNG while retaining Windows font
    # rendering (the offscreen plugin does not provide the production fonts).
    $env:QT_QPA_PLATFORM = 'windows:dpiawareness=0'
    $env:QT_ENABLE_HIGHDPI_SCALING = '0'
    $env:QT_AUTO_SCREEN_SCALE_FACTOR = '0'
    $env:QT_SCALE_FACTOR = '1'; $env:QT_SCREEN_SCALE_FACTORS = '1'
    $env:PATH = $qtBin + [System.IO.Path]::PathSeparator + $env:PATH

    foreach ($entry in $pages.GetEnumerator()) {
        $routeRoot = [System.IO.Path]::GetFullPath((Join-Path $neutralRoot $entry.Key))
        if (-not $routeRoot.StartsWith($neutralRoot + [System.IO.Path]::DirectorySeparatorChar,
                                       [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Unsafe screenshot fixture path: $routeRoot"
        }
        if (Test-Path -LiteralPath $routeRoot) {
            Remove-Item -LiteralPath $routeRoot -Recurse -Force
        }
        $routeTemp = Join-Path $routeRoot 'Temp'
        $routeLocal = Join-Path $routeRoot 'LocalAppData'
        $routeRoaming = Join-Path $routeRoot 'AppData'
        New-Item -ItemType Directory -Path $routeTemp, $routeLocal, $routeRoaming -Force | Out-Null
        $env:TEMP = $routeTemp; $env:TMP = $routeTemp
        $env:LOCALAPPDATA = $routeLocal; $env:APPDATA = $routeRoaming
        $env:WIMFORGE_NOTIFICATION_STORE = Join-Path $routeRoot 'NotificationStore'

        $destination = Join-Path $outputPath $entry.Value
        $arguments = if ($entry.Key -eq 'project-start') {
            @('--project-start', '--language', $Language, '--screenshot', $destination)
        } else {
            @('--demo', '--language', $Language, '--page', $entry.Key,
              '--screenshot', $destination)
        }
        $process = Start-Process -FilePath $executable `
            -ArgumentList $arguments `
            -Wait -PassThru -WindowStyle Hidden
        if ($process.ExitCode -ne 0) {
            throw "Screenshot capture failed for route '$($entry.Key)' with exit code $($process.ExitCode)"
        }
        if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
            throw "Screenshot capture did not create $destination"
        }
    }
}
finally {
    foreach ($name in $originalEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], 'Process')
    }
}

Add-Type -AssemblyName System.Drawing
$captures = foreach ($entry in $pages.GetEnumerator()) {
    $path = Join-Path $outputPath $entry.Value
    $image = [System.Drawing.Image]::FromFile($path)
    try {
        [pscustomobject] @{ Route = $entry.Key; File = $entry.Value; Width = $image.Width
            Height = $image.Height; Bytes = (Get-Item -LiteralPath $path).Length }
    } finally { $image.Dispose() }
}
$dimensions = @($captures | ForEach-Object { "$($_.Width)x$($_.Height)" } | Sort-Object -Unique)
if ($dimensions.Count -ne 1) {
    throw "Documentation screenshots have inconsistent dimensions: $($dimensions -join ', ')"
}
$captures | Format-Table -AutoSize
