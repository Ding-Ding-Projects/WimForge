[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$scriptPath = Join-Path $repoRoot 'scripts\bootstrap-build.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'wimforge-bootstrap-tests-{0}' -f [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

try {
    $tokens = $null
    $parseErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile(
        $scriptPath, [ref]$tokens, [ref]$parseErrors)
    if (@($parseErrors).Count -ne 0) {
        $messages = @($parseErrors | ForEach-Object { $_.Message }) -join '; '
        throw "bootstrap-build.ps1 has parser errors: $messages"
    }

    $planLog = Join-Path $testRoot 'existing-checkout-plan.log'
    $planOutput = @(& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
        -File $scriptPath -Plan -RepositoryPath $repoRoot -LogPath $planLog 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Existing-checkout plan failed: $($planOutput -join [Environment]::NewLine)"
    }
    $planText = $planOutput -join "`n"
    foreach ($expected in @(
        'WimForge bootstrap plan (no changes executed)',
        'Elevation: skipped because -Plan was supplied',
        'Git.Git',
        'Kitware.CMake',
        'Ninja-build.Ninja',
        'Microsoft.VisualStudio.2022.BuildTools',
        'x64 Windows SDK',
        'miurahr.aqtinstall',
        'JRSoftware.InnoSetup',
        'Normal-token user package plan:',
        'Qt selected:',
        'External limits: UAC consent, WinGet/App Installer, network, vendor availability, and free disk space',
        'PLAN COMPLETE: no elevation, package install, clone, configure, build, or test was run.'
    )) {
        if ($planText -notmatch [Regex]::Escape($expected)) {
            throw "Plan output is missing: $expected"
        }
    }
    if (-not (Test-Path -LiteralPath $planLog -PathType Leaf)) {
        throw 'Plan mode did not write its deterministic transcript path.'
    }

    $firstLogLength = (Get-Item -LiteralPath $planLog).Length
    $repeatOutput = @(& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
        -File $scriptPath -Plan -RepositoryPath $repoRoot -LogPath $planLog 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Repeated plan failed: $($repeatOutput -join [Environment]::NewLine)"
    }
    $appendedLog = Get-Content -LiteralPath $planLog -Raw
    if ((Get-Item -LiteralPath $planLog).Length -le $firstLogLength -or
        ([Regex]::Matches($appendedLog, 'PLAN COMPLETE:')).Count -lt 2) {
        throw 'An explicit existing log was overwritten instead of preserved and appended.'
    }

    $autoPlanLog = Join-Path $testRoot 'auto-checkout-plan.log'
    Push-Location $repoRoot
    try {
        $autoOutput = @(& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
            -File $scriptPath -Plan -LogPath $autoPlanLog 2>&1)
    } finally {
        Pop-Location
    }
    if ($LASTEXITCODE -ne 0 -or
        ($autoOutput -join "`n") -notmatch 'Repository: use .+\[existing checkout\]') {
        throw 'Plan mode did not auto-detect the checkout containing the invocation.'
    }

    $integrityRepo = Join-Path $testRoot 'integrity-checkout'
    New-Item -ItemType Directory -Path $integrityRepo -Force | Out-Null
    $testGit = (Get-Command git.exe -CommandType Application -ErrorAction Stop).Source
    & $testGit -C $integrityRepo init --quiet
    Set-Content -LiteralPath (Join-Path $integrityRepo 'CMakeLists.txt') `
        -Value 'project(WimForge VERSION 1.2.3)' -Encoding Ascii
    & $testGit -C $integrityRepo add -- CMakeLists.txt
    & $testGit -c 'user.name=WimForge Bootstrap Test' `
        -c 'user.email=bootstrap-test@example.invalid' -C $integrityRepo `
        commit --quiet -m initial
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not initialize the temporary integrity checkout.'
    }
    $previousGitDir = $env:GIT_DIR
    try {
        $env:GIT_DIR = Join-Path $repoRoot '.git'
        $redirectedGitOutput = @(& powershell.exe -NoLogo -NoProfile `
            -ExecutionPolicy Bypass -File $scriptPath -Plan `
            -RepositoryPath $integrityRepo `
            -LogPath (Join-Path $testRoot 'redirected-git-plan.log') 2>&1)
    } finally {
        if ($null -eq $previousGitDir) {
            Remove-Item Env:\GIT_DIR -ErrorAction SilentlyContinue
        } else {
            $env:GIT_DIR = $previousGitDir
        }
    }
    if ($LASTEXITCODE -ne 0 -or
        ($redirectedGitOutput -join "`n") -notmatch `
            'Checkout integrity: clean and commit-identifiable') {
        throw 'Plan mode allowed GIT_DIR to redirect checks away from the explicit repository.'
    }
    & $testGit -C $integrityRepo config status.showUntrackedFiles no
    Set-Content -LiteralPath (Join-Path $integrityRepo 'hidden-untracked.txt') `
        -Value 'must be detected' -Encoding Ascii
    $hiddenUntrackedOutput = @(& powershell.exe -NoLogo -NoProfile `
        -ExecutionPolicy Bypass -File $scriptPath -Plan `
        -RepositoryPath $integrityRepo `
        -LogPath (Join-Path $testRoot 'hidden-untracked-plan.log') 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        ($hiddenUntrackedOutput -join "`n") -notmatch 'Checkout integrity: DIRTY') {
        throw 'Plan mode honored a Git setting that hid an untracked source file.'
    }
    Remove-Item -LiteralPath (Join-Path $integrityRepo 'hidden-untracked.txt') -Force
    & $testGit -C $integrityRepo update-index --assume-unchanged CMakeLists.txt
    $hiddenIndexOutput = @(& powershell.exe -NoLogo -NoProfile `
        -ExecutionPolicy Bypass -File $scriptPath -Plan `
        -RepositoryPath $integrityRepo `
        -LogPath (Join-Path $testRoot 'hidden-index-plan.log') 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        ($hiddenIndexOutput -join "`n") -notmatch 'Checkout integrity: DIRTY') {
        throw 'Plan mode did not detect an assume-unchanged index entry.'
    }

    $cloneTarget = Join-Path $testRoot "not cloned O'Brien"
    $clonePlanLog = Join-Path $testRoot 'clone-plan.log'
    $cloneOutput = @(& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
        -File $scriptPath -Plan -RepositoryPath $cloneTarget -LogPath $clonePlanLog 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Clone plan failed: $($cloneOutput -join [Environment]::NewLine)"
    }
    $cloneText = $cloneOutput -join "`n"
    if ($cloneText -notmatch 'clone https://github\.com/codingmachineedge/WimForge\.git') {
        throw 'Clone plan did not identify the pinned WimForge repository.'
    }
    if (Test-Path -LiteralPath $cloneTarget) {
        throw 'Plan mode created or cloned the planned repository target.'
    }

    $nonEmptyTarget = Join-Path $testRoot 'occupied-clone-target'
    New-Item -ItemType Directory -Path $nonEmptyTarget -Force | Out-Null
    $sentinel = Join-Path $nonEmptyTarget 'preserve-me.txt'
    Set-Content -LiteralPath $sentinel -Value 'do not delete' -Encoding Ascii
    $unsafeCloneOutput = @(& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
        -File $scriptPath -Plan -RepositoryPath $nonEmptyTarget `
        -LogPath (Join-Path $testRoot 'unsafe-clone-plan.log') 2>&1)
    if ($LASTEXITCODE -eq 0 -or
        ($unsafeCloneOutput -join "`n") -notmatch 'Refusing to clone into a non-empty directory' -or
        -not (Test-Path -LiteralPath $sentinel -PathType Leaf)) {
        throw 'Plan mode did not fail closed while preserving an occupied clone target.'
    }

    $junctionDestination = Join-Path $testRoot 'junction-destination'
    $junctionTarget = Join-Path $testRoot 'junction-clone-target'
    New-Item -ItemType Directory -Path $junctionDestination -Force | Out-Null
    New-Item -ItemType Junction -Path $junctionTarget -Target $junctionDestination | Out-Null
    $junctionOutput = @(& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
        -File $scriptPath -Plan -RepositoryPath $junctionTarget `
        -LogPath (Join-Path $testRoot 'junction-clone-plan.log') 2>&1)
    if ($LASTEXITCODE -eq 0 -or
        ($junctionOutput -join "`n") -notmatch 'Refusing a reparse-point path') {
        throw 'Plan mode did not reject a reparse-point clone target.'
    }

    $source = Get-Content -LiteralPath $scriptPath -Raw
    foreach ($safeElevationToken in @(
        'PSSerializer', 'Get-FileHash', '-EncodedCommand', 'Import-Clixml',
        'SystemDirectory', 'SetAccessRuleProtection', 'Bootstrap source changed during elevation handoff',
        'Get-AppxPackage', 'Get-AuthenticodeSignature'
    )) {
        if ($source -notmatch [Regex]::Escape($safeElevationToken)) {
            throw "Argument-safe elevation handoff is missing $safeElevationToken."
        }
    }

    $scriptAst = [Management.Automation.Language.Parser]::ParseFile(
        $scriptPath, [ref]$tokens, [ref]$parseErrors)
    $elevationFunction = $scriptAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Start-ElevatedCopy'
    }, $true)
    if (-not $elevationFunction) {
        throw 'Could not inspect the elevation function AST.'
    }
    $quotedScriptPath = "'$($scriptPath.Replace("'", "''"))'"
    $elevationFunctionText = $elevationFunction.Extent.Text.Replace(
        '$PSCommandPath', $quotedScriptPath)
    Invoke-Expression $elevationFunctionText
    $LogPath = Join-Path $testRoot 'handoff.log'
    $script:capturedElevationArguments = $null
    $script:capturedElevationFile = $null
    $script:capturedElevationWorkingDirectory = $null
    function Start-Process {
        param(
            [string] $FilePath,
            [string] $Verb,
            [switch] $Wait,
            [switch] $PassThru,
            [string] $WorkingDirectory,
            [object[]] $ArgumentList
        )
        $script:capturedElevationFile = $FilePath
        $script:capturedElevationWorkingDirectory = $WorkingDirectory
        $script:capturedElevationArguments = $ArgumentList
        return [pscustomobject]@{ ExitCode = 0 }
    }
    try {
        $null = Start-ElevatedCopy -PackageIds @('Git.Git', 'Kitware.CMake') `
            -WingetPath 'C:\Program Files\WindowsApps\trusted\winget.exe'
    } finally {
        Remove-Item Function:\Start-Process -Force
    }
    $expectedPowerShell = Join-Path ([Environment]::SystemDirectory) `
        'WindowsPowerShell\v1.0\powershell.exe'
    if ($script:capturedElevationFile -ne $expectedPowerShell) {
        throw 'Elevation did not use the absolute trusted Windows PowerShell path.'
    }
    $expectedWorkingDirectory = Split-Path -Parent ([Environment]::SystemDirectory)
    if ($script:capturedElevationWorkingDirectory -ne $expectedWorkingDirectory) {
        throw 'Elevation inherited an untrusted user-controlled working directory.'
    }
    $encodedIndex = [Array]::IndexOf(
        $script:capturedElevationArguments, '-EncodedCommand') + 1
    if ($encodedIndex -le 0 -or $encodedIndex -ge $script:capturedElevationArguments.Count) {
        throw 'Elevation did not produce an encoded handoff command.'
    }
    $handoffText = [Text.Encoding]::Unicode.GetString(
        [Convert]::FromBase64String($script:capturedElevationArguments[$encodedIndex]))
    $handoffTokens = $null
    $handoffErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseInput(
        $handoffText, [ref]$handoffTokens, [ref]$handoffErrors)
    if (@($handoffErrors).Count -ne 0) {
        throw "Encoded elevation handoff has parser errors: $(@($handoffErrors.Message) -join '; ')"
    }
    foreach ($handoffContract in @(
        'SetAccessRuleProtection', 'Bootstrap source changed during elevation handoff',
        'parameters.clixml', 'invoke.ps1', 'trustedPowerShell',
        'GetOwner([Security.Principal.SecurityIdentifier])',
        'Secure elevation staging ACL rules are invalid'
    )) {
        if ($handoffText -notmatch [Regex]::Escape($handoffContract)) {
            throw "Encoded elevation handoff is missing: $handoffContract"
        }
    }
    if ($handoffText -match 'WimForge-bootstrap-[0-9a-f]{32}') {
        throw 'The secure staging name was exposed in the pre-elevation command line.'
    }
    if ($handoffText.IndexOf("SetEnvironmentVariable('PSModulePath'") -gt
        $handoffText.IndexOf('Test-Path')) {
        throw 'The elevated handoff invokes cmdlets before restricting module resolution.'
    }
    $payloadMatch = [Regex]::Match(
        $handoffText, "FromBase64String\('(?<payload>[A-Za-z0-9+/=]+)'\)")
    if (-not $payloadMatch.Success) {
        throw 'Could not inspect the serialized elevation repair plan.'
    }
    $payloadXml = [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String($payloadMatch.Groups['payload'].Value))
    $handoffParameters = [Management.Automation.PSSerializer]::Deserialize($payloadXml)
    if (@($handoffParameters.RepairPackageIds).Count -ne 2 -or
        $handoffParameters.TrustedWingetPath -ne `
            'C:\Program Files\WindowsApps\trusted\winget.exe') {
        throw 'Elevation did not preserve the exact package plan and trusted WinGet path.'
    }
    foreach ($requiredContract in @(
        'Find-WindowsSdk', 'Qt6QuickDialogs2', 'Start-Transcript -Path $fullLogPath -Append',
        'Refusing to publish artifacts', 'Get-VerifiedArtifactSummary',
        'git clone --depth 1 --no-tags', 'DependencyRepairOnly',
        'Get-DependencyRepairPlan',
        'Get-UserDependencyRepairPlan', 'Install-UserWingetPackage',
        'Invoke-ElevatedDependencyRepair -PackageIds',
        'TrustedWingetPath', "'--scope', 'user'", "'--scope', 'machine'",
        'signed App Installer copy of WinGet from protected Program Files',
        'tests, and packaging must not inherit an administrator token',
        'New-VerifiedSourceSnapshot', 'GIT_CONFIG_NOSYSTEM',
        'clone --no-local --no-checkout --no-tags',
        'Get-RepositoryIntegrityState -Git $git', '-TrackedOnly',
        '[Environment]::Is64BitProcess', "'rc.exe'", "'mt.exe'",
        'QMAKE_XSPEC', 'Test-PeX64Executable',
        'Run -Plan from a normal Windows PowerShell session'
    )) {
        if ($source -notmatch [Regex]::Escape($requiredContract)) {
            throw "Bootstrap hardening contract is missing: $requiredContract"
        }
    }
    if ($source -match '(?i)\b(?:curl|irm|iex)\b.*\|\s*(?:iex|Invoke-Expression)\b') {
        throw 'The bootstrap must not download and execute arbitrary remote scripts.'
    }

    foreach ($functionName in @('Get-DependencyRepairPlan', 'Get-UserDependencyRepairPlan')) {
        $functionAst = $scriptAst.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq $functionName
        }, $true)
        Invoke-Expression $functionAst.Extent.Text
    }
    $missingDependencies = [pscustomobject]@{
        GitVersion = $null
        CMakeCompatible = $false
        NinjaVersion = $null
        VisualStudio = $null
        WindowsSdk = $null
        Qt = $null
        AqtVersion = $null
        InnoCompatible = $false
    }
    $machineRepair = @(Get-DependencyRepairPlan -Dependencies $missingDependencies)
    $userRepair = @(Get-UserDependencyRepairPlan -Dependencies $missingDependencies)
    if ($machineRepair.Count -ne 4 -or
        @($machineRepair | Where-Object { $_ -in @('Ninja-build.Ninja', 'miurahr.aqtinstall') }).Count -ne 0 -or
        $userRepair.Count -ne 2 -or
        @($userRepair | Where-Object { $_ -notin @('Ninja-build.Ninja', 'miurahr.aqtinstall') }).Count -ne 0) {
        throw 'User-scoped dependencies were not isolated from the elevated machine repair plan.'
    }

    foreach ($functionName in @(
        'Assert-NoReparsePath', 'Test-WimForgeCheckout',
        'Get-RepositoryIntegrityState', 'Assert-ManagedChildSafe',
        'New-VerifiedSourceSnapshot'
    )) {
        $functionAst = $scriptAst.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq $functionName
        }, $true)
        if (-not $functionAst) {
            throw "Could not inspect snapshot function: $functionName"
        }
        Invoke-Expression $functionAst.Extent.Text
    }
    $snapshotWorkspace = Join-Path $integrityRepo 'build-bootstrap'
    New-Item -ItemType Directory -Path $snapshotWorkspace -Force | Out-Null
    $integrityCommit = (& $testGit -C $integrityRepo rev-parse --verify HEAD).Trim()
    $snapshot = New-VerifiedSourceSnapshot -Git $testGit `
        -RepositoryRoot $integrityRepo -SourceCommit $integrityCommit `
        -Workspace $snapshotWorkspace
    $snapshotCommit = (& $testGit -C $snapshot.RepositoryRoot `
        rev-parse --verify HEAD).Trim()
    if ($snapshotCommit -ne $integrityCommit -or
        -not (Test-Path -LiteralPath (Join-Path $snapshot.RepositoryRoot 'CMakeLists.txt') `
            -PathType Leaf) -or
        -not $snapshot.RepositoryRoot.StartsWith(
            $snapshotWorkspace, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unique source snapshot did not bind the build tree to the verified commit.'
    }

    foreach ($functionName in @('Assert-NoReparsePath', 'Get-VerifiedArtifactSummary')) {
        $functionAst = $scriptAst.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq $functionName
        }, $true)
        if (-not $functionAst) {
            throw "Could not inspect bootstrap function: $functionName"
        }
        Invoke-Expression $functionAst.Extent.Text
    }
    $artifactDirectory = Join-Path $testRoot 'artifact-verification'
    New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
    $artifactVersion = '1.2.3'
    $portableArtifact = Join-Path $artifactDirectory `
        "WimForge-portable-x64-$artifactVersion.zip"
    $installerArtifact = Join-Path $artifactDirectory `
        "WimForge-Setup-x64-$artifactVersion.exe"
    Set-Content -LiteralPath $portableArtifact -Value 'current portable bytes' -Encoding Ascii
    Set-Content -LiteralPath $installerArtifact -Value 'current installer bytes' -Encoding Ascii
    $artifactSummary = @(Get-VerifiedArtifactSummary `
        -OutputDirectory $artifactDirectory -ProductVersion $artifactVersion)
    if ($artifactSummary.Count -ne 2 -or
        @($artifactSummary | Where-Object { $_.Sha256 -notmatch '^[0-9a-f]{64}$' }).Count -ne 0) {
        throw 'Artifact verification did not return two nonempty SHA-256 summaries.'
    }
    Clear-Content -LiteralPath $portableArtifact
    $emptyArtifactRejected = $false
    try {
        $null = Get-VerifiedArtifactSummary -OutputDirectory $artifactDirectory `
            -ProductVersion $artifactVersion
    } catch {
        $emptyArtifactRejected = $true
    }
    if (-not $emptyArtifactRejected) {
        throw 'Artifact verification accepted an empty package.'
    }

    $sitePath = Join-Path $repoRoot 'website\index.html'
    $siteText = Get-Content -LiteralPath $sitePath -Raw
    foreach ($siteContract in @(
        "[Guid]::NewGuid().ToString('N')",
        '[Environment]::SystemDirectory',
        'Normal PowerShell · UAC if repair is needed',
        'requests UAC only when machine package repair is needed',
        'replace <code>main</code> with a reviewed commit SHA',
        'dirty checkouts',
        'Microsoft App Installer/WinGet',
        'catalog versions current at run time'
    )) {
        if ($siteText -notmatch [Regex]::Escape($siteContract)) {
            throw "Website bootstrap instructions are missing: $siteContract"
        }
    }
    if ($siteText -match '\$env:TEMP\\WimForge-bootstrap\.ps1') {
        throw 'Website bootstrap instructions reuse a predictable temporary script path.'
    }

    $bootstrapMatch = [Regex]::Match(
        $siteText,
        '<pre id="bootstrap-code"><code>(?<command>.*?)</code></pre>',
        [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $bootstrapMatch.Success) {
        throw 'Could not extract the website bootstrap command.'
    }
    $bootstrapCommand = [Net.WebUtility]::HtmlDecode($bootstrapMatch.Groups['command'].Value)
    $commandTokens = $null
    $commandErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseInput(
        $bootstrapCommand, [ref]$commandTokens, [ref]$commandErrors)
    if (@($commandErrors).Count -ne 0) {
        throw "Website bootstrap command has parser errors: $(@($commandErrors.Message) -join '; ')"
    }

    Write-Host 'bootstrap_build_tests: PASS'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
