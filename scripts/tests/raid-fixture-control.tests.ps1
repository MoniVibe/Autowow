Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:ControlPath = Join-Path $script:ScriptsRoot 'autowow-control.ps1'
    $script:ModuleRoot = Join-Path (Split-Path -Parent $script:ScriptsRoot) '_phase1_worktree\mod-playerbots'
    $script:BridgePath = Join-Path $script:ModuleRoot 'src\AutoWow\AutoWowBridge.cpp'
    $script:RaidControlPath = Join-Path $script:ModuleRoot 'src\AutoWow\RaidFixtureControl.cpp'

    function Get-RaidMembers {
        param([ValidateSet(10, 25, 40)][int]$Size)
        return [uint32[]](2..$Size)
    }
}

Describe 'AutoWoW raid fixture request construction' {
    It 'builds canonical exact-size create requests for <Size> members at difficulty <Difficulty>' -ForEach @(
        @{ Size = 10; Difficulty = 0 },
        @{ Size = 10; Difficulty = 1 },
        @{ Size = 25; Difficulty = 0 },
        @{ Size = 25; Difficulty = 1 },
        @{ Size = 40; Difficulty = 0 },
        @{ Size = 40; Difficulty = 1 }
    ) {
        $members = Get-RaidMembers -Size $Size
        $request = & $script:ControlPath -Action raid-create -BotGuid 1 -MemberGuid $members `
            -RaidDifficulty $Difficulty -EmitRequestOnly
        $request | Should -BeExactly ("raid create 1 $($members -join ' ') difficulty=$Difficulty")
    }

    It 'builds leader status and scoped member-or-leader leave requests' {
        & $script:ControlPath -Action raid-status -BotGuid 1 -EmitRequestOnly |
            Should -BeExactly 'raid status 1'
        & $script:ControlPath -Action raid-leave -BotGuid 9 -EmitRequestOnly |
            Should -BeExactly 'raid leave 9'
        & $script:ControlPath -Action raid-leave -BotGuid 1 -EmitRequestOnly |
            Should -BeExactly 'raid leave 1'
    }

    It 'rejects wrong sizes, duplicate, zero, repeated leader, invalid difficulty, and extra status roster offline' {
        { & $script:ControlPath -Action raid-create -BotGuid 1 -MemberGuid ([uint32[]](2..9)) -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action raid-create -BotGuid 1 -MemberGuid ([uint32[]](@(2..9) + 9)) -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action raid-create -BotGuid 1 -MemberGuid ([uint32[]](@(0) + @(3..10))) -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action raid-create -BotGuid 1 -MemberGuid ([uint32[]](1..9)) -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action raid-create -BotGuid 1 -MemberGuid ([uint32[]](2..10)) -RaidDifficulty 2 -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action raid-status -BotGuid 1 -MemberGuid 2 -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action raid-leave -BotGuid 0 -EmitRequestOnly } | Should -Throw
    }

    It 'preserves the existing atomic WSG request construction' {
        $wsgRoster = [uint32[]](1..20)
        & $script:ControlPath -Action wsg-queue -MemberGuid $wsgRoster -EmitRequestOnly |
            Should -BeExactly ('wsg queue ' + ($wsgRoster -join ' '))
        & $script:ControlPath -Action wsg-status -MemberGuid $wsgRoster -EmitRequestOnly |
            Should -BeExactly ('wsg status ' + ($wsgRoster -join ' '))
        & $script:ControlPath -Action wsg-leave -MemberGuid $wsgRoster -EmitRequestOnly |
            Should -BeExactly ('wsg leave ' + ($wsgRoster -join ' '))
    }
}

Describe 'AutoWoW raid fixture bounded source contract' {
    It 'keeps the canonical raid work behind the world-thread queue' {
        $bridge = Get-Content -LiteralPath $script:BridgePath -Raw
        $bridge | Should -Match 'AutoWowRaid::ParseWireRequest'
        $bridge | Should -Match 'AutoWowRaid::Create'
        $bridge | Should -Match 'AutoWowRaid::Status'
        $bridge | Should -Match 'AutoWowRaid::Leave'
        $bridge | Should -Match 'PlayerbotWorldThreadProcessor::instance\(\)\.QueueOperation'
    }

    It 'uses exact owner-scoped ordinary group operations and has no raid-control teleport' {
        $source = Get-Content -LiteralPath $script:RaidControlPath -Raw
        $source | Should -Match 'GroupInviteOperation'
        $source | Should -Match 'GroupConvertToRaidOperation'
        $source | Should -Match 'SetRaidDifficulty'
        $source | Should -Match 'autowow_league_member'
        $source | Should -Match 'raid_foreign_group_member'
        $source | Should -Match 'fixture\.groupId'
        $source | Should -Match 'group->RemoveMember'
        $source | Should -Match 'group->Disband'
        $source | Should -Not -Match 'TeleportTo\('
        $source | Should -Not -Match 'new\s+Group'
    }

    It 'relocates only to raid exteriors and advances through data-driven normal portal protocols' {
        $bridge = Get-Content -LiteralPath $script:BridgePath -Raw
        $routeStart = $bridge.IndexOf('bool RouteParty()')
        $advanceStart = $bridge.IndexOf('bool AdvanceDungeonParty()', $routeStart)
        $advanceEnd = $bridge.IndexOf('bool EngageNearby()', $advanceStart)
        $routeStart | Should -BeGreaterOrEqual 0
        $advanceStart | Should -BeGreaterThan $routeStart
        $advanceEnd | Should -BeGreaterThan $advanceStart

        $routeSection = $bridge.Substring($routeStart, $advanceStart - $routeStart)
        $advanceSection = $bridge.Substring($advanceStart, $advanceEnd - $advanceStart)
        $routeSection | Should -Match '\{ "ony-exterior", 1, -4747\.17f, -3753\.27f, 49\.8122f, 0\.0f \}'
        $routeSection | Should -Match '\{ "voa-exterior", 571, 5494\.88f, 2839\.81f, 420\.811f, 0\.0f \}'
        $routeSection | Should -Not -Match '"ony-portal"|"ony-boss-approach"|"voa-portal"|"voa-central-hub"'
        $advanceSection | Should -Match '\{ "ony-portal", 1, -4760\.0f, -3752\.8f, 49\.43f, false \}'
        $advanceSection | Should -Match '\{ "ony-upper-warders", 249, -45\.0f, -96\.0f, -36\.0f, true \}'
        $advanceSection | Should -Match '\{ "ony-descending-tunnel", 249, -105\.0f, -116\.0f, -45\.0f, true \}'
        $advanceSection | Should -Match '\{ "ony-lower-turn", 249, -160\.0f, -160\.0f, -57\.0f, true \}'
        $advanceSection | Should -Match '\{ "ony-lower-warders", 249, -165\.0f, -200\.0f, -66\.0f, true \}'
        $advanceSection | Should -Match '\{ "ony-chamber-mouth", 249, -111\.0f, -214\.0f, -75\.0f, true \}'
        $advanceSection | Should -Match '\{ "ony-boss-approach", 249, -10\.0f, -180\.0f, -87\.0f, true \}'
        $advanceSection | Should -Match '\{ "voa-portal", 571, 5494\.88f, 2839\.81f, 416\.811f, false \}'
        $advanceSection | Should -Match '\{ "voa-entrance-staging", 624, -285\.0f, -103\.4f, 106\.1f, true \}'
        $advanceSection | Should -Match '\{ "voa-central-hub", 624, -219\.29f, -126\.94f, 102\.95f, true \}'
        $advanceSection | Should -Match '\{ "voa-archavon-west-staging", 624, -150\.0f, -103\.5f, 103\.3f, true \}'
        $advanceSection | Should -Match '\{ "voa-archavon-approach", 624, 93\.885f, -101\.531f, 91\.401f, true \}'
        $advanceSection | Should -Match '\{ "voa-emalon-north-staging", 624, -219\.0f, -145\.0f, 101\.0f, true \}'
        $advanceSection | Should -Match '\{ "voa-emalon-south-corridor", 624, -218\.85f, -196\.20f, 97\.59f, true \}'
        $advanceSection | Should -Match '\{ "voa-emalon-approach", 624, -221\.8f, -243\.8f, 96\.8f, true \}'
        $advanceSection | Should -Match '\{ "voa-koralon-south-staging", 624, -218\.9f, -82\.0f, 100\.0f, true \}'
        $advanceSection | Should -Match '\{ "voa-koralon-first-warder", 624, -218\.8f, -65\.0f, 97\.6f, true \}'
        $advanceSection | Should -Match '\{ "voa-koralon-second-warder", 624, -218\.9f, -3\.0f, 97\.7f, true \}'
        $advanceSection | Should -Match '\{ "voa-koralon-approach", 624, -218\.5f, 62\.0f, 96\.8f, true \}'
        $advanceSection | Should -Match '\{ "voa-toravon-east-staging", 624, -219\.0f, -175\.0f, 98\.5f, true \}'
        $advanceSection | Should -Match '\{ "voa-toravon-arch-corner", 624, -130\.0f, -175\.0f, 98\.0f, true \}'
        $advanceSection | Should -Match '\{ "voa-toravon-safe-staging", 624, -80\.0f, -175\.0f, 98\.0f, true \}'
        $advanceSection | Should -Match '\{ "voa-toravon-first-warder", 624, -43\.2f, -185\.0f, 97\.6f, true \}'
        $advanceSection | Should -Match '\{ "voa-toravon-second-warder", 624, -43\.2f, -201\.0f, 97\.6f, true \}'
        $advanceSection | Should -Match '\{ "voa-toravon-approach", 624, -43\.3f, -247\.0f, 96\.8f, true \}'
        $advanceSection | Should -Match 'AutoWowDungeonWalkAction\s+walk\(memberAI\)'
        $advanceSection | Should -Match '\{ "ony-portal", 1, 249, 2848, 16\.0f \}'
        $advanceSection | Should -Match '\{ "voa-portal", 571, 624, 5258, 8\.0f \}'
        $advanceSection | Should -Match 'CMSG_AREATRIGGER'
        $advanceSection | Should -Match 'HandleAreaTriggerOpcode'
        $advanceSection | Should -Match 'area_trigger_protocol'
        $advanceSection | Should -Match 'stage\\\":\\\"leader'
        $advanceSection | Should -Match 'stage\\\":\\\"follower'
        $advanceSection | Should -Match 'one follower'
        $advanceSection | Should -Match 'started_members'
        $advanceSection | Should -Not -Match 'AutoWowDungeonWalkAction\s+walk\(leaderAI\)'
        $advanceSection | Should -Not -Match 'TeleportTo\('
    }

    It 'emits a generic coordinate advance through the same fail-closed movement order' {
        $request = & $script:ControlPath -Action advance-point -BotGuid 72 `
            -CoordinateX -501.963 -CoordinateY -103.197 -CoordinateZ 155.947 -EmitRequestOnly

        $request | Should -Be 'advancepoint 72 -501.963 -103.197 155.947'

        $headed = & $script:ControlPath -Action advance-point -BotGuid 72 `
            -CoordinateX -501.963 -CoordinateY -103.197 -CoordinateZ 155.947 `
            -UseCoordinateOrientation -CoordinateOrientation 0.039 -EmitRequestOnly
        $headed | Should -Be 'advancepoint 72 -501.963 -103.197 155.947 0.039'
    }
}

Describe 'AutoWoW raid fixture PowerShell syntax' {
    It 'parse-checks the control and focused test scripts' {
        foreach ($path in @($script:ControlPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile(
                (Resolve-Path -LiteralPath $path).Path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
