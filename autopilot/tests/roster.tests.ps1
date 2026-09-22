# AutoWoW Autopilot V1.2 - roster discovery module tests.
# Run: Invoke-Pester -Path .\autopilot\tests\roster.tests.ps1 -Output Detailed
# Fully offline: fixture snapshots plus TestDrive-only scratch documents.
# No sockets, no database, no server process, no writes outside TestDrive.

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotRoster.ps1')

    $script:RosterLeague = Join-Path $script:AutopilotRoot 'fixtures\roster\league-sample.json'
    $script:RosterUnknownHeavy = Join-Path $script:AutopilotRoot 'fixtures\roster\unknown-heavy.json'
    # Matches observed_utc in league-sample.json.
    $script:T0 = [datetime]::new(2026, 7, 18, 20, 0, 0, [System.DateTimeKind]::Utc)

    function New-LeagueProvider { New-AutopilotRosterProvider -Kind fixture -Path $script:RosterLeague }
}

Describe 'Roster fixture provider' {
    It 'parses league-sample into 11 characters keyed by guid' {
        $provider = New-LeagueProvider
        $provider.Kind | Should -Be 'fixture'
        $provider.Available | Should -BeTrue
        $provider.Reason | Should -Be 'roster snapshot loaded'
        $provider.SnapshotPath | Should -Be $script:RosterLeague
        $provider.Characters.Count | Should -Be 11
        $provider.ObservedUtc | Should -Be $script:T0
        $provider.ObservedUtc.Kind | Should -Be ([System.DateTimeKind]::Utc)
        ($null -eq $provider.ServerSessionId) | Should -BeTrue
        foreach ($guid in @(10, 7, 6, 3, 2, 8, 24, 26, 45, 49, 99)) {
            $provider.Characters.Contains([int64]$guid) | Should -BeTrue
        }
    }

    It 'fails closed on an unknown schema version' {
        $path = Join-Path $TestDrive 'roster-v2.json'
        Write-AutopilotFileAtomic -Path $path -Content '{"schema":"autowow.autopilot.roster-snapshot.v1","schema_version":2,"source":"fixture","observed_utc":"2026-07-18T20:00:00Z","server_session_id":null,"characters":[]}'
        $provider = New-AutopilotRosterProvider -Kind fixture -Path $path
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Match 'not supported'
        $provider.Reason | Should -Match 'fail closed'
        $provider.Characters.Count | Should -Be 0
    }

    It 'fails closed on an unknown schema name' {
        $path = Join-Path $TestDrive 'roster-wrong-schema.json'
        Write-AutopilotFileAtomic -Path $path -Content '{"schema":"autowow.somebody.else.v1","schema_version":1,"source":"fixture","observed_utc":"2026-07-18T20:00:00Z","server_session_id":null,"characters":[]}'
        $provider = New-AutopilotRosterProvider -Kind fixture -Path $path
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Match 'not supported'
    }

    It 'fails closed on malformed JSON' {
        $path = Join-Path $TestDrive 'roster-malformed.json'
        Write-AutopilotFileAtomic -Path $path -Content '{ this is not JSON !!!'
        $provider = New-AutopilotRosterProvider -Kind fixture -Path $path
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Match 'unreadable'
        $provider.Reason | Should -Match 'fail closed'
        (Get-AutopilotRosterFacts -Provider $provider).Count | Should -Be 0
    }

    It 'fails closed on a missing snapshot file' {
        $provider = New-AutopilotRosterProvider -Kind fixture -Path (Join-Path $TestDrive 'nope.json')
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Match 'not found'
    }

    It 'fails closed on a duplicate character guid' {
        $path = Join-Path $TestDrive 'roster-dup.json'
        Write-AutopilotFileAtomic -Path $path -Content '{"schema":"autowow.autopilot.roster-snapshot.v1","schema_version":1,"source":"fixture","observed_utc":"2026-07-18T20:00:00Z","server_session_id":null,"characters":[{"guid":5,"name":"A","observed_utc":"2026-07-18T20:00:00Z","unknown_fields":[]},{"guid":5,"name":"B","observed_utc":"2026-07-18T20:00:00Z","unknown_fields":[]}]}'
        $provider = New-AutopilotRosterProvider -Kind fixture -Path $path
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Match 'duplicate'
    }
}

Describe 'Bridge and unavailable providers stay honest' {
    It 'reports the bridge roster provider as not deployed rather than inferring' {
        $provider = New-AutopilotRosterProvider -Kind bridge
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Be 'bridge roster provider not deployed; fail closed - facts are never guessed'
        (Get-AutopilotRosterFacts -Provider $provider).Count | Should -Be 0
        ($null -eq (Get-AutopilotRosterFact -Provider $provider -Guid 10)) | Should -BeTrue
    }

    It 'returns null facts and an empty roster from the unavailable provider' {
        $provider = New-AutopilotRosterProvider -Kind unavailable
        $provider.Available | Should -BeFalse
        (Get-AutopilotRosterFacts -Provider $provider).Count | Should -Be 0
        ($null -eq (Get-AutopilotRosterFact -Provider $provider -Guid 2)) | Should -BeTrue
    }
}

