Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:LibraryPath = Join-Path $script:ScriptsRoot 'probe-rotation-lib.ps1'
    $script:ScriptPath = Join-Path $script:ScriptsRoot 'probe-rotation.ps1'
    $script:CatalogPath = Join-Path $script:ScriptsRoot 'fixtures\probe-rotation-catalog.json'
    . $script:LibraryPath

    function Write-TestJson {
        param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$Object)
        $Object | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
    }

    function New-TestManifest {
        param([Parameter(Mandatory)][string]$Path, [string]$Id = 'candidate')
        $manifest = [ordered]@{
            schema = 'autowow.probe-lab.manifest.v1'
            lab_id = "test-$Id"
            probes = @([ordered]@{
                id = $Id; kind = 'party'; size = 5; leader_guid = 1001; expected_map_id = 576
                exterior_route = 'test-exterior'
                members = @(1..5 | ForEach-Object { [ordered]@{ guid = [uint32](1000 + $_); name = "Bot$_"; level = 80; spec_index = 0; quality = 3 } })
            })
        }
        Write-TestJson -Path $Path -Object $manifest
    }

    function New-TestHistory {
        param(
            [Parameter(Mandatory)][string]$Directory,
            [Parameter(Mandatory)][string]$CandidateId,
            [ValidateSet('PASS','FAIL')][string]$Status = 'FAIL',
            [string]$BuildId = 'build-a',
            [string]$FixId = 'fix-a',
            [string]$ResetId = ''
        )
        $summaryPath = Join-Path $Directory "$CandidateId-summary.json"
        $receiptPath = Join-Path $Directory "$CandidateId-receipt.jsonl"
        Write-TestJson -Path $summaryPath -Object ([ordered]@{
            schema = 'autowow.probe-lab.summary.v1'; status = $Status; reason = if ($Status -eq 'FAIL') { 'test_failure' } else { 'all_probes_healthy' }
            completed_at_utc = [datetime]::UtcNow.ToString('o')
            build_id = $BuildId; fix_id = $FixId
            probes = @([ordered]@{ probe_id = $CandidateId; status = $Status; reason = if ($Status -eq 'FAIL') { 'test_failure' } else { 'all_probes_healthy' } })
        })
        $terminal = [ordered]@{ schema = 'autowow.probe-lab.receipt.v1'; timestamp_utc = [datetime]::UtcNow.ToString('o'); event = 'monitor_complete'; status = $Status; build_id = $BuildId; fix_id = $FixId }
        $terminal | ConvertTo-Json -Compress -Depth 20 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM
        [ordered]@{ summary_path = $summaryPath; receipt_path = $receiptPath; build_id = $BuildId; fix_id = $FixId; reset_id = $ResetId }
    }

    function New-TestCatalog {
        param(
            [Parameter(Mandatory)][string]$Directory,
            [Parameter(Mandatory)]$Candidates
        )
        $path = Join-Path $Directory 'catalog.json'
        Write-TestJson -Path $path -Object ([ordered]@{ schema = 'autowow.probe-rotation.catalog.v1'; candidates = @($Candidates) })
        return $path
    }

    function New-TestCandidate {
        param(
            [Parameter(Mandatory)][string]$Directory,
            [Parameter(Mandatory)][string]$Id,
            [string[]]$Mechanics = @('party','boss'),
            [string]$BuildId = 'build-a',
            [string]$FixId = 'fix-a',
            [object[]]$History = @(),
            [string]$ResetId = '',
            [int]$Priority = 0
        )
        $manifestPath = Join-Path $Directory "$Id-manifest.json"
        New-TestManifest -Path $manifestPath -Id $Id
        [ordered]@{ id = $Id; label = $Id; kind = 'party'; probe_id = $Id; manifest_path = $manifestPath; mechanics = $Mechanics; priority = $Priority; build_id = $BuildId; fix_id = $FixId; reset_id = $ResetId; history = @($History) }
    }
}

