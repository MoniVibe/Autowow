Set-StrictMode -Version Latest

BeforeAll {
    $script:DirectorPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'dungeon-director.ps1'
}

Describe 'Dungeon Director route and recovery contract' {
    It 'dry-runs the grounded upper-floor route without mutation' {
        $plan = (& $script:DirectorPath -StartAt uk-gauntlet-portcullis) | ConvertFrom-Json
        $plan.apply | Should -BeFalse
        $plan.waypoints[0] | Should -Be 'uk-gauntlet-portcullis'
        $plan.waypoints[-1] | Should -Be 'uk-ingvar-platform'
        @($plan.waypoints).Count | Should -Be 9
        $plan.movement | Should -Match 'normal mmap pathfinding'
        $plan.movement | Should -Match 'no dungeon teleports'
    }

    It 'retries transient party visibility and always releases a held leader' {
        $source = Get-Content -LiteralPath $script:DirectorPath -Raw
        $source | Should -Match '\$attempt\s*=\s*1;\s*\$attempt\s*-le\s*5'
        $source | Should -Match 'All five dungeon bots must be online after transition retries'
        $source | Should -Match 'finally\s*\{[\s\S]*Invoke-ControlJson\s+-Action\s+resume'
        $source | Should -Match '\[ValidateRange\(5000,120000\)\]\[int\]\$ControlTimeoutMs\s*=\s*30000'
    }

    It 'backtracks through normal pathfinding when a persistent cohesion hold strands followers' {
        $source = Get-Content -LiteralPath $script:DirectorPath -Raw
        $source | Should -Match '\$cohesionHoldPolls\s*-ge\s*10'
        $source | Should -Match '\$previousName\s*=\s*\$allRouteNames\[\$routeIndex\s*-\s*1\]'
        $source | Should -Match 'Invoke-AdvanceWaypoint\s+-Name\s+\$previousName\s+-Target\s+\$previousTarget'
        $source | Should -Match 'return\s+Invoke-ControlJson\s+-Action\s+advance\s+-Guid\s+\$LeaderGuid\s+-Destination\s+\$Name'
        $source | Should -Match "Write-Receipt\s+-Event\s+'cohesion_backtrack'"
        $source | Should -Not -Match 'cohesion_backtrack[\s\S]{0,500}TeleportTo'
    }

    It 'parse-checks the director and this focused test' {
        foreach ($path in @($script:DirectorPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
