$ErrorActionPreference = 'Stop'

Describe 'AutoWoW project skill hierarchy' {
    BeforeAll {
        $script:ServerRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
        $script:SkillRoot = Join-Path $script:ServerRoot '.codex\skills'
        $script:ExpectedSkills = @(
            'autowow-orchestrate-lab'
            'autowow-operate-runtime'
            'autowow-run-probes'
            'autowow-advance-questers'
            'autowow-validate-gatherers'
            'autowow-review-evidence'
        )
    }

    It 'contains the complete bounded hierarchy' {
        $actual = @(Get-ChildItem -LiteralPath $script:SkillRoot -Directory | ForEach-Object Name | Sort-Object)
        $actual | Should -Be ($script:ExpectedSkills | Sort-Object)
    }

    It 'keeps every SKILL frontmatter limited to name and description' -ForEach @(
        @{ Skill = 'autowow-orchestrate-lab' }
        @{ Skill = 'autowow-operate-runtime' }
        @{ Skill = 'autowow-run-probes' }
        @{ Skill = 'autowow-advance-questers' }
        @{ Skill = 'autowow-validate-gatherers' }
        @{ Skill = 'autowow-review-evidence' }
    ) {
        $path = Join-Path $script:SkillRoot "$Skill\SKILL.md"
        $raw = Get-Content -LiteralPath $path -Raw
        $frontmatter = [regex]::Match($raw, '(?ms)\A---\r?\n(?<body>.*?)\r?\n---').Groups['body'].Value
        $keys = @([regex]::Matches($frontmatter, '(?m)^([a-z_]+):') | ForEach-Object { $_.Groups[1].Value })
        $keys | Should -Be @('name', 'description')
        $raw | Should -Not -Match '\[TODO|TODO:'
    }

    It 'keeps UI metadata aligned with each skill name' -ForEach @(
        @{ Skill = 'autowow-orchestrate-lab' }
        @{ Skill = 'autowow-operate-runtime' }
        @{ Skill = 'autowow-run-probes' }
        @{ Skill = 'autowow-advance-questers' }
        @{ Skill = 'autowow-validate-gatherers' }
        @{ Skill = 'autowow-review-evidence' }
    ) {
        $path = Join-Path $script:SkillRoot "$Skill\agents\openai.yaml"
        $raw = Get-Content -LiteralPath $path -Raw
        $raw | Should -Match 'display_name:'
        $raw | Should -Match 'short_description:'
        $raw | Should -Match ([regex]::Escape("`$$Skill"))
    }

    It 'assigns every specialist from the parent orchestrator' {
        $raw = Get-Content -LiteralPath (Join-Path $script:SkillRoot 'autowow-orchestrate-lab\SKILL.md') -Raw
        foreach ($skill in $script:ExpectedSkills | Where-Object { $_ -ne 'autowow-orchestrate-lab' }) {
            $raw | Should -Match ([regex]::Escape("`$$skill"))
        }
    }

    It 'separates service ownership from disjoint gameplay mutators' {
        $orchestrator = Get-Content -LiteralPath (Join-Path $script:SkillRoot 'autowow-orchestrate-lab\SKILL.md') -Raw
        $runtime = Get-Content -LiteralPath (Join-Path $script:SkillRoot 'autowow-operate-runtime\SKILL.md') -Raw
        $orchestrator | Should -Match 'one service/configuration runtime operator'
        $orchestrator | Should -Match 'gameplay mutator per lane'
        $runtime | Should -Match 'Disjoint gameplay lanes may issue their own roster-scoped orders'
    }

    It 'requires receipts for every lane including the read-only reviewer' -ForEach @(
        @{ Skill = 'autowow-orchestrate-lab' }
        @{ Skill = 'autowow-operate-runtime' }
        @{ Skill = 'autowow-run-probes' }
        @{ Skill = 'autowow-advance-questers' }
        @{ Skill = 'autowow-validate-gatherers' }
        @{ Skill = 'autowow-review-evidence' }
    ) {
        $raw = Get-Content -LiteralPath (Join-Path $script:SkillRoot "$Skill\SKILL.md") -Raw
        $raw | Should -Match '## Receipt \(Required\)'
    }

    It 'keeps the evidence reviewer explicitly non-mutating' {
        $raw = Get-Content -LiteralPath (Join-Path $script:SkillRoot 'autowow-review-evidence\SKILL.md') -Raw
        $raw | Should -Match 'Do not issue gameplay or runtime mutations'
    }
}