Describe 'Probe rotation fixture and anti-treadmill classification' {
    It 'retires DTK after the ca44 PASS and quarantines unchanged Nexus and Onyxia failures' {
        $catalog = Read-ProbeRotationCatalog -Path $script:CatalogPath
        $plan = New-ProbeRotationPlan -Catalog $catalog -MaxCandidates 10
        (@($plan.candidates | Where-Object id -eq 'dtk-party')[0]).status | Should -Be 'RETIRED'
        (@($plan.candidates | Where-Object id -eq 'nexus-party')[0]).status | Should -Be 'QUARANTINED'
        (@($plan.candidates | Where-Object id -eq 'nexus-party')[0]).reason | Should -Be 'unchanged_failure_quarantine'
        (@($plan.candidates | Where-Object id -eq 'onyxia-raid')[0]).status | Should -Be 'QUARANTINED'
        (@($plan.candidates | Where-Object id -eq 'onyxia-raid')[0]).reason | Should -Be 'unchanged_failure_quarantine'
        @($plan.next_candidates.id) | Should -Contain 'uk-party'
        @($plan.next_candidates.id) | Should -Contain 'voa-raid'
    }

    It 'allows one changed-build retest, then quarantines the result on the new identity' {
        $history = New-TestHistory -Directory $TestDrive -CandidateId 'nexus-test' -Status FAIL -BuildId 'ca44' -FixId 'ca44'
        $candidate = New-TestCandidate -Directory $TestDrive -Id 'nexus-test' -BuildId 'ca45' -FixId 'ca44' -History @($history)
        $catalogPath = New-TestCatalog -Directory $TestDrive -Candidates @($candidate)
        $first = New-ProbeRotationPlan -Catalog (Read-ProbeRotationCatalog $catalogPath) -MaxCandidates 1
        $first.candidates[0].status | Should -Be 'RETEST'
        $first.candidates[0].reason | Should -Be 'new_fix_or_build_after_failure'
        $secondHistory = New-TestHistory -Directory $TestDrive -CandidateId 'nexus-test-new' -Status FAIL -BuildId 'ca45' -FixId 'ca44'
        $candidate.history += [ordered]@{ summary_path = $secondHistory.summary_path; receipt_path = $secondHistory.receipt_path; build_id = 'ca45'; fix_id = 'ca44' }
        $secondCatalog = New-TestCatalog -Directory $TestDrive -Candidates @($candidate)
        $second = New-ProbeRotationPlan -Catalog (Read-ProbeRotationCatalog $secondCatalog) -MaxCandidates 1
        $second.candidates[0].status | Should -Be 'QUARANTINED'
        $second.candidates[0].reason | Should -Be 'unchanged_failure_quarantine'
        @($second.next_candidates).Count | Should -Be 0
    }

    It 'never reopens a passed candidate unless the catalog reset id changes' {
        $history = New-TestHistory -Directory $TestDrive -CandidateId 'passed-test' -Status PASS -BuildId 'ca44' -FixId 'ca44'
        $candidate = New-TestCandidate -Directory $TestDrive -Id 'passed-test' -BuildId 'ca45' -FixId 'ca45' -History @($history)
        $catalogPath = New-TestCatalog -Directory $TestDrive -Candidates @($candidate)
        (New-ProbeRotationPlan -Catalog (Read-ProbeRotationCatalog $catalogPath) -MaxCandidates 1).candidates[0].status | Should -Be 'RETIRED'
        $candidate.reset_id = 'reset-20260717'
        $resetCatalog = New-TestCatalog -Directory $TestDrive -Candidates @($candidate)
        $resetPlan = New-ProbeRotationPlan -Catalog (Read-ProbeRotationCatalog $resetCatalog) -MaxCandidates 1
        $resetPlan.candidates[0].status | Should -Be 'ELIGIBLE'
        $resetPlan.candidates[0].reason | Should -Be 'explicit_catalog_reset'
    }

    It 'prefers unseen candidates and novel mechanics deterministically' {
        $knownHistory = New-TestHistory -Directory $TestDrive -CandidateId 'known' -Status PASS -BuildId 'ca44' -FixId 'ca44'
        $known = New-TestCandidate -Directory $TestDrive -Id 'known' -Mechanics @('party','boss','shared-regroup') -History @($knownHistory)
        $novelA = New-TestCandidate -Directory $TestDrive -Id 'zeta-new' -Mechanics @('party','vehicle','elevator')
        $novelB = New-TestCandidate -Directory $TestDrive -Id 'alpha-new' -Mechanics @('raid','portal','branching')
        $catalogPath = New-TestCatalog -Directory $TestDrive -Candidates @($known, $novelA, $novelB)
        $plan = New-ProbeRotationPlan -Catalog (Read-ProbeRotationCatalog $catalogPath) -MaxCandidates 2
        @($plan.next_candidates.id) | Should -Be @('alpha-new','zeta-new')
        @($plan.next_candidates[0].novel_mechanics) | Should -Be @('branching','portal','raid')
    }

    It 'is offline by construction and emits a machine-readable dry-run plan' {
        $raw = & $script:ScriptPath -CatalogPath $script:CatalogPath -MaxCandidates 2 | Out-String
        $plan = $raw | ConvertFrom-Json
        $plan.schema | Should -Be 'autowow.probe-rotation.plan.v1'
        $plan.dry_run | Should -BeTrue
        $plan.safety.bridge_calls | Should -Be 0
        $plan.safety.process_launches | Should -Be 0
        $plan.safety.database_writes | Should -Be 0
        $source = Get-Content -LiteralPath $script:ScriptPath -Raw
        $source | Should -Not -Match 'Start-Process|Stop-Process|Invoke-WebRequest|TcpClient|mysql|Restart-Service'
        $source | Should -Match 'probe-rotation-lib\.ps1'
    }
}

