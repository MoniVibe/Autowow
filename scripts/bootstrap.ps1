[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WowClientDir = $(if ($env:WOW_CLIENT_DIR) { $env:WOW_CLIENT_DIR } else { '' }),
    [switch]$SkipClone
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$resolvedServerRoot = Resolve-Path -LiteralPath $ServerRoot -ErrorAction SilentlyContinue
if ($resolvedServerRoot) { $ServerRoot = $resolvedServerRoot.Path }
Initialize-AutoWoWLayout -Root $ServerRoot
$logPath = New-AutoWoWLogPath -Name 'bootstrap'

function Get-CommandVersion {
    param([string]$Name, [string[]]$Arguments = @('--version'))
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) { return 'MISSING' }
    try {
        $output = & $command.Source @Arguments 2>&1 | Select-Object -First 2
        return ((@($output) -join ' ') -replace '\s+', ' ').Trim()
    }
    catch {
        return "PRESENT ($($command.Source))"
    }
}

function Get-VsInstanceSummary {
    $vswhereCandidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe',
        'C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe'
    )
    $vswhere = Get-FirstExistingPath -Candidates $vswhereCandidates
    if (-not $vswhere) { return @('MISSING') }

    $jsonText = (& $vswhere -all -products * -format json 2>$null) -join [Environment]::NewLine
    if ([string]::IsNullOrWhiteSpace($jsonText)) { return @('vswhere found, no instances returned') }

    $instances = @($jsonText | ConvertFrom-Json)
    $result = [System.Collections.Generic.List[string]]::new()
    foreach ($instance in $instances) {
        $result.Add(("{0} {1} at {2}; complete={3}" -f $instance.displayName, $instance.installationVersion, $instance.installationPath, $instance.isComplete))
    }
    return $result
}

function Get-WowClientEvidence {
    if ([string]::IsNullOrWhiteSpace($WowClientDir) -or -not (Test-Path -LiteralPath $WowClientDir)) {
        return [ordered]@{ Status = 'NOT_PRESENT'; Path = $WowClientDir; Version = 'N/A'; Build12340 = 'UNKNOWN' }
    }

    $wowExe = Get-ChildItem -LiteralPath $WowClientDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @('Wow.exe', 'WoW.exe', 'Wow-64.exe', 'WoW-64.exe') } |
        Select-Object -First 1
    if (-not $wowExe) {
        return [ordered]@{ Status = 'DIRECTORY_FOUND_NO_WOW_EXE'; Path = $WowClientDir; Version = 'N/A'; Build12340 = 'UNKNOWN' }
    }

    $fileVersion = $wowExe.VersionInfo.FileVersion
    $productVersion = $wowExe.VersionInfo.ProductVersion
    $binaryMarker = $false
    if (($fileVersion -notmatch '12340') -and ($productVersion -notmatch '12340')) {
        $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($wowExe.FullName))
        $binaryMarker = $ascii.Contains('12340')
    }
    $versionText = "FileVersion=$fileVersion; ProductVersion=$productVersion; BinaryMarker12340=$binaryMarker"
    $looks12340 = ($fileVersion -match '3\.3\.5\.12340|12340') -or ($productVersion -match '3\.3\.5\.12340|12340') -or $binaryMarker
    return [ordered]@{ Status = 'FOUND'; Path = $WowClientDir; Version = $versionText; Build12340 = $(if ($looks12340) { 'YES' } else { 'NO_OR_UNCONFIRMED' }) }
}

$coreRoot = Join-Path $ServerRoot 'azerothcore-wotlk'
$moduleRoot = Join-Path $coreRoot 'modules\mod-playerbots'
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) { throw 'Git is required but was not found.' }

if (-not $SkipClone) {
    if (-not (Test-Path -LiteralPath (Join-Path $coreRoot '.git'))) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $coreRoot) -Force | Out-Null
        Invoke-NativeLogged -FilePath $git.Source -ArgumentList @('clone', '--branch', 'Playerbot', '--single-branch', 'https://github.com/mod-playerbots/azerothcore-wotlk.git', $coreRoot) -WorkingDirectory $ServerRoot -LogPath (New-AutoWoWLogPath -Name 'clone-core') | Out-Null
    }
    if (-not (Test-Path -LiteralPath (Join-Path $moduleRoot '.git'))) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $moduleRoot) -Force | Out-Null
        Invoke-NativeLogged -FilePath $git.Source -ArgumentList @('clone', '--branch', 'master', '--single-branch', 'https://github.com/mod-playerbots/mod-playerbots.git', $moduleRoot) -WorkingDirectory $coreRoot -LogPath (New-AutoWoWLogPath -Name 'clone-module') | Out-Null
    }
}

