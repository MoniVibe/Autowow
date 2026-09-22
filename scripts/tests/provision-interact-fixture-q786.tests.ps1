Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:ProvisionPath = Join-Path $script:ScriptsRoot 'provision-interact-fixture-q786.ps1'
    $script:Source = Get-Content -LiteralPath $script:ProvisionPath -Raw
    $null = . $script:ProvisionPath
    $script:Definition = Get-Q786ProvisionDefinition

    function New-CleanQ786Rows {
        $accounts = @(
            [pscustomobject]@{ id = '501'; username = 'AWQ786OWNER' },
            [pscustomobject]@{ id = '502'; username = 'AWQ786HELPER' }
        )
        $characters = @()
        foreach ($member in @($script:Definition.ordered_members)) {
            $account = @($accounts | Where-Object username -eq $member.account_name)[0]
            $characters += [pscustomobject]@{
                guid = [string](9000 + [int]$member.ordinal)
                account = [string]$account.id
                name = $member.character_name
                race = [string]$member.race_id
                class = [string]$member.class_id
                gender = '0'
                level = '1'
                online = '0'
                map = '1'
                x = '-618.518'
                y = '-4251.67'
                z = '38.718'
                o = '0'
                home_map = '1'
                home_zone = '14'
                home_x = '-618.518'
                home_y = '-4251.67'
                home_z = '38.718'
                inventory_count = '0'
                questlog_count = '0'
                rewarded_count = '0'
            }
        }
        return [pscustomobject]@{ accounts = $accounts; characters = $characters }
    }

    function New-ResolvedQ786Manifest {
        $rows = New-CleanQ786Rows
        return Resolve-Q786FixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters
    }
}

Describe 'Dedicated q786 fixture definition and dry-run' {
    It 'defines exactly two immutable Horde identities with valid Wrath pairs in Durotar' {
        $members = @($script:Definition.ordered_members)
        $members.Count | Should -Be 2
        ($members.role -join ',') | Should -Be 'owner,helper'
        ($members.account_name -join ',') | Should -Be 'AWQ786OWNER,AWQ786HELPER'
        ($members.character_name -join ',') | Should -Be 'Kolkarown,Kolkarhelp'
        ($members.faction | Sort-Object -Unique) | Should -Be 'Horde'
        ($members.race_id -join ',') | Should -Be '2,2'
        ($members.class_id -join ',') | Should -Be '1,7'
        ($members.bootstrap_level -join ',') | Should -Be '1,1'
        ($members.fixture_init_level -join ',') | Should -Be '8,7'
        ($members.position.map -join ',') | Should -Be '1,1'
        ($members.position.zone -join ',') | Should -Be '14,14'
        ($members.position.x -join ',') | Should -Be '-618.518,-618.518'
        ($members.position.y -join ',') | Should -Be '-4251.67,-4251.67'
        Assert-Q786ProvisionDefinition -Definition $script:Definition | Should -BeTrue
    }

    It 'is dry-run by default and does not require a live root, bridge, database, or secret' {
        $missingRoot = Join-Path $TestDrive 'does-not-exist'
        $json = & $script:ProvisionPath -ServerRoot $missingRoot | Out-String
        $result = $json | ConvertFrom-Json
        $result.status | Should -Be 'DRY_RUN'
        $result.dry_run | Should -BeTrue
        $result.apply | Should -BeFalse
        $result.exact_scope.account_count | Should -Be 2
        $result.exact_scope.character_count | Should -Be 2
        @($result.account_bootstrap.commands).Count | Should -Be 2
        $result.live_mutations_executed | Should -Be 0
        Test-Path -LiteralPath $missingRoot | Should -BeFalse
    }

    It 'advertises the exact additive setup and forbids live normalization' {
        $plan = Get-Q786ProvisionPlan -Definition $script:Definition
        @($plan.controlled_setup).Count | Should -Be 3
        $plan.controlled_setup[0] | Should -Match 'only the two resolved GUIDs'
        $plan.controlled_setup[1] | Should -Match 'only the two resolved GUIDs'
        $plan.lifecycle.apply_requires_normal_worldserver_stopped | Should -BeTrue
        $plan.lifecycle.apply_requires_bridge_stopped | Should -BeTrue
        $plan.lifecycle.normal_server_stop_by_script | Should -BeFalse
        $plan.lifecycle.normal_server_start_or_restart_by_script | Should -BeFalse
        $plan.lifecycle.fixture_init_performed | Should -BeFalse
        $plan.forbidden_actions | Should -Contain 'fixture-init'
    }
}

