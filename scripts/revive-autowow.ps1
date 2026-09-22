<#
.SYNOPSIS
    Resume the existing AutoWoW server and a small, named Oracle roster.
.DESCRIPTION
    Status is read-only. Start and Park require -Apply. This script never initializes a
    database, edits configuration, seeds characters, builds binaries, or starts a director.
    The default roster remains the three observer-session bots. The six zone scouts can be
    selected explicitly with -RosterGuids 101,112,121,123,236,244.
#>
[CmdletBinding()]
param(
    [ValidateSet('Status','Start','Park')][string]$Action = 'Status',
    [switch]$Apply,
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$WorldserverBinary = '/root/autowow-advisor-t1-build/src/server/apps/worldserver',
    [uint32[]]$RosterGuids = @(101,112,123)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
if ($Action -ne 'Status' -and -not $Apply) { throw "$Action requires -Apply." }
$RosterGuids = @($RosterGuids | Sort-Object -Unique)
if ($RosterGuids.Count -eq 0 -or @($RosterGuids | Where-Object { $_ -notin @(101,112,121,123,236,244) }).Count) {
    throw 'This revival helper is restricted to the existing GUIDs 101, 112, 121, 123, 236, and 244.'
}

$mysqlBinary = Join-Path $ServerRoot 'third_party\mysql\bin\mysqld.exe'
$mysqlIni = Join-Path $ServerRoot 'server\mysql-auto.ini'
$mysqlData = Join-Path $ServerRoot 'server\mysql-data\mysql'
$authBinary = Join-Path $ServerRoot 'server\authserver.exe'
$authPidPath = Join-Path $ServerRoot 'authserver.pid'
$mysqlPidPath = Join-Path $ServerRoot 'mysql.pid'
$bridgeScript = Join-Path $PSScriptRoot 'autowow-control.ps1'
$authLauncher = Join-Path $PSScriptRoot 'start-server.ps1'
$worldLauncher = Join-Path $PSScriptRoot 'start-phase1-wsl-worldserver.ps1'

function Get-ExactWindowsProcess {
    param([string]$Name, [string]$ExpectedPath, [string]$PidPath)
    $matches = @(Get-CimInstance Win32_Process -Filter "Name = '$Name'" -ErrorAction Stop)
    $exact = @($matches | Where-Object { [string]$_.ExecutablePath -ieq $ExpectedPath })
    $maxExpected = if ($Name -ieq 'mysqld.exe') { 2 } else { 1 }
    if ($matches.Count -ne $exact.Count -or $exact.Count -gt $maxExpected) {
        throw "Conflicting $Name process detected; inspect it manually before revival."
    }
    $recordedPid = 0
    if (Test-Path -LiteralPath $PidPath -PathType Leaf) {
        $raw = (Get-Content -LiteralPath $PidPath -Raw).Trim()
        if (-not [int]::TryParse($raw, [ref]$recordedPid)) { throw "Invalid PID file: $PidPath" }
        $owner = @(Get-CimInstance Win32_Process -Filter "ProcessId = $recordedPid" -ErrorAction Stop)
        if ($owner.Count -and ($owner[0].Name -ine $Name -or [string]$owner[0].ExecutablePath -ine $ExpectedPath)) {
            throw "PID file $PidPath names an unrelated process; refusing cleanup or startup."
        }
        if ($exact.Count -and $recordedPid -notin @($exact | ForEach-Object { [int]$_.ProcessId })) {
            throw "Live $Name does not match its PID file; inspect before revival."
        }
    }
    if ($exact.Count -gt 1 -and -not $recordedPid) { throw "Multiple $Name processes have no trustworthy PID record." }
    if ($exact.Count) {
        if ($recordedPid) { return @($exact | Where-Object { [int]$_.ProcessId -eq $recordedPid })[0] }
        return $exact[0]
    }
    return $null
}

function Test-LocalPort {
    param([int]$Port)
    $tcp = [Net.Sockets.TcpClient]::new()
    try {
        return [bool]($tcp.ConnectAsync('127.0.0.1',$Port).Wait(1000) -and $tcp.Connected)
    }
    catch { return $false }
    finally { $tcp.Dispose() }
}

function Get-WorldProcess {
    if (Get-CimInstance Win32_Process -Filter "Name = 'worldserver.exe'" -ErrorAction Stop) {
        throw 'A Windows worldserver is running; refusing a second world.'
    }
    $ids = @(& wsl.exe -d $WslDistro -u root -- pgrep -x worldserver 2>$null)
    if ($LASTEXITCODE -eq 1) { return $null }
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect WSL worldserver processes in $WslDistro." }
    if ($ids.Count -ne 1) { throw "Expected at most one WSL worldserver; found $($ids.Count)." }
    [int]$worldPid = 0
    if (-not [int]::TryParse(([string]$ids[0]).Trim(), [ref]$worldPid)) { throw 'Invalid WSL worldserver PID.' }
    $exe = @(& wsl.exe -d $WslDistro -u root -- readlink -f "/proc/$worldPid/exe" 2>$null)
    if ($LASTEXITCODE -ne 0 -or @($exe).Count -ne 1 -or ([string]$exe[0]).Trim() -cne $WorldserverBinary) {
        throw "WSL worldserver PID $worldPid does not use the approved binary $WorldserverBinary."
    }
    return $worldPid
}

function Get-BridgeList {
    if (-not (Test-LocalPort 18787)) { return $null }
    $lines = @(& $bridgeScript -Action list 2>$null)
    if ($lines.Count -eq 0) { throw 'Bridge port is open but list failed.' }
    $result = ([string]$lines[-1]) | ConvertFrom-Json
    if (-not $result.ok) { throw 'Bridge list returned a non-ok response.' }
    return $result
}

function Invoke-BridgeAction {
    param([ValidateSet('activate','independent','deactivate')][string]$Verb, [uint32]$Guid)
    for ($attempt = 1; $attempt -le 11; $attempt++) {
        $lines = @(& $bridgeScript -Action $Verb -BotGuid $Guid)
        if ($lines.Count -eq 0) { throw "$Verb failed for GUID $Guid." }
        $result = ([string]$lines[-1]) | ConvertFrom-Json
        if ($result.ok) { return $result }
        # A newly logged-in scout may already be fighting before it can be armed.
        if ($Verb -ne 'independent' -or $result.error -ne 'independent_deferred_combat' -or $attempt -eq 11) {
            throw "$Verb was rejected for GUID ${Guid}: $($result.error)"
        }
        Start-Sleep -Seconds 2
    }
}

function Wait-ForRosterOnline {
    param([uint32[]]$Guids, [int]$TimeoutSeconds = 90)
    if (-not $Guids.Count) { return }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $list = Get-BridgeList
        if (-not $list) { throw 'Bridge disappeared while waiting for the selected roster.' }
        $online = @($list.bots | ForEach-Object { [uint32]$_.guid })
        $missing = @($Guids | Where-Object { $_ -notin $online })
        if (-not $missing.Count) { return }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for newly activated roster: $($missing -join ',')."
}

function Get-RevivalState {
    $mysql = Get-ExactWindowsProcess -Name 'mysqld.exe' -ExpectedPath $mysqlBinary -PidPath $mysqlPidPath
    $auth = Get-ExactWindowsProcess -Name 'authserver.exe' -ExpectedPath $authBinary -PidPath $authPidPath
    $world = Get-WorldProcess
    $bridge = Get-BridgeList
    if ($bridge -and -not $world) { throw 'Bridge is serving without the expected WSL worldserver.' }
    $online = if ($bridge) { @($bridge.bots | ForEach-Object { [uint32]$_.guid }) } else { @() }
    [pscustomobject]@{
        mysql_pid = if ($mysql) { [int]$mysql.ProcessId } else { $null }
        mysql_port = Test-LocalPort 3306
        auth_pid = if ($auth) { [int]$auth.ProcessId } else { $null }
        auth_port = Test-LocalPort 3724
        world_pid = $world
        bridge_ready = [bool]$bridge
        roster_online = @($RosterGuids | Where-Object { $_ -in $online })
        roster_missing = @($RosterGuids | Where-Object { $_ -notin $online })
    }
}

$state = Get-RevivalState
switch ($Action) {
    'Status' { $state | ConvertTo-Json -Depth 5; return }
    'Start' {
        foreach ($required in @($mysqlBinary,$mysqlIni,$mysqlData,$authBinary,$authLauncher,$worldLauncher,$bridgeScript)) {
            if (-not (Test-Path -LiteralPath $required)) { throw "Existing revival prerequisite missing: $required" }
        }
        if (-not $state.mysql_pid) {
            if ($state.mysql_port) { throw 'Port 3306 is occupied without the approved MySQL process.' }
            # Existing data/config only. Never call start-mysql.ps1: it can initialize a new database.
            $mysql = Start-Process -FilePath $mysqlBinary -ArgumentList "--defaults-file=$mysqlIni" `
                -WorkingDirectory (Split-Path -Parent $mysqlBinary) -WindowStyle Hidden -PassThru
            for ($i = 0; $i -lt 30 -and -not (Test-LocalPort 3306); $i++) { Start-Sleep -Seconds 1 }
            if (-not (Test-LocalPort 3306)) { throw "Existing MySQL did not open port 3306 (PID $($mysql.Id))." }
        }
        elseif (-not $state.mysql_port) { throw 'Approved MySQL process exists but port 3306 is unavailable.' }
        if (-not $state.auth_pid) {
            if ($state.auth_port) { throw 'Port 3724 is occupied without the approved authserver.' }
            & $authLauncher -ServerRoot $ServerRoot -AuthOnly | Out-Null
        }
        elseif (-not $state.auth_port) { throw 'Approved authserver exists but port 3724 is unavailable.' }
        if (-not $state.world_pid) {
            if ($state.bridge_ready -or (Test-LocalPort 8085)) { throw 'World ports occupied without approved worldserver.' }
            & $worldLauncher -ServerRoot $ServerRoot -WslDistro $WslDistro -WorldserverBinary $WorldserverBinary | Out-Null
        }
        $state = Get-RevivalState
        if (-not $state.bridge_ready) { throw 'Worldserver is not bridge-ready; roster untouched.' }
        $newlyActivated = [Collections.Generic.List[uint32]]::new()
        foreach ($guid in @($state.roster_missing)) {
            Invoke-BridgeAction -Verb activate -Guid $guid | Out-Null
            [void]$newlyActivated.Add($guid)
        }
        Wait-ForRosterOnline -Guids @($newlyActivated)
        foreach ($guid in $newlyActivated) {
            Invoke-BridgeAction -Verb independent -Guid $guid | Out-Null
        }
        $state = Get-RevivalState
        if ($state.roster_missing.Count) { throw "Selected roster still missing: $($state.roster_missing -join ',')." }
        $state | ConvertTo-Json -Depth 5
    }
    'Park' {
        if (-not $state.bridge_ready) { throw 'Bridge is unavailable; no roster was changed.' }
        foreach ($guid in @($state.roster_online)) {
            Invoke-BridgeAction -Verb deactivate -Guid $guid | Out-Null
        }
        $state = Get-RevivalState
        if ($state.roster_online.Count) { throw "Selected roster still online: $($state.roster_online -join ',')." }
        $state | ConvertTo-Json -Depth 5
    }
}