Describe 'Fact lookup' {
    It 'returns the character ordered hashtable on a guid hit' {
        $provider = New-LeagueProvider
        $fact = Get-AutopilotRosterFact -Provider $provider -Guid 10
        $fact | Should -Not -BeNullOrEmpty
        [string]$fact.name | Should -Be 'Brandreas'
        [int64]$fact.level | Should -Be 9
        [string]$fact.class | Should -Be 'Warrior'
        @($fact.roles) | Should -Be @('tank', 'dps')
    }

    It 'returns null on a guid miss without inventing a character' {
        $provider = New-LeagueProvider
        ($null -eq (Get-AutopilotRosterFact -Provider $provider -Guid 12345)) | Should -BeTrue
    }

    It 'enumerates all facts as an array' {
        $provider = New-LeagueProvider
        $facts = Get-AutopilotRosterFacts -Provider $provider
        @($facts).Count | Should -Be 11
        @($facts | ForEach-Object { [int64]$_.guid } | Sort-Object) | Should -Be @(2, 3, 6, 7, 8, 10, 24, 26, 45, 49, 99)
    }
}

Describe 'Fact freshness (observed_utc, UTC-safe)' {
    It 'is fresh within MaxAgeSeconds and exactly at the boundary' {
        $provider = New-LeagueProvider
        $fact = Get-AutopilotRosterFact -Provider $provider -Guid 2
        Test-AutopilotRosterFactFresh -Fact $fact -Now $script:T0.AddMinutes(4) | Should -BeTrue
        Test-AutopilotRosterFactFresh -Fact $fact -Now $script:T0.AddSeconds(300) | Should -BeTrue
    }

    It 'is stale past MaxAgeSeconds, honoring a custom window' {
        $provider = New-LeagueProvider
        $fact = Get-AutopilotRosterFact -Provider $provider -Guid 2
        Test-AutopilotRosterFactFresh -Fact $fact -Now $script:T0.AddSeconds(301) | Should -BeFalse
        Test-AutopilotRosterFactFresh -Fact $fact -Now $script:T0.AddMinutes(6) | Should -BeFalse
        Test-AutopilotRosterFactFresh -Fact $fact -Now $script:T0.AddMinutes(6) -MaxAgeSeconds 600 | Should -BeTrue
    }

    It 'fails closed on a missing, null, unparseable, or future-dated observed_utc' {
        Test-AutopilotRosterFactFresh -Fact ([ordered]@{ guid = 1 }) -Now $script:T0 | Should -BeFalse
        Test-AutopilotRosterFactFresh -Fact ([ordered]@{ observed_utc = $null }) -Now $script:T0 | Should -BeFalse
        Test-AutopilotRosterFactFresh -Fact ([ordered]@{ observed_utc = 'not-a-time' }) -Now $script:T0 | Should -BeFalse
        Test-AutopilotRosterFactFresh -Fact $null -Now $script:T0 | Should -BeFalse
        # An observation dated after Now is not provably fresh either.
        $provider = New-LeagueProvider
        $fact = Get-AutopilotRosterFact -Provider $provider -Guid 2
        Test-AutopilotRosterFactFresh -Fact $fact -Now $script:T0.AddMinutes(-10) | Should -BeFalse
    }
}

