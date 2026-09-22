Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:LibraryPath = Join-Path $script:ScriptsRoot 'oracle-gear-fixture-lib.ps1'
    $script:RunnerPath = Join-Path $script:ScriptsRoot 'oracle-gear-fixture.ps1'
    $script:ManifestPath = Join-Path $script:ScriptsRoot 'fixtures\oracle-gear-fixture.example.json'
    . $script:LibraryPath
    $script:Manifest = Read-OracleGearFixtureManifest -Path $script:ManifestPath

    function New-OracleStatusEvidence {
        param([Parameter(Mandatory)][psobject]$Member)
        [pscustomobject][ordered]@{
            ok = $true
            guid = [uint32]$Member.guid
            identity = [pscustomobject]@{ name = $Member.name; account_id = [uint32]$Member.account_id; faction = $Member.faction }
            class = [pscustomobject]@{ id = [int]$Member.class_id; name = $Member.class_name }
            level = 80
            role = [pscustomobject]@{ combat = if ($Member.role_category -eq 'dps') { 'dps' } else { [string]$Member.role_category } }
            spec = [pscustomobject]@{ stored_index = [int]$Member.spec_index; name = $Member.spec_name }
            gear = [pscustomobject]@{ equipped_slots = 16; other_quality_slots = 0; quality_counts = [pscustomobject]@{ rare = 16 } }
            talents = [pscustomobject]@{ allocated = 51; free_points = 0 }
            spells = [pscustomobject]@{ known = 120; role_critical_ready = $true }
            weapons = [pscustomobject]@{ main_hand = $true; off_hand = $false; ranged = $false; ready = $true }
            inventory = [pscustomobject]@{ free_slots = 12; loot_slot_reserve_met = $true }
        }
    }

    function New-OracleSnapshotEvidence {
        param([Parameter(Mandatory)][psobject]$Member)
        [pscustomobject]@{ name = $Member.name; progress = [pscustomobject]@{ level = 80 } }
    }
}

Describe 'Oracle adequate-gear profile contract' {
    It 'loads the exact 40-member manifest and all four deterministic sizes' {
        $script:Manifest.schema | Should -Be 'autowow.oracle.gear-fixture.manifest.v1'
        @($script:Manifest.roster).Count | Should -Be 40
        foreach ($size in @(5, 10, 25, 40)) {
            $members = @(Get-OracleGearProfileMembers -Manifest $script:Manifest -Size $size)
            $members.Count | Should -Be $size
            (@($members.ordinal) -join ',') | Should -Be ((1..$size) -join ',')
            @($members | Where-Object faction -eq 'Alliance').Count | Should -Be $size
        }
    }

    It 'has the intended role and damage coverage at every comparison size' {
        foreach ($size in @(5, 10, 25, 40)) {
            $members = @(Get-OracleGearProfileMembers -Manifest $script:Manifest -Size $size)
            $contract = (Get-OracleGearFixtureContracts)[[string]$size]
            @($members | Where-Object role_category -eq 'tank').Count | Should -BeGreaterOrEqual $contract.tank
            @($members | Where-Object role_category -eq 'healer').Count | Should -BeGreaterOrEqual $contract.healer
            @($members | Where-Object role_category -eq 'dps').Count | Should -BeGreaterOrEqual $contract.dps
            @($members | Where-Object damage_kind -eq 'melee').Count | Should -BeGreaterOrEqual $contract.melee
            @($members | Where-Object { $_.damage_kind -in @('physical-ranged', 'caster', 'ranged') }).Count | Should -BeGreaterOrEqual $contract.ranged
        }
    }

    It 'emits the existing exact fixture-init wire request' {
        $member = @($script:Manifest.roster)[0]
        Get-OracleFixtureInitRequest -Member $member -Manifest $script:Manifest | Should -Be 'fixture init 900001 80 1 3'
    }

    It 'adapts the existing role-fixture resolved-member shape without database access' {
        $source = [pscustomobject]@{
            ordinal = 1; character_guid = 1234; account_id = 5678; character_name = 'Roleaflag'; faction = 'Alliance'
            class_id = 11; role_category = 'tank'; spec_index = 1; spec_name = 'bear pve'
        }
        $converted = @(ConvertFrom-OracleRoleFixtureMembers -Members @($source))
        $converted.Count | Should -Be 1
        $converted[0].guid | Should -Be 1234
        $converted[0].account_id | Should -Be 5678
        $converted[0].class_name | Should -Be 'Druid'
        $converted[0].damage_kind | Should -Be 'melee'
    }
}

