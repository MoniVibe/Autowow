# AutoWoW Autopilot V1.4 - deployed-runtime manifest generator/verifier tests.
# Run: Invoke-Pester -Path .\autopilot\tests\manifest.tests.ps1 -Output Detailed
# Fully offline: -SkipWsl everywhere, sha overrides, TestDrive fake project with
# a runtime fixture bound to THIS process (real pid + real start time = a live,
# deterministic session identity). Zero bridge traffic, zero WSL calls.

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    $script:ProjectRoot = Split-Path -Parent $script:AutopilotRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotManifest.ps1')

    $script:T0 = [datetime]::new(2026, 7, 18, 21, 0, 0, [System.DateTimeKind]::Utc)
    $script:Sha = 'a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2'

    function New-FakeProject {
        # A TestDrive project whose runtime manifest points at this very process.
        param([string]$Name = ('proj-' + [guid]::NewGuid().ToString('n').Substring(0, 8)), [int64]$WrapperPid = $PID)
        $root = Join-Path $TestDrive $Name
        $runtimeDir = Join-Path $root 'work\phase1-wsl-runtime'
        $null = New-Item -ItemType Directory -Force -Path $runtimeDir
        $doc = [ordered]@{
            schema = 'autowow.phase1.wsl-processes.v1'
            wsl_wrapper_pid = $WrapperPid
            relay_pid = 0
            distro = 'Ubuntu-24.04'
            binary = '/root/p1core/build/src/server/apps/worldserver'
            config = '/root/p1runtime/worldserver.conf'
            started_utc = '2026-07-18T10:07:06Z'
        }
        Write-AutopilotFileAtomic -Path (Join-Path $runtimeDir 'runtime-processes.json') -Content ($doc | ConvertTo-Json)
        return $root
    }

    function New-Declarations {
        param([string]$Root)
        $path = Join-Path $Root 'declarations.json'
        $doc = [ordered]@{
            schema = 'autowow.autopilot.capability-declarations.v1'; schema_version = 1
            runtime_enabled = $true
            sql_read_deployed = $false
            operations = @(
                [ordered]@{ operation = 'quest_objective'; deployed = $true; native_adapter_available = $true; runtime_authorized = $true; receipt_export_available = $false; constraints = $null },
                [ordered]@{ operation = 'quest_acquire'; deployed = $true; native_adapter_available = $true; runtime_authorized = $true; receipt_export_available = $false; constraints = $null },
                [ordered]@{ operation = 'gather_source'; deployed = $true; native_adapter_available = $true; runtime_authorized = $true; receipt_export_available = $true; constraints = $null }
            )
        }
        Write-AutopilotFileAtomic -Path $path -Content ($doc | ConvertTo-Json -Depth 10)
        return $path
    }

    function New-Proofs {
        param([string]$Root, [array]$Entries)
        $path = Join-Path $Root 'proofs.json'
        $doc = [ordered]@{ schema = 'autowow.autopilot.capability-proofs.v1'; schema_version = 1; proofs = @($Entries) }
        Write-AutopilotFileAtomic -Path $path -Content ($doc | ConvertTo-Json -Depth 10)
        return $path
    }

    function New-PublicationManifest {
        # Standard happy-path publication into the fake project.
        param([string]$Root, [string]$ProofsPath, [datetime]$Now = $script:T0, [string]$ModuleFingerprintOverride = 'fp-test-0001')
        $outPath = Join-Path $Root 'staged-manifest.json'
        $generateParams = @{
            OutPath = $outPath; ProjectRoot = $Root
            DeclarationsPath = (New-Declarations -Root $Root)
            AsSolPublication = $true; SkipWsl = $true
            WorldserverSha256Override = $script:Sha
            ModuleFingerprintOverride = $ModuleFingerprintOverride
            Now = $Now
        }
        if ($ProofsPath) { $generateParams['ProofsPath'] = $ProofsPath }
        New-AutopilotDeployedManifest @generateParams
    }
}