Describe 'Suitability judgments (explanations over scores)' {
    BeforeAll {
        $script:League = New-LeagueProvider
    }

    It 'judges Nathalis raid-suitable with nothing missing or unknown' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 2 -Activity raid
        $result.suitable | Should -BeTrue
        @($result.missing).Count | Should -Be 0
        @($result.unknown).Count | Should -Be 0
        $result.summary | Should -Match 'suitable for raid'
    }

    It 'judges Brandreas raid-unsuitable with level and item level both in missing' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 10 -Activity raid
        ($result.suitable -is [bool]) | Should -BeTrue
        $result.suitable | Should -BeFalse
        $names = @($result.missing | ForEach-Object { $_.requirement })
        $names | Should -Contain 'minimum level'
        $names | Should -Contain 'minimum equipped item level'
        $levelMiss = @($result.missing | Where-Object { $_.requirement -eq 'minimum level' })[0]
        $levelMiss.detail | Should -Match 'level 9'
        $levelMiss.detail | Should -Match '80'
        @($result.unknown).Count | Should -Be 0
    }

    It 'judges Brandreas pvp-unsuitable at the default minimum level 10' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 10 -Activity pvp
        ($result.suitable -is [bool]) | Should -BeTrue
        $result.suitable | Should -BeFalse
        $names = @($result.missing | ForEach-Object { $_.requirement })
        $names | Should -Contain 'minimum level'
    }

    It 'judges Saewash at level 10 pvp-suitable' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 7 -Activity pvp
        $result.suitable | Should -BeTrue
        @($result.missing).Count | Should -Be 0
        @($result.unknown).Count | Should -Be 0
    }

    It 'judges Kurl gather-suitable on Mining 305' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 6 -Activity gather
        $result.suitable | Should -BeTrue
        @($result.missing).Count | Should -Be 0
        @($result.unknown).Count | Should -Be 0
    }

    It 'returns null gather suitability for guid 99 with the unobserved facts listed, never guessed' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 99 -Activity gather
        ($null -eq $result.suitable) | Should -BeTrue
        @($result.missing).Count | Should -Be 0
        $fields = @($result.unknown | ForEach-Object { $_.field })
        $fields | Should -Contain 'professions'
        @($result.unknown | Where-Object { $_.field -eq 'professions' })[0].why | Should -Be 'fact not observed'
        $result.summary | Should -Match 'never guessed|stays unknown'
    }

    It 'judges offline Pikli gather-unsuitable with online in missing and her unobserved facts in unknown' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 49 -Activity gather
        ($result.suitable -is [bool]) | Should -BeTrue
        $result.suitable | Should -BeFalse
        $names = @($result.missing | ForEach-Object { $_.requirement })
        $names | Should -Contain 'online'
        # A definitive miss coexists with honestly listed unknowns (alive, bag_slots_free).
        $fields = @($result.unknown | ForEach-Object { $_.field })
        $fields | Should -Contain 'alive'
        $fields | Should -Contain 'bag_slots_free'
    }

    It 'judges the level-80 five dungeon-suitable' {
        foreach ($guid in @(2, 8, 24, 26, 45)) {
            $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid $guid -Activity dungeon
            $result.suitable | Should -BeTrue
            @($result.missing).Count | Should -Be 0
            @($result.unknown).Count | Should -Be 0
        }
    }

    It 'judges Nathalis craft-suitable and guid 99 craft-unknown (money and professions unobserved)' {
        (Get-AutopilotRosterSuitability -Provider $script:League -Guid 2 -Activity craft).suitable | Should -BeTrue
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 99 -Activity craft
        ($null -eq $result.suitable) | Should -BeTrue
        $fields = @($result.unknown | ForEach-Object { $_.field })
        $fields | Should -Contain 'professions'
        $fields | Should -Contain 'money_copper'
    }

    It 'honors Context overrides of the default thresholds' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 10 -Activity raid -Context @{ minLevel = 5; minItemLevel = 5 }
        $result.suitable | Should -BeTrue
    }

    It 'returns null suitability with a typed unknown when the provider is unavailable' {
        $provider = New-AutopilotRosterProvider -Kind unavailable
        $result = Get-AutopilotRosterSuitability -Provider $provider -Guid 2 -Activity raid
        ($null -eq $result.suitable) | Should -BeTrue
        @($result.missing).Count | Should -Be 0
        @($result.unknown | ForEach-Object { $_.field }) | Should -Contain 'character'
        $result.summary | Should -Match 'never guessed'
    }

    It 'returns null suitability for a guid absent from the roster' {
        $result = Get-AutopilotRosterSuitability -Provider $script:League -Guid 777 -Activity dungeon
        ($null -eq $result.suitable) | Should -BeTrue
        @($result.unknown)[0].why | Should -Match 'not present'
    }
}

Describe 'Unknown-heavy roster (honesty path)' {
    BeforeAll {
        $script:Heavy = New-AutopilotRosterProvider -Kind fixture -Path $script:RosterUnknownHeavy
    }

    It 'parses the unknown-heavy snapshot' {
        $script:Heavy.Available | Should -BeTrue
        $script:Heavy.Characters.Count | Should -Be 3
    }

    It 'keeps nulled facts null and mirrored in unknown_fields, not defaulted' {
        $fact = Get-AutopilotRosterFact -Provider $script:Heavy -Guid 201
        ($null -eq $fact.level) | Should -BeTrue
        ($null -eq $fact.online) | Should -BeTrue
        ($null -eq $fact.professions) | Should -BeTrue
        @($fact.unknown_fields) | Should -Contain 'level'
        @($fact.unknown_fields) | Should -Contain 'online'
        @($fact.unknown_fields) | Should -Contain 'professions'
    }

    It 'returns null dungeon suitability for a fully unknown character with every needed fact listed' {
        $result = Get-AutopilotRosterSuitability -Provider $script:Heavy -Guid 201 -Activity dungeon
        ($null -eq $result.suitable) | Should -BeTrue
        @($result.missing).Count | Should -Be 0
        $fields = @($result.unknown | ForEach-Object { $_.field })
        foreach ($needed in @('online', 'alive', 'level', 'roles', 'party.raid')) { $fields | Should -Contain $needed }
        foreach ($entry in @($result.unknown)) { $entry.why | Should -Be 'fact not observed' }
    }

    It 'still reaches a definitive verdict when the facts that matter are observed' {
        # Tavr: level 42, online, alive are known; pvp needs nothing else.
        $result = Get-AutopilotRosterSuitability -Provider $script:Heavy -Guid 203 -Activity pvp
        $result.suitable | Should -BeTrue
        # But gather stays unknown because professions and bag space were not observed.
        $gather = Get-AutopilotRosterSuitability -Provider $script:Heavy -Guid 203 -Activity gather
        ($null -eq $gather.suitable) | Should -BeTrue
        @($gather.unknown | ForEach-Object { $_.field }) | Should -Contain 'professions'
    }
}
