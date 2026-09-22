Set-StrictMode -Version Latest

BeforeAll {
    $script:ServerRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $script:CatalogPath = Join-Path $script:ServerRoot 'scripts\micro-scenarios\catalog.json'
    $script:Catalog = Get-Content -LiteralPath $script:CatalogPath -Raw | ConvertFrom-Json -Depth 100
    $script:ManifestRoot = Split-Path -Parent $script:CatalogPath
}

Describe 'AutoWoW micro-scenario catalog' {
    It 'has the expected schema and six fixture examples' {
        $script:Catalog.schema | Should -BeExactly 'autowow.micro-scenario.catalog.v1'
        @($script:Catalog.manifests).Count | Should -Be 6
        @($script:Catalog.manifests | Select-Object -ExpandProperty id -Unique).Count | Should -Be 6
        @($script:Catalog.manifests.id) | Should -Contain 'gather-exact-node-v1'
        @($script:Catalog.manifests.id) | Should -Contain 'blacksmith-one-craft-v1'
        @($script:Catalog.manifests.id) | Should -Contain 'quest-giver-acquisition-v1'
        @($script:Catalog.manifests.id) | Should -Contain 'combat-role-readiness-v1'
        @($script:Catalog.manifests.id) | Should -Contain 'quest-progression-accelerated-fixture-v1'
        @($script:Catalog.manifests.id) | Should -Contain 'harvest-accelerated-fixture-v1'
    }

    It 'keeps the catalog fail-closed for identity, proof shortcuts, and pacing' {
        $script:Catalog.default_mode | Should -BeExactly 'dry_run'
        $script:Catalog.fixture_policy.fixture_only | Should -BeTrue
        $script:Catalog.fixture_policy.neutral_faction_claim_allowed | Should -BeFalse
        $script:Catalog.fixture_policy.synthetic_quest_completion_allowed | Should -BeFalse
        $script:Catalog.fixture_policy.direct_database_completion_allowed | Should -BeFalse
        $script:Catalog.fixture_policy.teleport_based_proof_allowed | Should -BeFalse
        $script:Catalog.fixture_policy.accelerated_pacing.multiplier | Should -Be 10
        $script:Catalog.fixture_policy.accelerated_pacing.label | Should -BeExactly '1000_percent_of_baseline'
    }

    It 'resolves every catalog entry to a manifest with preparation, operation, and observable postconditions' {
        foreach ($entry in @($script:Catalog.manifests)) {
            $manifestPath = Join-Path $script:ManifestRoot ([string]$entry.file)
            Test-Path -LiteralPath $manifestPath -PathType Leaf | Should -BeTrue
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -Depth 100

            $manifest.schema | Should -BeExactly 'autowow.micro-scenario.manifest.v1'
            $manifest.id | Should -BeExactly $entry.id
            $manifest.fixture_only | Should -BeTrue
            $manifest.identity.neutral_faction_claim | Should -BeFalse
            @('wayfarer', 'out-of-roster') | Should -Contain ([string]$manifest.identity.affiliation_semantics)
            @('wayfarer', 'out-of-roster') | Should -Contain ([string]$manifest.identity.roster_semantics)
            $null -ne $manifest.preparation | Should -BeTrue
            $null -ne $manifest.operation | Should -BeTrue
            @($manifest.preparation.steps).Count | Should -BeGreaterThan 0
            @($manifest.operation.allowed_mutations).Count | Should -BeGreaterThan 0
            @($manifest.operation.native_evidence_required).Count | Should -BeGreaterThan 0
            @($manifest.postconditions).Count | Should -BeGreaterThan 0
            foreach ($postcondition in @($manifest.postconditions)) {
                [string]::IsNullOrWhiteSpace([string]$postcondition.observable) | Should -BeFalse
                [string]::IsNullOrWhiteSpace([string]$postcondition.predicate) | Should -BeFalse
            }
        }
    }

    It 'keeps preparation distinct from operation' {
        foreach ($entry in @($script:Catalog.manifests)) {
            $manifestPath = Join-Path $script:ManifestRoot ([string]$entry.file)
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -Depth 100
            @($manifest.preparation.allowed_mutations) | Should -Not -Contain 'craft'
            @($manifest.preparation.allowed_mutations) | Should -Not -Contain 'engage'
            @($manifest.preparation.allowed_mutations) | Should -Not -Contain 'deploy'
            @($manifest.operation.allowed_mutations).Count | Should -BeGreaterThan 0
            @($manifest.preparation.baseline_observations).Count | Should -BeGreaterThan 0
        }
    }

    It 'does not hide synthetic completion, teleport proof, or seeded output as evidence' {
        foreach ($entry in @($script:Catalog.manifests)) {
            $manifestPath = Join-Path $script:ManifestRoot ([string]$entry.file)
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -Depth 100
            $operationJson = $manifest.operation | ConvertTo-Json -Depth 100
            $operationJson | Should -Not -Match '(?i)synthetic_quest_completion_allowed\s*"\s*:\s*true'
            $operationJson | Should -Not -Match '(?i)direct_database_quest_write_allowed\s*"\s*:\s*true'
            $operationJson | Should -Not -Match '(?i)teleport_proof_allowed\s*"\s*:\s*true'
            $nativeEvidence = @($manifest.operation.native_evidence_required | ForEach-Object { [string]$_ })
            $nativeEvidence | Should -Not -Contain 'seeded_output'
            $nativeEvidence | Should -Not -Contain 'seeded_credit'
            $nativeEvidence | Should -Not -Contain 'seeded_completion'
        }
    }

    It 'marks both accelerated examples as fixture-only 10x and adapter-gated' {
        foreach ($id in @('quest-progression-accelerated-fixture-v1', 'harvest-accelerated-fixture-v1')) {
            $entry = @($script:Catalog.manifests | Where-Object id -eq $id)
            $entry.Count | Should -Be 1
            $manifestPath = Join-Path $script:ManifestRoot ([string]$entry[0].file)
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -Depth 100

            $manifest.status | Should -BeExactly 'blocked_until_adapter'
            $manifest.acceleration.scope | Should -BeExactly 'fixture-only'
            $manifest.acceleration.pacing_percent_of_baseline | Should -Be 1000
            $manifest.acceleration.pacing_multiplier | Should -Be 10
            $manifest.acceleration.production_or_campaign_allowed | Should -BeFalse
            $manifest.acceleration.exact_target_instant_kill.fallback_when_unsupported | Should -BeExactly 'blocked'
            @($manifest.operation.adapter_required).Count | Should -BeGreaterThan 0
            @($manifest.operation.native_evidence_required).Count | Should -BeGreaterThan 0
            [string]$manifest.adapter_boundary.native_evidence_remains_mandatory -join ' ' |
                Should -Not -BeNullOrEmpty
        }
    }

    It 'keeps exact-target instant-kill conditional and unable to replace native credit' {
        foreach ($id in @('quest-progression-accelerated-fixture-v1', 'harvest-accelerated-fixture-v1')) {
            $entry = @($script:Catalog.manifests | Where-Object id -eq $id)[0]
            $manifest = Get-Content -LiteralPath (Join-Path $script:ManifestRoot ([string]$entry.file)) -Raw | ConvertFrom-Json -Depth 100
            $kill = $manifest.acceleration.exact_target_instant_kill
            $kill.requested_for_kill_objectives -or $kill.requested_for_guarded_sources | Should -BeTrue
            $kill.does_not_replace_native_credit | Should -BeTrue
            @($kill.target_must_match).Count | Should -BeGreaterThan 0
            [string]$kill.supported_only_when | Should -Match '(?i)capability|adapter'
        }
    }
}
