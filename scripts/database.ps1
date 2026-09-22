[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$MysqlRoot = $(if ($env:MYSQL_ROOT_DIR) { $env:MYSQL_ROOT_DIR } else { '' })
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot

if ([string]::IsNullOrWhiteSpace($env:MYSQL_ROOT_PASSWORD)) {
    throw 'Set MYSQL_ROOT_PASSWORD in the process environment before running database.ps1. The password is never accepted as a script argument or written to reports.'
}

if ([string]::IsNullOrWhiteSpace($MysqlRoot)) {
    $MysqlRoot = Get-FirstExistingPath -Candidates @(
        (Join-Path $ServerRoot 'third_party\mysql'),
        'C:\Program Files\MySQL\MySQL Server 8.4',
        'C:\Program Files\MySQL\MySQL Server 8.0'
    )
}
$mysqlExe = if ($MysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $MysqlRoot 'bin\mysql.exe')) } else { $null }
if (-not $mysqlExe) { throw 'MySQL CLI was not found. Set MYSQL_ROOT_DIR or stage MySQL under third_party\mysql.' }

$coreRoot = Join-Path $ServerRoot 'azerothcore-wotlk'
$serverRoot = Join-Path $ServerRoot 'server'
$createSql = Join-Path $coreRoot 'data\sql\create\create_mysql.sql'
$dbImport = Join-Path $serverRoot 'dbimport.exe'
$dbImportConfig = Join-Path $serverRoot 'configs\dbimport.conf'
if (-not (Test-Path -LiteralPath $createSql)) { throw "Core database create script missing: $createSql" }
if (-not (Test-Path -LiteralPath $dbImport)) { throw "dbimport.exe missing: $dbImport. Run build.ps1 first." }
if (-not (Test-Path -LiteralPath $dbImportConfig)) { throw "dbimport config missing: $dbImportConfig. Run configure-server.ps1 first." }

function Invoke-MySql {
    param(
        [Parameter(Mandatory = $true)][string]$Sql,
        [string]$Database = ''
    )

    $oldPwd = $env:MYSQL_PWD
    $env:MYSQL_PWD = $env:MYSQL_ROOT_PASSWORD
    try {
        $args = @('--protocol=tcp', '--host=127.0.0.1', '--port=3306', '--user=root', '--batch', '--skip-column-names')
        if ($Database) { $args += @('--database', $Database) }
    $output = $Sql | & $mysqlExe @args 2>&1
    $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
    }
    $textOutput = @($output | ForEach-Object { $_.ToString() })
    $filteredOutput = @($textOutput | Where-Object { $_ -notmatch '^mysql: (Unknown OS character set|Switching to the default character set)' })
    if ($exitCode -ne 0) { throw "MySQL command failed: $($filteredOutput -join ' ')" }
    return $filteredOutput
}

$portReady = $false
for ($attempt = 1; $attempt -le 30; $attempt++) {
    try {
        $tcp = [System.Net.Sockets.TcpClient]::new()
        $tcp.Connect('127.0.0.1', 3306)
        $tcp.Dispose()
        $portReady = $true
        break
    }
    catch {
        Start-Sleep -Seconds 1
    }
}
if (-not $portReady) { throw 'MySQL is not accepting connections on 127.0.0.1:3306.' }

$existing = @(Invoke-MySql -Sql "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME IN ('acore_auth','acore_world','acore_characters','acore_playerbots') ORDER BY SCHEMA_NAME;")
$expected = @('acore_auth','acore_world','acore_characters','acore_playerbots')
$missing = @($expected | Where-Object { $_ -notin $existing })
if ($missing.Count) {
    $sql = [System.IO.File]::ReadAllText($createSql)
    $sql += @'

CREATE DATABASE IF NOT EXISTS `acore_playerbots` DEFAULT CHARACTER SET UTF8MB4 COLLATE utf8mb4_general_ci;
DROP USER IF EXISTS 'acore'@'127.0.0.1';
CREATE USER 'acore'@'127.0.0.1' IDENTIFIED BY 'acore' WITH MAX_QUERIES_PER_HOUR 0 MAX_CONNECTIONS_PER_HOUR 0 MAX_UPDATES_PER_HOUR 0;
GRANT ALL PRIVILEGES ON `acore_world`.* TO 'acore'@'127.0.0.1' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON `acore_characters`.* TO 'acore'@'127.0.0.1' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON `acore_auth`.* TO 'acore'@'127.0.0.1' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON `acore_playerbots`.* TO 'acore'@'localhost' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON `acore_playerbots`.* TO 'acore'@'127.0.0.1' WITH GRANT OPTION;
FLUSH PRIVILEGES;
'@
    Invoke-MySql -Sql $sql | Out-Null
}

$dbImportArgs = @('--config', $dbImportConfig)
$logPath = New-AutoWoWLogPath -Name 'dbimport'
Invoke-NativeLogged -FilePath $dbImport -ArgumentList $dbImportArgs -WorkingDirectory $serverRoot -LogPath $logPath | Out-Null

$checks = [ordered]@{}
foreach ($db in $expected) {
    $checks[$db] = ((@(Invoke-MySql -Sql "SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$db';"))[0]).Trim()
}
$playerbotTables = ((@(Invoke-MySql -Sql "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='acore_playerbots';"))[0]).Trim()
$checks['acore_playerbots_table_count'] = $playerbotTables

$schemaRows = (@($checks.Keys | ForEach-Object { "| $_ | $($checks[$_]) |" }) -join [Environment]::NewLine)
$report = @(
    '# AutoWoW database report',
    '',
    ('Generated: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')),
    '',
    ('- MySQL CLI: {0}' -f $mysqlExe),
    '- Server address: 127.0.0.1:3306',
    '- Root password: supplied through MYSQL_ROOT_PASSWORD (value intentionally omitted)',
    ('- dbimport log: {0}' -f $logPath),
    '',
    '## Schema checks',
    '',
    '| Schema/check | Result |',
    '|---|---:|',
    $schemaRows,
    '',
    'The acore application password remains the local default used by the generated server configs; the root password is not written here.'
) -join [Environment]::NewLine
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'DATABASE_REPORT.md'), $report, $utf8NoBom)
Write-Output "Database setup and dbimport completed. Report: $(Join-Path $ServerRoot 'DATABASE_REPORT.md')"