$coreSha = if (Test-Path -LiteralPath (Join-Path $coreRoot '.git')) { (& git -C $coreRoot rev-parse HEAD).Trim() } else { 'MISSING' }
$moduleSha = if (Test-Path -LiteralPath (Join-Path $moduleRoot '.git')) { (& git -C $moduleRoot rev-parse HEAD).Trim() } else { 'MISSING' }
$coreBranch = if ($coreSha -ne 'MISSING') { (& git -C $coreRoot branch --show-current).Trim() } else { 'MISSING' }
$moduleBranch = if ($moduleSha -ne 'MISSING') { (& git -C $moduleRoot branch --show-current).Trim() } else { 'MISSING' }
$coreStatus = if ($coreSha -ne 'MISSING') { ((& git -C $coreRoot status --short) -join ' ').Trim() } else { 'MISSING' }
$moduleStatus = if ($moduleSha -ne 'MISSING') { ((& git -C $moduleRoot status --short) -join ' ').Trim() } else { 'MISSING' }

$wowEvidence = Get-WowClientEvidence
$vs = @(Get-VsInstanceSummary)
$clPath = Get-FirstExistingPath -Candidates @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe'
)
$mysqlCandidates = @(
    $env:MYSQL_ROOT_DIR,
    (Join-Path $ServerRoot 'third_party\mysql'),
    'C:\Program Files\MySQL\MySQL Server 8.4',
    'C:\Program Files\MySQL\MySQL Server 8.0'
)
$opensslCandidates = @(
    $env:OPENSSL_ROOT_DIR,
    (Join-Path $ServerRoot 'third_party\openssl'),
    'C:\Program Files\OpenSSL-Win64',
    'C:\Program Files\OpenSSL'
)
$boostCandidates = @(
    $env:Boost_ROOT,
    $env:BOOST_ROOT,
    (Join-Path $ServerRoot 'third_party\boost\1.83.0'),
    (Join-Path $ServerRoot 'third_party\boost\boost_1_83_0')
)
$mysqlRoot = Get-FirstExistingPath -Candidates $mysqlCandidates
$opensslRoot = Get-FirstExistingPath -Candidates $opensslCandidates
$boostRoot = Get-FirstExistingPath -Candidates $boostCandidates
$mysqlExe = if ($mysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $mysqlRoot 'bin\mysql.exe')) } else { $null }
$opensslExe = Get-Command openssl -ErrorAction SilentlyContinue
$mysqlService = @(Get-Service -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '(?i)mysql|maria|percona' -or $_.DisplayName -match '(?i)mysql|maria|percona' })

$diskRows = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object {
    [ordered]@{ Drive = $_.Name; FreeGiB = [math]::Round($_.Free / 1GB, 1); UsedGiB = [math]::Round($_.Used / 1GB, 1) }
})

$envNames = @('WOW_CLIENT_DIR','MYSQL_ROOT_DIR','MYSQL_ROOT_PASSWORD','Boost_ROOT','BOOST_ROOT','OPENSSL_ROOT_DIR')
$envRows = foreach ($name in $envNames) {
    [ordered]@{ Name = $name; Present = $(if ($name -eq 'MYSQL_ROOT_PASSWORD') { Get-SecretPresence -Name $name } else { -not [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name)) }) }
}

