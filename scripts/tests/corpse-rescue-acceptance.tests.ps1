<#
    Synthetic, offline coverage for corpse-rescue-acceptance.ps1.
#>

BeforeAll {
    $script:Tool = Join-Path $PSScriptRoot '..\corpse-rescue-acceptance.ps1'

    function Invoke-CorpseRescueFixture {
        param(
            [Parameter(Mandatory)][string[]]$Lines,
            [hashtable]$Extra = @{}
        )

        $path = Join-Path $TestDrive 'Playerbots.log'
        $Lines | Set-Content -LiteralPath $path -Encoding utf8
        $arguments = @{ PlayerbotsLogPath = $path }
        foreach ($entry in $Extra.GetEnumerator()) { $arguments[$entry.Key] = $entry.Value }
        return ((& $script:Tool @arguments | Out-String) | ConvertFrom-Json)
    }

    function New-StartedLine {
        param(
            [string]$Target = 'Fallenbot',
            [ValidateSet('unit','corpse')][string]$Mode = 'unit',
            [bool]$CasterCombat = $false,
            [Nullable[bool]]$GroupCombat,
            [bool]$CombatRes = $false,
            [uint64]$Corpse = 0,
            [string]$Healer = 'Healerbot',
            [string]$Spell = 'resurrection'
        )

        $corpseField = if ($Mode -eq 'corpse' -and $Corpse -ne 0) { " corpse=$Corpse" } else { '' }
        $caster = "$CasterCombat".ToLowerInvariant()
        $combat = "$CombatRes".ToLowerInvariant()
        $groupField = if ($PSBoundParameters.ContainsKey('GroupCombat')) {
            " group_combat=$("$([bool]$GroupCombat)".ToLowerInvariant())"
        } else { '' }
        return "[CorpseRescue] cast_started healer=$Healer target=$Target spell=$Spell mode=$Mode$corpseField caster_combat=$caster$groupField combat_res=$combat"
    }

    function New-AcceptLine {
        param(
            [string]$Target = 'Fallenbot',
            [bool]$Released = $false,
            [bool]$AliveAfter = $true,
            [string]$Source = '4242'
        )

        $releasedText = "$Released".ToLowerInvariant()
        $aliveText = "$AliveAfter".ToLowerInvariant()
        return "[CorpseRescue] accept target=$Target source=$Source released=$releasedText alive_after=$aliveText"
    }

    function New-CompletedLine {
        param(
            [string]$Target = 'Fallenbot',
            [bool]$Alive = $true,
            [uint32]$Map = 0,
            [uint32]$Instance = 0
        )

        $aliveText = "$Alive".ToLowerInvariant()
        return "[CorpseRescue] completed target=$Target alive=$aliveText map=$Map instance=$Instance"
    }
}

