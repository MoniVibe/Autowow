Set-StrictMode -Version Latest

BeforeAll {
    $script:ObserverPath = Join-Path $PSScriptRoot '..\league-progression-observer.ps1'
    $script:ObserverSource = Get-Content -LiteralPath $script:ObserverPath -Raw
    . $script:ObserverPath -LibraryOnly

    $script:CampaignGuids = [uint32[]](7,12,13,18,20,10,11,14,15,19,3,6,49)
    $script:ProbeGuids = [uint32[]](@(2,8,24,26,41,45,60,68,72,83) + @(17392..17408))

    function New-ProgressionBot {
        param(
            [uint32]$Guid,
            [int]$Level = 10,
            [uint32]$Xp = 100,
            [uint32]$MoneyCopper = 1000,
            [bool]$Alive = $true,
            [bool]$Combat = $false
        )

        [pscustomobject][ordered]@{
            guid = $Guid
            name = "Bot$Guid"
            team = 'ember'
            guild = 'campaign-looking-fixture'
            progress = [pscustomobject][ordered]@{
                level = $Level
                xp = $Xp
                money_copper = $MoneyCopper
            }
            alive = $Alive
            combat = $Combat
        }
    }
}

Describe 'league progression observer campaign roster isolation' {
    It 'parses and exposes the exact deterministic 13-bot campaign allowlist' {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($script:ObserverPath, [ref]$tokens, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0

        $roster = @(Get-LeagueProgressionCampaignRoster)
        @($roster.guid) | Should -Be $script:CampaignGuids
        @($roster.team) | Should -Be @(
            'ember','ember','ember','ember','ember',
            'northstar','northstar','northstar','northstar','northstar',
            'wayfarers','wayfarers','wayfarers'
        )
    }

    It 'excludes every named probe GUID despite campaign-like metadata' {
        $allBots = @(
            @($script:ProbeGuids | ForEach-Object { New-ProgressionBot -Guid $_ })
            @($script:CampaignGuids | Sort-Object -Descending | ForEach-Object { New-ProgressionBot -Guid $_ })
        )

        $selection = Select-LeagueProgressionCampaignBots -BridgeBots $allBots

        @($selection.bots.guid) | Should -Be $script:CampaignGuids
        @($selection.bots | Where-Object { $_.guid -in $script:ProbeGuids }).Count | Should -Be 0
        $selection.observed_count | Should -Be ($script:CampaignGuids.Count + $script:ProbeGuids.Count)
        $selection.included_count | Should -Be 13
        $selection.excluded_count | Should -Be $script:ProbeGuids.Count
    }

    It 'keeps team totals and deltas stable when probe telemetry changes wildly' {
        $baselineBots = @(
            @($script:CampaignGuids | ForEach-Object { New-ProgressionBot -Guid $_ })
            @($script:ProbeGuids | ForEach-Object { New-ProgressionBot -Guid $_ -Level 1 -Xp 0 -MoneyCopper 0 })
        )
        $currentBots = @(
            @($script:ProbeGuids | ForEach-Object { New-ProgressionBot -Guid $_ -Level 80 -Xp 999999 -MoneyCopper 999999 })
            @($script:CampaignGuids | ForEach-Object {
                $leveled = $_ -in [uint32[]](7,10,3)
                New-ProgressionBot -Guid $_ -Level $(if ($leveled) { 11 } else { 10 }) `
                    -Xp $(if ($leveled) { 5 } else { 110 }) -MoneyCopper 1100 `
                    -Alive $($_ -notin [uint32[]](19,49)) -Combat $($_ -in [uint32[]](7,3))
            })
        )

        $baselineSelection = Select-LeagueProgressionCampaignBots -BridgeBots $baselineBots
        $baseline = @{}
        foreach ($bot in @($baselineSelection.bots)) { $baseline[[uint32]$bot.guid] = $bot }
        $currentSelection = Select-LeagueProgressionCampaignBots -BridgeBots $currentBots
        $deltas = @(Get-LeagueProgressionDeltas -Baseline $baseline -Bots @($currentSelection.bots))
        $teams = Get-LeagueProgressionTeamTotals -Deltas $deltas

        @($deltas.guid) | Should -Be $script:CampaignGuids
        $teams.ember.online | Should -Be 5
        $teams.ember.level_gains | Should -Be 1
        $teams.ember.xp_delta_same_level | Should -Be 40
        $teams.ember.money_delta_copper | Should -Be 500
        $teams.ember.in_combat | Should -Be 1
        $teams.ember.dead | Should -Be 0
        $teams.northstar.online | Should -Be 5
        $teams.northstar.level_gains | Should -Be 1
        $teams.northstar.xp_delta_same_level | Should -Be 40
        $teams.northstar.money_delta_copper | Should -Be 500
        $teams.northstar.in_combat | Should -Be 0
        $teams.northstar.dead | Should -Be 1
        $teams.wayfarers.online | Should -Be 3
        $teams.wayfarers.level_gains | Should -Be 1
        $teams.wayfarers.xp_delta_same_level | Should -Be 20
        $teams.wayfarers.money_delta_copper | Should -Be 300
        $teams.wayfarers.in_combat | Should -Be 1
        $teams.wayfarers.dead | Should -Be 1
    }

    It 'keeps the live path read-only and adds counts without removing receipt payloads' {
        $script:ObserverSource | Should -Match '& \$control -Action list'
        $script:ObserverSource | Should -Not -Match '& \$control -Action (activate|deactivate|party|rally|deploy|route|engage|quest|pause|resume|travel|recover)'
        $script:ObserverSource | Should -Not -Match '(?im)^\s*(INSERT|UPDATE|DELETE|REPLACE)\s'
        $script:ObserverSource | Should -Match '-Event ''baseline''[\s\S]*bots = \$sample'
        $script:ObserverSource | Should -Match '-Event ''progress_delta''[\s\S]*teams = \$teams[\s\S]*bots = \$deltas'
        $script:ObserverSource | Should -Match 'observed_bot_count = \$observation\.observed_count'
        $script:ObserverSource | Should -Match 'included_bot_count = \$observation\.included_count'
        $script:ObserverSource | Should -Match 'excluded_bot_count = \$observation\.excluded_count'
    }
}
