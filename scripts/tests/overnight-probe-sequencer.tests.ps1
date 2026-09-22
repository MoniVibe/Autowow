BeforeAll {
    $env:AUTOWOW_OVERNIGHT_PROBE_SEQUENCER_TESTS = '1'
    . (Join-Path $PSScriptRoot '..\overnight-probe-sequencer.ps1')
}
Describe 'overnight probe sequencer offline seams' {
    It 'plans without writing or invoking live paths' {
        $q = [pscustomobject]@{ entries=@([pscustomobject]@{ manifest_path='m.json'; probe_id='p'; guids=@(101) }) }
        $plan = New-SequencerPlan $q 2
        $plan.dry_run | Should -BeTrue
        $plan.entries[0].mutations_in_plan | Should -Be 0
    }
    It 'rejects protected and duplicate GUIDs while reading queue manifests' {
        $m = Join-Path $TestDrive 'm.json'; $q = Join-Path $TestDrive 'q.json'
        @{ schema='autowow.probe-lab.manifest.v1'; lab_id='x'; probes=@(@{ id='a'; members=@(@{guid=7},@{guid=100}) },@{ id='b'; members=@(@{guid=100}) }) } | ConvertTo-Json -Depth 10 | Set-Content $m
        @{ entries=@(@{manifest_path=$m;probe_id='a'},@{manifest_path=$m;probe_id='b'}) } | ConvertTo-Json | Set-Content $q
        { Read-SequencerQueue $q } | Should -Throw '*protected*'
    }
    It 'rejects duplicate GUIDs across otherwise valid probes' {
        $m = Join-Path $TestDrive 'm.json'; $q = Join-Path $TestDrive 'q.json'
        @{ schema='autowow.probe-lab.manifest.v1'; lab_id='x'; probes=@(@{ id='a'; members=@(@{guid=100}) },@{ id='b'; members=@(@{guid=100}) }) } | ConvertTo-Json -Depth 10 | Set-Content $m
        @{ entries=@(@{manifest_path=$m;probe_id='a'},@{manifest_path=$m;probe_id='b'}) } | ConvertTo-Json | Set-Content $q
        { Read-SequencerQueue $q } | Should -Throw '*Duplicate GUID*'
    }
    It 'applies Probe Lab defaults and validates optional monitor settings' {
        $m = Join-Path $TestDrive 'settings manifest.json'; $q = Join-Path $TestDrive 'settings queue.json'
        @{ schema='autowow.probe-lab.manifest.v1'; lab_id='x'; probes=@(@{ id='p'; members=@(@{guid=100}) }) } | ConvertTo-Json -Depth 10 | Set-Content $m
        @{ entries=@(@{manifest_path=$m;probe_id='p'}) } | ConvertTo-Json | Set-Content $q
        $entry = (Read-SequencerQueue $q).entries[0]
        $entry.duration_seconds | Should -Be 300; $entry.poll_seconds | Should -Be 5; $entry.stall_seconds | Should -Be 60
        @{ entries=@(@{manifest_path=$m;probe_id='p';duration_seconds=86401}) } | ConvertTo-Json | Set-Content $q
        { Read-SequencerQueue $q } | Should -Throw '*duration_seconds*'
    }
    It 'adopts only exact monitor identity' {
        $script:ProbeLabPath = (Join-Path $TestDrive 'probe-lab.ps1'); Set-Content $script:ProbeLabPath '#'
        $m = Join-Path $TestDrive 'm.json'; Set-Content $m '{}'
        $e = [pscustomobject]@{ manifest_path=$m; probe_id='alpha beta'; duration_seconds=7200; poll_seconds=11; stall_seconds=900 }
        $good = [pscustomobject]@{ ProcessId=42; CommandLine="pwsh -File `"$script:ProbeLabPath`" -Mode Monitor -ManifestPath `"$m`" -ProbeId `"alpha beta`" -DurationSeconds 7200 -PollSeconds 11 -StallSeconds 900" }
        $bad = [pscustomobject]@{ ProcessId=43; CommandLine="pwsh -File $script:ProbeLabPath -Mode Monitor -ManifestPath $m -ProbeId beta" }
        @(Get-ExactMonitorProcesses $e { @($good,$bad) }).Count | Should -Be 1
    }
    It 'executes a first run through seams and quotes spaced child arguments' {
        $script:ProbeLabPath = Join-Path $TestDrive 'probe lab.ps1'; Set-Content $script:ProbeLabPath '#'
        $statePath = Join-Path $TestDrive 'state.json'; $receiptPath = Join-Path $TestDrive 'receipts.jsonl'
        $entry = [pscustomobject]@{ manifest_path=(Join-Path $TestDrive 'manifest with spaces.json'); probe_id='probe with spaces'; guids=@(101); duration_seconds=7200; poll_seconds=11; stall_seconds=900 }
        $queue = [pscustomobject]@{ entries=@($entry) }; $counts=[pscustomobject]@{ launches=0; starters=0; args=$null }
        $state = Invoke-OvernightProbeSequencer $queue $statePath $receiptPath 1 `
            { param($e) ++$counts.launches; [pscustomobject]@{ probes=@([pscustomobject]@{status='LAUNCHED'}) } } `
            { @() } `
            { param($e) $counts.args = @(New-MonitorArgumentList $e); [pscustomobject]@{ Id=7001 } }
        $state.entries[0].launch_status | Should -Be 'LAUNCHED'
        $state.entries[0].monitor_pid | Should -Be 7001
        $counts.args -contains ('"' + $entry.manifest_path + '"') | Should -BeTrue
        $counts.args -contains ('"' + $entry.probe_id + '"') | Should -BeTrue
        $counts.args -contains '7200' | Should -BeTrue
        $counts.args -contains '11' | Should -BeTrue
        $counts.args -contains '900' | Should -BeTrue
        (Get-Content $statePath -Raw | ConvertFrom-Json).updated_utc | Should -Not -BeNullOrEmpty
    }
    It 'resumes idempotently and adopts an exact existing monitor' {
        $script:ProbeLabPath = Join-Path $TestDrive 'probe lab.ps1'; Set-Content $script:ProbeLabPath '#'
        $statePath = Join-Path $TestDrive 'state.json'; $receiptPath = Join-Path $TestDrive 'receipts.jsonl'
        $entry = [pscustomobject]@{ manifest_path=(Join-Path $TestDrive 'manifest with spaces.json'); probe_id='probe with spaces'; guids=@(101); duration_seconds=7200; poll_seconds=11; stall_seconds=900 }
        Set-Content -LiteralPath $entry.manifest_path -Value '{}'
        $queue = [pscustomobject]@{ entries=@($entry) }
        $first = Invoke-OvernightProbeSequencer $queue $statePath $receiptPath 1 `
            { param($e) [pscustomobject]@{ probes=@([pscustomobject]@{status='LAUNCHED'}) } } `
            { @() } `
            { param($e) [pscustomobject]@{ Id=7001 } }
        $process = [pscustomobject]@{ ProcessId=7001; CommandLine="pwsh -File `"$script:ProbeLabPath`" -Mode Monitor -ManifestPath `"$($entry.manifest_path)`" -ProbeId `"$($entry.probe_id)`" -DurationSeconds 7200 -PollSeconds 11 -StallSeconds 900" }
        $second = Invoke-OvernightProbeSequencer $queue $statePath $receiptPath 1 `
            { param($e) ++$counts.launches; throw 'launch should not repeat' } `
            { @($process) } `
            { param($e) ++$counts.starters; throw 'monitor should be adopted' }
        $second.entries[0].monitor_pid | Should -Be 7001
        $second.entries[0].monitor_adopted | Should -BeTrue
    }
}
AfterAll { Remove-Item Env:AUTOWOW_OVERNIGHT_PROBE_SEQUENCER_TESTS -ErrorAction SilentlyContinue }
