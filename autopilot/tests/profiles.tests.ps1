# AutoWoW Autopilot V1.2 - PROFILES module tests.
# Run: Invoke-Pester -Path .\autopilot\tests\profiles.tests.ps1 -Output Detailed
# Fully offline: the shipped sample profiles are read read-only; every writable
# fixture lives under TestDrive. No sockets, no server, no live execution.

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotProfiles.ps1')

    $script:ProfilesDir = Join-Path $script:AutopilotRoot 'profiles'
    $script:ProfileSchemaPath = Join-Path $script:AutopilotRoot 'schemas\profile.schema.json'
    $script:AllSampleNames = @(
        'all-rounder', 'battleground', 'crafter', 'dungeon',
        'gatherer', 'idle-support', 'quester', 'raider'
    )

    function New-TestProfilesDir {
        param([string]$Name = ('profiles-' + [guid]::NewGuid().ToString('n').Substring(0, 8)))
        $dir = Join-Path $TestDrive $Name
        $null = New-Item -ItemType Directory -Force -Path $dir
        return $dir
    }

    function New-UtcTime {
        param([Parameter(Mandatory)][int]$Hour, [int]$Minute = 0)
        return [datetime]::new(2026, 7, 18, $Hour, $Minute, 0, [System.DateTimeKind]::Utc)
    }

    function New-WindowProfile {
        param([Parameter(Mandatory)][array]$Windows)
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.schedule = [ordered]@{ playWindowsUtc = @($Windows) }
        return $profile
    }
}

Describe 'Sample profiles' {
    It 'lists exactly the eight shipped sample profiles, sorted' {
        $names = Get-AutopilotProfileNames
        @($names) | Should -Be $script:AllSampleNames
    }

    It 'validates every sample profile semantically and against the JSON schema' {
        foreach ($name in $script:AllSampleNames) {
            $profile = Get-AutopilotProfile -Name $name
            ($profile -is [System.Collections.IDictionary]) | Should -BeTrue -Because "profile '$name' should load as an ordered hashtable"
            $profile.name | Should -Be $name
            $result = Test-AutopilotProfile -Profile $profile
            $result.Errors | Should -BeNullOrEmpty -Because "profile '$name' must have zero validation errors"
            $result.Valid | Should -BeTrue
            $result.SchemaEngine | Should -Not -Be 'failed' -Because "profile '$name' must not violate the JSON schema"
        }
    }

    It 'keeps the sample kind sets distinct per role' {
        @((Get-AutopilotProfile -Name 'gatherer').allowedJobKinds) | Should -Be @('Gather', 'Travel', 'BankDeposit')
        @((Get-AutopilotProfile -Name 'idle-support').allowedJobKinds) | Should -Be @('Idle', 'VendorRepair', 'MailTransfer')
        (Get-AutopilotProfile -Name 'battleground').pvpOptIn | Should -BeTrue
        (Get-AutopilotProfile -Name 'dungeon').groupRole | Should -Be 'flex'
        (Get-AutopilotProfile -Name 'idle-support').groupingAllowed | Should -BeFalse
        [int64](Get-AutopilotProfile -Name 'quester').priorityWeights['QuestLevel'] | Should -Be 10
        $materials = @((Get-AutopilotProfile -Name 'gatherer').preferredMaterials)
        @($materials | ForEach-Object { [int64]$_.itemId }) | Should -Contain 2770
    }
}

