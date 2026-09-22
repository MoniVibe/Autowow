Describe 'q435 escort fixture contract' {
    BeforeAll {
        $scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'escort-fixture-q435.ps1'
        $source = Get-Content -LiteralPath $scriptPath -Raw
    }

    It 'parses cleanly' {
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$errors)
        $errors.Count | Should -Be 0
    }

    It 'is dry-run by default and declares zero quest-state writes' {
        $plan = (& $scriptPath | Out-String) | ConvertFrom-Json
        $plan.applies | Should -BeFalse
        $plan.quest | Should -Be 435
        $plan.quest_state_writes | Should -Be 0
        $plan.stage | Should -Be 'q435-erland'
    }

    It 'never writes authoritative quest or reward tables' {
        $source | Should -Not -Match '(?im)^\s*(INSERT|UPDATE|DELETE|REPLACE)\s+.*character_quest'
        $source | Should -Not -Match '(?im)^\s*(INSERT|UPDATE|DELETE|REPLACE)\s+.*queststatus'
        $source | Should -Not -Match '(?im)^\s*(INSERT|UPDATE|DELETE|REPLACE)\s+.*rewarded'
    }

    It 'requires normal NPC acceptance and native escort evidence' {
        $source | Should -Match 'normal quest-giver interaction'
        $source | Should -Match "phase -eq 'escort_event'"
        $source | Should -Match 'saw_native_completion'
        $source | Should -Match 'helper_rewarded'
    }
}
