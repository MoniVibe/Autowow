Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:LibraryPath = Join-Path $script:ScriptsRoot 'pvp-role-fixture-lib.ps1'
    $script:WorldPath = Join-Path $script:ScriptsRoot 'pvp-role-fixture-world.ps1'
    . $script:LibraryPath
    $script:Definition = Get-PvpRoleFixtureDefinition

    function New-CompletePvpRoleFixtureRows {
        $accounts = New-Object System.Collections.Generic.List[object]
        $characters = New-Object System.Collections.Generic.List[object]
        foreach ($member in @($script:Definition.ordered_roster)) {
            $accountId = 100 + [int]$member.ordinal
            $guid = 1000 + [int]$member.ordinal
            $accounts.Add([pscustomobject]@{ id = [string]$accountId; username = $member.account_name })
            $characters.Add([pscustomobject]@{
                guid = [string]$guid
                account = [string]$accountId
                name = $member.character_name
                race = [string]$member.race_id
                class = [string]$member.class_id
                gender = '0'
                level = '80'
                online = '0'
                map = [string]$member.position.map
            })
        }
        return [pscustomobject]@{ accounts = $accounts.ToArray(); characters = $characters.ToArray() }
    }

    function New-CompletePvpRoleFixtureEvidence {
        $rows = New-Object System.Collections.Generic.List[object]
        foreach ($member in @($script:Definition.ordered_roster)) {
            $rows.Add([pscustomobject]@{
                character_name = $member.character_name
                character_guid = [uint32](1000 + [int]$member.ordinal)
                class_id = [int]$member.class_id
                level = 80
                spec_index = [int]$member.spec_index
                spec_name = $member.spec_name
                combat_role = $member.role_category
                equipped_slots = 16
                expected_quality_slots = 16
                other_quality_slots = 0
            })
        }
        return $rows.ToArray()
    }
}

