<#
    Offline Pester coverage for capture-live-quest-acquisition-proof.ps1.

    These tests dot-source the pure grading functions only. They do not invoke the
    script entrypoint, WSL, the bridge, a server process, a client, or any bot.
#>

BeforeAll {
    $script:CapturePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'capture-live-quest-acquisition-proof.ps1'
    . $script:CapturePath

    function New-TestRuntime {
        param([string]$Start = '2026-07-18T20:00:00.0000000Z')
        [pscustomobject][ordered]@{
            captured_utc = '2026-07-18T20:00:01.0000000Z'
            distro = 'Ubuntu-24.04'
            available = $true
            session_identity_available = $true
            worldserver = [pscustomobject][ordered]@{
                pid = [int64]94755
                path = '/root/autowow-quest-giver-core/build-tests/src/server/apps/worldserver'
                sha256 = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
            }
            session = [pscustomobject][ordered]@{
                pid = [int64]94755
                start_time_utc = $Start
            }
            module_config = [pscustomobject][ordered]@{
                path = '/usr/local/etc/modules/playerbots.conf'
                exists = $true
                sha256 = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
            }
            errors = @()
        }
    }

    function New-TestSnapshot {
        param(
            [int]$GroupCount = 1,
            [object[]]$MemberGuids = @()
        )
        [pscustomobject][ordered]@{
            ok = $true
            guid = [uint32]7
            bot = [pscustomobject][ordered]@{
                guid = [uint32]7
                name = 'Saewash'
                online = $true
                alive = $true
                position = [pscustomobject][ordered]@{ map = 1; x = 1329.48; y = -4608.88; z = 23.6607; o = 0.0 }
                group = [pscustomobject][ordered]@{
                    members = $GroupCount
                    leader_guid = [uint32]7
                    member_guids = @($MemberGuids)
                }
            }
        }
    }

    function New-TestQuestlog {
        param(
            [uint32]$Guid = 7,
            [switch]$IncludeQuest
        )
        $quests = @()
        if ($IncludeQuest) {
            $quests = @([pscustomobject][ordered]@{
                id = [uint32]827
                title = 'Skull Rock'
                level = 12
                quest_type = 2
                status = 3
                is_complete = $false
                objectives = @([pscustomobject][ordered]@{
                    slot = 1; kind = 'npc'; entry = 322; current = 0; required = 1; done = $false
                })
            })
        }
        [pscustomobject][ordered]@{ ok = $true; guid = $Guid; quests = $quests }
    }

    function New-TestAcceptance {
        param([int]$TeleportCount = 0, [int]$DbMutationCount = 0)
        [pscustomobject][ordered]@{
            ok = $true
            guid = [uint32]7
            read_only = $true
            invariants = [pscustomobject][ordered]@{
                teleport_count = $TeleportCount
                direct_quest_db_mutation_count = $DbMutationCount
            }
        }
    }
}

Describe 'live quest acquisition proof grading (offline)' {
    It 'proves exact q827 leader-only acquisition and preserves the non-party grade' {
        $document = New-CaptureProofDocument -LeaderGuid 7 -QuestId 827 `
            -SnapshotResponse (New-TestSnapshot) `
            -QuestlogResponse (New-TestQuestlog -IncludeQuest) `
            -AcceptanceResponse (New-TestAcceptance) `
            -RuntimeBefore (New-TestRuntime) -RuntimeAfter (New-TestRuntime) `
            -CapturedUtc ([DateTimeOffset]'2026-07-18T20:00:02Z')

        $document.schema | Should -BeExactly 'autowow.quest.acquisition.proof.v1'
        $document.grading.mode | Should -BeExactly 'proven-live'
        $document.grading.proven_live | Should -BeTrue
        $document.grading.leader_acquisition | Should -BeTrue
        $document.grading.cohesive_party_acquisition | Should -BeFalse
        $document.questlog.exact_requested_quest.status | Should -BeExactly 'incomplete'
        $document.questlog.exact_requested_quest.entry.id | Should -Be 827
        $document.party.reasons | Should -Contain 'no_multi_member_party'
    }

    It 'fails closed when q827 is absent from the exact leader questlog' {
        $document = New-CaptureProofDocument -LeaderGuid 7 -QuestId 827 `
            -SnapshotResponse (New-TestSnapshot) `
            -QuestlogResponse (New-TestQuestlog) `
            -AcceptanceResponse (New-TestAcceptance) `
            -RuntimeBefore (New-TestRuntime) -RuntimeAfter (New-TestRuntime)

        $document.grading.mode | Should -BeExactly 'unproven'
        $document.grading.proven_live | Should -BeFalse
        $document.grading.leader_acquisition | Should -BeFalse
        $document.grading.reasons | Should -Contain 'exact_quest_missing_from_questlog'
    }

    It 'fails closed when the acceptance teleport invariant is nonzero' {
        $document = New-CaptureProofDocument -LeaderGuid 7 -QuestId 827 `
            -SnapshotResponse (New-TestSnapshot) `
            -QuestlogResponse (New-TestQuestlog -IncludeQuest) `
            -AcceptanceResponse (New-TestAcceptance -TeleportCount 1) `
            -RuntimeBefore (New-TestRuntime) -RuntimeAfter (New-TestRuntime)

        $document.acceptance.zero_teleport | Should -BeFalse
        $document.grading.mode | Should -BeExactly 'unproven'
        $document.grading.leader_acquisition | Should -BeFalse
        $document.grading.reasons | Should -Contain 'teleport_counter_nonzero_1'
    }

    It 'never claims cohesive party acquisition when one expected member lacks q827' {
        $snapshot = New-TestSnapshot -GroupCount 2 -MemberGuids @(7, 12)
        $memberEvidence = @([pscustomobject][ordered]@{
            guid = [uint32]12
            response = New-TestQuestlog -Guid 12
        })
        $document = New-CaptureProofDocument -LeaderGuid 7 -QuestId 827 `
            -SnapshotResponse $snapshot `
            -QuestlogResponse (New-TestQuestlog -IncludeQuest) `
            -AcceptanceResponse (New-TestAcceptance) `
            -MemberQuestlogs $memberEvidence `
            -RuntimeBefore (New-TestRuntime) -RuntimeAfter (New-TestRuntime)

        $document.grading.leader_acquisition | Should -BeTrue
        $document.grading.cohesive_party_acquisition | Should -BeFalse
        $document.party.valid | Should -BeFalse
        $document.party.reasons | Should -Contain 'member_12_exact_quest_missing_from_questlog'
        $document.party.evidence | Where-Object guid -eq 12 | Select-Object -ExpandProperty exact_questlog | Should -BeFalse
    }
}

