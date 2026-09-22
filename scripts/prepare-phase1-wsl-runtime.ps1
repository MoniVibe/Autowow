<#
.SYNOPSIS
    Prepare ephemeral WSL runtime configs for the isolated Phase 1 worldserver.

.DESCRIPTION
    Copies the existing local server configuration into WSL, rewrites only the
    Linux paths and the private MySQL-relay endpoint, and disables database
    auto-updates for the behavior proof. Secrets are never printed or placed in
    command-line arguments. The generated WSL config is mode 0600.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = 'D:\Games\wowstuff\AutoWoW',
    [string]$WslDistro = '',
    [string]$RelayAddress = '',
    [ValidateRange(1024, 65535)][int]$RelayPort = 13306,
    [string]$ExpectedBinarySha256 = '23a2d68e3b10f49bdda2b78419a53cc0518e1be0a643c9c8b704ce82d9de42be',
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$worldConfigPath = Join-Path $ServerRoot 'server\configs\worldserver.conf'
$moduleConfigPath = Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
$manifestPath = Join-Path $ServerRoot 'work\phase1-wsl-runtime\runtime-manifest.json'
$binaryLinuxPath = '/root/p1core/build/src/server/apps/worldserver'
$runtimeLinuxRoot = '/root/p1runtime'
$runtimeLogLinuxPath = '/mnt/d/Games/wowstuff/AutoWoW/logs/phase1-runtime'
$dataLinuxPath = '/root/p1data'

foreach ($requiredPath in @($worldConfigPath, $moduleConfigPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Missing required config: $requiredPath"
    }
}

if ([string]::IsNullOrWhiteSpace($WslDistro)) {
    $WslDistro = ((& wsl.exe -l -q | Select-Object -First 1) -replace "`0", '').Trim()
}
if ([string]::IsNullOrWhiteSpace($WslDistro)) { throw 'No WSL distribution was found.' }

if ([string]::IsNullOrWhiteSpace($RelayAddress)) {
    $RelayAddress = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.InterfaceAlias -match 'WSL' } |
        Select-Object -First 1).IPAddress
}
if ([string]::IsNullOrWhiteSpace($RelayAddress)) { throw 'No WSL adapter address was found.' }

$existingLinuxWorldserver = (& wsl.exe -d $WslDistro -u root -- pgrep -x worldserver 2>$null)
if ($LASTEXITCODE -eq 0 -and $existingLinuxWorldserver) {
    throw "A WSL worldserver is already running (pid $existingLinuxWorldserver)."
}

$binaryHash = (& wsl.exe -d $WslDistro -u root -- sha256sum $binaryLinuxPath 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $binaryHash) { throw "Missing WSL binary: $binaryLinuxPath" }
$binaryHash = ($binaryHash -split '\s+')[0].ToLowerInvariant()
if ($binaryHash -ne $ExpectedBinarySha256.ToLowerInvariant()) {
    throw "WSL binary hash mismatch: expected $ExpectedBinarySha256, got $binaryHash"
}

function Replace-ConfigSetting {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Value
    )
    $pattern = '(?m)^\s*' + [regex]::Escape($Name) + '\s*=.*$'
    if (-not [regex]::IsMatch($Text, $pattern)) { throw "Config setting not found: $Name" }
    return [regex]::Replace($Text, $pattern, "$Name = $Value", 1)
}

$worldText = [System.IO.File]::ReadAllText($worldConfigPath)
foreach ($databaseKey in @('LoginDatabaseInfo', 'WorldDatabaseInfo', 'CharacterDatabaseInfo')) {
    $pattern = '(?m)^\s*' + [regex]::Escape($databaseKey) + '\s*=\s*"([^"]+)"\s*$'
    $match = [regex]::Match($worldText, $pattern)
    if (-not $match.Success) { throw "Could not parse $databaseKey" }
    $parts = $match.Groups[1].Value.Split(';')
    if ($parts.Count -lt 5) { throw "$databaseKey has an unexpected connection format." }
    $parts[0] = $RelayAddress
    $parts[1] = [string]$RelayPort
    $worldText = Replace-ConfigSetting -Text $worldText -Name $databaseKey -Value ('"' + ($parts -join ';') + '"')
}

