<#
.SYNOPSIS
    Explicitly adopts an already-running, user-designated WoW observer window.

.DESCRIPTION
    This is a file-and-read-only-observation operation. It never launches, focuses,
    sends input to, relocates, or closes WoW. The caller must opt in with -Adopt.

    Adoption is currently restricted to the user-designated observer character GUID
    21. Before the session manifest is written, this script binds the exact process
    PID, process start time, approved executable path, HWND, and exact window title,
    then performs the read-only bridge command `observe status 21`. The bridge reply
    must prove protected=true, is_gm=true, gm_visible=false, in_group=false, and
    in_combat=false.

    The resulting session is marked session_mode=adopted and is not owned by the
    launch/relocation automation. No credentials are read or inferred.
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory = $true)][switch]$Adopt,
    [Parameter(Mandatory = $true)][ValidateRange(1, [uint32]::MaxValue)][uint32]$ProcessId,
    [ValidateSet(21)][uint32]$ObserverGuid = 21,
    [ValidateNotNullOrEmpty()][string]$ExpectedTitle = 'World of Warcraft',
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [switch]$ReplaceExistingSession
)

. (Join-Path $PSScriptRoot 'observer-automation-lib.ps1')

$ErrorActionPreference = 'Stop'
if (-not $Adopt) { throw 'Adoption is opt-in. Re-run with -Adopt.' }
if ($ObserverGuid -ne 21) { throw 'User-designated observer adoption is restricted to observer GUID 21.' }

$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
$stateDirectory = Join-Path $ServerRoot 'work\observer-state'
$sessionPath = Join-Path $stateDirectory 'observer-session.json'
$controlPath = Join-Path $PSScriptRoot 'observer-control.ps1'
if (-not (Test-Path -LiteralPath $controlPath -PathType Leaf)) { throw "Missing read-only observer control client: $controlPath" }

if ((Test-Path -LiteralPath $sessionPath -PathType Leaf) -and -not $ReplaceExistingSession) {
    throw "Observer session manifest already exists. Pass -ReplaceExistingSession only after explicitly reviewing it: $sessionPath"
}

# Process and window inspection is read-only.  Get-ObserverActualProcessIdentity
# fails closed if the process disappears or its executable path cannot be read.
$processIdentity = Get-ObserverActualProcessIdentity -ProcessId $ProcessId
if (-not $processIdentity) { throw "No process with PID $ProcessId could be inspected." }
$approvedExecutable = Get-ObserverApprovedExecutable -ServerRoot $ServerRoot -ExecutablePath ([string]$processIdentity.executable_path)
if (-not $approvedExecutable) {
    throw "Observer adoption refused: executable is outside the explicit approved Wow.exe roots (current wow-client or validated observer-client). Path: $($processIdentity.executable_path)"
}

$windowIdentity = Get-ObserverActualWindowIdentity -ProcessId $ProcessId
if (-not $windowIdentity) { throw "PID $ProcessId has no readable top-level WoW window/HWND." }
if ([string]$windowIdentity.title -cne $ExpectedTitle) {
    throw "Observer adoption refused: exact window title mismatch. Expected '$ExpectedTitle', observed '$($windowIdentity.title)'."
}
$windowProcessIdentity = [pscustomobject][ordered]@{
    process_id = [uint32]$windowIdentity.process_id
    start_time_utc = [string]$windowIdentity.start_time_utc
    executable_path = [string]$windowIdentity.executable_path
    required_command_line_tokens = @()
}
$expectedProcessIdentity = [pscustomobject][ordered]@{
    process_id = [uint32]$processIdentity.process_id
    start_time_utc = [string]$processIdentity.start_time_utc
    executable_path = [string]$processIdentity.executable_path
    required_command_line_tokens = @()
}
if (-not (Test-ObserverProcessIdentity -Expected $expectedProcessIdentity -Actual $windowProcessIdentity)) {
    throw 'Observer adoption refused: PID was reused or process start-time/path changed while the window was inspected.'
}
if (-not (Test-ObserverCaptureWindowIdentity -ExpectedProcessId $ProcessId `
    -ExpectedWindowHandle ([uint64]$windowIdentity.window_handle) `
    -ExpectedExecutablePath ([string]$approvedExecutable.executable_path) `
    -AllowedTitles @($ExpectedTitle) `
    -ActualProcessId ([uint32]$windowIdentity.process_id) `
    -ActualWindowHandle ([uint64]$windowIdentity.window_handle) `
    -ActualExecutablePath ([string]$windowIdentity.executable_path) `
    -ActualTitle ([string]$windowIdentity.title))) {
    throw 'Observer adoption refused: exact PID/HWND/path/title guard failed.'
}

# The only bridge operation in this script is the read-only status request.  Do not
# replace this with protect/watch/relocate: adoption must not mutate the live observer.
$rawStatus = & $controlPath -Action status -ObserverGuid $ObserverGuid -BridgeHost $BridgeHost -Port $Port
$statusLine = @($rawStatus | ForEach-Object { $_.ToString() }) | Select-Object -Last 1
if ([string]::IsNullOrWhiteSpace($statusLine)) { throw 'Observer bridge returned no status response.' }
try { $bridgeStatus = $statusLine | ConvertFrom-Json }
catch { throw "Observer bridge status was not valid JSON: $($_.Exception.Message)" }
$statusVerification = Test-ObserverBridgeStatusForAdoption -Response $bridgeStatus -ObserverGuid $ObserverGuid
if (-not $statusVerification.valid) {
    throw "Observer adoption refused: bridge status guard failed: $($statusVerification.reasons -join ', ')"
}

$observedUtc = (Get-Date).ToUniversalTime().ToString('o')
$session = New-ObserverAdoptedSessionManifest -ObserverGuid $ObserverGuid `
    -ProcessIdentity $processIdentity -WindowIdentity $windowIdentity `
    -ApprovedExecutable $approvedExecutable -BridgeStatus $bridgeStatus `
    -BridgeHost $BridgeHost -BridgePort $Port -ObservedUtc $observedUtc

if ($PSCmdlet.ShouldProcess($sessionPath, "write an adopted observer session for PID $ProcessId and HWND $($windowIdentity.window_handle)")) {
    New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
    $temporaryPath = Join-Path $stateDirectory ("observer-session.json.{0}.{1}.tmp" -f $ProcessId, ([guid]::NewGuid().ToString('N')))
    try {
        [IO.File]::WriteAllText($temporaryPath, ($session | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $sessionPath -PathType Leaf) {
            [IO.File]::Replace($temporaryPath, $sessionPath, $null, $true)
        }
        else {
            [IO.File]::Move($temporaryPath, $sessionPath)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) { Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue }
    }
    Write-Output ("Adopted observer GUID {0}: PID {1}, HWND {2}, title '{3}', root {4}. Session: {5}" -f `
        $ObserverGuid, $ProcessId, $windowIdentity.window_handle, $windowIdentity.title, $approvedExecutable.label, $sessionPath)
}
else {
    Write-Output ("Validated observer GUID {0} without writing a manifest (WhatIf): PID {1}, HWND {2}, root {3}." -f `
        $ObserverGuid, $ProcessId, $windowIdentity.window_handle, $approvedExecutable.label)
}
