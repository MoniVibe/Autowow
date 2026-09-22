<#
.SYNOPSIS
    Launches the isolated observer client, logs it in, verifies bridge protection, then starts relocation.

.DESCRIPTION
    Uses only Windows user-session facilities (WScript.Shell and user32). Before every SendKeys call,
    the foreground HWND, owning PID, executable path, and exact window title must match the process
    launched from work\observer-client. The DPAPI password is never passed as an argument or logged.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32]$LeaderGuid = 10,
    [ValidateRange(5, 300)][int]$IntervalSeconds = 15,
    [ValidateRange(1, 1440)][int]$DurationMinutes = 720,
    [ValidateRange(5, 120)][int]$ClientWindowTimeoutSeconds = 45,
    [ValidateRange(5, 120)][int]$LoginSettleSeconds = 12,
    [ValidateRange(30, 300)][int]$ObserverOnlineTimeoutSeconds = 90,
    [string[]]$AllowedWindowTitle = @('World of Warcraft')
)

. (Join-Path $PSScriptRoot 'observer-automation-lib.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$stateDirectory = Join-Path $ServerRoot 'work\observer-state'
$statePath = Join-Path $stateDirectory 'observer-state.json'
$secretPath = Join-Path $stateDirectory 'observer-password.clixml'
$clientDirectory = Join-Path $ServerRoot 'work\observer-client'
$clientManifestPath = Join-Path $clientDirectory 'observer-client-manifest.json'
$sessionPath = Join-Path $stateDirectory 'observer-session.json'
$observerControl = Join-Path $PSScriptRoot 'observer-control.ps1'
$relocateHost = Join-Path $PSScriptRoot 'observer-relocate-host.ps1'
foreach ($required in @($statePath,$secretPath,$clientManifestPath,$observerControl,$relocateHost)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing observer automation prerequisite: $required" }
}
if (Test-Path -LiteralPath $sessionPath) { throw "Observer session manifest already exists. Run stop-observer-automation.ps1 first: $sessionPath" }

$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
$clientManifest = Get-Content -LiteralPath $clientManifestPath -Raw | ConvertFrom-Json
if ([string]$state.account_name -cne 'AUTOWATCH' -or [string]$state.character_name -cne 'Autowatch') { throw 'Observer state does not match the dedicated identity contract.' }
if ([uint64]$state.account_id -eq 3 -or [uint64]$state.character_guid -eq 21) { throw 'Observer state references a forbidden legacy identity.' }
if ([int]$state.race -ne 1 -or [int]$state.gmlevel -ne 3 -or [bool]$state.group_member) { throw 'Observer state is not human, GM3, and ungrouped.' }
$observerGuid = [uint32]$state.character_guid
$wowPath = [IO.Path]::GetFullPath([string]$clientManifest.executable_path)
if (-not $wowPath.StartsWith($clientDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Observer executable is outside the dedicated client directory.' }
if (-not (Test-Path -LiteralPath $wowPath -PathType Leaf)) { throw "Observer Wow.exe is missing: $wowPath" }
$configText = Get-Content -LiteralPath (Join-Path $clientDirectory 'WTF\Config.wtf') -Raw
if ($configText -notmatch '(?m)^SET accountName "AUTOWATCH"$' -or $configText -match '(?i)password') { throw 'Observer Config.wtf must save only AUTOWATCH and must not contain a password field.' }

if (-not ('AutoWowObserverWindowNative' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class AutoWowObserverWindowNative {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
    [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool value);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
}
'@
}

function Get-ObserverWindowSnapshot {
    param([Parameter(Mandatory)][Diagnostics.Process]$Process)

    $Process.Refresh()
    $handle = [uint64]$Process.MainWindowHandle.ToInt64()
    if ($handle -eq 0) { return $null }
    [uint32]$ownerPid = 0
    $null = [AutoWowObserverWindowNative]::GetWindowThreadProcessId([IntPtr]$Process.MainWindowHandle, [ref]$ownerPid)
    $builder = [Text.StringBuilder]::new(512)
    $null = [AutoWowObserverWindowNative]::GetWindowText([IntPtr]$Process.MainWindowHandle, $builder, $builder.Capacity)
    $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($Process.Id)" -ErrorAction Stop
    [pscustomobject]@{
        process_id = $ownerPid
        window_handle = $handle
        foreground_handle = [uint64](([AutoWowObserverWindowNative]::GetForegroundWindow()).ToInt64())
        executable_path = [string]$cim.ExecutablePath
        title = $builder.ToString()
    }
}

$shell = New-Object -ComObject WScript.Shell
function Send-ObserverKeys {
    param(
        [Parameter(Mandatory)][Diagnostics.Process]$Process,
        [Parameter(Mandatory)][uint64]$ExpectedWindowHandle,
        [Parameter(Mandatory)][string]$Keys,
        [string]$Step = 'unspecified'
    )

    # Windows can legally defer foreground activation while another process is yielding focus.
    # Retry activation for a bounded window, but never send keys until every identity guard passes.
    $actual = $null
    $identityMatched = $false
    for ($attempt = 1; $attempt -le 20 -and -not $identityMatched; $attempt++) {
        $null = $shell.AppActivate($Process.Id)
        $null = [AutoWowObserverWindowNative]::SetForegroundWindow([IntPtr][int64]$ExpectedWindowHandle)
        Start-Sleep -Milliseconds 250
        $actual = Get-ObserverWindowSnapshot -Process $Process
        $identityMatched = $actual -and (Test-ObserverWindowIdentity -ExpectedProcessId $Process.Id `
            -ExpectedWindowHandle $ExpectedWindowHandle -ExpectedExecutablePath $wowPath -AllowedTitles $AllowedWindowTitle `
            -ActualProcessId $actual.process_id -ActualWindowHandle $actual.window_handle `
            -ForegroundWindowHandle $actual.foreground_handle -ActualExecutablePath $actual.executable_path -ActualTitle $actual.title)
    }
    if (-not $identityMatched) {
        # Foreground ownership can be denied while another application owns the input queue.
        # Temporarily attach the current thread to both queues, activate the exact verified HWND,
        # then detach before sending anything. No key is sent unless foreground verification passes.
        $actual = Get-ObserverWindowSnapshot -Process $Process
        if (-not $actual -or -not (Test-ObserverCaptureWindowIdentity -ExpectedProcessId $Process.Id `
            -ExpectedWindowHandle $ExpectedWindowHandle -ExpectedExecutablePath $wowPath -AllowedTitles $AllowedWindowTitle `
            -ActualProcessId $actual.process_id -ActualWindowHandle $actual.window_handle `
            -ActualExecutablePath $actual.executable_path -ActualTitle $actual.title)) {
            throw "Target-window identity guard refused input at step '$Step'; no keys were sent."
        }
        $hwnd = [IntPtr][int64]$ExpectedWindowHandle
        [uint32]$ignoredPid = 0
        [uint32]$targetThread = [AutoWowObserverWindowNative]::GetWindowThreadProcessId($hwnd, [ref]$ignoredPid)
        $foregroundHwnd = [AutoWowObserverWindowNative]::GetForegroundWindow()
        [uint32]$foregroundPid = 0
        [uint32]$foregroundThread = if ($foregroundHwnd -ne [IntPtr]::Zero) {
            [AutoWowObserverWindowNative]::GetWindowThreadProcessId($foregroundHwnd, [ref]$foregroundPid)
        }
        else { 0 }
        [uint32]$currentThread = [AutoWowObserverWindowNative]::GetCurrentThreadId()
        $attachedForeground = $false
        $attachedTarget = $false
        try {
            if ($foregroundThread -ne 0 -and $foregroundThread -ne $currentThread) {
                $attachedForeground = [AutoWowObserverWindowNative]::AttachThreadInput($currentThread, $foregroundThread, $true)
            }
            if ($targetThread -ne 0 -and $targetThread -ne $currentThread) {
                $attachedTarget = [AutoWowObserverWindowNative]::AttachThreadInput($currentThread, $targetThread, $true)
            }
            $null = [AutoWowObserverWindowNative]::ShowWindow($hwnd, 9)
            $null = [AutoWowObserverWindowNative]::BringWindowToTop($hwnd)
            $null = [AutoWowObserverWindowNative]::SetForegroundWindow($hwnd)
            $null = [AutoWowObserverWindowNative]::SetActiveWindow($hwnd)
            $null = [AutoWowObserverWindowNative]::SetFocus($hwnd)
        }
        finally {
            if ($attachedTarget) { $null = [AutoWowObserverWindowNative]::AttachThreadInput($currentThread, $targetThread, $false) }
            if ($attachedForeground) { $null = [AutoWowObserverWindowNative]::AttachThreadInput($currentThread, $foregroundThread, $false) }
        }
        Start-Sleep -Milliseconds 250
        $actual = Get-ObserverWindowSnapshot -Process $Process
        if (-not $actual -or -not (Test-ObserverWindowIdentity -ExpectedProcessId $Process.Id `
            -ExpectedWindowHandle $ExpectedWindowHandle -ExpectedExecutablePath $wowPath -AllowedTitles $AllowedWindowTitle `
            -ActualProcessId $actual.process_id -ActualWindowHandle $actual.window_handle `
            -ForegroundWindowHandle $actual.foreground_handle -ActualExecutablePath $actual.executable_path -ActualTitle $actual.title)) {
            throw "Forced foreground identity guard refused SendKeys at step '$Step'; no keys were sent."
        }
    }
    $shell.SendKeys($Keys)
}

function Invoke-ObserverControlJson {
    param([Parameter(Mandatory)][ValidateSet('status','protect')][string]$Action)
    $raw = & $observerControl -Action $Action -ObserverGuid $observerGuid
    $line = @($raw | ForEach-Object { $_.ToString() }) | Select-Object -Last 1
    if ([string]::IsNullOrWhiteSpace($line)) { throw "Observer bridge returned no response for $Action." }
    return $line | ConvertFrom-Json
}

$wow = $null
$relocate = $null
try {
    $wow = Start-Process -FilePath $wowPath -WorkingDirectory $clientDirectory -PassThru
    $windowDeadline = (Get-Date).AddSeconds($ClientWindowTimeoutSeconds)
    $window = $null
    do {
        Start-Sleep -Milliseconds 500
        if ($wow.HasExited) { throw "Observer WoW exited before exposing its window (exit $($wow.ExitCode))." }
        $window = Get-ObserverWindowSnapshot -Process $wow
    } while ((-not $window -or $window.title -notin $AllowedWindowTitle -or $window.executable_path -ine $wowPath) -and (Get-Date) -lt $windowDeadline)
    if (-not $window -or $window.title -notin $AllowedWindowTitle -or $window.executable_path -ine $wowPath) {
        throw 'Timed out waiting for the exact observer WoW window identity.'
    }
    $expectedHwnd = [uint64]$window.window_handle

    $sessionToken = [guid]::NewGuid().ToString('N')
    $session = [pscustomobject][ordered]@{
        schema = 'autowow.observer.session.v1'
        session_token = $sessionToken
        observer_guid = $observerGuid
        leader_guid = $LeaderGuid
        started_utc = (Get-Date).ToUniversalTime().ToString('o')
        wow = [pscustomobject]@{
            process_id = [uint32]$wow.Id
            start_time_utc = $wow.StartTime.ToUniversalTime().ToString('o')
            executable_path = $wowPath
            required_command_line_tokens = @()
            window_handle = $expectedHwnd
            allowed_titles = $AllowedWindowTitle
        }
        relocate = $null
    }
    [IO.File]::WriteAllText($sessionPath, ($session | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))

    # Config.wtf pre-fills AUTOWATCH, but this 3.3.5a client leaves focus in the account box.
    # Move focus once to the password box through the same exact-window guard before exposing
    # the alphanumeric DPAPI secret to SendKeys.
    Send-ObserverKeys -Process $wow -ExpectedWindowHandle $expectedHwnd -Keys '{TAB}' -Step 'focus-password'
    $secure = Import-ObserverPassword -Path $secretPath
    Use-ObserverPlainTextPassword -Password $secure -Action {
        param($plainText)
        if ($plainText.Length -gt 16 -or $plainText -notmatch '^[A-Za-z0-9]+$') { throw 'DPAPI secret is outside the WoW-compatible contract.' }
        Send-ObserverKeys -Process $wow -ExpectedWindowHandle $expectedHwnd -Keys $plainText -Step 'password'
    }
    Send-ObserverKeys -Process $wow -ExpectedWindowHandle $expectedHwnd -Keys '{ENTER}' -Step 'submit-login'
    Start-Sleep -Seconds $LoginSettleSeconds

    # Provisioning enforces exactly one character on AUTOWATCH. Enter therefore has one deterministic target.
    Send-ObserverKeys -Process $wow -ExpectedWindowHandle $expectedHwnd -Keys '{ENTER}' -Step 'enter-world'

    $onlineDeadline = (Get-Date).AddSeconds($ObserverOnlineTimeoutSeconds)
    $status = $null
    do {
        Start-Sleep -Seconds 1
        try { $status = Invoke-ObserverControlJson -Action status } catch { $status = $null }
        if ($status -and -not $status.ok -and $status.error -eq 'not_an_observer') {
            throw "Observer GUID $observerGuid is not enrolled in AutoWow.ObserverGuids. No relocation was started."
        }
    } while ((-not $status -or -not $status.ok) -and (Get-Date) -lt $onlineDeadline)
    if (-not $status -or -not $status.ok) { throw 'Timed out waiting for the dedicated observer to become bridge-visible online.' }
    if ([uint32]$status.observer -ne $observerGuid -or [string]$status.name -cne 'Autowatch') { throw 'Bridge returned the wrong observer identity.' }

    $protected = Invoke-ObserverControlJson -Action protect
    if (-not $protected.ok) { throw "Bridge refused observer protection: $($protected.error)" }
    $status = Invoke-ObserverControlJson -Action status
    if (-not $status.ok -or -not [bool]$status.protected -or -not [bool]$status.is_gm -or [bool]$status.gm_visible -or [bool]$status.in_group -or [bool]$status.in_combat) {
        throw 'Observer bridge protection/no-group verification failed. No relocation was started.'
    }

    $receiptPath = Join-Path $ServerRoot ("logs\observer-relocate-dedicated-{0}.jsonl" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $hostExecutable = (Get-Process -Id $PID).MainModule.FileName
    $stdoutPath = Join-Path $ServerRoot ("logs\observer-relocate-host-{0}.stdout.log" -f $sessionToken)
    $stderrPath = Join-Path $ServerRoot ("logs\observer-relocate-host-{0}.stderr.log" -f $sessionToken)
    $arguments = @('-NoLogo','-NoProfile','-File',$relocateHost,'-ServerRoot',$ServerRoot,'-ObserverGuid',[string]$observerGuid,
        '-LeaderGuid',[string]$LeaderGuid,'-SessionToken',$sessionToken,'-ReceiptPath',$receiptPath,
        '-IntervalSeconds',[string]$IntervalSeconds,'-DurationMinutes',[string]$DurationMinutes)
    $relocate = Start-Process -FilePath $hostExecutable -ArgumentList $arguments -WorkingDirectory $ServerRoot `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    if ($relocate.HasExited) { throw "Observer relocation host exited immediately. See $stderrPath" }

    $session.relocate = [pscustomobject]@{
        process_id = [uint32]$relocate.Id
        start_time_utc = $relocate.StartTime.ToUniversalTime().ToString('o')
        executable_path = $hostExecutable
        required_command_line_tokens = @('observer-relocate-host.ps1',$sessionToken,[string]$observerGuid,[string]$LeaderGuid)
        receipt_path = $receiptPath
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
    }
    [IO.File]::WriteAllText($sessionPath, ($session | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    Write-Output "Dedicated observer is online, GM-protected, ungrouped, and relocating. Session: $sessionPath"
}
catch {
    if ($relocate -and -not $relocate.HasExited) { Stop-Process -Id $relocate.Id -Force -ErrorAction SilentlyContinue }
    if ($wow -and -not $wow.HasExited) {
        $null = $wow.CloseMainWindow()
        if (-not $wow.WaitForExit(5000)) { Stop-Process -Id $wow.Id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item -LiteralPath $sessionPath -Force -ErrorAction SilentlyContinue
    throw
}
