Set-StrictMode -Version Latest

BeforeAll {
    $script:ProofPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'gathering-proof.ps1'
    $script:RosterPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'fixtures\gathering-chelleney.json'
    $script:Source = Get-Content -LiteralPath $script:ProofPath -Raw
    . $script:ProofPath -LibraryOnly
}

Describe 'Gathering proof persistence contract' {
    It 'is dry-run by default and declares exact normal-logout persistence' {
        $plan = (& $script:ProofPath -FixtureRosterPath $script:RosterPath -DurationSeconds 5 | Out-String) | ConvertFrom-Json
        $plan.schema | Should -BeExactly 'autowow.gathering-proof.v2'
        $plan.mode | Should -BeExactly 'dry_run'
        $plan.safety.direct_db_writes | Should -Be 0
        @($plan.safety.allowed_apply_bridge_actions) | Should -Contain 'deactivate'
        ($plan.planned_sequence -join ' ') | Should -Match 'poll read-only SELECT snapshots after logout until stable'
        $plan.safety.persistence.read_only | Should -BeTrue
    }

    It 'takes the final live snapshot before exact deactivation and reads SQL afterward' {
        $snapshot = $script:Source.IndexOf("`$stage = 'final_live_snapshot'")
        $logout = $script:Source.IndexOf("`$stage = 'persistence_logout'")
        $current = $script:Source.IndexOf("`$stage = 'current_snapshot'", $logout)
        $snapshot | Should -BeGreaterOrEqual 0
        $logout | Should -BeGreaterThan $snapshot
        $current | Should -BeGreaterThan $logout
        $script:Source | Should -Match 'Invoke-ControlJson\s+-Action\s+deactivate\s+-BotGuid'
        $script:Source | Should -Match 'Wait-ForRosterOffline'
        $script:Source | Should -Match 'Wait-ForPersistedSnapshot'
        $script:Source | Should -Match 'gather_route'
        $script:Source | Should -Match 'it\.class IN \(3,7\)'
        $script:Source | Should -Match 'it\.subclass'
        $script:Source | Should -Not -Match 'Start-Sleep\s+-Seconds\s+2'
    }

    It 'accepts an empty route-observation collector for the first polling snapshot' {
        $observations = [System.Collections.Generic.List[object]]::new()
        $snapshot = [pscustomobject]@{
            bot = [pscustomobject]@{
                gather_route = [pscustomobject]@{
                    status = 'walking'
                    credit_evidence = 'none'
                    candidate = [pscustomobject]@{ profession = 'herbalism'; entry = 1620; spawn_id = 207617 }
                }
            }
        }

        { Add-GatherRouteObservation -Observations $observations -Guid ([uint32]49) -SnapshotResponse $snapshot -Phase 'observation' } |
            Should -Not -Throw

        $observations.Count | Should -Be 1
        $observations[0].guid | Should -Be ([uint32]49)
        $observations[0].phase | Should -BeExactly 'observation'
        $observations[0].route.status | Should -BeExactly 'walking'
    }

    It 'accepts empty material deltas and yields no durable gathering credit' {
        $member = [pscustomobject]@{
            guid = [uint32]49
            name = 'Pikli'
            profession = 'Herbalism'
            campaign_character = $true
        }
        $baseline = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @()
            tools = @()
        }
        $current = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @()
            tools = @()
        }

        { Get-GatheringAttribution -Member $member -MaterialDeltas @() -RouteObservations @() } |
            Should -Not -Throw
        $proof = New-MemberProof -Member $member -Baseline $baseline -Current $current -RouteObservations @()
        $aggregate = Get-ProofAggregate -MemberProofs @($proof)

        $proof.status | Should -BeExactly 'inconclusive'
        $proof.attribution.status | Should -BeExactly 'none'
        $proof.attribution.reason | Should -BeExactly 'no_positive_inventory_delta_observed'
        $proof.observed_positive_material_units | Should -Be 0
        $proof.positive_material_units | Should -Be 0
        $proof.unattributed_positive_material_units | Should -Be 0
        $aggregate.status | Should -BeExactly 'inconclusive'
        $aggregate.durable_positive_material_member_count | Should -Be 0
        $aggregate.durable_positive_material_units | Should -Be 0
    }

    It 'reports partial success without weakening the all-members-required full-pass contract' {
        $aggregate = Get-ProofAggregate -MemberProofs @(
            [pscustomobject]@{ guid = [uint32]49; status = 'pass'; positive_material_units = 3 },
            [pscustomobject]@{ guid = [uint32]3; status = 'inconclusive'; positive_material_units = 0 },
            [pscustomobject]@{ guid = [uint32]6; status = 'inconclusive'; positive_material_units = 0 }
        )

        $aggregate.status | Should -BeExactly 'partial'
        $aggregate.full_pass_requires_all_members | Should -BeTrue
        $aggregate.contract | Should -BeExactly 'all_members_required_for_pass'
        $aggregate.durable_positive_material_member_count | Should -Be 1
        @($aggregate.durable_positive_material_member_guids) | Should -Contain ([uint32]49)
        $aggregate.non_pass_member_count | Should -Be 2
    }

    It 'keeps a member inconclusive until the read-only persistence snapshot is stable' {
        $script:ReadSnapshotCount = 0
        Mock New-ReadSnapshot {
            $script:ReadSnapshotCount++
            $materialCount = if ($script:ReadSnapshotCount -eq 1) { 1 } else { 2 }
            [pscustomobject][ordered]@{
                collected_utc = 'test'
                read_only = $true
                skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
                materials = @([pscustomobject]@{ guid = [uint32]49; item_entry = 3369; count = $materialCount })
                tools = @()
            }
        }

        $observation = Wait-ForPersistedSnapshot -Members @([pscustomobject]@{ guid = [uint32]49 }) -Connections ([ordered]@{}) -ClientPath 'unused' -TimeoutSeconds 5 -PollSeconds 1 -StableReadsRequired 2

        $observation.stable | Should -BeTrue
        $observation.reason | Should -BeExactly 'stable_read_set_reached'
        $observation.attempts | Should -Be 3
        $observation.read_only | Should -BeTrue
        $script:ReadSnapshotCount | Should -Be 3
    }

    It 'does not call a positive inventory delta durable when persistence times out' {
        $member = [pscustomobject]@{
            guid = [uint32]49
            name = 'Pikli'
            profession = 'Herbalism'
            campaign_character = $true
        }
        $baseline = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @([pscustomobject]@{ guid = [uint32]49; item_entry = 3369; item_name = 'Grave Moss'; count = 3 })
            tools = @()
        }
        $current = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @([pscustomobject]@{ guid = [uint32]49; item_entry = 3369; item_name = 'Grave Moss'; count = 6 })
            tools = @()
        }

        $proof = New-MemberProof -Member $member -Baseline $baseline -Current $current -PersistenceStable:$false

        $proof.status | Should -BeExactly 'inconclusive'
        $proof.positive_material_units | Should -Be 0
        $proof.observed_positive_material_units | Should -Be 3
        $proof.unattributed_positive_material_units | Should -Be 3
        $proof.persistence_stable | Should -BeFalse
        @($proof.reasons) | Should -Contain 'positive_inventory_delta_not_yet_attributed_or_durable'
    }

    It 'does not prove Herbalism from the exact persisted class-7 meat false-positive receipt' {
        $member = [pscustomobject]@{
            guid = [uint32]49
            name = 'Pikli'
            profession = 'Herbalism'
            campaign_character = $true
        }
        $baseline = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @(
                [pscustomobject]@{ guid = [uint32]49; item_entry = 1081; item_name = 'Crisp Spider Meat'; item_class = 7; item_subclass = 8; count = 3 }
                [pscustomobject]@{ guid = [uint32]49; item_entry = 2251; item_name = 'Gooey Spider Leg'; item_class = 7; item_subclass = 8; count = 3 }
            )
            tools = @()
        }
        $current = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @(
                [pscustomobject]@{ guid = [uint32]49; item_entry = 1081; item_name = 'Crisp Spider Meat'; item_class = 7; item_subclass = 8; count = 4 }
                [pscustomobject]@{ guid = [uint32]49; item_entry = 2251; item_name = 'Gooey Spider Leg'; item_class = 7; item_subclass = 8; count = 5 }
            )
            tools = @()
        }

        $proof = New-MemberProof -Member $member -Baseline $baseline -Current $current -RouteObservations @()

        $proof.status | Should -BeExactly 'inconclusive'
        $proof.positive_material_units | Should -Be 0
        $proof.observed_positive_material_units | Should -Be 3
        $proof.unattributed_positive_material_units | Should -Be 3
        $proof.attribution.status | Should -BeExactly 'inconclusive'
        $proof.attribution.reason | Should -BeExactly 'gather_route_attribution_absent'
        @($proof.reasons) | Should -Contain 'gather_route_attribution_absent'
        @($proof.attribution.unattributed_deltas | ForEach-Object { $_.material_category }) | Should -Be @('meat', 'meat')
    }

    It 'proves Herbalism only when a herb yield matches an exact source receipt' {
        $member = [pscustomobject]@{ guid = [uint32]49; name = 'Pikli'; profession = 'Herbalism'; campaign_character = $true }
        $baseline = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @([pscustomobject]@{ guid = [uint32]49; item_entry = 785; item_name = 'Mageroyal'; item_class = 7; item_subclass = 9; count = 1 })
            tools = @()
        }
        $current = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]49; skill_id = 182; value = 303; max = 375 })
            materials = @([pscustomobject]@{ guid = [uint32]49; item_entry = 785; item_name = 'Mageroyal'; item_class = 7; item_subclass = 9; count = 4 })
            tools = @()
        }
        $route = [pscustomobject][ordered]@{
            source_guid = 263
            requested_material_item_id = 785
            source_yields_requested_material = $true
            credit_evidence = 'loot_consumed'
            candidate = [pscustomobject]@{ spawn_id = 207617; entry = 1620; name = 'Mageroyal'; profession = 'herbalism' }
            oracle_source = [pscustomobject]@{ valid = $false; spawn_id = 0; entry = 0 }
        }

        $proof = New-MemberProof -Member $member -Baseline $baseline -Current $current -RouteObservations @(
            [ordered]@{ guid = [uint32]49; phase = 'observation'; route = $route }
        )

        $proof.status | Should -BeExactly 'pass'
        $proof.positive_material_units | Should -Be 3
        $proof.attribution.status | Should -BeExactly 'proven'
        $proof.attribution.attributed_positive_material_units | Should -Be 3
        $proof.attribution.attributed_deltas[0].material_category | Should -BeExactly 'herb'
    }

    It 'proves Mining for an ore or stone yield only with an exact mining-source receipt' {
        $member = [pscustomobject]@{ guid = [uint32]6; name = 'Kurl'; profession = 'Mining'; campaign_character = $true }
        $baseline = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]6; skill_id = 186; value = 355; max = 375 })
            materials = @([pscustomobject]@{ guid = [uint32]6; item_entry = 2835; item_name = 'Rough Stone'; item_class = 7; item_subclass = 7; count = 2 })
            tools = @([pscustomobject]@{ guid = [uint32]6; item_entry = 2901; count = 1 })
        }
        $current = [pscustomobject]@{
            skills = @([pscustomobject]@{ guid = [uint32]6; skill_id = 186; value = 355; max = 375 })
            materials = @([pscustomobject]@{ guid = [uint32]6; item_entry = 2835; item_name = 'Rough Stone'; item_class = 7; item_subclass = 7; count = 5 })
            tools = @([pscustomobject]@{ guid = [uint32]6; item_entry = 2901; count = 1 })
        }
        $route = [pscustomobject][ordered]@{
            source_guid = 9001
            requested_material_item_id = 2835
            source_yields_requested_material = $true
            credit_evidence = 'skill_credit'
            candidate = [pscustomobject]@{ spawn_id = 201104; entry = 324; name = 'Small Thorium Vein'; profession = 'mining' }
            oracle_source = [pscustomobject]@{ valid = $false; spawn_id = 0; entry = 0 }
        }

        $proof = New-MemberProof -Member $member -Baseline $baseline -Current $current -RouteObservations @(
            [ordered]@{ guid = [uint32]6; phase = 'observation'; route = $route }
        )

        $proof.status | Should -BeExactly 'pass'
        $proof.positive_material_units | Should -Be 3
        $proof.attribution.status | Should -BeExactly 'proven'
        $proof.attribution.attributed_deltas[0].material_category | Should -BeExactly 'ore_or_stone'
    }

    It 'gives failures precedence over partial success' {
        $aggregate = Get-ProofAggregate -MemberProofs @(
            [pscustomobject]@{ guid = [uint32]49; status = 'pass'; positive_material_units = 3 },
            [pscustomobject]@{ guid = [uint32]3; status = 'fail'; positive_material_units = 0 }
        )

        $aggregate.status | Should -BeExactly 'fail'
        $aggregate.durable_positive_material_member_count | Should -Be 1
    }

    It 'contains no direct database write language and parses cleanly' {
        $script:Source | Should -Not -Match '(?im)^\s*(INSERT|UPDATE|DELETE|REPLACE|ALTER|CREATE|DROP|TRUNCATE)(?:\s+|$)'
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($script:ProofPath, [ref]$null, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0
    }
}
