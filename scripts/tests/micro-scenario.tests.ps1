Set-StrictMode -Version Latest

BeforeAll {
    $script:ServerRoot = Split-Path -Parent $PSScriptRoot
    $script:Runner = Join-Path $script:ServerRoot 'micro-scenario.ps1'
    $script:Manifest = Join-Path $script:ServerRoot 'tests\fixtures\micro-scenario-runner-smoke.json'
    $script:AcceleratedManifest = Join-Path $script:ServerRoot 'tests\fixtures\micro-scenario-accelerated-smoke.json'
}

Describe 'AutoWoW micro-scenario runner' {
    It 'parse-checks runner and library' {
        foreach ($path in @(
            $script:Runner,
            (Join-Path $script:ServerRoot 'micro-scenario-lib.ps1'),
            (Join-Path $script:ServerRoot 'autowow-control.ps1')
        )) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile(
                $path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }

    It 'plans every step without contacting the bridge' {
        $plan = (& $script:Runner -Action plan -ManifestPath $script:Manifest | Out-String) | ConvertFrom-Json -Depth 100
        $plan.dry_run | Should -BeTrue
        $plan.execute_required | Should -BeTrue
        @($plan.steps).Count | Should -Be 5
        @($plan.steps | Where-Object mutation).Count | Should -Be 2
        @($plan.steps.request) | Should -Contain 'fixture init 236 5 0 2'
        @($plan.steps.request) | Should -Contain 'independent 236'
    }

    It 'requires both live safety switches before execution' {
        {
            & $script:Runner -Action run -ManifestPath $script:Manifest -RunId 'runner-safety-test' -Execute
        } | Should -Throw '*both -Apply and -Execute*'
    }

    It 'keeps acceleration wire construction exact and bounded' {
        $control = Join-Path $script:ServerRoot 'autowow-control.ps1'
        & $control -Action fixture-accelerate -BotGuid 236 -PacingPercent 1000 -EmitRequestOnly |
            Should -BeExactly 'fixture accelerate 236 1000'
        & $control -Action fixture-kill -BotGuid 236 -TargetEntry 324 -StableSpawnId 201104 -EmitRequestOnly |
            Should -BeExactly 'fixture kill 236 entry 324 spawn 201104'
        {
            & $control -Action fixture-accelerate -BotGuid 236 -PacingPercent 999 -EmitRequestOnly
        } | Should -Throw
    }

    It 'plans an accelerated fixture with explicit cleanup' {
        $plan = (& $script:Runner -Action plan -ManifestPath $script:AcceleratedManifest | Out-String) | ConvertFrom-Json -Depth 100
        $plan.dry_run | Should -BeTrue
        @($plan.steps).Count | Should -Be 4
        @($plan.steps | Where-Object mutation).Count | Should -Be 2
        @($plan.steps.request) | Should -Contain 'fixture accelerate 236 1000'
        @($plan.steps.request) | Should -Contain 'fixture accelerate-off 236'
    }
}