Describe 'corpse-rescue-acceptance status classification' {
    It 'returns COMBAT_RES_PASS from a completed combat resurrection even when accept is not alive yet' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -CasterCombat $true -CombatRes $true -Spell rebirth)
            (New-AcceptLine -AliveAfter $false)
            (New-CompletedLine -Map 571 -Instance 88)
        )

        $result.status | Should -BeExactly 'COMBAT_RES_PASS'
        $result.read_only | Should -BeTrue
        @($result.matched_evidence.line_number) | Should -Be @(1, 2, 3)
        $result.matched_evidence[0].fields.combat_res | Should -BeTrue
        $result.matched_evidence[1].fields.alive_after | Should -BeFalse
        $result.matched_evidence[2].fields.alive | Should -BeTrue
        $result.matched_evidence[2].fields.map | Should -Be 571
        $result.matched_evidence[2].fields.instance | Should -Be 88
    }

    It 'returns POSTCOMBAT_RES_PASS for a normal out-of-combat completed resurrection' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine)
            (New-AcceptLine)
            (New-CompletedLine)
        )

        $result.status | Should -BeExactly 'POSTCOMBAT_RES_PASS'
        @($result.matched_evidence.line_number) | Should -Be @(1, 2, 3)
    }

    It 'returns RELEASED_CORPSE_PASS only with corpse mode, released accept, and alive completion' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -Mode corpse -Corpse 77)
            (New-AcceptLine -Released $true -AliveAfter $false)
            (New-CompletedLine -Map 0 -Instance 0)
        )

        $result.status | Should -BeExactly 'RELEASED_CORPSE_PASS'
        $result.matched_evidence[0].fields.mode | Should -BeExactly 'corpse'
        $result.matched_evidence[0].fields.corpse | Should -Be 77
        $result.matched_evidence[1].fields.released | Should -BeTrue
        $result.matched_evidence[2].fields.alive | Should -BeTrue
    }

    It 'returns POLICY_VIOLATION only for a normal cast_started while the caster is in combat' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -Target Safe)
            (New-AcceptLine -Target Safe)
            (New-CompletedLine -Target Safe)
            (New-StartedLine -Target Violation -CasterCombat $true)
            '[CorpseRescue] completed target=Broken alive=true map=nope instance=0'
        )

        $result.status | Should -BeExactly 'POLICY_VIOLATION'
        $result.reasons | Should -Contain 'normal_resurrection_started_in_combat'
        @($result.matched_evidence.line_number) | Should -Be @(4)
        $result.matched_evidence[0].fields.target | Should -BeExactly 'Violation'
        $result.malformed_event_count | Should -Be 1
    }

    It 'treats group_combat as an optional diagnostic that cannot gate classification' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -Target Diagnostic -GroupCombat $true)
            (New-AcceptLine -Target Diagnostic)
            (New-CompletedLine -Target Diagnostic)
        )

        $result.status | Should -BeExactly 'POSTCOMBAT_RES_PASS'
        $result.matched_evidence[0].fields.caster_combat | Should -BeFalse
        $result.matched_evidence[0].fields.group_combat | Should -BeTrue
    }

    It 'returns NO_SAMPLE when the interval has no acceptance-relevant telemetry' {
        $result = Invoke-CorpseRescueFixture @(
            'ordinary Playerbots log line'
            '[CorpseRescue] move healer=A target=B mode=unit reason=los method=los started=true'
            '[CorpseRescue] cast_blocked healer=A target=B reason=los'
        )

        $result.status | Should -BeExactly 'NO_SAMPLE'
        $result.telemetry_event_count | Should -Be 0
        @($result.matched_evidence).Count | Should -Be 0
    }

    It 'returns INCOMPLETE for a combat block without a qualifying completed chain' {
        $result = Invoke-CorpseRescueFixture @(
            '[CorpseRescue] cast_blocked healer=Healerbot target=Fallenbot reason=combat caster_combat=true combat_res=false'
        )

        $result.status | Should -BeExactly 'INCOMPLETE'
        $result.reasons | Should -Contain 'combat_cast_blocked_without_qualifying_completion'
        @($result.matched_evidence.line_number) | Should -Be @(1)
    }

    It 'retains accept alive_after=false as diagnostic and remains incomplete without completion' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine)
            (New-AcceptLine -AliveAfter $false)
        )

        $result.status | Should -BeExactly 'INCOMPLETE'
        $result.reasons | Should -Contain 'accept_without_matching_completion'
        @($result.matched_evidence.line_number) | Should -Be @(1, 2)
        $result.matched_evidence[1].fields.alive_after | Should -BeFalse
    }

    It 'returns INCOMPLETE when the matching post-teleport completion says alive=false' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine)
            (New-AcceptLine -AliveAfter $true)
            (New-CompletedLine -Alive $false)
        )

        $result.status | Should -BeExactly 'INCOMPLETE'
        $result.reasons | Should -Contain 'matching_completion_not_alive'
        @($result.matched_evidence.line_number) | Should -Be @(1, 2, 3)
    }

    It 'does not classify corpse mode when the matching accept was not released' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -Mode corpse -Corpse 77)
            (New-AcceptLine -Released $false)
            (New-CompletedLine)
        )

        $result.status | Should -BeExactly 'INCOMPLETE'
        $result.reasons | Should -Contain 'corpse_mode_accept_not_released'
    }
}

