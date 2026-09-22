<#
.SYNOPSIS
    Captures only the exact observer-owned WoW window to a timestamped PNG.

.DESCRIPTION
    Validates the observer session PID, start time, executable, HWND owner, and exact title before
    PrintWindow. If PrintWindow fails or returns a near-uniform black frame, CopyFromScreen may be
    used for that window rectangle only, and only while that exact HWND is foreground. Minimized,
    locked-desktop, identity-mismatch, and persistently black captures fail closed.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDirectory = ''
)

. (Join-Path $PSScriptRoot 'observer-automation-lib.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$sessionPath = Join-Path $ServerRoot 'work\observer-state\observer-session.json'
if (-not (Test-Path -LiteralPath $sessionPath -PathType Leaf)) { throw 'No dedicated observer session manifest exists.' }
$session = Get-Content -LiteralPath $sessionPath -Raw | ConvertFrom-Json
$expected = $session.wow
if (-not $session -or [string]$session.schema -cne 'autowow.observer.session.v1' -or [uint32]$session.observer_guid -eq 0 -or -not $expected) {
    throw 'Observer session manifest is invalid.'
}
$sessionMode = if ($session.PSObject.Properties['session_mode']) { [string]$session.session_mode } else { 'managed' }
if ($sessionMode -notin @('managed','adopted')) { throw 'Observer session manifest has an unsupported session mode.' }
# GUID 21 is the explicit user-designated observer accepted by adoption.  Existing
# managed sessions retain their separate dedicated observer identity contract.
if ($sessionMode -eq 'adopted' -and [uint32]$session.observer_guid -ne 21) {
    throw 'Adopted observer session must identify the user-designated observer GUID 21.'
}
if ($sessionMode -eq 'managed' -and [uint32]$session.observer_guid -eq 21) {
    throw 'Managed observer session cannot use the user-designated observer GUID 21.'
}
if ([uint32]$expected.process_id -eq 0 -or [uint64]$expected.window_handle -eq 0 -or @($expected.allowed_titles).Count -eq 0) {
    throw 'Observer session manifest is missing exact process/HWND/title identity fields.'
}
$approvedExecutable = Get-ObserverApprovedExecutable -ServerRoot $ServerRoot -ExecutablePath ([string]$expected.executable_path)
if (-not $approvedExecutable) { throw 'Observer screenshot refused: manifest executable is outside the explicit approved observer roots.' }
if ($expected.PSObject.Properties['approved_executable_root'] -and
    [string]$expected.approved_executable_root -cne [string]$approvedExecutable.label) {
    throw 'Observer screenshot refused: manifest executable-root label does not match the approved path.'
}

$actualProcess = Get-ObserverActualProcessIdentity -ProcessId ([uint32]$expected.process_id)
if (-not $actualProcess -or -not (Test-ObserverProcessIdentity -Expected $expected -Actual $actualProcess)) {
    throw 'Observer screenshot refused: exact process identity mismatch.'
}
$process = Get-Process -Id ([uint32]$expected.process_id) -ErrorAction Stop
$process.Refresh()
if ([uint64]$process.MainWindowHandle.ToInt64() -ne [uint64]$expected.window_handle) { throw 'Observer screenshot refused: exact HWND mismatch.' }

if (-not ('AutoWowObserverCaptureNative' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class AutoWowObserverCaptureNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
}
'@
}
Add-Type -AssemblyName System.Drawing

$hwnd = [IntPtr][int64]([uint64]$expected.window_handle)
if ([AutoWowObserverCaptureNative]::IsIconic($hwnd)) { throw 'Observer screenshot refused: the observer window is minimized.' }
[uint32]$ownerPid = 0
$null = [AutoWowObserverCaptureNative]::GetWindowThreadProcessId($hwnd, [ref]$ownerPid)
$titleBuilder = [Text.StringBuilder]::new(512)
$null = [AutoWowObserverCaptureNative]::GetWindowText($hwnd, $titleBuilder, $titleBuilder.Capacity)
$actualHwnd = [uint64]$process.MainWindowHandle.ToInt64()
if (-not (Test-ObserverCaptureWindowIdentity -ExpectedProcessId ([uint32]$expected.process_id) `
    -ExpectedWindowHandle ([uint64]$expected.window_handle) -ExpectedExecutablePath ([string]$expected.executable_path) `
    -AllowedTitles @($expected.allowed_titles) -ActualProcessId $ownerPid -ActualWindowHandle $actualHwnd `
    -ActualExecutablePath ([string]$actualProcess.executable_path) -ActualTitle $titleBuilder.ToString())) {
    throw 'Observer screenshot refused: exact PID/HWND/path/title guard failed.'
}

$rect = New-Object AutoWowObserverCaptureNative+RECT
if (-not [AutoWowObserverCaptureNative]::GetWindowRect($hwnd, [ref]$rect)) { throw 'Could not read the observer window rectangle.' }
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -lt 64 -or $height -lt 64 -or $width -gt 8192 -or $height -gt 8192) { throw "Observer window rectangle is unsafe: ${width}x${height}." }

function Test-NearUniformBlackFrame {
    param([Parameter(Mandatory)][Drawing.Bitmap]$Bitmap)
    $dark = 0
    $samples = 0
    for ($x = 0; $x -lt $Bitmap.Width; $x += [Math]::Max(1, [int]($Bitmap.Width / 16))) {
        for ($y = 0; $y -lt $Bitmap.Height; $y += [Math]::Max(1, [int]($Bitmap.Height / 16))) {
            $pixel = $Bitmap.GetPixel($x, $y)
            if ($pixel.R -le 4 -and $pixel.G -le 4 -and $pixel.B -le 4) { $dark++ }
            $samples++
        }
    }
    return ($samples -gt 0 -and ($dark / $samples) -ge 0.98)
}

$bitmap = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
$method = 'PrintWindow'
try {
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try { $printed = [AutoWowObserverCaptureNative]::PrintWindow($hwnd, $hdc, 2) }
        finally { $graphics.ReleaseHdc($hdc) }
    }
    finally { $graphics.Dispose() }

    if (-not $printed -or (Test-NearUniformBlackFrame -Bitmap $bitmap)) {
        $foreground = [uint64](([AutoWowObserverCaptureNative]::GetForegroundWindow()).ToInt64())
        if (-not (Test-ObserverWindowIdentity -ExpectedProcessId ([uint32]$expected.process_id) `
            -ExpectedWindowHandle ([uint64]$expected.window_handle) -ExpectedExecutablePath ([string]$expected.executable_path) `
            -AllowedTitles @($expected.allowed_titles) -ActualProcessId $ownerPid -ActualWindowHandle $actualHwnd `
            -ForegroundWindowHandle $foreground -ActualExecutablePath ([string]$actualProcess.executable_path) -ActualTitle $titleBuilder.ToString())) {
            throw 'PrintWindow failed/black and CopyFromScreen fallback was refused because the exact observer window is not foreground.'
        }
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, [Drawing.Size]::new($width, $height), [Drawing.CopyPixelOperation]::SourceCopy) }
        finally { $graphics.Dispose() }
        $method = 'CopyFromScreen-window-rectangle'
        if (Test-NearUniformBlackFrame -Bitmap $bitmap) { throw 'Observer capture remained black; desktop may be locked or rendering unavailable.' }
    }

    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $ServerRoot 'work\observer-client\Screenshots' }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    $allowedRoot = [IO.Path]::GetFullPath((Join-Path $ServerRoot 'work\observer-client\Screenshots'))
    if (-not ([string]::Equals($OutputDirectory,$allowedRoot,[StringComparison]::OrdinalIgnoreCase) -or
        $OutputDirectory.StartsWith($allowedRoot + '\',[StringComparison]::OrdinalIgnoreCase))) {
        throw 'Screenshot output must remain in the dedicated observer Screenshots directory.'
    }
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $outputPath = Join-Path $OutputDirectory ("observer-{0}.png" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    $bitmap.Save($outputPath, [Drawing.Imaging.ImageFormat]::Png)
    Write-Output "Observer window screenshot captured via ${method}: $outputPath"
}
finally {
    $bitmap.Dispose()
}
