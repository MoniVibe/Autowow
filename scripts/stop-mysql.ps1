[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$MysqlRoot = $(if ($env:MYSQL_ROOT_DIR) { $env:MYSQL_ROOT_DIR } else { '' })
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$pidPath = Join-Path $ServerRoot 'mysql.pid'
if (-not (Test-Path -LiteralPath $pidPath)) {
    Write-Output 'No AutoWoW-managed MySQL PID file was found.'
    exit 0
}

$raw = (Get-Content -LiteralPath $pidPath -Raw).Trim()
$mysqlPid = 0
if (-not [int]::TryParse($raw, [ref]$mysqlPid)) { throw "Invalid MySQL PID file: $pidPath" }
$process = Get-Process -Id $mysqlPid -ErrorAction SilentlyContinue
if (-not $process) {
    Remove-Item -LiteralPath $pidPath -Force
    Write-Output 'AutoWoW-managed MySQL was not running; removed the stale PID file.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($env:MYSQL_ROOT_PASSWORD)) {
    throw 'Set MYSQL_ROOT_PASSWORD before stopping the managed MySQL instance so shutdown is graceful.'
}
if ([string]::IsNullOrWhiteSpace($MysqlRoot)) {
    $MysqlRoot = Get-FirstExistingPath -Candidates @(
        (Join-Path $ServerRoot 'third_party\mysql'),
        'C:\Program Files\MySQL\MySQL Server 8.4',
        'C:\Program Files\MySQL\MySQL Server 8.0'
    )
}
$mysqlAdmin = if ($MysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $MysqlRoot 'bin\mysqladmin.exe')) } else { $null }
if (-not $mysqlAdmin) { throw 'mysqladmin.exe was not found.' }

$oldPwd = $env:MYSQL_PWD
$env:MYSQL_PWD = $env:MYSQL_ROOT_PASSWORD
try {
    & $mysqlAdmin '--protocol=tcp' '--host=127.0.0.1' '--port=3306' '--user=root' 'shutdown' 2>&1 | Out-Null
    $exitCode = $LASTEXITCODE
}
finally {
    if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
}
if ($exitCode -ne 0) { throw "mysqladmin shutdown failed with exit code $exitCode." }

for ($attempt = 1; $attempt -le 30; $attempt++) {
    $process.Refresh()
    if ($process.HasExited) { break }
    Start-Sleep -Seconds 1
}
if (-not $process.HasExited) { throw "MySQL PID $mysqlPid did not exit after graceful shutdown." }
Remove-Item -LiteralPath $pidPath -Force
Write-Output "MySQL stopped (PID $mysqlPid)."
