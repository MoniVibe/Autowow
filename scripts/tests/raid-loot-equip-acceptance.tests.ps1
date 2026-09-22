<#
    Offline Pester coverage for raid-loot-equip-acceptance.ps1.
    All fixtures live under TestDrive; the script only reads them.
#>

BeforeAll {
    $script:Tool = Join-Path $PSScriptRoot '..\raid-loot-equip-acceptance.ps1'
    $script:RollRaw = [uint64]4294972297
    $script:ItemRaw = [uint64]8589943593
    $script:OtherItemRaw = [uint64]8589943594

    function New-TestSnapshot {
        param(
            [Parameter(Mandatory)][string]$Path,
            [Parameter(Mandatory)][uint32]$EquippedGuid,
            [Parameter(Mandatory)][uint32]$EquippedEntry,
            [int]$EquippedSlot = 0,
            [uint32]$CharacterGuid = 101,
            [object[]]$CarriedItems = @()
        )

        $slots = for ($slot = 0; $slot -le 18; $slot++) {
            [ordered]@{
                slot = $slot
                slot_name = "slot_$slot"
                item_guid = if ($slot -eq $EquippedSlot -and $EquippedGuid -ne 0) { $EquippedGuid } else { $null }
                entry = if ($slot -eq $EquippedSlot -and $EquippedGuid -ne 0) { $EquippedEntry } else { $null }
                name = if ($slot -eq $EquippedSlot -and $EquippedGuid -ne 0) { "Item $EquippedEntry" } else { $null }
                quality = if ($slot -eq $EquippedSlot -and $EquippedGuid -ne 0) { 3 } else { $null }
                item_level = if ($slot -eq $EquippedSlot -and $EquippedGuid -ne 0) { 60 } else { $null }
                durability = if ($slot -eq $EquippedSlot -and $EquippedGuid -ne 0) { 40 } else { $null }
                count = if ($slot -eq $EquippedSlot -and $EquippedGuid -ne 0) { 1 } else { $null }
            }
        }
        $snapshot = [ordered]@{
            schema_version = 'raid-loot-snapshot-v1'
            captured_utc = '2026-07-16T00:00:00Z'
            roster_guids = @($CharacterGuid)
            source = [ordered]@{ read_only = $true }
            characters = @([ordered]@{
                guid = $CharacterGuid
                equipped_slots = @($slots)
                carried_items = @($CarriedItems)
                aggregates = [ordered]@{}
            })
            aggregates = [ordered]@{}
            comparison = $null
        }
        $snapshot | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Path -Encoding utf8
    }

    function New-ExactChainLines {
        param(
            [ValidateSet('need','greed')][string]$Vote = 'need',
            [uint32]$VoteUsage = 2,
            [uint64]$EvaluationItemRaw = $script:ItemRaw,
            [uint32]$EvaluationItemCounter = 9001,
            [uint64]$EquipItemRaw = $script:ItemRaw,
            [uint32]$EquipItemCounter = 9001,
            [uint64]$AwardRollRaw = $script:RollRaw,
            [uint32]$AwardRollCounter = 5001
        )

        return @(
            "2026-07-16T00:00:01Z [RaidLoot] event=vote source=periodic bot=Tester bot_guid=101 item=500 item_guid=$($script:RollRaw) usage=$VoteUsage vote=$Vote"
            "2026-07-16T00:00:02Z [RaidLoot] event=award winner=Tester winner_guid=101 winner_guid_counter=101 item=500 entry=500 roll_guid=$AwardRollRaw roll_guid_counter=$AwardRollCounter item_guid=$($script:ItemRaw) item_guid_counter=9001 count=1 vote=$Vote"
            "2026-07-16T00:00:03Z [RaidLoot] event=evaluate trigger=inventory_upgrade_scan source=item_push_result bot=Tester bot_guid=101 bot_guid_counter=101 item=500 entry=500 item_guid=$EvaluationItemRaw item_guid_counter=$EvaluationItemCounter usage=2 selected=true"
            "2026-07-16T00:00:04Z [RaidLoot] event=equip bot=Tester bot_guid=101 bot_guid_counter=101 item=500 entry=500 item_guid=$EquipItemRaw item_guid_counter=$EquipItemCounter slot=0 previous_item=400 previous_item_guid_counter=7001 result=equipped"
        )
    }

    function Invoke-AcceptanceFixture {
        param(
            [Parameter(Mandatory)][string]$Log,
            [Parameter(Mandatory)][string]$Before,
            [Parameter(Mandatory)][string]$After,
            [hashtable]$Extra = @{}
        )

        $arguments = @{
            PlayerbotsLogPath = $Log
            BeforeSnapshotPath = $Before
            AfterSnapshotPath = $After
        }
        foreach ($entry in $Extra.GetEnumerator()) { $arguments[$entry.Key] = $entry.Value }
        return ((& $script:Tool @arguments | Out-String) | ConvertFrom-Json)
    }
}