Describe 'Role-balanced 10v10 WSG fixture definition' {
    It 'defines the fixed isolated local fixture and exact 10+10 order' {
        $script:Definition.schema | Should -Be 'autowow.pvp.role-fixture.definition.v1'
        $script:Definition.fixture_id | Should -Be 'awrole1'
        $script:Definition.local_only | Should -BeTrue
        $script:Definition.disposable | Should -BeTrue
        @($script:Definition.ordered_roster).Count | Should -Be 20
        @($script:Definition.ordered_roster | Where-Object faction -eq 'Alliance').Count | Should -Be 10
        @($script:Definition.ordered_roster | Where-Object faction -eq 'Horde').Count | Should -Be 10
        (@($script:Definition.ordered_roster.ordinal) -join ',') | Should -Be ((1..20) -join ',')
        ($script:Definition.ordered_roster.account_name -join ',') | Should -Be (
            (@(1..10 | ForEach-Object { 'AWROLE1A{0:D2}' -f $_ }) + @(1..10 | ForEach-Object { 'AWROLE1H{0:D2}' -f $_ })) -join ','
        )
        Assert-PvpRoleFixtureDefinition -Definition $script:Definition | Should -BeTrue
    }

    It 'has an identical 1 carrier-tank, 2 healer, 7 DPS role mirror per faction' {
        foreach ($faction in @('Alliance', 'Horde')) {
            $side = @($script:Definition.ordered_roster | Where-Object faction -eq $faction)
            @($side | Where-Object role_category -eq 'tank').Count | Should -Be 1
            @($side | Where-Object role_category -eq 'healer').Count | Should -Be 2
            @($side | Where-Object role_category -eq 'dps').Count | Should -Be 7
        }
        foreach ($roleSlot in @($script:Definition.ordered_roster.role_slot | Sort-Object -Unique)) {
            $pair = @($script:Definition.ordered_roster | Where-Object role_slot -eq $roleSlot | Sort-Object faction)
            $pair.Count | Should -Be 2
            ($pair.class_id -join ',') | Should -Be "$($pair[0].class_id),$($pair[0].class_id)"
            ($pair.spec_index -join ',') | Should -Be "$($pair[0].spec_index),$($pair[0].spec_index)"
            ($pair.role_category -join ',') | Should -Be "$($pair[0].role_category),$($pair[0].role_category)"
            ((@($pair[0].capabilities | Sort-Object) -join ',') -eq (@($pair[1].capabilities | Sort-Object) -join ',')) | Should -BeTrue
        }
    }

    It 'uses exact faction-valid Wrath race/class pairs and PvP-oriented specs' {
        $expected = @(
            'flag-carrier-tank:Druid:1:Night Elf:Tauren',
            'healer-discipline:Priest:3:Human:Undead',
            'healer-restoration:Shaman:5:Draenei:Tauren',
            'dps-rogue:Rogue:5:Human:Undead',
            'dps-warrior:Warrior:3:Human:Orc',
            'dps-hunter:Hunter:4:Dwarf:Troll',
            'dps-mage:Mage:6:Gnome:Blood Elf',
            'dps-warlock:Warlock:3:Gnome:Orc',
            'dps-elemental:Shaman:3:Draenei:Troll',
            'dps-paladin:Paladin:5:Human:Blood Elf'
        )
        $actual = foreach ($roleSlot in @($script:Definition.ordered_roster.role_slot | Select-Object -First 10)) {
            $alliance = @($script:Definition.ordered_roster | Where-Object { $_.role_slot -eq $roleSlot -and $_.faction -eq 'Alliance' })[0]
            $horde = @($script:Definition.ordered_roster | Where-Object { $_.role_slot -eq $roleSlot -and $_.faction -eq 'Horde' })[0]
            "$roleSlot`:$($alliance.class_name):$($alliance.spec_index):$($alliance.race_name):$($horde.race_name)"
        }
        ($actual -join '|') | Should -Be ($expected -join '|')
    }

    It 'provides melee, physical-ranged, caster, and interrupt DPS coverage on both sides' {
        foreach ($faction in @('Alliance', 'Horde')) {
            $coverage = @($script:Definition.ordered_roster |
                Where-Object { $_.faction -eq $faction -and $_.role_category -eq 'dps' } |
                ForEach-Object capabilities | Sort-Object -Unique)
            foreach ($required in @('melee', 'physical-ranged', 'caster', 'interrupt')) { $coverage | Should -Contain $required }
        }
    }

    It 'uses deterministic client-valid names and no existing generic fixture identity' {
        @($script:Definition.ordered_roster | Where-Object character_name -NotMatch '^[A-Z][a-z]{2,11}$').Count | Should -Be 0
        @($script:Definition.ordered_roster | Where-Object account_name -NotMatch '^AWROLE1[AH](0[1-9]|10)$').Count | Should -Be 0
        @($script:Definition.ordered_roster.character_name | Sort-Object -Unique).Count | Should -Be 20
        $script:Definition.ordered_roster.account_name | Should -Not -Contain 'AWPVP1A01'
    }
}

