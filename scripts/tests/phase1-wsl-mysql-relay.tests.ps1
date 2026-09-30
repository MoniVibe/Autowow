<#
    Native Windows PowerShell 5.1 integration coverage for the Phase 1 TCP relay.
    The test extracts the exact embedded C# helper and uses only ephemeral loopback
    listeners. It does not contact MySQL, WSL, or production relay port 13306.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Describe 'Phase 1 WSL MySQL relay compatibility' {
    It 'compiles and forwards concurrent duplex traffic, disconnects, and cancels cleanly on native PowerShell 5.1' {
        $PSVersionTable.PSVersion.Major | Should Be 5
        $PSVersionTable.PSEdition | Should Be 'Desktop'

        $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
        $relayPath = Join-Path $repositoryRoot 'scripts\phase1-wsl-mysql-relay.ps1'
        $relayText = [System.IO.File]::ReadAllText($relayPath)
        $sourceMatch = [regex]::Match(
            $relayText,
            '(?s)Add-Type -TypeDefinition @''\r?\n(?<source>.*?)\r?\n''@')
        $sourceMatch.Success | Should Be $true
        Add-Type -TypeDefinition $sourceMatch.Groups['source'].Value

        if (-not ('AutoWow.Phase1.Tests.LoopbackEcho' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.ExceptionServices;
using System.Threading;
using System.Threading.Tasks;

namespace AutoWow.Phase1.Tests
{
    public sealed class LoopbackEcho : IDisposable
    {
        private readonly TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private readonly List<Task> clients = new List<Task>();
        private readonly object clientGate = new object();
        private Task runTask;

        public int Port { get; private set; }

        public void Start()
        {
            listener.Start();
            Port = ((IPEndPoint)listener.LocalEndpoint).Port;
            runTask = RunAsync();
        }

        public void Stop()
        {
            cancellation.Cancel();
            if (runTask != null) runTask.GetAwaiter().GetResult();
        }

        private async Task RunAsync()
        {
            using (cancellation.Token.Register(delegate { listener.Stop(); }))
            {
                ExceptionDispatchInfo failure = null;
                Task[] remaining;
                try
                {
                    while (!cancellation.IsCancellationRequested)
                    {
                        TcpClient client;
                        try
                        {
                            client = await listener.AcceptTcpClientAsync().ConfigureAwait(false);
                        }
                        catch (ObjectDisposedException)
                        {
                            if (cancellation.IsCancellationRequested) break;
                            throw;
                        }
                        catch (SocketException)
                        {
                            if (cancellation.IsCancellationRequested) break;
                            throw;
                        }

                        Task task = EchoAsync(client, cancellation.Token);
                        lock (clientGate) clients.Add(task);
                        RemoveWhenCompleted(task, clients, clientGate);
                    }
                }
                catch (Exception error)
                {
                    failure = ExceptionDispatchInfo.Capture(error);
                }
                finally
                {
                    cancellation.Cancel();
                    listener.Stop();
                    lock (clientGate) remaining = clients.ToArray();
                }
                if (remaining.Length != 0)
                    await Task.WhenAll(remaining).ConfigureAwait(false);
                if (failure != null) failure.Throw();
            }
        }

        private static void RemoveWhenCompleted(
            Task task,
            List<Task> tasks,
            object taskGate)
        {
            task.ContinueWith(
                delegate(Task completed)
                {
                    lock (taskGate) tasks.Remove(completed);
                },
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }

        private static async Task EchoAsync(TcpClient client, CancellationToken token)
        {
            using (client)
            using (token.Register(delegate { client.Close(); }))
            {
                try
                {
                    NetworkStream stream = client.GetStream();
                    var buffer = new byte[4096];
                    while (true)
                    {
                        int count = await stream.ReadAsync(buffer, 0, buffer.Length)
                            .ConfigureAwait(false);
                        if (count == 0) return;
                        await stream.WriteAsync(buffer, 0, count).ConfigureAwait(false);
                    }
                }
                catch (IOException) { }
                catch (ObjectDisposedException) { }
                catch (SocketException) { }
            }
        }

        public static async Task<byte[]> RoundTripAsync(int port, byte[] payload)
        {
            using (var client = new TcpClient(AddressFamily.InterNetwork))
            {
                await client.ConnectAsync(IPAddress.Loopback, port).ConfigureAwait(false);
                NetworkStream stream = client.GetStream();
                await stream.WriteAsync(payload, 0, payload.Length).ConfigureAwait(false);
                var result = new byte[payload.Length];
                int offset = 0;
                while (offset != result.Length)
                {
                    int count = await stream.ReadAsync(result, offset, result.Length - offset)
                        .ConfigureAwait(false);
                    if (count == 0) throw new EndOfStreamException();
                    offset += count;
                }
                return result;
            }
        }

        public void Dispose()
        {
            if (!cancellation.IsCancellationRequested) Stop();
            cancellation.Dispose();
        }
    }
}
'@
        }

        $echo = [AutoWow.Phase1.Tests.LoopbackEcho]::new()
        $relayCancellation = [System.Threading.CancellationTokenSource]::new()
        $relayTask = $null
        $idleClient = $null
        $relayPort = 0
        try {
            $echo.Start()

            $reservation = [System.Net.Sockets.TcpListener]::new(
                [System.Net.IPAddress]::Loopback,
                0)
            $reservation.Start()
            $relayPort = ([System.Net.IPEndPoint]$reservation.LocalEndpoint).Port
            $reservation.Stop()

            $relayTask = [AutoWow.Phase1.TcpRelay]::RunAsync(
                [System.Net.IPAddress]::Loopback,
                $relayPort,
                [System.Net.IPAddress]::Loopback,
                $echo.Port,
                $relayCancellation.Token)

            $firstText = 'client-one:' + ('A' * 32768)
            $secondText = 'client-two:' + ('B' * 49152)
            $firstPayload = [System.Text.Encoding]::UTF8.GetBytes($firstText)
            $secondPayload = [System.Text.Encoding]::UTF8.GetBytes($secondText)
            $first = [AutoWow.Phase1.Tests.LoopbackEcho]::RoundTripAsync(
                $relayPort,
                $firstPayload)
            $second = [AutoWow.Phase1.Tests.LoopbackEcho]::RoundTripAsync(
                $relayPort,
                $secondPayload)

            [System.Text.Encoding]::UTF8.GetString(
                [byte[]]$first.GetAwaiter().GetResult()) | Should Be $firstText
            [System.Text.Encoding]::UTF8.GetString(
                [byte[]]$second.GetAwaiter().GetResult()) | Should Be $secondText

            # Both clients have disconnected; a fresh connection must still work.
            $afterDisconnect = [System.Text.Encoding]::UTF8.GetBytes('after-disconnect')
            $third = [AutoWow.Phase1.Tests.LoopbackEcho]::RoundTripAsync(
                $relayPort,
                $afterDisconnect)
            [System.Text.Encoding]::UTF8.GetString(
                [byte[]]$third.GetAwaiter().GetResult()) | Should Be 'after-disconnect'

            # Keep one connection open so cancellation must close and drain it.
            $idleClient = [System.Net.Sockets.TcpClient]::new(
                [System.Net.Sockets.AddressFamily]::InterNetwork)
            $idleClient.ConnectAsync(
                [System.Net.IPAddress]::Loopback,
                $relayPort).GetAwaiter().GetResult()
            Start-Sleep -Milliseconds 100
            $relayCancellation.Cancel()
            $relayTask.Wait(5000) | Should Be $true
            $relayTask.GetAwaiter().GetResult()

            $idleClosed = $false
            try {
                $idleClient.GetStream().ReadTimeout = 1000
                $idleClosed = $idleClient.GetStream().ReadByte() -eq -1
            }
            catch [System.IO.IOException] {
                $idleClosed = $true
            }
            $idleClosed | Should Be $true

            $closedProbe = [System.Net.Sockets.TcpClient]::new(
                [System.Net.Sockets.AddressFamily]::InterNetwork)
            try {
                $accepted = $false
                try {
                    $accepted = $closedProbe.ConnectAsync(
                        [System.Net.IPAddress]::Loopback,
                        $relayPort).Wait(750) -and $closedProbe.Connected
                }
                catch [System.AggregateException] { }
                $accepted | Should Be $false
            }
            finally {
                $closedProbe.Dispose()
            }
        }
        finally {
            if (-not $relayCancellation.IsCancellationRequested) {
                $relayCancellation.Cancel()
            }
            if ($relayTask) {
                try { $relayTask.Wait(5000) | Out-Null } catch { }
            }
            if ($idleClient) { $idleClient.Dispose() }
            $relayCancellation.Dispose()
            $echo.Dispose()
        }

        $launcherPath = Join-Path $repositoryRoot 'scripts\start-phase1-wsl-worldserver.ps1'
        $launcherText = [System.IO.File]::ReadAllText($launcherPath)
        $launcherText.Contains(
            "Start-Process -FilePath 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'") |
            Should Be $true
    }
}
