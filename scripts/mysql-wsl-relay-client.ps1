[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgumentList
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$serverRoot = Split-Path -Parent $PSScriptRoot
$statusPath = Join-Path $serverRoot 'work\phase1-wsl-runtime\mysql-relay.json'
if (-not (Test-Path -LiteralPath $statusPath -PathType Leaf)) {
    throw "The private Phase 1 MySQL relay is not running: $statusPath"
}

$relay = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
if ([string]$relay.schema -cne 'autowow.phase1.mysql-relay.v1' -or
    -not [bool]$relay.private_wsl_adapter_only) {
    throw 'The Phase 1 MySQL relay status contract is invalid or not private.'
}

# The proof harness supplies the Windows worldserver endpoint (127.0.0.1:3306).
# Replace only that transport pair; preserve the database, user, encoding, batch mode,
# and read-only SQL exactly as supplied. MYSQL_PWD remains an inherited process secret.
$forwarded = @($ArgumentList | Where-Object { $_ -notmatch '^--(?:host|port)=' })
$forwarded += "--host=$([string]$relay.listen_address)"
$forwarded += "--port=$([int]$relay.listen_port)"

& wsl.exe -d Ubuntu-24.04 -- env "MYSQL_PWD=$env:MYSQL_PWD" mysql @forwarded
exit $LASTEXITCODE