Describe 'Idempotent exact manifest resolution' {
    It 'resolves the same ordered manifest regardless of database row order' {
        $rows = New-CompletePvpRoleFixtureRows
        $first = Resolve-PvpRoleFixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters
        $second = Resolve-PvpRoleFixtureManifest -Definition $script:Definition `
            -AccountRows @($rows.accounts | Sort-Object id -Descending) -CharacterRows @($rows.characters | Sort-Object guid -Descending)
        $first.complete | Should -BeTrue
        @($first.resolved_members).Count | Should -Be 20
        ($first.ordered_wsg_guids -join ',') | Should -Be ((1001..1020) -join ',')
        (($first.resolved_members | ConvertTo-Json -Depth 8 -Compress)) | Should -Be `
            (($second.resolved_members | ConvertTo-Json -Depth 8 -Compress))
    }

    It 'classifies only missing exact identities without mutating them' {
        $rows = New-CompletePvpRoleFixtureRows
        $partialCharacters = @($rows.characters | Select-Object -First 19)
        $manifest = Resolve-PvpRoleFixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $partialCharacters
        $manifest.complete | Should -BeFalse
        @($manifest.missing_accounts).Count | Should -Be 0
        @($manifest.missing_characters).Count | Should -Be 1
        $manifest.missing_characters[0].character_name | Should -Be 'Rolehpaladin'
    }

    It 'fails closed on ownership, race/class, and extra-character collisions' {
        $rows = New-CompletePvpRoleFixtureRows
        $rows.characters[0].account = $rows.accounts[1].id
        { Resolve-PvpRoleFixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters } | Should -Throw '*belongs to another account*'

        $rows = New-CompletePvpRoleFixtureRows
        $rows.characters[0].class = '1'
        { Resolve-PvpRoleFixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters } | Should -Throw '*unexpected race/class*'

        $rows = New-CompletePvpRoleFixtureRows
        $rows.characters += [pscustomobject]@{ guid = '9999'; account = '101'; name = 'Unrelated'; race = '1'; class = '1'; gender = '0'; level = '80'; online = '0'; map = '0' }
        { Resolve-PvpRoleFixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters } | Should -Throw '*owns more than one character*'
    }
}

Describe 'Scoped direct character bootstrap SQL' {
    It 'uses the existing safe SQL helpers, a named lock, and exact account/name predicates' {
        $member = @($script:Definition.ordered_roster)[0]
        $sql = New-PvpRoleFixtureCharacterInsertSql -CharactersDatabaseName 'acore_characters' -AccountId 123 -Member $member
        $sql | Should -Match 'GET_LOCK\(''autowow\.pvp\.role-fixture\.awrole1'', 30\)'
        $sql | Should -Match 'account = 123'
        $sql | Should -Match "name = 'Roleaflag'"
        $sql | Should -Match 'SELECT @pvp_role_fixture_guid, 123, ''Roleaflag'', 4, 11, 0,'
        $sql | Should -Match 'WHERE name = ''Roleaflag'' OR account = 123 OR guid = @pvp_role_fixture_guid'
        $sql | Should -Match 'character_homebind'
        $sql | Should -Not -Match '(?i)\b(DELETE|DROP|TRUNCATE|ALTER|UPDATE)\b'
    }

    It 'uses faction-correct bootstrap and homebind maps for both sides' {
        $allianceSql = New-PvpRoleFixtureCharacterInsertSql -CharactersDatabaseName 'chars' -AccountId 1 -Member $script:Definition.ordered_roster[0]
        $hordeSql = New-PvpRoleFixtureCharacterInsertSql -CharactersDatabaseName 'chars' -AccountId 2 -Member $script:Definition.ordered_roster[10]
        $allianceSql | Should -Match 'SELECT @pvp_role_fixture_guid, 0, 12,'
        $hordeSql | Should -Match 'SELECT @pvp_role_fixture_guid, 1, 14,'
    }
}

Describe 'Post-login exact fixture-init evidence' {
    It 'passes only exact per-member class, level, spec, role, and rare gear evidence' {
        $evidence = New-CompletePvpRoleFixtureEvidence
        $result = Test-PvpRoleFixtureLoadoutEvidence -Definition $script:Definition -EvidenceRows $evidence
        $result.status | Should -Be 'PASS'
        $result.evidence_count | Should -Be 20
        @($result.reasons).Count | Should -Be 0

        $evidence[0].spec_index = 9
        $failed = Test-PvpRoleFixtureLoadoutEvidence -Definition $script:Definition -EvidenceRows $evidence
        $failed.status | Should -Be 'FAIL'
        $failed.reasons | Should -Contain 'spec_mismatch:Roleaflag'
    }

    It 'accepts one compatible-item quality fallback but rejects two' {
        $evidence = New-CompletePvpRoleFixtureEvidence
        $evidence[0].expected_quality_slots = 15
        (Test-PvpRoleFixtureLoadoutEvidence -Definition $script:Definition -EvidenceRows $evidence).status | Should -Be 'PASS'

        $evidence[0].expected_quality_slots = 14
        $failed = Test-PvpRoleFixtureLoadoutEvidence -Definition $script:Definition -EvidenceRows $evidence
        $failed.status | Should -Be 'FAIL'
        $failed.reasons | Should -Contain 'gear_quality_mismatch:Roleaflag'
    }
}

