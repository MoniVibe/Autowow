[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$AuthOnly
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$installRoot = Join-Path $ServerRoot 'server'
$auth = Join-Path $installRoot 'authserver.exe'
$world = Join-Path $installRoot 'worldserver.exe'
$configRoot = Join-Path $installRoot 'configs'
foreach ($required in @($auth,$world,(Join-Path $configRoot 'authserver.conf'),(Join-Path $configRoot 'worldserver.conf'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Server prerequisite missing: $required" }
}
Initialize-AutoWoWLayout -Root $ServerRoot

function Start-ServerProcess {
    param([string]$Name, [string]$Executable)
    $pidFile = Join-Path $ServerRoot ("{0}.pid" -f $Name)
    if (Test-Path -LiteralPath $pidFile) {
        $oldId = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
        $old = Get-Process -Id $oldId -ErrorAction SilentlyContinue
        if ($old) { throw "$Name is already running with PID $oldId" }
        Remove-Item -LiteralPath $pidFile -Force
    }

    $stdout = Join-Path $installRoot ("logs\{0}-stdout.log" -f $Name)
    $stderr = Join-Path $installRoot ("logs\{0}-stderr.log" -f $Name)
    $process = Start-Process -FilePath $Executable -WorkingDirectory $installRoot -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru
    Set-Content -LiteralPath $pidFile -Value $process.Id -NoNewline
    Start-Sleep -Seconds 2
    $process.Refresh()
    if ($process.HasExited) { throw "$Name exited immediately with code $($process.ExitCode). See $stdout and $stderr" }
    Write-Output "$Name started with PID $($process.Id)"
}

Start-ServerProcess -Name 'authserver' -Executable $auth
if ($AuthOnly) {
    Write-Output 'Authserver is running; worldserver startup was intentionally skipped.'
    return
}
Start-ServerProcess -Name 'worldserver' -Executable $world
Write-Output 'Server processes are running. Use scripts\smoke-test.ps1 for checks and scripts\stop-server.ps1 to stop them.'