Describe 'Fail-closed validation' {
    It 'rejects an unknown schema_version (fail closed)' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.schema_version = 2
        $result = Test-AutopilotProfile -Profile $profile
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'schema_version'
        ($result.Errors -join ';') | Should -Match 'fail closed'
    }

    It 'rejects an unknown schema id (fail closed)' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.schema = 'autowow.autopilot.profile.v9'
        $result = Test-AutopilotProfile -Profile $profile
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'fail closed'
    }

    It 'rejects an unknown job kind in allowedJobKinds' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.allowedJobKinds = @('QuestLevel', 'MindControl')
        $result = Test-AutopilotProfile -Profile $profile
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'MindControl'
    }

    It 'rejects out-of-range priority weights in both directions and accepts the boundary values' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.priorityWeights = [ordered]@{ QuestLevel = 21 }
        (Test-AutopilotProfile -Profile $profile).Valid | Should -BeFalse
        $profile.priorityWeights = [ordered]@{ QuestLevel = -21 }
        (Test-AutopilotProfile -Profile $profile).Valid | Should -BeFalse
        $profile.priorityWeights = [ordered]@{ QuestLevel = 20; Travel = -20 }
        (Test-AutopilotProfile -Profile $profile).Valid | Should -BeTrue
    }

    It 'rejects a priority weight keyed by an unknown job kind' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.priorityWeights = [ordered]@{ MindControl = 5 }
        $result = Test-AutopilotProfile -Profile $profile
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'MindControl'
    }

    It 'rejects a non-integer priority weight instead of coercing it' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.priorityWeights = [ordered]@{ QuestLevel = 'ten' }
        (Test-AutopilotProfile -Profile $profile).Valid | Should -BeFalse
    }

    It 'rejects a malformed economy shape' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.economy = [ordered]@{ allowSpending = 'yes' }
        $result = Test-AutopilotProfile -Profile $profile
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'economy'
    }

    It 'rejects a negative maxSpendCopper and an out-of-range repairThresholdPct' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.economy.maxSpendCopper = -1
        (Test-AutopilotProfile -Profile $profile).Valid | Should -BeFalse
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.repairThresholdPct = 101
        (Test-AutopilotProfile -Profile $profile).Valid | Should -BeFalse
    }

    It 'rejects a malformed schedule window' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.schedule = [ordered]@{ playWindowsUtc = @([ordered]@{ start = '25:00'; end = '06:00' }) }
        $result = Test-AutopilotProfile -Profile $profile
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'HH:mm'
    }

    It 'fails closed on malformed profile JSON on disk' {
        $dir = New-TestProfilesDir
        Set-Content -LiteralPath (Join-Path $dir 'broken.json') -Value '{ this is not json' -NoNewline
        { Get-AutopilotProfile -Name 'broken' -ProfilesDir $dir } | Should -Throw '*fail closed*'
    }

    It 'rejects a profile whose document name does not match its file name' {
        $dir = New-TestProfilesDir
        $doc = Get-AutopilotProfile -Name 'quester'
        ($doc | ConvertTo-Json -Depth 20) | Set-Content -LiteralPath (Join-Path $dir 'alias.json')
        { Get-AutopilotProfile -Name 'alias' -ProfilesDir $dir } | Should -Throw '*does not match*'
    }

    It 'throws for an unknown profile name and lists the available names' {
        { Get-AutopilotProfile -Name 'does-not-exist' } | Should -Throw '*quester*'
        $message = $null
        try { $null = Get-AutopilotProfile -Name 'does-not-exist' } catch { $message = $_.Exception.Message }
        $message | Should -Match 'gatherer'
        $message | Should -Match 'raider'
        $message | Should -Match 'idle-support'
    }
}

