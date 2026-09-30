<#
.SYNOPSIS
    Local-only TCP relay from the private WSL adapter to Windows MySQL loopback.

.DESCRIPTION
    Used only for the isolated Phase 1 Linux worldserver maintenance proof. The
    listener is required to bind to a private address assigned to a Windows WSL
    virtual adapter, and the upstream is hard-locked to 127.0.0.1:3306. It does
    not change MySQL bindings, Windows Firewall, or any database state.
#>
[CmdletBinding()]
param(
    [string]$ListenAddress = '',
    [ValidateRange(1024, 65535)][int]$ListenPort = 13306,
    [string]$StatusPath = 'D:\Games\wowstuff\AutoWoW\work\phase1-wsl-runtime\mysql-relay.json',
    [switch]$ProbeOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-PrivateIPv4 {
    param([Parameter(Mandatory)][System.Net.IPAddress]$Address)
    $bytes = $Address.GetAddressBytes()
    return ($bytes[0] -eq 10) -or
        ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or
        ($bytes[0] -eq 192 -and $bytes[1] -eq 168)
}

$wslAddresses = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
    Where-Object { $_.InterfaceAlias -match 'WSL' })

if ([string]::IsNullOrWhiteSpace($ListenAddress)) {
    $candidate = $wslAddresses | Select-Object -First 1
    if (-not $candidate) { throw 'No Windows WSL virtual-adapter IPv4 address was found.' }
    $ListenAddress = $candidate.IPAddress
}

$listenIp = [System.Net.IPAddress]::Parse($ListenAddress)
if (-not (Test-PrivateIPv4 -Address $listenIp)) {
    throw "Relay listener must be RFC1918 private, got $ListenAddress."
}
if ($ListenAddress -notin @($wslAddresses.IPAddress)) {
    throw "Relay listener $ListenAddress is not assigned to a Windows WSL virtual adapter."
}

$probe = [System.Net.Sockets.TcpClient]::new()
try {
    if (-not $probe.ConnectAsync('127.0.0.1', 3306).Wait(3000) -or -not $probe.Connected) {
        throw 'Windows MySQL is not reachable on 127.0.0.1:3306.'
    }
}
finally {
    $probe.Dispose()
}

$status = [ordered]@{
    schema = 'autowow.phase1.mysql-relay.v1'
    pid = $PID
    listen_address = $ListenAddress
    listen_port = $ListenPort
    target_address = '127.0.0.1'
    target_port = 3306
    private_wsl_adapter_only = $true
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
}

if ($ProbeOnly) {
    [pscustomobject]$status | ConvertTo-Json -Compress
    return
}

$statusDirectory = Split-Path -Parent $StatusPath
New-Item -ItemType Directory -Path $statusDirectory -Force | Out-Null