Describe 'corpse-rescue-acceptance chronological matching and intervals' {
    It 'matches duplicate targets FIFO through completion and keeps evidence chronological' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -Target Duplicate)
            (New-StartedLine -Target Duplicate -CasterCombat $true -GroupCombat $false -CombatRes $true -Spell rebirth)
            (New-AcceptLine -Target Duplicate -Source 100 -AliveAfter $false)
            (New-AcceptLine -Target Duplicate -Source 200 -AliveAfter $false)
            (New-CompletedLine -Target Duplicate -Map 1 -Instance 10)
            (New-CompletedLine -Target Duplicate -Map 2 -Instance 20)
        )

        $result.status | Should -BeExactly 'POSTCOMBAT_RES_PASS'
        $result.matched_pair_count | Should -Be 2
        $result.matched_chain_count | Should -Be 2
        $result.qualifying_match_count | Should -Be 2
        @($result.matched_evidence.line_number) | Should -Be @(1, 3, 5)
        $result.matched_evidence[2].fields.instance | Should -Be 10
        @($result.evidence.line_number) | Should -Be @(1, 2, 3, 4, 5, 6)
    }

    It 'does not reuse one completion for duplicate accepted targets' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -Target Duplicate)
            (New-StartedLine -Target Duplicate)
            (New-AcceptLine -Target Duplicate -Source 100)
            (New-AcceptLine -Target Duplicate -Source 200)
            (New-CompletedLine -Target Duplicate)
        )

        $result.status | Should -BeExactly 'POSTCOMBAT_RES_PASS'
        $result.matched_pair_count | Should -Be 2
        $result.matched_chain_count | Should -Be 1
        $result.qualifying_match_count | Should -Be 1
        @($result.matched_evidence.line_number) | Should -Be @(1, 3, 5)
    }

    It 'does not match an accept that occurs before its cast_started line' {
        $result = Invoke-CorpseRescueFixture @(
            (New-AcceptLine -Target Ordered)
            (New-StartedLine -Target Ordered)
            (New-CompletedLine -Target Ordered)
        )

        $result.status | Should -BeExactly 'INCOMPLETE'
        $result.matched_pair_count | Should -Be 0
        $result.matched_chain_count | Should -Be 0
        $result.reasons | Should -Contain 'cast_started_without_matching_accept'
        $result.reasons | Should -Contain 'accept_without_matching_cast_started'
        $result.reasons | Should -Contain 'completion_without_matching_accept'
    }

    It 'does not match a completion that occurs before its accept' {
        $result = Invoke-CorpseRescueFixture @(
            (New-StartedLine -Target Ordered)
            (New-CompletedLine -Target Ordered)
            (New-AcceptLine -Target Ordered)
        )

        $result.status | Should -BeExactly 'INCOMPLETE'
        $result.matched_pair_count | Should -Be 1
        $result.matched_chain_count | Should -Be 0
        $result.reasons | Should -Contain 'accept_without_matching_completion'
        $result.reasons | Should -Contain 'completion_without_matching_accept'
    }

    It 'applies StartLine and WindowLines while retaining absolute source line numbers' {
        $result = Invoke-CorpseRescueFixture -Lines @(
            '[CorpseRescue] cast_started healer=Old target=Old spell=resurrection mode=unit caster_combat=bad combat_res=false'
            'window prefix noise'
            (New-StartedLine -Target Windowed -CasterCombat $true -GroupCombat $false -CombatRes $true -Spell rebirth)
            (New-AcceptLine -Target Windowed -AliveAfter $false)
            (New-CompletedLine -Target Windowed -Map 530 -Instance 4)
            (New-StartedLine -Target Outside -CasterCombat $true)
        ) -Extra @{ StartLine = 2; WindowLines = 4 }

        $result.status | Should -BeExactly 'COMBAT_RES_PASS'
        $result.interval.start_line | Should -Be 2
        $result.interval.window_lines | Should -Be 4
        $result.interval.end_line | Should -Be 5
        @($result.matched_evidence.line_number) | Should -Be @(3, 4, 5)
        @($result.evidence.line_number) | Should -Be @(3, 4, 5)
    }
}