Describe 'Dry-run and mutation safety contract' {
    It 'reports empty idempotent missing lists without strict-mode property enumeration' {
        $source = Get-Content -LiteralPath $script:WorldPath -Raw
        $source | Should -Not -Match '\$manifest\.missing_accounts\.account_name'
        $source | Should -Not -Match '\$manifest\.missing_characters\.character_name'
        $source | Should -Match '\$manifest\.missing_accounts\s*\|\s*ForEach-Object'
        $source | Should -Match '\$manifest\.missing_characters\s*\|\s*ForEach-Object'
    }

    It 'treats a reachable local bridge as a running WSL server for bootstrap safety' {
        $source = Get-Content -LiteralPath $script:WorldPath -Raw
        $source | Should -Match 'function Test-PvpRoleFixtureBridgeReady'
        $source | Should -Match 'Get-PvpRoleFixtureWorldserverProcesses[^\r\n]+-or \(Test-PvpRoleFixtureBridgeReady\)'
        $source | Should -Match '\$bridgeReady = Test-PvpRoleFixtureBridgeReady'
    }

    It 'dry-runs by default without connecting or mutating' {
        $result = (& $script:WorldPath) | ConvertFrom-Json
        $result.status | Should -Be 'DRY_RUN'
        $result.dry_run | Should -BeTrue
        $result.apply | Should -BeFalse
        $result.live_mutations_executed | Should -Be 0
        $result.plan.roster_count | Should -Be 20
        @($result.plan.account_console_commands).Count | Should -Be 20
        @($result.plan.forbidden_actions) | Should -Contain 'server restart'
    }

    It 'accepts the WoW account secret only from the environment and never emits it in dry-run' {
        $source = Get-Content -LiteralPath $script:WorldPath -Raw
        $source | Should -Match '\$env:WOW_ACCOUNT_PASSWORD'
        $source | Should -Match '\$password\.Length\s+-gt\s+16'
        $source | Should -Match '3-16 characters'
        $source | Should -Not -Match '(?i)param\([\s\S]*\[string\]\$.*password'
        $old = $env:WOW_ACCOUNT_PASSWORD
        try {
            $env:WOW_ACCOUNT_PASSWORD = 'NoLeak123'
            $json = (& $script:WorldPath) | Out-String
            $json | Should -Not -Match 'NoLeak123'
            $json | Should -Match '<WOW_ACCOUNT_PASSWORD>'
        }
        finally {
            if ($null -eq $old) { Remove-Item Env:WOW_ACCOUNT_PASSWORD -ErrorAction SilentlyContinue }
            else { $env:WOW_ACCOUNT_PASSWORD = $old }
        }
    }

    It 'does not build, restart, reconfigure, mutate WSG templates, or dispatch the queue' {
        $source = Get-Content -LiteralPath $script:WorldPath -Raw
        $source | Should -Not -Match '(?i)&\s*[^\r\n]*(start-world|stop-world|build\.ps1|bootstrap\.ps1)'
        $source | Should -Not -Match '(?i)Set-ConfigValue|Ensure-PvpFixtureExactWsgTemplate|UPDATE\s+battleground_template'
        $source | Should -Not -Match '(?i)-Action\s+wsg-(queue|leave|status)'
        $source | Should -Not -Match '(?i)autowow_league_|gathering'
        $source | Should -Match '\. \$roleFixtureLibrary'
        (Get-Content -LiteralPath $script:LibraryPath -Raw) | Should -Match '\. \$existingFixtureLibrary'
    }
}

Describe 'PvP role fixture PowerShell syntax' {
    It 'parse-checks only the new lane scripts and focused tests' {
        foreach ($path in @($script:LibraryPath, $script:WorldPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