Describe 'Manifest generator (downgrade-only, publication-gated)' {
    It 'produces interim-probe without -AsSolPublication even with full declarations' {
        $root = New-FakeProject
        $result = New-AutopilotDeployedManifest -OutPath (Join-Path $root 'm.json') -ProjectRoot $root `
            -DeclarationsPath (New-Declarations -Root $root) -SkipWsl -WorldserverSha256Override $script:Sha -Now $script:T0
        $result.Source | Should -Be 'interim-probe'
    }

    It 'refuses publication when bindings are missing, with explicit notes' {
        $root = New-FakeProject
        # No sha override + SkipWsl -> no binary hash -> publication refused.
        $result = New-AutopilotDeployedManifest -OutPath (Join-Path $root 'm.json') -ProjectRoot $root `
            -DeclarationsPath (New-Declarations -Root $root) -AsSolPublication -SkipWsl -Now $script:T0
        $result.Source | Should -Be 'interim-probe'
        (@($result.Manifest.notes) -join ';') | Should -Match 'publication refused: worldserver sha256 not established'
    }

    It 'binds sha, session identity, and module fingerprint on a publication manifest' {
        $root = New-FakeProject
        $result = New-PublicationManifest -Root $root
        $result.Source | Should -Be 'deployed-runtime'
        $manifest = $result.Manifest
        $manifest.worldserver_sha256 | Should -Be $script:Sha
        $manifest.server_build | Should -Be ('worldserver-sha256:' + $script:Sha.Substring(0, 12))
        $manifest.session_id | Should -Match ('^' + $PID + '@')
        $manifest.session_evidence.wrapper_pid | Should -Be $PID
        $manifest.module_fingerprint.fingerprint | Should -Be 'fp-test-0001'
        @($manifest.operations).Count | Should -Be 3
        # The provider parses the extended manifest and the live gate accepts it when fresh.
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $result.Path
        $provider.Origin | Should -Be 'deployed-runtime'
        (Test-AutopilotCapabilityProviderLiveEligible -Provider $provider -Now $script:T0.AddMinutes(30)).Eligible | Should -BeTrue
    }
}

