[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$RequireBots
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$installRoot = Join-Path $ServerRoot 'server'
$worldLogCandidates = @(
    (Join-Path $installRoot 'logs\worldserver-stderr.log'),
    (Join-Path $installRoot 'logs\worldserver-stdout.log'),
    (Join-Path $installRoot 'worldserver.log')
)

function Assert-TcpPort {
    param([string]$Name, [int]$Port)
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $task = $client.ConnectAsync('127.0.0.1', $Port)
        if (-not $task.Wait(3000) -or -not $client.Connected) { throw "not accepting connections" }
        Write-Output "${Name}: 127.0.0.1:$Port is open"
    }
    finally { $client.Dispose() }
}

foreach ($name in @('authserver','worldserver')) {
    $pidFile = Join-Path $ServerRoot ("{0}.pid" -f $name)
    if (-not (Test-Path -LiteralPath $pidFile)) { throw "$name PID file is missing." }
    $processId = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if (-not $process) { throw "$name PID $processId is not running." }
}

Assert-TcpPort -Name 'Auth' -Port 3724
Assert-TcpPort -Name 'World' -Port 8085
Assert-TcpPort -Name 'AutoWow bridge' -Port 18787

if (Get-NetTCPConnection -State Listen -LocalPort 8888 -ErrorAction SilentlyContinue) {
    throw 'Legacy Playerbots command port 8888 is listening. AutoWow requires it to remain disabled.'
}

$mysqlRoot = Get-FirstExistingPath -Candidates @(
    $env:MYSQL_ROOT_DIR,
    (Join-Path $ServerRoot 'third_party\mysql'),
    'C:\Program Files\MySQL\MySQL Server 8.4',
    'C:\Program Files\MySQL\MySQL Server 8.0'
)
$mysqlExe = if ($mysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $mysqlRoot 'bin\mysql.exe')) } else { $null }
if (-not $mysqlExe) { throw 'MySQL CLI not found for smoke checks.' }
if ([string]::IsNullOrWhiteSpace($env:MYSQL_ROOT_PASSWORD)) { throw 'Set MYSQL_ROOT_PASSWORD for database smoke checks.' }

function Invoke-MySqlSmoke {
    param([string]$Sql)
    $oldPwd = $env:MYSQL_PWD
    $env:MYSQL_PWD = $env:MYSQL_ROOT_PASSWORD
    try {
        $result = & $mysqlExe --protocol=tcp --host=127.0.0.1 --port=3306 --user=root --batch --skip-column-names --execute=$Sql 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
    }
    $textResult = @($result | ForEach-Object { $_.ToString() })
    $filteredResult = @($textResult | Where-Object { $_ -notmatch '^mysql: (Unknown OS character set|Switching to the default character set)' })
    if ($exitCode -ne 0) { throw "MySQL smoke query failed: $($filteredResult -join ' ')" }
    return $filteredResult
}

$checks = [ordered]@{}
$checks['schemas'] = ((@(Invoke-MySqlSmoke -Sql "SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME IN ('acore_auth','acore_world','acore_characters','acore_playerbots');"))[0]).Trim()
$checks['playerbot_tables'] = ((@(Invoke-MySqlSmoke -Sql "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='acore_playerbots';"))[0]).Trim()
$checks['playerbot_random_rows'] = ((@(Invoke-MySqlSmoke -Sql "SELECT COUNT(*) FROM acore_playerbots.playerbots_random_bots;" ))[0]).Trim()

if ($checks['schemas'] -ne '4') { throw "Expected four AutoWoW schemas, got $($checks['schemas'])." }
if ([int]$checks['playerbot_tables'] -lt 1) { throw 'No Playerbots tables were found.' }
if ($RequireBots -and [int]$checks['playerbot_random_rows'] -lt 1) { throw 'Playerbots table exists but no random-bot rows are present.' }

$badPatterns = 'Could not find.*DBC|Incorrect DataDir|Failed to find map files|Map file.*(not found|incompatible|error opening)|VMap file.*(not found|couldn''t|incompatible)|MMap.*(not found|incompatible|failed)|PlayerbotsDatabase.*failed|Failed to initialize.*database|ERROR.*playerbot'
$badLines = [System.Collections.Generic.List[string]]::new()
foreach ($log in $worldLogCandidates) {
    if (Test-Path -LiteralPath $log) {
        foreach ($line in (Select-String -LiteralPath $log -Pattern $badPatterns -CaseSensitive:$false -ErrorAction SilentlyContinue)) {
            $badLines.Add($line.Line)
        }
    }
}
if ($badLines.Count) { throw ("Server logs contain failure signatures:`n" + ($badLines -join "`n")) }

$report = @"
# AutoWoW smoke-test report

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

| Check | Result |
|---|---:|
| Auth port 3724 | PASS |
| World port 8085 | PASS |
| AutoWow bridge port 18787 | PASS |
| Legacy Playerbots port 8888 | disabled |
| AutoWoW schemas | $($checks['schemas']) |
| Playerbots tables | $($checks['playerbot_tables']) |
| Playerbots random rows | $($checks['playerbot_random_rows']) |
| Log failure scan | PASS |

The client login, character creation, bot follow/fight/loot behavior, chat-command response, and restart persistence still require an interactive 3.3.5a client session.
"@
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'SMOKE_TEST_REPORT.md'), $report, $utf8NoBom)
Write-Output "Smoke test passed. Report: $(Join-Path $ServerRoot 'SMOKE_TEST_REPORT.md')"
