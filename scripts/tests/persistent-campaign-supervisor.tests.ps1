<##
    Offline, non-mutating Pester tests for persistent-campaign-supervisor.ps1.
    The supervisor is never invoked with -Apply here, and no bridge/server/database is contacted.
#>

BeforeAll {
    $script:SupervisorPath = Join-Path $PSScriptRoot '..\persistent-campaign-supervisor.ps1'
    $script:SupervisorSource = Get-Content -LiteralPath $script:SupervisorPath -Raw
    . $script:SupervisorPath -LibraryOnly
}

Describe 'persistent campaign supervisor parse and safety contract' {
    It 'parses without PowerShell errors' {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($script:SupervisorPath, [ref]$tokens, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0
    }

    It 'defaults to a JSON dry-run without creating state, receipts, snapshots, or processes' {
        $root = Join-Path $TestDrive 'dry-run-root'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $before = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'persistent-campaign-supervisor\.ps1' }).Count
        $result = (& $script:SupervisorPath -ServerRoot $root | Out-String) | ConvertFrom-Json
        $after = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'persistent-campaign-supervisor\.ps1' }).Count

        $result.dry_run | Should -BeTrue
        $result.apply | Should -BeFalse
        $result.schema | Should -Be 'autowow.persistent-campaign-supervisor'
        $result.schema_version | Should -Be 1
        $result.safety.process_starts | Should -Be 0
        $result.safety.bridge_orders_issued_by_supervisor | Should -Be 0
        $result.safety.database_writes_by_supervisor | Should -Be 0
        $result.safety.destructive_cleanup_actions | Should -Be 0
        $after | Should -Be $before
        (Test-Path -LiteralPath (Join-Path $root 'work\persistent-campaign-supervisor\state.json')) | Should -BeFalse
        (Test-Path -LiteralPath (Join-Path $root 'leagues\results\persistent-campaign-supervisor.jsonl')) | Should -BeFalse
        (Test-Path -LiteralPath (Join-Path $root 'logs\persistent-campaign-supervisor-status.json')) | Should -BeFalse
    }

    It 'adopts only an exact script path and ServerRoot command line' {
        $root = 'D:\Games\wowstuff\AutoWoW'
        $scriptPath = Join-Path $root 'scripts\league-simulation-director.ps1'
        $good = '"C:\Program Files\PowerShell\7\pwsh.exe" -NoProfile -File "{0}" -ServerRoot "{1}" -DurationMinutes 480' -f $scriptPath, $root
        (Test-ExactChildCommandLine -Role quest_director -CommandLine $good -ScriptPath $scriptPath -ExpectedServerRoot $root).matches | Should -BeTrue

        $wrongRoot = $good.Replace($root, 'D:\Other\AutoWoW')
        (Test-ExactChildCommandLine -Role quest_director -CommandLine $wrongRoot -ScriptPath $scriptPath -ExpectedServerRoot $root).matches | Should -BeFalse
        $wrongScript = $good.Replace('league-simulation-director.ps1', 'start-league-simulation-director.ps1')
        (Test-ExactChildCommandLine -Role quest_director -CommandLine $wrongScript -ScriptPath $scriptPath -ExpectedServerRoot $root).matches | Should -BeFalse
    }

    It 'fails closed for observe-only authority and contradictory teleport flags' {
        $root = 'D:\Games\wowstuff\AutoWoW'
        $scriptPath = Join-Path $root 'scripts\league-simulation-director.ps1'
        $observeOnly = 'pwsh.exe -File "{0}" -ServerRoot "{1}" -ObserveOnly' -f $scriptPath, $root
        (Test-ExactChildCommandLine -Role quest_director -CommandLine $observeOnly -ScriptPath $scriptPath -ExpectedServerRoot $root).matches | Should -BeFalse
        $teleport = 'pwsh.exe -File "{0}" -ServerRoot "{1}" -AllowTeleport true' -f $scriptPath, $root
        (Test-ExactChildCommandLine -Role quest_director -CommandLine $teleport -ScriptPath $scriptPath -ExpectedServerRoot $root).matches | Should -BeFalse
    }

    It 'adopts the exact overnight guardian and binds it to the authoritative director PID' {
        $root = 'D:\Games\wowstuff\AutoWoW'
        $guardianPath = Join-Path $root 'scripts\autowow-overnight-guardian.ps1'
        $command = '"C:\Program Files\PowerShell\7\pwsh.exe" -NoProfile -File "{0}" -DurationHours 8 -PollSeconds 60 -QuestDirectorPid 33580 -ReleaseLiveMutations -ReceiptPath "D:\Games\wowstuff\AutoWoW\logs\overnight\guardian.jsonl"' -f $guardianPath
        $match = Test-ExactChildCommandLine -Role guardian -CommandLine $command -ScriptPath $guardianPath -ExpectedQuestDirectorPid 33580
        $match.matches | Should -BeTrue
        $match.has_exact_server_root | Should -BeTrue
        $match.has_exact_quest_director_pid | Should -BeTrue
    }

    It 'rejects a guardian tied to the wrong Quest Director PID' {
        $root = 'D:\Games\wowstuff\AutoWoW'
        $guardianPath = Join-Path $root 'scripts\autowow-overnight-guardian.ps1'
        $wrongPid = 'pwsh.exe -NoProfile -File "{0}" -DurationHours 8 -PollSeconds 60 -QuestDirectorPid 40512 -ReleaseLiveMutations -ReceiptPath "D:\guardian.jsonl"' -f $guardianPath
        (Test-ExactChildCommandLine -Role guardian -CommandLine $wrongPid -ScriptPath $guardianPath -ExpectedQuestDirectorPid 33580).matches | Should -BeFalse
    }

    It 'does not classify league-simulation-monitor as the guardian' {
        $root = 'D:\Games\wowstuff\AutoWoW'
        $monitorPath = Join-Path $root 'scripts\league-simulation-monitor.ps1'
        $monitor = 'pwsh.exe -NoProfile -File "{0}" -ServerRoot "{1}" -DurationMinutes 480 -PollSeconds 20' -f $monitorPath, $root
        (Test-ExactChildCommandLine -Role guardian -CommandLine $monitor -ScriptPath (Join-Path $root 'scripts\autowow-overnight-guardian.ps1') -ExpectedQuestDirectorPid 33580).matches | Should -BeFalse
    }

    It 'derives guardian duration safely and only adds ReleaseLiveMutations for apply' {
        $common = @{
            Role = 'guardian'; ServerRoot = 'D:\Games\wowstuff\AutoWoW'; ReceiptPath = 'D:\guardian.jsonl'
            DurationMinutes = 480; QuestPollSeconds = 20; NoProgressSeconds = 120; GuardianPollSeconds = 60
            ProgressionPollSeconds = 300; SanityPollSeconds = 900; QuestDirectorPid = 33580
        }
        $dryArgs = New-ChildArguments @common -Apply $false
        $dryArgs | Should -Contain '-DurationHours'; $dryArgs | Should -Contain 8
        $dryArgs | Should -Contain '-QuestDirectorPid'; $dryArgs | Should -Contain 33580
        $dryArgs | Should -Not -Contain '-ServerRoot'
        $dryArgs | Should -Not -Contain '-ReleaseLiveMutations'
        $applyArgs = New-ChildArguments @common -Apply $true
        $applyArgs | Should -Contain '-ReleaseLiveMutations'
    }

    It 'keeps current questing/gathering gates open and future gates locked by default' {
        (Get-PhaseGate -RequestedPhase questing).status | Should -Be 'open'
        (Get-PhaseGate -RequestedPhase gathering).status | Should -Be 'open'
        (Get-PhaseGate -RequestedPhase raid).status | Should -Be 'locked'
    }

    It 'requires both explicit enablement and passing evidence for a future phase' {
        $evidencePath = Join-Path $TestDrive 'raid-evidence.json'
        [ordered]@{
            schema = 'autowow.campaign.evidence.v1'
            phase = 'raid'
            verdict = 'PASS'
            no_teleport = $true
            no_database_writes = $true
            no_server_restart = $true
        } | ConvertTo-Json | Set-Content -LiteralPath $evidencePath -Encoding utf8
        $gate = Get-PhaseGate -RequestedPhase raid -EnableFuturePromotions -EvidencePath $evidencePath
        $gate.status | Should -Be 'eligible_metadata_only'
        $gate.mutating_promotion_enabled | Should -BeTrue
    }

    It 'contains no destructive process or stale-file cleanup operation' {
        $script:SupervisorSource | Should -Not -Match '(?m)\bStop-Process\b'
        $script:SupervisorSource | Should -Not -Match '(?m)\bRemove-Item\b'
    }
}