Describe 'capture proof safety and runtime binding (offline)' {
    It 'rejects a changed worldserver session even when quest evidence is otherwise complete' {
        $before = New-TestRuntime
        $after = New-TestRuntime -Start '2026-07-18T20:01:00.0000000Z'
        $document = New-CaptureProofDocument -LeaderGuid 7 -QuestId 827 `
            -SnapshotResponse (New-TestSnapshot) `
            -QuestlogResponse (New-TestQuestlog -IncludeQuest) `
            -AcceptanceResponse (New-TestAcceptance) `
            -RuntimeBefore $before -RuntimeAfter $after

        $document.runtime.stable_session | Should -BeFalse
        $document.grading.mode | Should -BeExactly 'unproven'
        $document.grading.reasons | Should -Contain 'stale_worldserver_session_identity'
    }

    It 'keeps screenshot evidence explicitly non-substitutive' {
        $document = New-CaptureProofDocument -LeaderGuid 7 -QuestId 827 `
            -SnapshotResponse (New-TestSnapshot) `
            -QuestlogResponse (New-TestQuestlog) `
            -AcceptanceResponse (New-TestAcceptance) `
            -RuntimeBefore (New-TestRuntime) -RuntimeAfter (New-TestRuntime) `
            -ScreenshotPath (Join-Path $TestDrive 'observer.png')

        $document.witness.screenshot.supplied | Should -BeTrue
        $document.witness.screenshot.substitutes_for_questlog | Should -BeFalse
        $document.grading.leader_acquisition | Should -BeFalse
        $document.grading.reasons | Should -Contain 'exact_quest_missing_from_questlog'
    }

    It 'records the observer watched leader and same-map witness without making it quest evidence' {
        $observer = [pscustomobject][ordered]@{
            ok = $true
            observer = [uint32]21
            position = [pscustomobject][ordered]@{ map = 1; instance_id = 0; x = 1320.0; y = -4600.0; z = 25.0 }
            watched = [pscustomobject][ordered]@{ leader = [uint32]7; same_map = $true; same_instance = $true }
        }
        $document = New-CaptureProofDocument -LeaderGuid 7 -QuestId 827 -ObserverGuid 21 `
            -SnapshotResponse (New-TestSnapshot) `
            -QuestlogResponse (New-TestQuestlog -IncludeQuest) `
            -AcceptanceResponse (New-TestAcceptance) `
            -ObserverResponse $observer `
            -RuntimeBefore (New-TestRuntime) -RuntimeAfter (New-TestRuntime)

        $document.observer.valid | Should -BeTrue
        $document.observer.watched_leader_matches | Should -BeTrue
        $document.observer.same_map | Should -BeTrue
        $document.grading.leader_acquisition | Should -BeTrue
        $document.bridge.requests | Should -Contain 'observe status 21'
    }

    It 'contains only the four permitted read-only bridge surfaces in the implementation' {
        $source = Get-Content -LiteralPath $script:CapturePath -Raw
        $source | Should -Match 'snapshot <leader-guid>'
        $source | Should -Match 'questlog <guid>'
        $source | Should -Match 'acceptance <leader-guid> <quest-id>'
        $source | Should -Match 'observe status <observer-guid>'
        $source | Should -Not -Match '(?i)quest \$LeaderGuid \$QuestId|quest-acquire|recover \$|party \$|fixture-init|Start-Process|SendKeys'
    }

    It 'does not assign PowerShell reserved PID and remains parse-clean' {
        $source = Get-Content -LiteralPath $script:CapturePath -Raw
        $source | Should -Not -Match '(?i)\$pid\b'
        $source | Should -Not -Match '(?i)\$host\b'
        $source | Should -Not -Match '(?i)\-Host\b'
        $parseErrors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile(
            (Resolve-Path -LiteralPath $script:CapturePath), [ref]$null, [ref]$parseErrors)
        if ($null -eq $parseErrors) { $parseErrors = @() }
        @($parseErrors).Count | Should -Be 0
    }

    It 'preserves the exact module path and runtime fields in the WSL probe contract' {
        $modulePath = '/usr/local/etc/modules/playerbots.conf'
        $invocation = New-CaptureWslRuntimeProbeInvocation -Distro 'Ubuntu-24.04' -ModuleConfigPath $modulePath
        $arguments = @($invocation.arguments)

        # Exercise the real Windows wsl.exe invocation contract: there are no
        # positional arguments after bash -lc, and the command contains only a
        # base64-safe payload plus the decode-and-execute pipeline.
        $invocation.executable | Should -BeExactly 'wsl.exe'
        $arguments.Count | Should -Be 6
        $arguments[0] | Should -BeExactly '--distribution'
        $arguments[1] | Should -BeExactly 'Ubuntu-24.04'
        $arguments[2] | Should -BeExactly '--'
        $arguments[3] | Should -BeExactly 'bash'
        $arguments[4] | Should -BeExactly '-lc'
        $bashCommand = [string]$arguments[5]
        $bashCommand | Should -Match '^echo [A-Za-z0-9+/=]+ \| base64 -d \| bash$'
        $encoded = [regex]::Match($bashCommand, '^echo ([A-Za-z0-9+/=]+) \| base64 -d \| bash$').Groups[1].Value
        $encoded | Should -BeExactly $invocation.probe_base64
        $probe = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($encoded))
        $probe | Should -BeExactly $invocation.probe
        $probe | Should -Match ([regex]::Escape("module='$modulePath'"))
        $probe | Should -Match 'module_config_missing_%s'
        $probe | Should -Not -Match '__MODULE_CONFIG_PATH__'
        $probe | Should -Not -Match 'module="\$1"'
        $invocation.module_config_path | Should -BeExactly $modulePath

        $runtime = ConvertFrom-CaptureWslRuntimeOutput -Distro 'Ubuntu-24.04' -ModuleConfigPath $modulePath `
            -RawLines @(
                "WORLD_PID`t94755",
                "WORLD_PATH`t/root/autowow-quest-giver-core/build-tests/src/server/apps/worldserver",
                "WORLD_SHA256`taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "WORLD_START_EPOCH`t1700000000.000000",
                "MODULE_EXISTS`t1",
                "MODULE_SHA256`tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            )

        $runtime.available | Should -BeTrue
        $runtime.session_identity_available | Should -BeTrue
        $runtime.worldserver.pid | Should -Be 94755
        $runtime.worldserver.path | Should -BeExactly '/root/autowow-quest-giver-core/build-tests/src/server/apps/worldserver'
        $runtime.worldserver.sha256 | Should -BeExactly (('a' * 64) -join '')
        $runtime.session.start_time_utc | Should -Not -BeNullOrEmpty
        $runtime.module_config.path | Should -BeExactly $modulePath
        $runtime.module_config.exists | Should -BeTrue
        $runtime.module_config.sha256 | Should -BeExactly (('b' * 64) -join '')

        $missing = ConvertFrom-CaptureWslRuntimeOutput -Distro 'Ubuntu-24.04' -ModuleConfigPath $modulePath `
            -RawLines @('MODULE_EXISTS`t0', "RUNTIME_ERROR`tmodule_config_missing_$modulePath")
        $missing.module_config.path | Should -BeExactly $modulePath
        $missing.errors | Should -Contain "module_config_missing_$modulePath"
    }

    It 'keeps requested proof output below logs/proofs' {
        $root = Join-Path $TestDrive 'AutoWoW'
        $resolved = Resolve-CaptureOutputPath -ServerRoot $root -OutputPath 'nested\q827.json' -LeaderGuid 7 -QuestId 827
        $resolved | Should -Be ([IO.Path]::GetFullPath((Join-Path $root 'logs\proofs\nested\q827.json')))
        { Resolve-CaptureOutputPath -ServerRoot $root -OutputPath '..\outside.json' -LeaderGuid 7 -QuestId 827 } | Should -Throw '*below logs\proofs*'
    }
}
