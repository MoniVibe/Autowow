[CmdletBinding()]
param([string]$ServerRoot = (Split-Path -Parent $PSScriptRoot))

. (Join-Path $PSScriptRoot 'observer-automation-lib.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$sessionPath = Join-Path $ServerRoot 'work\observer-state\observer-session.json'
if (-not (Test-Path -LiteralPath $sessionPath -PathType Leaf)) {
    Write-Output 'No dedicated observer session manifest exists; nothing was stopped.'
    return
}
$session = Get-Content -LiteralPath $sessionPath -Raw | ConvertFrom-Json
if ([string]$session.schema -cne 'autowow.observer.session.v1' -or [uint32]$session.observer_guid -eq 21) {
    throw 'Observer session manifest failed its schema/forbidden-GUID guard.'
}

function Stop-ExactObserverProcess {
    param(
        [Parameter(Mandatory)][psobject]$Expected,
        [Parameter(Mandatory)][ValidateSet('relocate','wow')][string]$Kind
    )

    $actual = Get-ObserverActualProcessIdentity -ProcessId ([uint32]$Expected.process_id)
    if (-not $actual) { return 'already_stopped' }
    if (-not (Test-ObserverProcessIdentity -Expected $Expected -Actual $actual)) {
        throw "Refusing to stop PID $($Expected.process_id): exact $Kind process identity mismatch."
    }

    if ($Kind -eq 'wow') {
        $process = Get-Process -Id ([uint32]$Expected.process_id) -ErrorAction Stop
        $process.Refresh()
        if ([uint64]$process.MainWindowHandle.ToInt64() -ne [uint64]$Expected.window_handle) {
            throw "Refusing to stop PID $($Expected.process_id): observer HWND changed."
        }
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(8000)) {
            # Force is permitted only after PID/start-time/path/HWND all matched this session.
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
        }
    }
    else {
        Stop-Process -Id ([uint32]$Expected.process_id) -Force -ErrorAction Stop
    }
    return 'stopped'
}

$results = [ordered]@{}
if ($session.relocate) { $results.relocate = Stop-ExactObserverProcess -Expected $session.relocate -Kind relocate }
$results.wow = Stop-ExactObserverProcess -Expected $session.wow -Kind wow
Remove-Item -LiteralPath $sessionPath -Force
Write-Output ("Dedicated observer processes stopped: {0}" -f (($results.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', '))