Describe 'Oracle exact identity and read-only evidence gates' {
    It 'passes a matching class, faction, name, account, spec, gear, talent, spell, and telemetry record' {
        $member = @($script:Manifest.roster)[0]
        $status = New-OracleStatusEvidence -Member $member
        $snapshot = New-OracleSnapshotEvidence -Member $member
        $combat = [pscustomobject]@{ counters = [pscustomobject]@{ available = $true } }
        (Test-OracleFixtureIdentity -Member $member -Status $status).status | Should -Be 'PASS'
        (Test-OracleFixtureEvidence -Member $member -Manifest $script:Manifest -Status $status -Snapshot $snapshot -Combat $combat).status | Should -Be 'PASS'
    }

    It 'fails closed on account ownership, class, and faction mismatches' {
        $member = @($script:Manifest.roster)[0]
        $status = New-OracleStatusEvidence -Member $member
        $status.identity.account_id = 999999
        $status.class.id = 1
        $status.identity.faction = 'Horde'
        $result = Test-OracleFixtureIdentity -Member $member -Status $status
        $result.status | Should -Be 'FAIL'
        $result.reasons | Should -Contain 'account_ownership_mismatch'
        $result.reasons | Should -Contain 'class_mismatch'
        $result.reasons | Should -Contain 'faction_mismatch'
    }

    It 'rejects insufficient gear, inventory, weapons, talents, role spells, or counters' {
        $member = @($script:Manifest.roster)[0]
        $status = New-OracleStatusEvidence -Member $member
        $status.gear.equipped_slots = 1
        $status.inventory.free_slots = 0
        $status.inventory.loot_slot_reserve_met = $false
        $status.talents.allocated = 0
        $status.spells.role_critical_ready = $false
        $status.weapons.ready = $false
        $combat = [pscustomobject]@{ counters = [pscustomobject]@{ available = $false } }
        $result = Test-OracleFixtureEvidence -Member $member -Manifest $script:Manifest -Status $status -Snapshot (New-OracleSnapshotEvidence -Member $member) -Combat $combat
        $result.status | Should -Be 'FAIL'
        foreach ($reason in @('insufficient_equipped_slots', 'loot_capacity_not_ready', 'weapon_not_ready', 'talents_not_allocated', 'role_critical_spells_not_ready', 'combat_counters_unavailable')) {
            $result.reasons | Should -Contain $reason
        }
    }
}

Describe 'Oracle ablation and mutation boundaries' {
    It 'exposes quality, slot, and talent ablation plans without enabling mutation' {
        $matrix = Get-OracleGearAblationMatrix
        Assert-OracleGearAblationMatrix -Matrix $matrix | Should -BeTrue
        $matrix.mutation_enabled | Should -BeFalse
        @($matrix.quality).Count | Should -BeGreaterThan 1
        @($matrix.slots).Count | Should -BeGreaterThan 1
        @($matrix.talents).Count | Should -BeGreaterThan 1
        $matrix.execution | Should -Match 'plan_only'
    }

    It 'requires the explicit factory mutation guard and contains no direct database or server lifecycle surface' {
        $source = Get-Content -LiteralPath $script:RunnerPath -Raw
        $source | Should -Match 'AllowFixtureFactoryMutation'
        $source | Should -Match 'fixture init'
        $source | Should -Not -Match '(?i)mysql|mysql\.exe|\b(DELETE|UPDATE|INSERT|DROP|TRUNCATE)\b|Start-Process|build\.ps1|configure-server\.ps1|worldserver\.exe|server\\configs'
    }
}

Describe 'Oracle PowerShell syntax' {
    It 'parse-checks the library, runner, and focused test' {
        foreach ($path in @($script:LibraryPath, $script:RunnerPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
