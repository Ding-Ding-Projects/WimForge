param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerScript
)

$ErrorActionPreference = 'Stop'

$resolvedScript = (Resolve-Path -LiteralPath $InstallerScript).Path
$source = Get-Content -LiteralPath $resolvedScript -Raw

$codeMatch = [regex]::Match(
    $source,
    '(?ms)^\[Code\]\s*(?<code>.*)\z')
if (-not $codeMatch.Success) {
    throw 'installer script has no terminal [Code] section'
}
$code = $codeMatch.Groups['code'].Value

$expectedKey =
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{D72458D7-6214-43E9-8F65-58E046A08F14}_is1'

if ($source -notmatch '(?m)^AppId=\{\{D72458D7-6214-43E9-8F65-58E046A08F14\}\s*$') {
    throw 'installer AppId changed from the published legacy identity'
}
if ($code -notlike "*$expectedKey*") {
    throw 'legacy migration does not check the exact published HKCU uninstall key'
}
if ($code -notmatch 'RegKeyExists\(HKCU,\s*LegacyPerUserUninstallKey\)') {
    throw 'legacy migration must inspect the current user HKCU registration'
}
if ($code -notmatch '(?ms)if\s+not\s+RegKeyExists\(.+?then\s+Exit;.+?Result\s*:=\s*False;') {
    throw 'legacy migration must fail closed when the legacy registration exists'
}
if ($code -notmatch '(?i)Settings\s*>\s*Apps\s*>\s*Installed apps') {
    throw 'legacy migration must give a safe manual-uninstall instruction'
}
if ($code -notmatch '\bSuppressibleMsgBox\s*\(') {
    throw 'legacy migration must not block suppressed silent setup with a message box'
}

$forbiddenElevatedActions = @(
    '\bExec\s*\(',
    '\bShellExec\s*\(',
    '\bUninstallString\b',
    '\bDeleteFile\s*\(',
    '\bDelTree\s*\(',
    '\bRemoveDir\s*\(',
    '\bRegDeleteKeyIncludingSubkeys\s*\(',
    '\bRegDeleteKeyIfEmpty\s*\(',
    '\bRegDeleteValue\s*\('
)
foreach ($pattern in $forbiddenElevatedActions) {
    if ($code -match $pattern) {
        throw "unsafe elevated legacy-migration action found: $pattern"
    }
}

Write-Host 'Installer migration contract passed.'
