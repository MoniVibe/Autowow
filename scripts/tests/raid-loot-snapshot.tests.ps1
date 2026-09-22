Describe 'raid-loot-snapshot.ps1' {
    BeforeAll {
        $script:Tool = Join-Path $PSScriptRoot '..\raid-loot-snapshot.ps1'
        $script:Config = Join-Path $TestDrive 'worldserver.conf'
        @'
# Fixture credentials are deliberately fake.
WorldDatabaseInfo = "db.fixture;3306;world_reader;world_secret;acore_world_fixture"
CharacterDatabaseInfo = "db.fixture;3306;character_reader;character_secret;acore_characters_fixture"
'@ | Set-Content -LiteralPath $script:Config -Encoding utf8

        $baseRows = @(
            [pscustomobject]@{ character_guid='202'; bag='25'; slot='1'; item_guid='9003'; entry='33470'; name='Frostweave Cloth'; quality='1'; item_level='70'; durability='0'; count='4' }
            [pscustomobject]@{ character_guid='101'; bag='0'; slot='0'; item_guid='9001'; entry='4080'; name='Blackforge Cowl'; quality='2'; item_level='45'; durability='40'; count='1' }
            [pscustomobject]@{ character_guid='101'; bag='0'; slot='23'; item_guid='9002'; entry='6948'; name='Hearthstone'; quality='1'; item_level='1'; durability='0'; count='1' }
        )
        $script:FixtureQuery = {
            param($QueryName, $Sql, $Headers, $Connection)
            if ($QueryName -ne 'inventory') { throw "Unexpected query: $QueryName" }
            return $baseRows
        }.GetNewClosure()
    }

    It 'captures every equipped slot, carried items, and aggregate counts for the exact roster' {
        $snapshot = (& $script:Tool -RosterGuid 202,101 -WorldServerConfigPath $script:Config -QueryExecutor $script:FixtureQuery | Out-String) | ConvertFrom-Json

        $snapshot.schema_version | Should -BeExactly 'raid-loot-snapshot-v1'
        @($snapshot.roster_guids) | Should -Be @(101,202)
        @($snapshot.characters).Count | Should -Be 2
        @($snapshot.characters[0].equipped_slots).Count | Should -Be 19
        $snapshot.characters[0].equipped_slots[0].item_guid | Should -Be 9001
        $snapshot.characters[0].equipped_slots[1].item_guid | Should -BeNullOrEmpty
        @($snapshot.characters[0].carried_items).Count | Should -Be 1
        $snapshot.characters[0].aggregates.stack_count | Should -Be 2
        $snapshot.aggregates.stack_count | Should -Be 6
        $snapshot.source.read_only | Should -BeTrue
    }

    It 'uses one SELECT-only query limited to the three approved tables and exact GUID list' {
        $holder = [pscustomobject]@{ Sql = $null }
        $query = {
            param($QueryName, $Sql, $Headers, $Connection)
            $holder.Sql = $Sql
            return @()
        }.GetNewClosure()
        $null = & $script:Tool -RosterGuid 202,101 -WorldServerConfigPath $script:Config -QueryExecutor $query

        $holder.Sql | Should -Match '^SELECT\b'
        $holder.Sql | Should -Match 'WHERE ci\.guid IN \(101,202\)'
        $holder.Sql | Should -Match '\.character_inventory\b'
        $holder.Sql | Should -Match '\.item_instance\b'
        $holder.Sql | Should -Match '\.item_template\b'
        $holder.Sql | Should -Not -Match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE INTO|CALL|SET|GRANT|REVOKE)\b'
    }

    It 'does not expose credentials in JSON output' {
        $json = & $script:Tool -RosterGuid 101,202 -WorldServerConfigPath $script:Config -QueryExecutor $script:FixtureQuery | Out-String
        $json | Should -Not -Match 'character_secret|world_secret|character_reader|world_reader'
        $json | Should -Match 'acore_characters_fixture'
        $json | Should -Match 'acore_world_fixture'
    }

    It 'writes deterministic JSON to an optional output path without also emitting it' {
        $path = Join-Path $TestDrive 'snapshot.json'
        $emitted = @(& $script:Tool -RosterGuid 101,202 -WorldServerConfigPath $script:Config -OutputPath $path -QueryExecutor $script:FixtureQuery)

        $emitted.Count | Should -Be 0
        $path | Should -Exist
        $snapshot = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        @($snapshot.PSObject.Properties.Name) | Should -Be @('schema_version','captured_utc','roster_guids','source','characters','aggregates','comparison')
        @($snapshot.characters | ForEach-Object guid) | Should -Be @(101,202)
    }

    It 'rejects malformed, zero, overflowing, and duplicate roster GUIDs' -TestCases @(
        @{ Roster = @('1.5') }
        @{ Roster = @('0') }
        @{ Roster = @('4294967296') }
        @{ Roster = @('101','101') }
    ) {
        param($Roster)
        { & $script:Tool -RosterGuid $Roster -WorldServerConfigPath $script:Config -QueryExecutor $script:FixtureQuery } | Should -Throw
    }

    It 'rejects fixture rows outside the exact roster' {
        { & $script:Tool -RosterGuid 101 -WorldServerConfigPath $script:Config -QueryExecutor $script:FixtureQuery } |
            Should -Throw '*outside the exact roster*'
    }

    It 'compares baselines for acquired, removed, replaced, and item-level changes' {
        $baselinePath = Join-Path $TestDrive 'baseline.json'
        $currentPath = Join-Path $TestDrive 'current.json'
        $baselineRows = @(
            [pscustomobject]@{ character_guid='101'; bag='0'; slot='0'; item_guid='7001'; entry='100'; name='Old Helm'; quality='2'; item_level='40'; durability='30'; count='1' }
            [pscustomobject]@{ character_guid='101'; bag='0'; slot='23'; item_guid='7002'; entry='200'; name='Old Token'; quality='1'; item_level='1'; durability='0'; count='1' }
        )
        $baselineQuery = { param($n,$s,$h,$c) return $baselineRows }.GetNewClosure()
        $null = & $script:Tool -RosterGuid 101 -WorldServerConfigPath $script:Config -OutputPath $baselinePath -QueryExecutor $baselineQuery
        $currentRows = @(
            [pscustomobject]@{ character_guid='101'; bag='0'; slot='0'; item_guid='8001'; entry='101'; name='Raid Helm'; quality='4'; item_level='60'; durability='45'; count='1' }
            [pscustomobject]@{ character_guid='101'; bag='0'; slot='24'; item_guid='8002'; entry='201'; name='Raid Token'; quality='3'; item_level='50'; durability='0'; count='2' }
        )
        $currentQuery = { param($n,$s,$h,$c) return $currentRows }.GetNewClosure()
        $null = & $script:Tool -RosterGuid 101 -WorldServerConfigPath $script:Config -BaselinePath $baselinePath -OutputPath $currentPath -QueryExecutor $currentQuery
        $snapshot = Get-Content -LiteralPath $currentPath -Raw | ConvertFrom-Json

        @($snapshot.comparison.new_item_guids | ForEach-Object item_guid) | Should -Be @(8001,8002)
        @($snapshot.comparison.removed_item_guids | ForEach-Object item_guid) | Should -Be @(7001,7002)
        @($snapshot.comparison.equipped_slot_replacements).Count | Should -Be 1
        $snapshot.comparison.equipped_slot_replacements[0].item_level_delta | Should -Be 20
        $snapshot.comparison.character_item_level_deltas[0].equipped_item_level_sum_delta | Should -Be 20
    }

    It 'requires the baseline roster to match exactly' {
        $baselinePath = Join-Path $TestDrive 'wrong-roster.json'
        $query = { param($n,$s,$h,$c) return @() }
        $null = & $script:Tool -RosterGuid 101,202 -WorldServerConfigPath $script:Config -OutputPath $baselinePath -QueryExecutor $query
        { & $script:Tool -RosterGuid 101 -WorldServerConfigPath $script:Config -BaselinePath $baselinePath -QueryExecutor $query } |
            Should -Throw '*exactly match*'
    }

    It 'uses the injected query seam without resolving or invoking a MySQL client' {
        $snapshot = (& $script:Tool -RosterGuid 101,202 -WorldServerConfigPath $script:Config -MySqlPath 'Z:\definitely-missing\mysql.exe' -QueryExecutor $script:FixtureQuery | Out-String) | ConvertFrom-Json
        $snapshot.source.query_count | Should -Be 1
    }
}
