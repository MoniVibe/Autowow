[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot)
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$installRoot = [System.IO.Path]::GetFullPath((Join-Path $ServerRoot 'server'))

foreach ($name in @('worldserver','authserver')) {
    $pidFile = Join-Path $ServerRoot ("{0}.pid" -f $name)
    if (-not (Test-Path -LiteralPath $pidFile)) { continue }
    $processId = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($process) {
        $path = $null
        try { $path = $process.Path } catch { }
        if ($path -and ([System.IO.Path]::GetFullPath($path)).StartsWith($installRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $processId -ErrorAction Stop
            $process.WaitForExit(10000)
            Write-Output "$name stopped (PID $processId)."
        }
        else {
            Write-Warning "PID $processId is not an AutoWoW server process; it was not stopped."
        }
    }
    Remove-Item -LiteralPath $pidFile -Force
}