if (-not ('AutoWow.Phase1.TcpRelay' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.ExceptionServices;
using System.Threading;
using System.Threading.Tasks;

namespace AutoWow.Phase1
{
    public static class TcpRelay
    {
        public static async Task RunAsync(
            IPAddress listenAddress,
            int listenPort,
            IPAddress targetAddress,
            int targetPort,
            CancellationToken cancellationToken)
        {
            var listener = new TcpListener(listenAddress, listenPort);
            listener.Server.ExclusiveAddressUse = true;
            listener.Start();
            using (var runCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            using (runCts.Token.Register(delegate { listener.Stop(); }))
            {
                var connections = new List<Task>();
                var connectionGate = new object();
                ExceptionDispatchInfo failure = null;
                Task[] remaining;
                try
                {
                    while (!runCts.IsCancellationRequested)
                    {
                        TcpClient client;
                        try
                        {
                            client = await listener.AcceptTcpClientAsync().ConfigureAwait(false);
                        }
                        catch (ObjectDisposedException)
                        {
                            if (runCts.IsCancellationRequested) break;
                            throw;
                        }
                        catch (SocketException)
                        {
                            if (runCts.IsCancellationRequested) break;
                            throw;
                        }

                        Task connection = RelayConnectionAsync(
                            client, targetAddress, targetPort, runCts.Token);
                        lock (connectionGate)
                        {
                            connections.Add(connection);
                        }
                        RemoveWhenCompleted(connection, connections, connectionGate);
                    }
                }
                catch (Exception error)
                {
                    failure = ExceptionDispatchInfo.Capture(error);
                }
                finally
                {
                    runCts.Cancel();
                    listener.Stop();
                    lock (connectionGate)
                    {
                        remaining = connections.ToArray();
                    }
                }
                if (remaining.Length != 0)
                {
                    await Task.WhenAll(remaining).ConfigureAwait(false);
                }
                if (failure != null) failure.Throw();
            }
        }

        private static void RemoveWhenCompleted(
            Task connection,
            List<Task> connections,
            object connectionGate)
        {
            connection.ContinueWith(
                delegate(Task completed)
                {
                    lock (connectionGate)
                    {
                        connections.Remove(completed);
                    }
                },
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }

        private static async Task RelayConnectionAsync(
            TcpClient client,
            IPAddress targetAddress,
            int targetPort,
            CancellationToken serverToken)
        {
            using (client)
            using (var upstream = new TcpClient(targetAddress.AddressFamily))
            using (var connectionCts = CancellationTokenSource.CreateLinkedTokenSource(serverToken))
            using (connectionCts.Token.Register(delegate
            {
                client.Close();
                upstream.Close();
            }))
            {
                try
                {
                    await upstream.ConnectAsync(targetAddress, targetPort)
                        .ConfigureAwait(false);
                    using (NetworkStream inbound = client.GetStream())
                    using (NetworkStream outbound = upstream.GetStream())
                    {
                        Task toUpstream = PumpAsync(inbound, outbound);
                        Task toClient = PumpAsync(outbound, inbound);
                        await Task.WhenAny(toUpstream, toClient).ConfigureAwait(false);
                        connectionCts.Cancel();
                        try
                        {
                            await Task.WhenAll(toUpstream, toClient).ConfigureAwait(false);
                        }
                        catch (OperationCanceledException) { }
                        catch (System.IO.IOException) { }
                    }
                }
                catch (OperationCanceledException) { }
                catch (SocketException) { }
                catch (IOException) { }
                catch (ObjectDisposedException) { }
            }
        }

        private static async Task PumpAsync(Stream source, Stream destination)
        {
            var buffer = new byte[81920];
            while (true)
            {
                int count = await source.ReadAsync(buffer, 0, buffer.Length)
                    .ConfigureAwait(false);
                if (count == 0) return;
                await destination.WriteAsync(buffer, 0, count).ConfigureAwait(false);
            }
        }
    }
}
'@
}

$cts = [System.Threading.CancellationTokenSource]::new()
$cancelHandler = [ConsoleCancelEventHandler]{
    param($sender, $eventArgs)
    $eventArgs.Cancel = $true
    $cts.Cancel()
}
[Console]::add_CancelKeyPress($cancelHandler)

try {
    Write-Host "Phase 1 MySQL relay: ${ListenAddress}:$ListenPort -> 127.0.0.1:3306"
    # Async methods execute synchronously through listener.Start() before returning
    # their first incomplete Task. Publish readiness only after that point.
    $relayTask = [AutoWow.Phase1.TcpRelay]::RunAsync(
        $listenIp,
        $ListenPort,
        [System.Net.IPAddress]::Loopback,
        3306,
        $cts.Token)
    [System.IO.File]::WriteAllText(
        $StatusPath,
        ([pscustomobject]$status | ConvertTo-Json -Depth 4),
        [System.Text.UTF8Encoding]::new($false))
    $relayTask.GetAwaiter().GetResult()
}
finally {
    [Console]::remove_CancelKeyPress($cancelHandler)
    $cts.Dispose()
    Remove-Item -LiteralPath $StatusPath -Force -ErrorAction SilentlyContinue
}