Describe 'raid-loot-equip-acceptance.ps1' {
    BeforeEach {
        $script:Log = Join-Path $TestDrive 'Playerbots.log'
        $script:Before = Join-Path $TestDrive 'before.json'
        $script:After = Join-Path $TestDrive 'after.json'
        New-TestSnapshot -Path $script:Before -EquippedGuid 7001 -EquippedEntry 400
        New-TestSnapshot -Path $script:After -EquippedGuid 9001 -EquippedEntry 500 -CarriedItems @(
            [ordered]@{ item_guid=7001; entry=400; name='Old Item'; quality=3; item_level=50; durability=30; count=1; bag=0; slot=23 }
        )
    }

Context 'raid loot equip exact-instance acceptance' {
    It 'passes only an exact NEED award evaluation equip and snapshot transition chain' {
        New-ExactChainLines | Set-Content -LiteralPath $script:Log -Encoding utf8

        $result = Invoke-AcceptanceFixture -Log $script:Log -Before $script:Before -After $script:After -Extra @{ RosterGuid = '101' }

        $result.status | Should -BeExactly 'PASS'
        $result.read_only | Should -BeTrue
        $result.qualifying_candidate_count | Should -Be 1
        $result.sample.roll_guid_counter | Should -Be 5001
        $result.sample.item_guid_counter | Should -Be 9001
        $result.sample.equipped_slot | Should -Be 0
        $result.sample.previous_item_guid_counter | Should -Be 7001
        $result.sample.snapshot_transition.before_item_guid_counter | Should -Be 7001
        $result.sample.snapshot_transition.after_item_guid_counter | Should -Be 9001
    }

    It 'rejects a same-entry evaluation of a different item instance' {
        $lines = New-ExactChainLines -EvaluationItemRaw $script:OtherItemRaw -EvaluationItemCounter 9002 -EquipItemRaw $script:OtherItemRaw -EquipItemCounter 9002
        $lines | Set-Content -LiteralPath $script:Log -Encoding utf8

        $result = Invoke-AcceptanceFixture $script:Log $script:Before $script:After

        $result.status | Should -BeExactly 'FAIL'
        $result.reasons | Should -Contain 'mismatched_evaluation_item_guid'
        $result.sample | Should -BeNullOrEmpty
    }

    It 'rejects an award associated with a different synthetic roll GUID' {
        New-ExactChainLines -AwardRollRaw ([uint64]4294972298) -AwardRollCounter 5002 | Set-Content -LiteralPath $script:Log -Encoding utf8

        $result = Invoke-AcceptanceFixture $script:Log $script:Before $script:After

        $result.status | Should -BeExactly 'FAIL'
        $result.reasons | Should -Contain 'mismatched_roll_guid'
    }

    It 'returns NO_SAMPLE for greed and non-upgrade votes' -TestCases @(
        @{ Vote = 'greed'; Usage = [uint32]2 }
        @{ Vote = 'need'; Usage = [uint32]4 }
    ) {
        param($Vote, $Usage)
        New-ExactChainLines -Vote $Vote -VoteUsage $Usage | Set-Content -LiteralPath $script:Log -Encoding utf8

        $result = Invoke-AcceptanceFixture $script:Log $script:Before $script:After

        $result.status | Should -BeExactly 'NO_SAMPLE'
        $result.reasons | Should -Contain 'no_qualifying_natural_need_upgrade'
    }

    It 'returns NO_SAMPLE honestly when the interval has no loot telemetry' {
        '2026-07-16T00:00:00Z ordinary playerbots log line' | Set-Content -LiteralPath $script:Log -Encoding utf8

        $result = Invoke-AcceptanceFixture $script:Log $script:Before $script:After

        $result.status | Should -BeExactly 'NO_SAMPLE'
        $result.telemetry_event_count | Should -Be 0
    }

    It 'fails when the after snapshot does not place the awarded low GUID in the logged slot' {
        New-ExactChainLines | Set-Content -LiteralPath $script:Log -Encoding utf8
        New-TestSnapshot -Path $script:After -EquippedGuid 9001 -EquippedEntry 500 -EquippedSlot 1

        $result = Invoke-AcceptanceFixture $script:Log $script:Before $script:After

        $result.status | Should -BeExactly 'FAIL'
        $result.reasons | Should -Contain 'after_snapshot_awarded_item_not_equipped_in_logged_slot'
    }

    It 'fails when the logged previous item does not match the before snapshot' {
        New-ExactChainLines | Set-Content -LiteralPath $script:Log -Encoding utf8
        New-TestSnapshot -Path $script:Before -EquippedGuid 7002 -EquippedEntry 401

        $result = Invoke-AcceptanceFixture $script:Log $script:Before $script:After

        $result.status | Should -BeExactly 'FAIL'
        $result.reasons | Should -Contain 'before_snapshot_previous_item_mismatch'
    }

    It 'requires a reset when fixture-init evidence occurs inside the selected interval' {
        $lines = @('2026-07-16T00:00:00Z event=bridge_mutation action=fixture-init guid=101') + @(New-ExactChainLines)
        $lines | Set-Content -LiteralPath $script:Log -Encoding utf8

        $result = Invoke-AcceptanceFixture $script:Log $script:Before $script:After

        $result.status | Should -BeExactly 'RESET_REQUIRED'
        $result.reasons | Should -Contain 'fixture_init_evidence_in_interval'
    }

    It 'can exclude earlier fixture-init evidence with exact line bounds' {
        $lines = @('2026-07-16T00:00:00Z event=bridge_mutation action=fixture-init guid=101') + @(New-ExactChainLines)
        $lines | Set-Content -LiteralPath $script:Log -Encoding utf8

        $result = Invoke-AcceptanceFixture -Log $script:Log -Before $script:Before -After $script:After -Extra @{ StartLine = 2 }

        $result.status | Should -BeExactly 'PASS'
        $result.interval.start_line | Should -Be 2
    }
}

Context 'raid loot equip acceptance input safety and syntax' {
    It 'rejects missing files, malformed JSON, and mismatched rosters' {
        New-ExactChainLines | Set-Content -LiteralPath $script:Log -Encoding utf8
        { & $script:Tool -PlayerbotsLogPath (Join-Path $TestDrive 'missing.log') -BeforeSnapshotPath $script:Before -AfterSnapshotPath $script:After } |
            Should -Throw '*Playerbots log not found*'

        '{not-json' | Set-Content -LiteralPath $script:Before -Encoding utf8
        { Invoke-AcceptanceFixture $script:Log $script:Before $script:After } | Should -Throw '*not valid JSON*'

        New-TestSnapshot -Path $script:Before -EquippedGuid 7001 -EquippedEntry 400
        New-TestSnapshot -Path $script:After -EquippedGuid 9001 -EquippedEntry 500 -CharacterGuid 102
        { Invoke-AcceptanceFixture $script:Log $script:Before $script:After } | Should -Throw '*rosters must exactly match*'
    }

    It 'rejects malformed telemetry instead of accepting a partial identity' {
        $lines = @(New-ExactChainLines)
        $lines[2] = $lines[2] -replace ' item_guid_counter=9001', ''
        $lines | Set-Content -LiteralPath $script:Log -Encoding utf8

        { Invoke-AcceptanceFixture $script:Log $script:Before $script:After } |
            Should -Throw "*missing 'item_guid_counter'*"
    }

    It 'rejects unsafe roster values and invalid intervals' {
        New-ExactChainLines | Set-Content -LiteralPath $script:Log -Encoding utf8

        { Invoke-AcceptanceFixture -Log $script:Log -Before $script:Before -After $script:After -Extra @{ RosterGuid = '101 OR 1=1' } } |
            Should -Throw '*Roster GUID*'
        { Invoke-AcceptanceFixture -Log $script:Log -Before $script:Before -After $script:After -Extra @{ StartLine = 4; EndLine = 2 } } |
            Should -Throw '*StartLine must be less than or equal to EndLine*'
    }

    It 'has valid PowerShell syntax and no database bridge or write surface' {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path -LiteralPath $script:Tool).Path, [ref]$tokens, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0

        $source = Get-Content -LiteralPath $script:Tool -Raw
        $source | Should -Not -Match '(?i)\b(Set-Content|Add-Content|Out-File|Remove-Item|Move-Item|Copy-Item|Invoke-RestMethod|Invoke-WebRequest|Start-Process|mysql(?:\.exe)?|Invoke-Sqlcmd)\b'
        $source | Should -Not -Match '(?i)\b(INSERT|UPDATE|DELETE|REPLACE|DROP|ALTER|CREATE|TRUNCATE)\s+(?:INTO|TABLE|DATABASE|VIEW)?\b'
        $source | Should -Not -Match '(?i)\bAutoWowBridge\b|\b-Action\s+(?:fixture-init|equip|loot)\b'
    }
}
}
