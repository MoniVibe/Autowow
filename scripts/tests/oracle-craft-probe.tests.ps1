Describe 'Oracle craft probe' {
    BeforeAll {
        $script:Probe = Join-Path (Split-Path -Parent $PSScriptRoot) 'probe-oracle-craft.ps1'
        $script:Source = Get-Content -LiteralPath $script:Probe -Raw
    }

    It 'parses cleanly' {
        $errors = $null
        $tokens = $null
        [System.Management.Automation.Language.Parser]::ParseFile(
            $script:Probe, [ref]$tokens, [ref]$errors) | Out-Null
        $errors.Count | Should -Be 0
    }

    It 'keeps mutation opt-in and selects a deterministic recipe order' {
        $script:Source | Should -Match '\[switch\]\$Execute'
        $script:Source | Should -Match 'Sort-Object \{ \[uint32\]\$_.spell_id \}'
        $script:Source | Should -Match 'mode = if \(\$Execute\) \{ ''execute_once'' \} else \{ ''read_only'' \}'
        $script:Source | Should -Match 'mutation_count = 0'
    }

    It 'limits the live mutation to one exact craft command' {
        $script:Source | Should -Match '\$receipt\.mutation_count = 1'
        $script:Source | Should -Match 'Invoke-ControlJson -Action craft -RecipeSpellId'
        $script:Source | Should -Not -Match 'for(each)?\s*\('
    }
}