$worldText = Replace-ConfigSetting -Text $worldText -Name 'BindIP' -Value '"127.0.0.1"'
$worldText = Replace-ConfigSetting -Text $worldText -Name 'DataDir' -Value ('"' + $dataLinuxPath + '"')
$worldText = Replace-ConfigSetting -Text $worldText -Name 'LogsDir' -Value ('"' + $runtimeLogLinuxPath + '"')
$worldText = Replace-ConfigSetting -Text $worldText -Name 'Updates.EnableDatabases' -Value '0'

$moduleText = [System.IO.File]::ReadAllText($moduleConfigPath)
if ($moduleText -notmatch '(?m)^\s*AutoWow\.BridgePort\s*=\s*18787\s*$') {
    throw 'playerbots.conf does not expose AutoWow.BridgePort = 18787.'
}
$playerbotsDatabasePattern = '(?m)^\s*PlayerbotsDatabaseInfo\s*=\s*"([^"]+)"\s*$'
$playerbotsDatabaseMatch = [regex]::Match($moduleText, $playerbotsDatabasePattern)
if (-not $playerbotsDatabaseMatch.Success) { throw 'Could not parse PlayerbotsDatabaseInfo.' }
$playerbotsDatabaseParts = $playerbotsDatabaseMatch.Groups[1].Value.Split(';')
if ($playerbotsDatabaseParts.Count -lt 5) {
    throw 'PlayerbotsDatabaseInfo has an unexpected connection format.'
}
$playerbotsDatabaseParts[0] = $RelayAddress
$playerbotsDatabaseParts[1] = [string]$RelayPort
$moduleText = Replace-ConfigSetting -Text $moduleText -Name 'PlayerbotsDatabaseInfo' `
    -Value ('"' + ($playerbotsDatabaseParts -join ';') + '"')

$plan = [ordered]@{
    schema = 'autowow.phase1.wsl-runtime.v1'
    apply = [bool]$Apply
    distro = $WslDistro
    binary = $binaryLinuxPath
    binary_sha256 = $binaryHash
    runtime_config = "$runtimeLinuxRoot/worldserver.conf"
    module_config = '/usr/local/etc/modules/playerbots.conf'
    data_dir = $dataLinuxPath
    logs_dir = $runtimeLogLinuxPath
    mysql_relay = "${RelayAddress}:$RelayPort"
    mysql_target = 'Windows 127.0.0.1:3306'
    database_auto_updates = $false
    credentials_redacted = $true
    prepared_utc = (Get-Date).ToUniversalTime().ToString('o')
}

if (-not $Apply) {
    [pscustomobject]$plan | ConvertTo-Json -Depth 5
    return
}

& wsl.exe -d $WslDistro -u root -- mkdir -p $runtimeLinuxRoot /usr/local/etc/modules $runtimeLogLinuxPath
if ($LASTEXITCODE -ne 0) { throw 'Failed to create WSL runtime directories.' }

$runtimeUncRoot = "\\wsl.localhost\$WslDistro\root\p1runtime"
$moduleUncRoot = "\\wsl.localhost\$WslDistro\usr\local\etc\modules"
[System.IO.File]::WriteAllText(
    (Join-Path $runtimeUncRoot 'worldserver.conf'),
    $worldText,
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    (Join-Path $moduleUncRoot 'playerbots.conf'),
    $moduleText,
    [System.Text.UTF8Encoding]::new($false))

& wsl.exe -d $WslDistro -u root -- chmod 600 "$runtimeLinuxRoot/worldserver.conf" /usr/local/etc/modules/playerbots.conf
if ($LASTEXITCODE -ne 0) { throw 'Failed to protect generated WSL config permissions.' }

$manifestDirectory = Split-Path -Parent $manifestPath
New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null
[System.IO.File]::WriteAllText(
    $manifestPath,
    ([pscustomobject]$plan | ConvertTo-Json -Depth 5),
    [System.Text.UTF8Encoding]::new($false))

[pscustomobject]$plan | ConvertTo-Json -Depth 5
