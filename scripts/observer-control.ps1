<#
.SYNOPSIS
    Thin, loopback-only client for the AutoWow observer-camera bridge command family.

.DESCRIPTION
    Sends a single `observe <sub> <observer-guid> [<leader-guid>]` line to the AutoWow bridge
    (127.0.0.1:18787) and returns the JSON response. This mirrors autowow-control.ps1 and adds
    NO capability of its own: all safety (allow-list, GM-protection precondition, no-group guard,
    observer-is-not-a-bot) is enforced server-side in ObserverControl.cpp.

    Subcommands:
      watch <observer> <leader>  record which leader the observer follows (server-side state)
      relocate <observer>        teleport the observer to the watched leader (server enforces guards)
      status <observer>          read-only snapshot of observer + watched leader
      protect <observer>         set the observer GM-invisible/non-interfering (its own runtime state)
      release <observer>         stop watching

    This client never issues quest/pause/resume/travel/recover orders and never touches bots.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('watch', 'relocate', 'status', 'release', 'protect')][string]$Action,
    [Parameter(Mandatory = $true)][uint32]$ObserverGuid,
    [uint32]$LeaderGuid = 0,
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [switch]$EmitRequestOnly
)

$ErrorActionPreference = 'Stop'

if ($ObserverGuid -eq 0) { throw 'ObserverGuid is required and must be a positive numeric GUID.' }
if ($Action -eq 'watch' -and $LeaderGuid -eq 0) { throw 'watch requires -LeaderGuid (the bot leader to follow).' }

$request = switch ($Action) {
    'watch'    { "observe watch $ObserverGuid $LeaderGuid" }
    'relocate' { "observe relocate $ObserverGuid" }
    'status'   { "observe status $ObserverGuid" }
    'release'  { "observe release $ObserverGuid" }
    'protect'  { "observe protect $ObserverGuid" }
}

# Test seam: print the exact wire request without opening a socket. Used by the unit tests.
if ($EmitRequestOnly) { return $request }

$client = [System.Net.Sockets.TcpClient]::new()
try {
    $connect = $client.ConnectAsync($BridgeHost, $Port)
    if (-not $connect.Wait(3000) -or -not $client.Connected) {
        throw "Could not connect to the AutoWow bridge at ${BridgeHost}:$Port."
    }

    $stream = $client.GetStream()
    $stream.ReadTimeout = 5000
    $writer = [System.IO.StreamWriter]::new($stream)
    $writer.NewLine = "`n"
    $writer.WriteLine($request)
    $writer.Flush()

    $reader = [System.IO.StreamReader]::new($stream)
    $response = $reader.ReadLine()
    if ([string]::IsNullOrWhiteSpace($response)) { throw 'The AutoWow bridge returned an empty response.' }

    $response
}
finally {
    $client.Dispose()
}
