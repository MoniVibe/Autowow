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
    throw 'Set MYSQL_ROOT_PASSWORD in the process environment before running start-mysql.ps1. The password is never accepted as a script argument or written to reports.'
}

if ([string]::IsNullOrWhiteSpace($MysqlRoot)) {
    $MysqlRoot = Get-FirstExistingPath -Candidates @(
        (Join-Path $ServerRoot 'third_party\mysql'),
        'C:\Program Files\MySQL\MySQL Server 8.4',
        'C:\Program Files\MySQL\MySQL Server 8.0'
    )
}
$MysqlRoot = if ($MysqlRoot) { (Resolve-Path -LiteralPath $MysqlRoot).Path } else { $null }
$mysqld = if ($MysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $MysqlRoot 'bin\mysqld.exe')) } else { $null }
$mysql = if ($MysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $MysqlRoot 'bin\mysql.exe')) } else { $null }
if (-not $mysqld -or -not $mysql) { throw 'MySQL server binaries were not found. Set MYSQL_ROOT_DIR or stage MySQL under third_party\mysql.' }

$installRoot = Join-Path $ServerRoot 'server'
$dataDir = Join-Path $installRoot 'mysql-data'
$configPath = Join-Path $installRoot 'mysql-auto.ini'
$pidPath = Join-Path $ServerRoot 'mysql.pid'
$mysqlLog = Join-Path $installRoot 'logs\mysqld.log'
$startupLog = New-AutoWoWLogPath -Name 'mysql-start'

function Test-TcpPort {
    param([string]$Address, [int]$Port)
    $tcp = $null
    try {
        $tcp = [System.Net.Sockets.TcpClient]::new()
        $tcp.Connect($Address, $Port)
        return $true
    }
    catch {
        return $false
    }
    finally {
        if ($tcp) { $tcp.Dispose() }
    }
}

function Get-ManagedProcess {
    if (-not (Test-Path -LiteralPath $pidPath)) { return $null }
    $raw = (Get-Content -LiteralPath $pidPath -Raw).Trim()
    $managedId = 0
    if (-not [int]::TryParse($raw, [ref]$managedId)) { return $null }
    return (Get-Process -Id $managedId -ErrorAction SilentlyContinue)
}

$managed = Get-ManagedProcess
if ($managed) {
    if (Test-TcpPort -Address '127.0.0.1' -Port 3306) {
        Write-Output "Managed MySQL is already running with PID $($managed.Id)."
        exit 0
    }
    throw "Managed MySQL PID $($managed.Id) exists but port 3306 is not accepting connections. See $mysqlLog"
}
if (Test-Path -LiteralPath $pidPath) { Remove-Item -LiteralPath $pidPath -Force }

if (Test-TcpPort -Address '127.0.0.1' -Port 3306) {
    throw 'Port 3306 is already occupied by an unmanaged process. Refusing to attach to it.'
}

New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
$baseDirIni = $MysqlRoot.Replace('\', '/')
$dataDirIni = $dataDir.Replace('\', '/')
$mysqlLogIni = $mysqlLog.Replace('\', '/')
$config = @"
[mysqld]
basedir="$baseDirIni"
datadir="$dataDirIni"
bind-address=127.0.0.1
port=3306
mysqlx=0
character-set-server=utf8mb4
collation-server=utf8mb4_unicode_ci
log-error="$mysqlLogIni"
pid-file="$($pidPath.Replace('\', '/'))"
"@
[System.IO.File]::WriteAllText($configPath, $config, [System.Text.UTF8Encoding]::new($false))

$fresh = -not (Test-Path -LiteralPath (Join-Path $dataDir 'mysql'))
if ($fresh) {
    $initLog = New-AutoWoWLogPath -Name 'mysql-initialize'
    Invoke-NativeLogged -FilePath $mysqld -ArgumentList @("--defaults-file=$configPath", '--initialize-insecure') -WorkingDirectory $MysqlRoot -LogPath $initLog | Out-Null
}

$stdout = Join-Path $installRoot 'logs\mysqld-stdout.log'
$stderr = Join-Path $installRoot 'logs\mysqld-stderr.log'
$process = Start-Process -FilePath $mysqld -ArgumentList @("--defaults-file=$configPath") -WorkingDirectory $MysqlRoot -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru
Set-Content -LiteralPath $pidPath -Value $process.Id -NoNewline

$ready = $false
for ($attempt = 1; $attempt -le 60; $attempt++) {
    $process.Refresh()
    if ($process.HasExited) {
        throw "mysqld exited during startup with code $($process.ExitCode). See $mysqlLog, $stdout, and $stderr"
    }
    if (Test-TcpPort -Address '127.0.0.1' -Port 3306) {
        $ready = $true
        break
    }
    Start-Sleep -Seconds 1
}
if (-not $ready) { throw "MySQL did not accept connections within 60 seconds. See $mysqlLog, $stdout, and $stderr" }

function ConvertTo-MySqlStringLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)
    $escaped = $Value.Replace('\', '\\').Replace("'", "''").Replace("`0", '\0').Replace("`r", '\r').Replace("`n", '\n')
    return "'$escaped'"
}

function Invoke-MySql {
    param(
        [Parameter(Mandatory = $true)][string]$Sql,
        [string]$Address = '127.0.0.1',
        [switch]$NoPassword
    )
    $oldPwd = $env:MYSQL_PWD
    try {
        if ($NoPassword) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $env:MYSQL_ROOT_PASSWORD }
        $args = @('--protocol=tcp', "--host=$Address", '--port=3306', '--user=root', '--batch', '--skip-column-names')
        $output = $Sql | & $mysql @args 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
    }
    if ($exitCode -ne 0) { throw "MySQL command failed: $($output -join ' ')" }
    return @($output)
}

if ($fresh) {
    $passwordLiteral = ConvertTo-MySqlStringLiteral -Value $env:MYSQL_ROOT_PASSWORD
    $setPasswordSql = "ALTER USER 'root'@'localhost' IDENTIFIED BY $passwordLiteral; FLUSH PRIVILEGES;"
    try {
        Invoke-MySql -Sql $setPasswordSql -Address 'localhost' -NoPassword | Out-Null
    }
    catch {
        throw "Fresh MySQL root password initialization failed: $($_.Exception.Message). See $mysqlLog"
    }
}
else {
    Invoke-MySql -Sql 'SELECT 1;' | Out-Null
}

$report = @"
# AutoWoW MySQL report

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

- MySQL root: $MysqlRoot
- Server endpoint: 127.0.0.1:3306
- Data directory: $dataDir
- Root password: supplied through `MYSQL_ROOT_PASSWORD` (value intentionally omitted)
- Startup log: $startupLog
- MySQL error log: $mysqlLog
"@
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'MYSQL_REPORT.md'), $report, [System.Text.UTF8Encoding]::new($false))
Write-Output "MySQL is running on 127.0.0.1:3306 with PID $($process.Id). Report: $(Join-Path $ServerRoot 'MYSQL_REPORT.md')"