Describe 'Exact fresh manifest resolution' {
    It 'accepts an empty initial league query so first-run enrollment can proceed' {
        $resolved = New-ResolvedQ786Manifest
        { Assert-Q786LeagueRows -Rows @() -ResolvedMembers $resolved.resolved_members } |
            Should -Not -Throw
        { Assert-Q786LeagueRows -Rows @() -ResolvedMembers $resolved.resolved_members -RequireComplete } |
            Should -Throw '*league enrollment is incomplete*'
    }

    It 'resolves exactly one owner and helper with ordered GUIDs and empty state' {
        $manifest = New-ResolvedQ786Manifest
        $manifest.complete | Should -BeTrue
        @($manifest.resolved_members).Count | Should -Be 2
        ($manifest.ordered_guids -join ',') | Should -Be '9001,9002'
        ($manifest.resolved_members.role -join ',') | Should -Be 'owner,helper'
        ($manifest.resolved_members.level_before_fixture_init -join ',') | Should -Be '1,1'
        ($manifest.resolved_members.inventory_rows -join ',') | Should -Be '0,0'
        ($manifest.resolved_members.quest_log_rows -join ',') | Should -Be '0,0'
        @($manifest.resolved_members | Where-Object { -not $_.fresh_before_fixture_init }).Count | Should -Be 0
    }

    It 'fails closed on extra characters, ownership collisions, or non-fresh state' {
        $rows = New-CleanQ786Rows
        $rows.characters += [pscustomobject]@{
            guid = '9999'; account = '501'; name = 'Unrelated'; race = '2'; class = '1'; gender = '0'; level = '1'; online = '0'; map = '1'
            x = '-618.518'; y = '-4251.67'; z = '38.718'; o = '0'; home_map = '1'; home_zone = '14'; home_x = '-618.518'; home_y = '-4251.67'; home_z = '38.718'
            inventory_count = '0'; questlog_count = '0'; rewarded_count = '0'
        }
        { Resolve-Q786FixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters } |
            Should -Throw '*out-of-scope character*'

        $rows = New-CleanQ786Rows
        $rows.characters[0].account = '502'
        { Resolve-Q786FixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters } |
            Should -Throw '*belongs to another account*'

        foreach ($property in @('level', 'inventory_count', 'questlog_count', 'rewarded_count')) {
            $rows = New-CleanQ786Rows
            $rows.characters[0].$property = if ($property -eq 'level') { '2' } else { '1' }
            { Resolve-Q786FixtureManifest -Definition $script:Definition -AccountRows $rows.accounts -CharacterRows $rows.characters } |
                Should -Throw
        }
    }

    It 'emits an exact GUID-bearing handoff manifest only from complete clean evidence' {
        $resolved = New-ResolvedQ786Manifest
        $leagueRows = @(
            [pscustomobject]@{ character_guid = '9001'; team_id = ''; affiliation = 'wayfarer'; role = 'adventurer'; class_plan = 'q786-owner'; active = '1'; retired_at = '' },
            [pscustomobject]@{ character_guid = '9002'; team_id = ''; affiliation = 'wayfarer'; role = 'adventurer'; class_plan = 'q786-helper'; active = '1'; retired_at = '' }
        )
        $config = [pscustomobject]@{
            config_path = 'playerbots.conf'; required_guids = @(9001, 9002); added_guids = @(9001, 9002)
            configured_guids = @(2, 8, 9001, 9002); config_mutated = $true; only_resolved_guids_added = $true
        }
        $accounts = [pscustomobject]@{
            account_names = @('AWQ786OWNER', 'AWQ786HELPER'); commands = @(); password_emitted_or_persisted = $false
            transient_console_used = $true; normal_server_stopped_or_restarted = $false
        }
        $manifest = New-Q786ReadyManifest -Definition $script:Definition -ResolvedManifest $resolved -FixtureGuidSetup $config `
            -LeagueRows $leagueRows -AccountBootstrap $accounts -OutputPath 'q786-fixture-manifest.json'
        $manifest.status | Should -Be 'READY_FOR_Q786_FIXTURE_INIT'
        $manifest.owner_guid | Should -Be 9001
        $manifest.helper_guid | Should -Be 9002
        ($manifest.ordered_guids -join ',') | Should -Be '9001,9002'
        @($manifest.members).Count | Should -Be 2
        $manifest.mutation_receipt.inventory_writes | Should -Be 0
        $manifest.mutation_receipt.quest_state_writes | Should -Be 0
        $manifest.mutation_receipt.fixture_init_calls | Should -Be 0
    }
}

Describe 'Scoped additive SQL and config generation' {
    It 'uses a named lock and inserts only character plus homebind rows at level 1' {
        $owner = @($script:Definition.ordered_members)[0]
        $sql = New-Q786CharacterInsertSql -CharactersDatabaseName 'acore_characters' -AccountId 501 -Member $owner
        $sql | Should -Match "GET_LOCK\('autowow\.q786\.fixture-provision\.v1', 30\)"
        $sql | Should -Match "name='Kolkarown' OR account=501"
        $sql | Should -Match "SELECT @q786_fixture_guid,501,'Kolkarown',2,1,0,"
        $sql | Should -Match '(?m)^\s*1,1,-618\.518,-4251\.67,38\.718,0,'
        ([regex]::Matches($sql, '(?i)INSERT\s+INTO')).Count | Should -Be 2
        $sql | Should -Match '\.characters\b'
        $sql | Should -Match '\.character_homebind\b'
        $sql | Should -Not -Match '(?i)\b(DELETE|UPDATE|REPLACE|DROP|TRUNCATE|ALTER)\b'
        $sql | Should -Not -Match '(?i)character_inventory|character_queststatus'
    }

    It 'enrolls only the resolved GUIDs with insert-only exact q786 league state' {
        $resolved = New-ResolvedQ786Manifest
        $sql = New-Q786LeagueEnrollmentSql -PlayerbotsDatabaseName 'acore_playerbots' -Members $resolved.resolved_members
        $sql | Should -Match "GET_LOCK\('autowow\.q786\.fixture-provision\.v1', 30\)"
        ([regex]::Matches($sql, '(?i)INSERT\s+INTO')).Count | Should -Be 2
        $sql | Should -Match "SELECT 9001,NULL,'wayfarer','adventurer','q786-owner'"
        $sql | Should -Match "SELECT 9002,NULL,'wayfarer','adventurer','q786-helper'"
        $sql | Should -Not -Match '(?i)\b(DELETE|UPDATE|REPLACE|DROP|TRUNCATE|ALTER)\b'
        $sql | Should -Not -Match '(?i)character_inventory|character_queststatus|autowow_league_team'
    }

    It 'adds only the two resolved GUIDs without removing, reordering, or changing other config' {
        $content = "Before = 1`r`nAutoWow.FixtureGuids = `"2,8,24`"`r`nAfter = keep`r`n"
        $update = Get-Q786FixtureGuidConfigUpdate -Content $content -RequiredGuids ([uint32[]]@(9001, 9002))
        $update.changed | Should -BeTrue
        ($update.before_guids -join ',') | Should -Be '2,8,24'
        ($update.added_guids -join ',') | Should -Be '9001,9002'
        ($update.after_guids -join ',') | Should -Be '2,8,24,9001,9002'
        $update.content | Should -Be "Before = 1`r`nAutoWow.FixtureGuids = `"2,8,24,9001,9002`"`r`nAfter = keep`r`n"
        { Get-Q786FixtureGuidConfigUpdate -Content $content -RequiredGuids ([uint32[]]@(9001, 9001)) } | Should -Throw '*exactly two distinct*'
    }

    It 'rejects mutation SQL aimed at inventory or quest state' {
        { Assert-Q786SqlSafety -Sql 'INSERT INTO db.character_inventory (guid) VALUES (1);' -Operation CharacterInsert } |
            Should -Throw '*inventory and quest-state writes*'
        { Assert-Q786SqlSafety -Sql 'INSERT INTO db.character_queststatus (guid) VALUES (1);' -Operation CharacterInsert } |
            Should -Throw '*inventory and quest-state writes*'
    }
}

Describe 'Secret and lifecycle fail-closed contract' {
    It 'accepts account creation secrets only from WOW_ACCOUNT_PASSWORD and never emits or persists dry-run secrets' {
        $header = ($script:Source -split 'Set-StrictMode', 2)[0]
        $header | Should -Not -Match '(?i)\$.*password'
        $script:Source | Should -Match '\$env:WOW_ACCOUNT_PASSWORD'
        $script:Source | Should -Match '\$password\.Length\s+-gt\s+16'
        $script:Source | Should -Match 'console output was not persisted to avoid secret disclosure'

        $old = $env:WOW_ACCOUNT_PASSWORD
        try {
            $env:WOW_ACCOUNT_PASSWORD = 'Q786NoLeak9'
            $json = & $script:ProvisionPath -ServerRoot (Join-Path $TestDrive 'missing') | Out-String
            $json | Should -Not -Match 'Q786NoLeak9'
            $json | Should -Match '<WOW_ACCOUNT_PASSWORD>'
            @(Get-ChildItem -LiteralPath $TestDrive -Force -Recurse -ErrorAction SilentlyContinue).Count | Should -Be 0
        }
        finally {
            if ($null -eq $old) { Remove-Item Env:WOW_ACCOUNT_PASSWORD -ErrorAction SilentlyContinue }
            else { $env:WOW_ACCOUNT_PASSWORD = $old }
        }
    }

    It 'requires explicit Apply at every database, account, and config mutation boundary' {
        $dummyContext = [pscustomobject]@{ mysql = 'missing.exe' }
        $dummyDatabase = [pscustomobject]@{ host = '127.0.0.1'; port = 3306; user = 'x'; password = 'x'; database = 'x' }
        { Invoke-Q786Sql -DatabaseContext $dummyContext -Database $dummyDatabase -Sql 'SELECT 1;' -Operation Read -ApplyConfirmed:$false } |
            Should -Throw '*requires explicit -Apply*'
        { Get-Q786FixtureRows -DatabaseContext $dummyContext -Definition $script:Definition -ApplyConfirmed:$false } |
            Should -Throw '*requires explicit -Apply*'
        { Get-Q786LeagueRows -DatabaseContext $dummyContext -Guids ([uint32[]]@(9001, 9002)) -ApplyConfirmed:$false } |
            Should -Throw '*requires explicit -Apply*'
        { Invoke-Q786AccountBootstrap -MissingMembers @($script:Definition.ordered_members[0]) -Root $TestDrive -ApplyConfirmed:$false } |
            Should -Throw '*requires explicit -Apply*'

        $config = Join-Path $TestDrive 'playerbots.conf'
        [IO.File]::WriteAllText($config, 'AutoWow.FixtureGuids = "2,8"')
        { Set-Q786FixtureGuidConfig -ConfigPath $config -RequiredGuids ([uint32[]]@(9001, 9002)) -Root $TestDrive -ApplyConfirmed:$false } |
            Should -Throw '*requires explicit -Apply*'
        [IO.File]::ReadAllText($config) | Should -Be 'AutoWow.FixtureGuids = "2,8"'
    }

    It 'fails closed when either the exact normal worldserver or bridge is reachable' {
        Mock Get-Q786WorldserverProcesses { @([pscustomobject]@{ Id = 77 }) }
        Mock Test-Q786ProvisionBridgeReady { $false }
        { Assert-Q786ProvisioningOffline -Root $TestDrive } | Should -Throw '*normal worldserver to be stopped*'

        Mock Get-Q786WorldserverProcesses { @() }
        Mock Test-Q786ProvisionBridgeReady { $true }
        { Assert-Q786ProvisioningOffline -Root $TestDrive } | Should -Throw '*bridge to be stopped*'
    }

    It 'contains no normal server lifecycle command and rechecks offline before each controlled setup' {
        $script:Source | Should -Not -Match '(?i)&\s*[^\r\n]*(start-world|stop-world|build\.ps1|bootstrap\.ps1)'
        $script:Source | Should -Not -Match '(?i)\b(Stop-Process|Restart-Service|Start-Service|Stop-Service)\b'
        ([regex]::Matches($script:Source, 'Assert-Q786ProvisioningOffline')).Count | Should -BeGreaterOrEqual 7
        $script:Source | Should -Match 'Direct character bootstrap is protected by a\s+named MySQL lock'
        $script:Source | Should -Match 'never starts, stops, or restarts the normal server'
    }
}

Describe 'q786 provisioning PowerShell syntax' {
    It 'parse-checks only the dedicated script and focused test' {
        foreach ($path in @($script:ProvisionPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