Describe 'Proof grading (registry-only, quest family hard-gated)' {
    It 'defaults every operation to unproven without a registry' {
        $root = New-FakeProject
        $result = New-PublicationManifest -Root $root
        foreach ($operation in @($result.Manifest.operations)) {
            $operation.proof.status | Should -Be 'unproven'
        }
    }

    It 'caps quest acquisition at compiled-only without the walk-to-giver/accepted-into-log receipt' {
        $root = New-FakeProject
        $evidence = Join-Path $root 'evidence.jsonl'
        Write-AutopilotFileAtomic -Path $evidence -Content '{"some":"receipts"}'
        $proofs = New-Proofs -Root $root -Entries @(
            [ordered]@{ operation = 'quest_acquire'; status = 'proven-live'; evidence_kind = 'lab-fixture-proof'; evidence_path = 'evidence.jsonl'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') },
            [ordered]@{ operation = 'gather_source'; status = 'proven-live'; evidence_kind = 'gather-evidence'; evidence_path = 'evidence.jsonl'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') }
        )
        $result = New-PublicationManifest -Root $root -ProofsPath $proofs
        $questAcquire = @($result.Manifest.operations | Where-Object { $_.operation -eq 'quest_acquire' })[0]
        $questAcquire.proof.status | Should -Be 'compiled-only'
        $questAcquire.proof.note | Should -Match 'walk-to-giver'
        # A non-quest operation with existing, sufficient evidence stays proven-live.
        $gather = @($result.Manifest.operations | Where-Object { $_.operation -eq 'gather_source' })[0]
        $gather.proof.status | Should -Be 'proven-live'
    }

    It 'accepts a quest-family proven-live only with the live-walk kind, and downgrades missing evidence to unproven' {
        $root = New-FakeProject
        $evidence = Join-Path $root 'walk-accept.jsonl'
        Write-AutopilotFileAtomic -Path $evidence -Content '{"walk":"accept"}'
        $proofs = New-Proofs -Root $root -Entries @(
            [ordered]@{ operation = 'quest_acquire'; status = 'proven-live'; evidence_kind = 'live-walk-accept-proof'; evidence_path = 'walk-accept.jsonl'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') },
            [ordered]@{ operation = 'quest_objective'; status = 'proven-live'; evidence_kind = 'live-walk-accept-proof'; evidence_path = 'does-not-exist.jsonl'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') }
        )
        $result = New-PublicationManifest -Root $root -ProofsPath $proofs
        (@($result.Manifest.operations | Where-Object { $_.operation -eq 'quest_acquire' })[0]).proof.status | Should -Be 'proven-live'
        $missing = @($result.Manifest.operations | Where-Object { $_.operation -eq 'quest_objective' })[0]
        $missing.proof.status | Should -Be 'unproven'
        $missing.proof.note | Should -Match 'evidence file does not exist'
    }

    It 'never accepts build success, movement, xp, or ok:true responses as live proof' {
        $root = New-FakeProject
        $evidence = Join-Path $root 'oktrue.json'
        Write-AutopilotFileAtomic -Path $evidence -Content '{"ok":true}'
        $proofs = New-Proofs -Root $root -Entries @(
            [ordered]@{ operation = 'gather_source'; status = 'proven-live'; evidence_kind = 'ok-true-response'; evidence_path = 'oktrue.json'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') },
            [ordered]@{ operation = 'quest_objective'; status = 'proven-live'; evidence_kind = 'build-success'; evidence_path = 'oktrue.json'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') }
        )
        $result = New-PublicationManifest -Root $root -ProofsPath $proofs
        foreach ($name in @('gather_source', 'quest_objective')) {
            $operation = @($result.Manifest.operations | Where-Object { $_.operation -eq $name })[0]
            $operation.proof.status | Should -Be 'compiled-only'
            $operation.proof.note | Should -Match 'never substantiate proven-live|not live proof'
        }
    }

    It 'fails closed on unknown proof status strings (overclaims)' {
        $root = New-FakeProject
        $proofs = New-Proofs -Root $root -Entries @(
            [ordered]@{ operation = 'gather_source'; status = 'definitely-works'; evidence_kind = 'x'; evidence_path = 'x'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') }
        )
        $result = New-PublicationManifest -Root $root -ProofsPath $proofs
        $operation = @($result.Manifest.operations | Where-Object { $_.operation -eq 'gather_source' })[0]
        $operation.proof.status | Should -Be 'unproven'
        $operation.proof.note | Should -Match 'unknown proof status'
    }
}

Describe 'Manifest verifier (recomputes every binding)' {
    It 'validates a fresh publication whose session identity still matches' {
        $root = New-FakeProject
        $result = New-PublicationManifest -Root $root -Now (Get-Date).ToUniversalTime()
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $result.Path -ProjectRoot $root -SkipWsl
        $verdict.Failures | Should -BeNullOrEmpty
        $verdict.Valid | Should -BeTrue
        # Sha binding is unverified under -SkipWsl: a warning, never a silent pass.
        (@($verdict.Warnings) -join ';') | Should -Match 'binding unverified'
    }

    It 'fails an interim-probe manifest outright' {
        $root = New-FakeProject
        $interim = New-AutopilotDeployedManifest -OutPath (Join-Path $root 'interim.json') -ProjectRoot $root `
            -DeclarationsPath (New-Declarations -Root $root) -SkipWsl -WorldserverSha256Override $script:Sha -Now (Get-Date).ToUniversalTime()
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $interim.Path -ProjectRoot $root -SkipWsl
        $verdict.Valid | Should -BeFalse
        (@($verdict.Failures) -join ';') | Should -Match 'not a deployed-runtime publication'
    }

    It 'fails a stale publication' {
        $root = New-FakeProject
        $result = New-PublicationManifest -Root $root -Now (Get-Date).ToUniversalTime().AddHours(-9)
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $result.Path -ProjectRoot $root -SkipWsl
        $verdict.Valid | Should -BeFalse
        (@($verdict.Failures) -join ';') | Should -Match 'stale'
    }

    It 'fails when the server session changed since generation' {
        $root = New-FakeProject
        $result = New-PublicationManifest -Root $root -Now (Get-Date).ToUniversalTime()
        # Simulate a restart: the runtime manifest now names a dead wrapper pid.
        $deadProcess = Start-Process -FilePath 'pwsh' -ArgumentList '-NoProfile', '-Command', 'exit 0' -PassThru -WindowStyle Hidden
        $deadProcess.WaitForExit()
        $runtimeFile = Join-Path $root 'work\phase1-wsl-runtime\runtime-processes.json'
        $doc = Get-Content $runtimeFile -Raw | ConvertFrom-Json
        $doc.wsl_wrapper_pid = $deadProcess.Id
        Write-AutopilotFileAtomic -Path $runtimeFile -Content ($doc | ConvertTo-Json)
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $result.Path -ProjectRoot $root -SkipWsl
        $verdict.Valid | Should -BeFalse
        (@($verdict.Failures) -join ';') | Should -Match 'session identity mismatch'
    }

    It 'fails proof-audit violations inside an otherwise valid manifest (tampered kind and overclaimed status)' {
        $root = New-FakeProject
        $evidence = Join-Path $root 'walk-accept.jsonl'
        Write-AutopilotFileAtomic -Path $evidence -Content '{"walk":"accept"}'
        $proofs = New-Proofs -Root $root -Entries @(
            [ordered]@{ operation = 'quest_acquire'; status = 'proven-live'; evidence_kind = 'live-walk-accept-proof'; evidence_path = 'walk-accept.jsonl'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') },
            [ordered]@{ operation = 'gather_source'; status = 'proven-live'; evidence_kind = 'gather-evidence'; evidence_path = 'walk-accept.jsonl'; recorded_by = 'sol'; recorded_at = $script:T0.ToString('o') }
        )
        $result = New-PublicationManifest -Root $root -ProofsPath $proofs -Now (Get-Date).ToUniversalTime()
        # Tamper AFTER generation: wrong kind on the quest proof + an overclaimed status.
        $manifest = ConvertTo-AutopilotHashtable (Get-Content $result.Path -Raw | ConvertFrom-Json)
        foreach ($operation in @($manifest.operations)) {
            if ($operation.operation -eq 'quest_acquire') { $operation.proof.evidence_kind = 'lab-fixture-proof' }
            if ($operation.operation -eq 'gather_source') { $operation.proof.status = 'definitely-works' }
        }
        Write-AutopilotFileAtomic -Path $result.Path -Content ($manifest | ConvertTo-Json -Depth 12)
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $result.Path -ProjectRoot $root -SkipWsl
        $verdict.Valid | Should -BeFalse
        (@($verdict.Failures) -join ';') | Should -Match 'must remain compiled-only or unproven'
        (@($verdict.Failures) -join ';') | Should -Match 'unknown proof status'
    }

    It 'honors the embedded max_age_minutes without an explicit override' {
        $root = New-FakeProject
        $outPath = Join-Path $root 'shortlived.json'
        $result = New-AutopilotDeployedManifest -OutPath $outPath -ProjectRoot $root `
            -DeclarationsPath (New-Declarations -Root $root) -AsSolPublication -SkipWsl `
            -WorldserverSha256Override $script:Sha -ModuleFingerprintOverride 'fp-x' `
            -MaxAgeMinutes 30 -Now (Get-Date).ToUniversalTime().AddMinutes(-45)
        [int64]$result.Manifest.max_age_minutes | Should -Be 30
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $outPath -ProjectRoot $root -SkipWsl
        $verdict.Valid | Should -BeFalse
        (@($verdict.Failures) -join ';') | Should -Match 'stale.*max 30'
    }

    It 'binds and re-verifies a topology-manifest fingerprint, and fails on its drift' {
        $root = New-FakeProject
        $topology = Join-Path $root 'topology.json'
        Write-AutopilotFileAtomic -Path $topology -Content '{"staged":"tree","files":123}'
        $outPath = Join-Path $root 'topo-manifest.json'
        $result = New-AutopilotDeployedManifest -OutPath $outPath -ProjectRoot $root `
            -DeclarationsPath (New-Declarations -Root $root) -AsSolPublication -SkipWsl `
            -WorldserverSha256Override $script:Sha -TopologyManifestPath $topology -Now (Get-Date).ToUniversalTime()
        $result.Manifest.module_fingerprint.source | Should -Be 'topology-manifest'
        $result.Manifest.module_fingerprint.fingerprint | Should -Be ((Get-FileHash -LiteralPath $topology -Algorithm SHA256).Hash.ToLowerInvariant())
        (Test-AutopilotDeployedManifest -ManifestPath $outPath -ProjectRoot $root -SkipWsl).Valid | Should -BeTrue
        # Drift the topology manifest.
        Write-AutopilotFileAtomic -Path $topology -Content '{"staged":"tree","files":124}'
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $outPath -ProjectRoot $root -SkipWsl
        $verdict.Valid | Should -BeFalse
        (@($verdict.Failures) -join ';') | Should -Match 'topology-manifest hash drift'
    }

    It 'fails on module source drift (git-worktree fingerprint)' {
        $root = New-FakeProject
        $repo = Join-Path $TestDrive ('repo-' + [guid]::NewGuid().ToString('n').Substring(0, 6))
        $null = New-Item -ItemType Directory -Force -Path $repo
        & git -C $repo init --quiet
        & git -C $repo config user.email 'test@local'
        & git -C $repo config user.name 'test'
        Set-Content -Path (Join-Path $repo 'a.txt') -Value 'original'
        & git -C $repo add . 2>$null
        & git -C $repo commit --quiet -m 'init' 2>$null
        $outPath = Join-Path $root 'drift.json'
        $result = New-AutopilotDeployedManifest -OutPath $outPath -ProjectRoot $root `
            -DeclarationsPath (New-Declarations -Root $root) -AsSolPublication -SkipWsl `
            -WorldserverSha256Override $script:Sha -ModuleDir $repo -Now (Get-Date).ToUniversalTime()
        $result.Source | Should -Be 'deployed-runtime'
        # No drift yet: valid.
        (Test-AutopilotDeployedManifest -ManifestPath $outPath -ProjectRoot $root -ModuleDir $repo -SkipWsl).Valid | Should -BeTrue
        # Drift: edit the module source.
        Add-Content -Path (Join-Path $repo 'a.txt') -Value 'drifted'
        $verdict = Test-AutopilotDeployedManifest -ManifestPath $outPath -ProjectRoot $root -ModuleDir $repo -SkipWsl
        $verdict.Valid | Should -BeFalse
        (@($verdict.Failures) -join ';') | Should -Match 'fingerprint drift'
    }
}