$report = [System.Text.StringBuilder]::new()
[void]$report.AppendLine('# AutoWoW bootstrap audit')
[void]$report.AppendLine('')
[void]$report.AppendLine(("Generated: {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')))
[void]$report.AppendLine('')
[void]$report.AppendLine('## Source snapshot')
[void]$report.AppendLine('')
[void]$report.AppendLine(("- Core path: {0}" -f $coreRoot))
[void]$report.AppendLine(("- Core branch: {0}" -f $coreBranch))
[void]$report.AppendLine(("- Core SHA: {0}" -f $coreSha))
[void]$report.AppendLine(("- Module path: {0}" -f $moduleRoot))
[void]$report.AppendLine(("- Module branch: {0}" -f $moduleBranch))
[void]$report.AppendLine(("- Module SHA: {0}" -f $moduleSha))
[void]$report.AppendLine(("- Core dirty status: {0}" -f $(if ($coreStatus) { $coreStatus } else { 'clean' })))
[void]$report.AppendLine(("- Module dirty status: {0}" -f $(if ($moduleStatus) { $moduleStatus } else { 'clean' })))
[void]$report.AppendLine('')
[void]$report.AppendLine('## Toolchain')
[void]$report.AppendLine('')
[void]$report.AppendLine(("- Git: {0}" -f (Get-CommandVersion -Name 'git')))
[void]$report.AppendLine(("- CMake: {0}" -f (Get-CommandVersion -Name 'cmake')))
[void]$report.AppendLine(("- Visual Studio: {0}" -f (($vs -join ' | '))))
[void]$report.AppendLine(("- x64 MSVC compiler: {0}" -f $(if ($clPath) { $clPath } else { 'MISSING' })))
[void]$report.AppendLine(("- MySQL root: {0}" -f $(if ($mysqlRoot) { $mysqlRoot } else { 'MISSING' })))
[void]$report.AppendLine(("- MySQL CLI: {0}" -f $(if ($mysqlExe) { $mysqlExe } else { 'MISSING' })))
[void]$report.AppendLine(("- MySQL service(s): {0}" -f $(if ($mysqlService.Count) { ($mysqlService.Name -join ', ') } else { 'NONE FOUND' })))
[void]$report.AppendLine(("- OpenSSL root: {0}" -f $(if ($opensslRoot) { $opensslRoot } else { 'MISSING' })))
[void]$report.AppendLine(("- OpenSSL CLI: {0}" -f $(if ($opensslExe) { $opensslExe.Source } else { 'MISSING' })))
[void]$report.AppendLine(("- Boost root: {0}" -f $(if ($boostRoot) { $boostRoot } else { 'MISSING' })))
[void]$report.AppendLine('')
[void]$report.AppendLine('## Environment variables (presence only)')
[void]$report.AppendLine('')
[void]$report.AppendLine('| Name | Present |')
[void]$report.AppendLine('|---|---|')
foreach ($row in $envRows) { [void]$report.AppendLine(("| {0} | {1} |" -f $row.Name, $row.Present)) }
[void]$report.AppendLine('')
[void]$report.AppendLine('## WoW client')
[void]$report.AppendLine('')
[void]$report.AppendLine(("- Client path: {0}" -f $wowEvidence.Path))
[void]$report.AppendLine(("- Detection: {0}" -f $wowEvidence.Status))
[void]$report.AppendLine(("- Version evidence: {0}" -f $wowEvidence.Version))
[void]$report.AppendLine(("- Build 12340: **{0}**" -f $wowEvidence.Build12340))
[void]$report.AppendLine('')
[void]$report.AppendLine('## Disk space')
[void]$report.AppendLine('')
[void]$report.AppendLine('| Drive | Free GiB | Used GiB |')
[void]$report.AppendLine('|---|---:|---:|')
foreach ($row in $diskRows) { [void]$report.AppendLine(("| {0}: | {1} | {2} |" -f $row.Drive, $row.FreeGiB, $row.UsedGiB)) }
[void]$report.AppendLine('')
[void]$report.AppendLine('## Blocking or privileged actions')
[void]$report.AppendLine('')
[void]$report.AppendLine('- Database initialization requires `MYSQL_ROOT_PASSWORD` in the process environment; the value is never written to this report.')
[void]$report.AppendLine('- Installing a Windows service or machine-wide dependency may require administrator privileges; the current audit did not install or start anything.')
$clientStatusLine = if ($wowEvidence.Status -eq 'FOUND' -and $wowEvidence.Build12340 -eq 'YES') {
    '- Client build 12340 has been detected; client-data extraction can proceed or has been completed by `extract-client-data.ps1`.'
} elseif ($wowEvidence.Status -eq 'NOT_PRESENT') {
    '- Client build validation and extraction are pending until the client download is complete.'
} else {
    '- Client path was found, but build validation or extraction remains pending.'
}
[void]$report.AppendLine($clientStatusLine)
[void]$report.AppendLine('')
[void]$report.AppendLine(("Audit log: {0}" -f $logPath))

$auditPath = Join-Path $ServerRoot 'BOOTSTRAP_AUDIT.md'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($auditPath, $report.ToString(), $utf8NoBom)

$pinned = @"
# Pinned source commits

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

| Repository | Branch | Commit | Remote |
|---|---|---|---|
| azerothcore-wotlk | $coreBranch | $coreSha | https://github.com/mod-playerbots/azerothcore-wotlk.git |
| mod-playerbots | $moduleBranch | $moduleSha | https://github.com/mod-playerbots/mod-playerbots.git |

These checkouts were cloned with `--single-branch`. This bootstrap does not fetch, pull, or update an existing checkout.
"@
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'PINNED_COMMITS.md'), $pinned, $utf8NoBom)

Write-Output "Audit written to $auditPath"
Write-Output "Pinned commits written to $(Join-Path $ServerRoot 'PINNED_COMMITS.md')"