Describe 'Job kind gating (strategic preference, never execution authority)' {
    It 'allows a listed kind and names the profile and the rule in the reason' {
        $profile = Get-AutopilotProfile -Name 'gatherer'
        $result = Test-AutopilotProfileJobKindAllowed -Profile $profile -JobKind 'Gather'
        $result.allowed | Should -BeTrue
        $result.reason | Should -Match 'gatherer'
        $result.reason | Should -Match 'allowedJobKinds'
        $result.reason | Should -Match 'never execution authority'
    }

    It 'refuses an unlisted kind with a reason naming the profile and the rule' {
        $profile = Get-AutopilotProfile -Name 'gatherer'
        $result = Test-AutopilotProfileJobKindAllowed -Profile $profile -JobKind 'Raid'
        $result.allowed | Should -BeFalse
        $result.reason | Should -Match 'gatherer'
        $result.reason | Should -Match 'allowedJobKinds'
    }

    It 'refuses PvP kinds when pvpOptIn is false even if the kind is listed' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.allowedJobKinds = @($profile.allowedJobKinds) + @('Battleground')
        $result = Test-AutopilotProfileJobKindAllowed -Profile $profile -JobKind 'Battleground'
        $result.allowed | Should -BeFalse
        $result.reason | Should -Match 'quester'
        $result.reason | Should -Match 'pvpOptIn'
    }

    It 'refuses grouping kinds when groupingAllowed is false even if the kind is listed' {
        $profile = Get-AutopilotProfile -Name 'idle-support'
        $profile.allowedJobKinds = @($profile.allowedJobKinds) + @('FormParty')
        $result = Test-AutopilotProfileJobKindAllowed -Profile $profile -JobKind 'FormParty'
        $result.allowed | Should -BeFalse
        $result.reason | Should -Match 'idle-support'
        $result.reason | Should -Match 'groupingAllowed'
    }

    It 'refuses an unknown job kind outright' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $result = Test-AutopilotProfileJobKindAllowed -Profile $profile -JobKind 'MindControl'
        $result.allowed | Should -BeFalse
        $result.reason | Should -Match 'unknown job kind'
    }
}

Describe 'Priority weighting' {
    It 'adds the profile weight to the base priority' {
        $profile = Get-AutopilotProfile -Name 'quester'
        Get-AutopilotProfileJobPriority -Profile $profile -JobKind 'QuestLevel' -BasePriority 50 | Should -Be 60
    }

    It 'treats a missing weight as zero' {
        $profile = Get-AutopilotProfile -Name 'quester'
        Get-AutopilotProfileJobPriority -Profile $profile -JobKind 'Idle' -BasePriority 37 | Should -Be 37
    }

    It 'clamps to 100 at the top end' {
        $profile = Get-AutopilotProfile -Name 'quester'
        Get-AutopilotProfileJobPriority -Profile $profile -JobKind 'QuestLevel' -BasePriority 95 | Should -Be 100
        Get-AutopilotProfileJobPriority -Profile $profile -JobKind 'QuestLevel' -BasePriority 100 | Should -Be 100
    }

    It 'clamps to 0 at the bottom end' {
        $profile = Get-AutopilotProfile -Name 'quester'
        $profile.priorityWeights = [ordered]@{ Idle = -20 }
        Get-AutopilotProfileJobPriority -Profile $profile -JobKind 'Idle' -BasePriority 5 | Should -Be 0
        Get-AutopilotProfileJobPriority -Profile $profile -JobKind 'Idle' -BasePriority 0 | Should -Be 0
    }
}

Describe 'Schedule windows (UTC)' {
    It 'is always active when playWindowsUtc is empty' {
        $profile = Get-AutopilotProfile -Name 'quester'
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 3) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 15 -Minute 30) | Should -BeTrue
    }

    It 'honors a plain daytime window with an inclusive start and exclusive end' {
        $profile = New-WindowProfile -Windows @([ordered]@{ start = '08:00'; end = '17:00' })
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 8) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 12 -Minute 30) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 7 -Minute 59) | Should -BeFalse
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 17) | Should -BeFalse
    }

    It 'wraps midnight for a 22:00-06:00 window' {
        $profile = New-WindowProfile -Windows @([ordered]@{ start = '22:00'; end = '06:00' })
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 22) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 23 -Minute 30) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 3) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 5 -Minute 59) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 6) | Should -BeFalse
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 12) | Should -BeFalse
    }

    It 'matches the raider sample raid-night window' {
        $profile = Get-AutopilotProfile -Name 'raider'
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 19) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 9) | Should -BeFalse
    }

    It 'matches the battleground sample midnight-wrap window' {
        $profile = Get-AutopilotProfile -Name 'battleground'
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 1) | Should -BeTrue
        Test-AutopilotProfileScheduleActive -Profile $profile -Now (New-UtcTime -Hour 12) | Should -BeFalse
    }
}
