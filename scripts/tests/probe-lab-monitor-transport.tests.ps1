Set-StrictMode -Version Latest

BeforeAll {
    $script:RunnerPath = Join-Path $PSScriptRoot '..\probe-lab.ps1'

    function New-MonitorTransportManifest {
        $members = @(foreach ($guid in 1..5) {
            [pscustomobject][ordered]@{
                guid = [uint32]$guid
                name = "Bot$guid"
                level = 80
                spec_index = 0
                quality = 3
            }
        })
        [pscustomobject][ordered]@{
            schema = 'autowow.probe-lab.manifest.v1'
            lab_id = 'monitor-transport-test'
            probes = @([pscustomobject][ordered]@{
                id = 'test-probe'
                kind = 'party'
                size = 5
                leader_guid = [uint32]1
                expected_map_id = [uint32]576
                exterior_route = 'safe-exterior'
                advance_waypoint = ''
                members = $members
            })
        }
    }

    function New-MonitorTransportFakeControl {
        param([Parameter(Mandatory)][string]$Path)

        @'
[CmdletBinding()]
param(
    [string]$Action, [string]$BridgeHost, [int]$Port, [int]$TimeoutMs,
    [uint32]$BotGuid = 0, [uint32[]]$MemberGuid = @(), [int]$Level = 1,
    [int]$SpecIndex = 0, [int]$Quality = 2, [int]$RaidDifficulty = 0,
    [string]$Destination = '', [uint32]$ExpectedMapId = 0, [uint32]$ExpectedDifficulty = 0
)
$ErrorActionPreference = 'Stop'
$callLog = [Environment]::GetEnvironmentVariable('PROBE_LAB_MONITOR_TRANSPORT_CALL_LOG')
$statePath = [Environment]::GetEnvironmentVariable('PROBE_LAB_MONITOR_TRANSPORT_STATE_PATH')
$failureCount = [int]([Environment]::GetEnvironmentVariable('PROBE_LAB_MONITOR_TRANSPORT_FAIL_LIST_COUNT'))
[System.IO.File]::AppendAllText($callLog, "$Action`n", [System.Text.UTF8Encoding]::new($false))

if ($Action -eq 'list') {
    $count = 0
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        $count = [int][System.IO.File]::ReadAllText($statePath)
    }
    [System.IO.File]::WriteAllText($statePath, [string]($count + 1), [System.Text.UTF8Encoding]::new($false))
    if ($count -lt $failureCount) { throw 'The AutoWoW bridge ReadLine operation timed out.' }

    $bots = foreach ($guid in 1..5) {
        [pscustomobject]@{
            guid = [uint32]$guid
            name = "Bot$guid"
            alive = $true
            combat = $false
            position = [pscustomobject]@{ map = 576; x = $guid; y = 0; z = 0 }
            group = [pscustomobject]@{ members = 5; leader_guid = 1 }
        }
    }
    [pscustomobject]@{ ok = $true; bots = @($bots) } | ConvertTo-Json -Compress -Depth 10
    return
}

if ($Action -eq 'combatlog') {
    $telemetry = [pscustomobject]@{
        alive = $true
        death_state = 'alive'
        in_combat = $false
        location = [pscustomobject]@{ map_id = 576; instance_id = 7 }
        group = [pscustomobject]@{ members = 5; leader_guid = 1 }
    }
    [pscustomobject]@{ ok = $true; telemetry = $telemetry } | ConvertTo-Json -Compress -Depth 10
    return
}

[pscustomobject]@{ ok = $true; action = $Action; guid = $BotGuid } | ConvertTo-Json -Compress
'@ | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
    }

    function Invoke-MonitorTransportRunner {
        param(
            [Parameter(Mandatory)][hashtable]$Arguments,
            [Parameter(Mandatory)][string]$CallLogPath,
            [Parameter(Mandatory)][string]$StatePath,
            [Parameter(Mandatory)][int]$FailListCount
        )

        $environmentNames = @(
            'PROBE_LAB_MONITOR_TRANSPORT_CALL_LOG',
            'PROBE_LAB_MONITOR_TRANSPORT_STATE_PATH',
            'PROBE_LAB_MONITOR_TRANSPORT_FAIL_LIST_COUNT'
        )
        $previous = @{}
        foreach ($name in $environmentNames) { $previous[$name] = [Environment]::GetEnvironmentVariable($name) }
        try {
            [Environment]::SetEnvironmentVariable('PROBE_LAB_MONITOR_TRANSPORT_CALL_LOG', $CallLogPath)
            [Environment]::SetEnvironmentVariable('PROBE_LAB_MONITOR_TRANSPORT_STATE_PATH', $StatePath)
            [Environment]::SetEnvironmentVariable('PROBE_LAB_MONITOR_TRANSPORT_FAIL_LIST_COUNT', [string]$FailListCount)
            return (& $script:RunnerPath @Arguments | Out-String)
        } finally {
            foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previous[$name]) }
        }
    }

    function Read-MonitorTransportReceipt {
        param([Parameter(Mandatory)][string]$Path)
        return @(Get-Content -LiteralPath $Path | ForEach-Object { $_ | ConvertFrom-Json })
    }
}

