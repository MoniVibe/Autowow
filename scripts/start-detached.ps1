<#
.SYNOPSIS
    Start a long-running command outside the caller's process tree, so it survives the caller (and the
    Claude Code session) being recycled. Uses WMI Win32_Process.Create: the child's parent is the WMI
    service, not this shell. Output goes to a log file.
.EXAMPLE
    .\start-detached.ps1 -Script .\start-phase1-wsl-worldserver.ps1 -Arguments "-WorldserverBinary '/root/...'" -Log ..\logs\phase1-runtime\detached-start.log
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$Arguments = '',
    [Parameter(Mandatory = $true)][string]$Log
)
$ErrorActionPreference = 'Stop'
$powershellExe = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path -LiteralPath $powershellExe)) { throw 'Native Windows PowerShell 5.1 is required.' }
$scriptPath = (Resolve-Path -LiteralPath $Script).Path
$logPath = [System.IO.Path]::GetFullPath($Log)
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = "cmd.exe /c `"`"$powershellExe`" -NoProfile -File `"$scriptPath`" $Arguments > `"$logPath`" 2>&1`""
    CurrentDirectory = (Split-Path -Parent $scriptPath)
}
if ($r.ReturnValue -ne 0) { throw "Win32_Process.Create failed: $($r.ReturnValue)" }
"detached pid $($r.ProcessId), log $logPath"
