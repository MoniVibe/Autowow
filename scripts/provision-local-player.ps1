[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot)
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
if ([string]::IsNullOrWhiteSpace($env:WOW_ACCOUNT_NAME) -or [string]::IsNullOrWhiteSpace($env:WOW_ACCOUNT_PASSWORD) -or [string]::IsNullOrWhiteSpace($env:MYSQL_ROOT_PASSWORD)) {
    throw 'Set WOW_ACCOUNT_NAME, WOW_ACCOUNT_PASSWORD, and MYSQL_ROOT_PASSWORD in this PowerShell process. Secrets are not accepted as arguments or recorded.'
}

$worldPidPath = Join-Path $ServerRoot 'worldserver.pid'
$authPidPath = Join-Path $ServerRoot 'authserver.pid'
$worldWasRunning = Test-Path -LiteralPath $worldPidPath
$authWasRunning = Test-Path -LiteralPath $authPidPath
if ($worldWasRunning -or $authWasRunning) { & (Join-Path $PSScriptRoot 'stop-server.ps1') -ServerRoot $ServerRoot }
try {
    & (Join-Path $PSScriptRoot 'create-account.ps1') -ServerRoot $ServerRoot
}
finally {
    if ($worldWasRunning -or $authWasRunning) { & (Join-Path $PSScriptRoot 'start-server.ps1') -ServerRoot $ServerRoot }
}