Describe 'Probe Lab monitor transport resilience' {
    It 'retries a transient bridge ReadLine timeout and counts only the recovered sample' {
        $root = Join-Path $TestDrive 'transient-server'
        $logs = Join-Path $root 'logs'
        New-Item -ItemType Directory -Path $logs -Force | Out-Null

        $manifestPath = Join-Path $TestDrive 'transient-manifest.json'
        (New-MonitorTransportManifest | ConvertTo-Json -Depth 10) | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
        $controlPath = Join-Path $TestDrive 'transient-control.ps1'
        New-MonitorTransportFakeControl -Path $controlPath
        $callLogPath = Join-Path $TestDrive 'transient-calls.log'
        $statePath = Join-Path $TestDrive 'transient-state.txt'

        $arguments = @{
            Mode = 'Monitor'
            ManifestPath = $manifestPath
            ServerRoot = $root
            ControlScriptPath = $controlPath
            DurationSeconds = 1
            PollSeconds = 1
            MonitorTransportMaxRetries = 2
            MonitorTransportRetryBackoffMs = 10
            MonitorTransportRetryMaxBackoffMs = 10
            ReceiptPath = 'transient-retry.jsonl'
            SummaryJsonPath = 'transient-retry-summary.json'
            SummaryMarkdownPath = 'transient-retry-summary.md'
        }

        $summary = Invoke-MonitorTransportRunner -Arguments $arguments -CallLogPath $callLogPath -StatePath $statePath -FailListCount 1 | ConvertFrom-Json
        $receiptPath = Join-Path $logs 'transient-retry.jsonl'
        $receipts = Read-MonitorTransportReceipt -Path $receiptPath
        $retry = @($receipts | Where-Object event -eq 'monitor_transport_retry')
        $recovered = @($receipts | Where-Object event -eq 'monitor_transport_retry_success')
        $samples = @($receipts | Where-Object event -eq 'monitor_sample')

        $summary.status | Should -Be 'PASS'
        $summary.sample_count | Should -BeGreaterThan 0
        $summary.probes[0].status | Should -Be 'PASS'
        @($receipts | Where-Object event -eq 'run_failed').Count | Should -Be 0
        $retry.Count | Should -Be 1
        $retry[0].attempt | Should -Be 1
        $retry[0].max_attempts | Should -Be 3
        $retry[0].backoff_ms | Should -Be 10
        $recovered.Count | Should -Be 1
        $recovered[0].attempts | Should -Be 2
        $recovered[0].retries | Should -Be 1
        @($samples | Where-Object transport_attempts -eq 2).Count | Should -Be 1
        @($samples | Where-Object transport_retries -eq 1).Count | Should -Be 1
        @((Get-Content -LiteralPath $callLogPath) | Where-Object { $_ -eq 'list' }).Count | Should -Be ([int]$summary.sample_count + 1)
    }

    It 'fails after the bounded retry budget and records no fabricated sample' {
        $root = Join-Path $TestDrive 'persistent-server'
        $logs = Join-Path $root 'logs'
        New-Item -ItemType Directory -Path $logs -Force | Out-Null

        $manifestPath = Join-Path $TestDrive 'persistent-manifest.json'
        (New-MonitorTransportManifest | ConvertTo-Json -Depth 10) | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
        $controlPath = Join-Path $TestDrive 'persistent-control.ps1'
        New-MonitorTransportFakeControl -Path $controlPath
        $callLogPath = Join-Path $TestDrive 'persistent-calls.log'
        $statePath = Join-Path $TestDrive 'persistent-state.txt'
        $arguments = @{
            Mode = 'Monitor'
            ManifestPath = $manifestPath
            ServerRoot = $root
            ControlScriptPath = $controlPath
            DurationSeconds = 1
            PollSeconds = 1
            MonitorTransportMaxRetries = 2
            MonitorTransportRetryBackoffMs = 10
            MonitorTransportRetryMaxBackoffMs = 20
            ReceiptPath = 'persistent-retry.jsonl'
            SummaryJsonPath = 'persistent-retry-summary.json'
            SummaryMarkdownPath = 'persistent-retry-summary.md'
        }

        $caught = $null
        try {
            Invoke-MonitorTransportRunner -Arguments $arguments -CallLogPath $callLogPath -StatePath $statePath -FailListCount 99 | Out-Null
        } catch {
            $caught = $_
        }

        $receiptPath = Join-Path $logs 'persistent-retry.jsonl'
        $receipts = Read-MonitorTransportReceipt -Path $receiptPath
        $retry = @($receipts | Where-Object event -eq 'monitor_transport_retry')
        $exhausted = @($receipts | Where-Object event -eq 'monitor_transport_retry_exhausted')
        $samples = @($receipts | Where-Object event -eq 'monitor_sample')
        $failed = @($receipts | Where-Object event -eq 'run_failed')

        $caught | Should -Not -BeNullOrEmpty
        $caught.Exception.Message | Should -Match 'retry budget exhausted'
        @((Get-Content -LiteralPath $callLogPath) | Where-Object { $_ -eq 'list' }).Count | Should -Be 3
        $retry.Count | Should -Be 2
        $retry[0].backoff_ms | Should -Be 10
        $retry[1].backoff_ms | Should -Be 20
        $exhausted.Count | Should -Be 1
        $exhausted[0].attempts | Should -Be 3
        $exhausted[0].max_retries | Should -Be 2
        $exhausted[0].max_attempts | Should -Be 3
        $samples.Count | Should -Be 0
        $failed.Count | Should -Be 1
        $failed[0].code | Should -Be 'bridge_error'
        Test-Path -LiteralPath (Join-Path $logs 'persistent-retry-summary.json') | Should -BeFalse
        Test-Path -LiteralPath (Join-Path $logs 'persistent-retry-summary.md') | Should -BeFalse
    }

    It 'parse-checks the monitor runner and focused transport seam' {
        foreach ($path in @($script:RunnerPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path -LiteralPath $path).Path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