Describe 'corpse-rescue-acceptance malformed telemetry and safety' {
    It 'fails closed to INCOMPLETE even when malformed fields are followed by a valid pass' -TestCases @(
        @{ BadLine = '[CorpseRescue] cast_started healer=A target=B spell=resurrection mode=unit caster_combat=yes combat_res=false' }
        @{ BadLine = '[CorpseRescue] cast_started healer=A target=B spell=resurrection mode=vehicle caster_combat=false combat_res=false' }
        @{ BadLine = '[CorpseRescue] cast_started healer=A target=B spell=resurrection mode=unit caster_combat=false' }
        @{ BadLine = '[CorpseRescue] cast_started healer=A target=B spell=resurrection mode=unit caster_combat=false group_combat=yes combat_res=false' }
        @{ BadLine = '[CorpseRescue] accept target=B source=42 released=false' }
        @{ BadLine = '[CorpseRescue] completed target=B alive=yes map=0 instance=0' }
        @{ BadLine = '[CorpseRescue] completed target=B alive=true map=-1 instance=0' }
        @{ BadLine = '[CorpseRescue] completed target=B alive=true map=4294967296 instance=0' }
        @{ BadLine = '[CorpseRescue] completed target=B alive=true map=0' }
    ) {
        param($BadLine)

        $result = Invoke-CorpseRescueFixture @(
            $BadLine
            (New-StartedLine -Target Good)
            (New-AcceptLine -Target Good)
            (New-CompletedLine -Target Good)
        )

        $result.status | Should -BeExactly 'INCOMPLETE'
        $result.reasons | Should -Contain 'malformed_telemetry'
        $result.malformed_event_count | Should -Be 1
        @($result.matched_evidence.line_number) | Should -Be @(1)
        $result.matched_evidence[0].valid | Should -BeFalse
    }

    It 'throws for a missing log and a start line beyond a non-empty log' {
        { & $script:Tool -PlayerbotsLogPath (Join-Path $TestDrive 'missing.log') } |
            Should -Throw '*Playerbots log not found*'

        $path = Join-Path $TestDrive 'short.log'
        'one line' | Set-Content -LiteralPath $path -Encoding utf8
        { & $script:Tool -PlayerbotsLogPath $path -StartLine 2 } |
            Should -Throw '*beyond the 1-line log*'
    }

    It 'contains no write, database, configuration, or control commands' {
        $tokens = $null
        $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseFile(
            $script:Tool, [ref]$tokens, [ref]$errors)
        @($errors).Count | Should -Be 0

        $commandNames = @($ast.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.CommandAst]
        }, $true) | ForEach-Object { $_.GetCommandName() })
        foreach ($forbidden in @(
            'Set-Content','Add-Content','Out-File','Export-Clixml','Export-Csv',
            'Invoke-Sqlcmd','Invoke-RestMethod','Invoke-WebRequest','Start-Process'
        )) {
            $commandNames | Should -Not -Contain $forbidden
        }

        $source = Get-Content -LiteralPath $script:Tool -Raw
        $source | Should -Not -Match '(?i)\b(mysql|autowow-control|configure-server)\b'
    }

    It 'parse-checks the checker and focused test script' {
        foreach ($path in @($script:Tool, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile(
                $path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
