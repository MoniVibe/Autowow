[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory)][uint32]$ObserverGuid,
    [Parameter(Mandatory)][uint32]$LeaderGuid,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{32}$')][string]$SessionToken,
    [Parameter(Mandatory)][string]$ReceiptPath,
    [ValidateRange(5, 300)][int]$IntervalSeconds = 15,
    [ValidateRange(1, 1440)][int]$DurationMinutes = 720
)

$ErrorActionPreference = 'Stop'
if ($ObserverGuid -eq 21) { throw 'Forbidden observer GUID 21.' }
$relocate = Join-Path $PSScriptRoot 'observer-relocate.ps1'
if (-not (Test-Path -LiteralPath $relocate -PathType Leaf)) { throw "Missing observer relocation dependency: $relocate" }

# SessionToken is intentionally unused by relocation logic. It is a high-entropy ownership marker
# embedded in this exact host process command line so stop-observer-automation.ps1 cannot match an
# unrelated PowerShell process that happens to run observer-relocate.ps1.
& $relocate -ServerRoot $ServerRoot -ObserverGuid $ObserverGuid -LeaderGuid $LeaderGuid `
    -IntervalSeconds $IntervalSeconds -DurationMinutes $DurationMinutes -Execute -ReceiptPath $ReceiptPath
