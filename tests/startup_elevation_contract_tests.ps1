[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ExecutablePath,

    [Parameter(Mandatory)]
    [string] $ManifestTool,

    [ValidateSet('asInvoker', 'highestAvailable', 'requireAdministrator')]
    [string] $ExpectedLevel = 'requireAdministrator'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$executable = [IO.Path]::GetFullPath($ExecutablePath)
$mt = [IO.Path]::GetFullPath($ManifestTool)
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "WimForge executable was not found: $executable"
}
if (-not (Test-Path -LiteralPath $mt -PathType Leaf)) {
    throw "Windows manifest tool was not found: $mt"
}

$manifestPath = Join-Path ([IO.Path]::GetTempPath()) `
    ("wimforge-manifest-{0}.xml" -f [guid]::NewGuid().ToString('N'))
try {
    $output = & $mt -nologo "-inputresource:$executable;#1" "-out:$manifestPath" 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not extract manifest resource #1 from $executable`n$($output -join [Environment]::NewLine)"
    }
    [xml] $manifest = [IO.File]::ReadAllText($manifestPath)
    $nodes = @($manifest.SelectNodes("//*[local-name()='requestedExecutionLevel']"))
    if ($nodes.Count -ne 1) {
        throw "Expected exactly one requestedExecutionLevel in $executable; found $($nodes.Count)."
    }
    $actualLevel = $nodes[0].GetAttribute('level')
    if ($actualLevel -cne $ExpectedLevel) {
        throw "WimForge elevation manifest is '$actualLevel'; expected '$ExpectedLevel': $executable"
    }
    Write-Host "Verified WimForge manifest elevation level: $actualLevel"
} finally {
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        Remove-Item -LiteralPath $manifestPath -Force
    }
}
