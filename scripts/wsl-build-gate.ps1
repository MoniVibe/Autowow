<##
.SYNOPSIS
    Invoke one safety-gated AutoWoW WSL build target.

.EXAMPLE
    .\wsl-build-gate.ps1 -BuildDirectory /root/autowow-phase1-ext4/build-tests `
        -Target worldserver -DryRun

.EXAMPLE
    .\wsl-build-gate.ps1 -Tool Ninja -BuildDirectory /root/autowow-phase1-ext4/build-tests `
        -Target unit_tests -Wait

    A real invocation is intentionally not performed by this lane's tests or
    proof.  Use -DryRun to inspect the exact argv vector and preflight result.
##>
[CmdletBinding()]
param(
    [ValidateSet('CMake', 'Ninja')][string]$Tool = 'CMake',
    [string]$BuildDirectory = '/root/autowow-phase1-ext4/build-tests',
    [AllowEmptyCollection()][string[]]$Target = @('worldserver'),
    [string]$WslDistro = 'Ubuntu-24.04',
    [ValidateRange(0, 12)][int]$Jobs = 0,
    [ValidateRange(256, 1048576)][int]$MinimumAvailableMemoryMiB = 4096,
    [switch]$Wait,
    [ValidateRange(1, 86400)][int]$WaitTimeoutSeconds = 300,
    [ValidateRange(100, 10000)][int]$LockPollMilliseconds = 1000,
    [switch]$DryRun,
    [string]$LockName = 'Global\AutoWoW.WslBuildGate.v1',
    [string]$ReceiptPath = '',
    [string]$OutputLogPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$libraryPath = Join-Path $PSScriptRoot 'wsl-build-gate-lib.ps1'
if (-not (Test-Path -LiteralPath $libraryPath -PathType Leaf)) {
    throw "Build-gate library is missing: $libraryPath"
}
. $libraryPath

try {
    $result = Invoke-WslBuildGate @PSBoundParameters
    $result | ConvertTo-Json -Depth 20
    exit 0
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}
