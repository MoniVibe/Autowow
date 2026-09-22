Set-StrictMode -Version Latest

$script:ProbeRotationCatalogSchema = 'autowow.probe-rotation.catalog.v1'
$script:ProbeRotationPlanSchema = 'autowow.probe-rotation.plan.v1'

function Get-ProbeRotationProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function Resolve-ProbeRotationFile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$BaseDirectory,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Label path is empty." }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) {
        $Path
    } else {
        Join-Path -Path $BaseDirectory -ChildPath $Path
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Missing $Label`: $candidate"
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Read-ProbeRotationJsonFile {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Label)

    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    } catch {
        throw "$Label is not valid JSON: $Path ($($_.Exception.Message))"
    }
}

function ConvertTo-ProbeRotationStatus {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    switch (([string]$Value).Trim().ToUpperInvariant()) {
        'PASS' { return 'PASS' }
        'COMPLETED' { return 'PASS' }
        'COMPLETE' { return 'PASS' }
        'SUCCESS' { return 'PASS' }
        'SUCCEEDED' { return 'PASS' }
        'IN_FLIGHT' { return 'IN_FLIGHT' }
        'RUNNING' { return 'IN_FLIGHT' }
        'FAIL' { return 'FAIL' }
        'FAILED' { return 'FAIL' }
        'DEGRADED' { return 'FAIL' }
        'ERROR' { return 'FAIL' }
        default { return '' }
    }
}

function Get-ProbeRotationIdentityValue {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Explicit,
        [AllowNull()][object]$Summary,
        [AllowNull()][object]$Receipt,
        [Parameter(Mandatory)][string]$Name
    )

    $explicitValue = [string](Get-ProbeRotationProperty $Explicit $Name '')
    if (-not [string]::IsNullOrWhiteSpace($explicitValue)) { return $explicitValue }
    $summaryValue = [string](Get-ProbeRotationProperty $Summary $Name '')
    if (-not [string]::IsNullOrWhiteSpace($summaryValue)) { return $summaryValue }
    $receiptValue = [string](Get-ProbeRotationProperty $Receipt $Name '')
    if (-not [string]::IsNullOrWhiteSpace($receiptValue)) { return $receiptValue }
    return ''
}

function Get-ProbeRotationSummaryEvidence {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][psobject]$Candidate)

    $root = Read-ProbeRotationJsonFile -Path $Path -Label 'Probe summary'
    $probes = @(Get-ProbeRotationProperty $root probes @())
    $probeId = [string](Get-ProbeRotationProperty $Candidate probe_id (Get-ProbeRotationProperty $Candidate id ''))
    $matching = @($probes | Where-Object { [string](Get-ProbeRotationProperty $_ probe_id '') -ceq $probeId })
    if ($matching.Count -eq 0 -and $probes.Count -eq 1) { $matching = @($probes[0]) }
    if ($matching.Count -gt 1) { throw "Probe summary '$Path' has multiple records for '$probeId'." }
    $probe = if ($matching.Count -eq 1) { $matching[0] } else { $null }
    $statusValue = if ($null -ne $probe) {
        Get-ProbeRotationProperty $probe status (Get-ProbeRotationProperty $root status '')
    } else {
        Get-ProbeRotationProperty $root status ''
    }
    $status = ConvertTo-ProbeRotationStatus $statusValue
    if ([string]::IsNullOrWhiteSpace($status)) {
        throw "Probe summary '$Path' has no recognized PASS/FAIL status for '$probeId'."
    }
    $reason = if ($null -ne $probe) {
        [string](Get-ProbeRotationProperty $probe reason (Get-ProbeRotationProperty $root reason ''))
    } else {
        [string](Get-ProbeRotationProperty $root reason '')
    }
    [pscustomobject][ordered]@{
        source = 'summary'
        path = $Path
        status = $status
        reason = $reason
        observed_at_utc = [string](Get-ProbeRotationProperty $root completed_at_utc '')
        build_id = [string](Get-ProbeRotationProperty $root build_id '')
        fix_id = [string](Get-ProbeRotationProperty $root fix_id '')
    }
}

function Read-ProbeRotationReceiptRows {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace([string]$line)) { continue }
        try {
            $rows.Add(([string]$line | ConvertFrom-Json))
        } catch {
            throw "Probe receipt contains malformed JSON: $Path ($($_.Exception.Message))"
        }
    }
    if ($rows.Count -eq 0) { throw "Probe receipt is empty: $Path" }
    return @($rows)
}

function Get-ProbeRotationReceiptEvidence {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][psobject]$Candidate)

    $rows = @(Read-ProbeRotationReceiptRows -Path $Path)
    $terminal = @($rows | Where-Object { [string](Get-ProbeRotationProperty $_ event '') -in @('monitor_complete', 'probe_complete', 'run_complete') } | Select-Object -Last 1)
    $completedEncounter = @($rows | Where-Object { [string](Get-ProbeRotationProperty $_ event '') -eq 'completed_encounter' } | Select-Object -Last 1)
    $firstFailure = @($rows | Where-Object { [string](Get-ProbeRotationProperty $_ event '') -eq 'first_failure' } | Select-Object -Last 1)
    $started = @($rows | Where-Object { [string](Get-ProbeRotationProperty $_ event '') -in @('launch_started', 'monitor_started', 'monitor_sample', 'admission_sample') })

    $status = ''
    $reason = ''
    $observed = ''
    if ($terminal.Count -eq 1) {
        $status = ConvertTo-ProbeRotationStatus (Get-ProbeRotationProperty $terminal[0] status '')
        $reason = [string](Get-ProbeRotationProperty $terminal[0] reason '')
        $observed = [string](Get-ProbeRotationProperty $terminal[0] timestamp_utc '')
    } elseif ($completedEncounter.Count -gt 0) {
        $status = 'PASS'
        $reason = 'completed_encounter'
        $observed = [string](Get-ProbeRotationProperty $completedEncounter[0] timestamp_utc '')
    } elseif ($firstFailure.Count -gt 0) {
        $status = 'FAIL'
        $failure = Get-ProbeRotationProperty $firstFailure[0] failure $null
        $reason = [string](Get-ProbeRotationProperty $failure code (Get-ProbeRotationProperty $firstFailure[0] reason 'first_failure'))
        $observed = [string](Get-ProbeRotationProperty $firstFailure[0] timestamp_utc '')
    } elseif ($started.Count -gt 0) {
        $status = 'IN_FLIGHT'
        $reason = 'attempt_started_without_terminal_result'
        $observed = [string](Get-ProbeRotationProperty ($started | Select-Object -Last 1) timestamp_utc '')
    }
    if ([string]::IsNullOrWhiteSpace($status)) {
        throw "Probe receipt '$Path' has no terminal, failure, or in-flight event for '$([string](Get-ProbeRotationProperty $Candidate id ''))'."
    }
    $identityRow = if ($terminal.Count -eq 1) { $terminal[0] } elseif ($firstFailure.Count -gt 0) { $firstFailure[0] } else { $rows[0] }
    [pscustomobject][ordered]@{
        source = 'receipt'
        path = $Path
        status = $status
        reason = $reason
        observed_at_utc = $observed
        build_id = [string](Get-ProbeRotationProperty $identityRow build_id '')
        fix_id = [string](Get-ProbeRotationProperty $identityRow fix_id '')
    }
}

function Get-ProbeRotationHistoryEvidence {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$History, [Parameter(Mandatory)][psobject]$Candidate)

    $summary = $null
    $receipt = $null
    $summaryPath = [string](Get-ProbeRotationProperty $History summary_path '')
    $receiptPath = [string](Get-ProbeRotationProperty $History receipt_path '')
    if (-not [string]::IsNullOrWhiteSpace($summaryPath)) {
        $summary = Get-ProbeRotationSummaryEvidence -Path $summaryPath -Candidate $Candidate
    }
    if (-not [string]::IsNullOrWhiteSpace($receiptPath)) {
        $receipt = Get-ProbeRotationReceiptEvidence -Path $receiptPath -Candidate $Candidate
    }
    if ($null -eq $summary -and $null -eq $receipt) {
        throw "History record for '$([string](Get-ProbeRotationProperty $Candidate id ''))' must contain summary_path or receipt_path."
    }
    if ($null -ne $summary -and $null -ne $receipt -and $summary.status -ne $receipt.status) {
        throw "Summary/receipt status mismatch for '$([string](Get-ProbeRotationProperty $Candidate id ''))': $($summary.status) versus $($receipt.status)."
    }
    $status = if ($null -ne $summary) { $summary.status } else { $receipt.status }
    $reason = if ($null -ne $summary -and -not [string]::IsNullOrWhiteSpace($summary.reason)) { $summary.reason } else { $receipt.reason }
    $observed = if ($null -ne $summary -and -not [string]::IsNullOrWhiteSpace($summary.observed_at_utc)) { $summary.observed_at_utc } else { $receipt.observed_at_utc }
    [pscustomobject][ordered]@{
        status = $status
        reason = [string]$reason
        observed_at_utc = [string]$observed
        build_id = Get-ProbeRotationIdentityValue -Explicit $History -Summary $summary -Receipt $receipt -Name 'build_id'
        fix_id = Get-ProbeRotationIdentityValue -Explicit $History -Summary $summary -Receipt $receipt -Name 'fix_id'
        reset_id = [string](Get-ProbeRotationProperty $History reset_id '')
        summary_path = if ($null -ne $summary) { $summary.path } else { $null }
        receipt_path = if ($null -ne $receipt) { $receipt.path } else { $null }
    }
}

function Assert-ProbeRotationToken {
    param([Parameter(Mandatory)][string]$Value, [Parameter(Mandatory)][string]$Field)
    if ($Value -notmatch '^[A-Za-z][A-Za-z0-9._-]{1,63}$') {
        throw "$Field '$Value' must be a 2-64 character token without whitespace."
    }
}

function Read-ProbeRotationCatalog {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    $catalogPath = Resolve-ProbeRotationFile -BaseDirectory (Get-Location).Path -Path $Path -Label 'rotation catalog'
    $catalog = Read-ProbeRotationJsonFile -Path $catalogPath -Label 'Rotation catalog'
    if ([string](Get-ProbeRotationProperty $catalog schema '') -ne $script:ProbeRotationCatalogSchema) {
        throw "Rotation catalog schema must be '$script:ProbeRotationCatalogSchema'."
    }
    $candidates = @(Get-ProbeRotationProperty $catalog candidates @())
    if ($candidates.Count -eq 0) { throw 'Rotation catalog has no candidates.' }
    $baseDirectory = Split-Path -Parent $catalogPath
    $ids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $normalized = foreach ($candidate in $candidates) {
        $id = [string](Get-ProbeRotationProperty $candidate id '')
        Assert-ProbeRotationToken -Value $id -Field 'candidate.id'
        if (-not $ids.Add($id)) { throw "Duplicate rotation candidate id: $id" }
        $manifestPath = Resolve-ProbeRotationFile -BaseDirectory $baseDirectory -Path ([string](Get-ProbeRotationProperty $candidate manifest_path '')) -Label "manifest for '$id'"
        $mechanics = @(
            Get-ProbeRotationProperty $candidate mechanics @() |
            ForEach-Object { [string]$_ } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Sort-Object -Unique
        )
        if ($mechanics.Count -eq 0) { throw "Rotation candidate '$id' must declare at least one mechanics tag." }
        $history = @(Get-ProbeRotationProperty $candidate history @())
        $normalizedHistory = foreach ($record in $history) {
            $summaryPath = [string](Get-ProbeRotationProperty $record summary_path '')
            $receiptPath = [string](Get-ProbeRotationProperty $record receipt_path '')
            if ([string]::IsNullOrWhiteSpace($summaryPath) -and [string]::IsNullOrWhiteSpace($receiptPath)) {
                throw "History record for '$id' must reference a summary or receipt path."
            }
            [pscustomobject][ordered]@{
                summary_path = if ([string]::IsNullOrWhiteSpace($summaryPath)) { $null } else { Resolve-ProbeRotationFile -BaseDirectory $baseDirectory -Path $summaryPath -Label "summary for '$id'" }
                receipt_path = if ([string]::IsNullOrWhiteSpace($receiptPath)) { $null } else { Resolve-ProbeRotationFile -BaseDirectory $baseDirectory -Path $receiptPath -Label "receipt for '$id'" }
                build_id = [string](Get-ProbeRotationProperty $record build_id '')
                fix_id = [string](Get-ProbeRotationProperty $record fix_id '')
                reset_id = [string](Get-ProbeRotationProperty $record reset_id '')
            }
        }
        [pscustomobject][ordered]@{
            id = $id
            label = [string](Get-ProbeRotationProperty $candidate label $id)
            description = [string](Get-ProbeRotationProperty $candidate description '')
            kind = [string](Get-ProbeRotationProperty $candidate kind '')
            probe_id = [string](Get-ProbeRotationProperty $candidate probe_id $id)
            manifest_path = $manifestPath
            mechanics = @($mechanics)
            priority = [int](Get-ProbeRotationProperty $candidate priority 0)
            build_id = [string](Get-ProbeRotationProperty $candidate build_id '')
            fix_id = [string](Get-ProbeRotationProperty $candidate fix_id '')
            reset_id = [string](Get-ProbeRotationProperty $candidate reset_id '')
            history = @($normalizedHistory)
        }
    }
    [pscustomobject][ordered]@{
        schema = $script:ProbeRotationCatalogSchema
        catalog_path = $catalogPath
        catalog_directory = $baseDirectory
        candidates = @($normalized)
    }
}

function Get-ProbeRotationCurrentHistory {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Candidate)

    $currentReset = [string](Get-ProbeRotationProperty $Candidate reset_id '')
    $evidence = foreach ($record in @($Candidate.history)) {
        $recordReset = [string](Get-ProbeRotationProperty $record reset_id '')
        if ($recordReset -ne $currentReset) { continue }
        Get-ProbeRotationHistoryEvidence -History $record -Candidate $Candidate
    }
    return @($evidence)
}

function Get-ProbeRotationLatestEvidence {
    param([AllowEmptyCollection()][object[]]$Evidence = @())
    if ($Evidence.Count -eq 0) { return $null }
    $dated = @($Evidence | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_.observed_at_utc) })
    if ($dated.Count -gt 0) {
        return $dated | Sort-Object @{ Expression = { try { [datetime]$_.observed_at_utc } catch { [datetime]::MinValue } }; Descending = $true } | Select-Object -First 1
    }
    return $Evidence[-1]
}

function Test-ProbeRotationIdentityChanged {
    param([Parameter(Mandatory)][psobject]$Candidate, [Parameter(Mandatory)][psobject]$Evidence)
    $currentBuild = [string](Get-ProbeRotationProperty $Candidate build_id '')
    $currentFix = [string](Get-ProbeRotationProperty $Candidate fix_id '')
    $previousBuild = [string](Get-ProbeRotationProperty $Evidence build_id '')
    $previousFix = [string](Get-ProbeRotationProperty $Evidence fix_id '')
    if ([string]::IsNullOrWhiteSpace($currentBuild) -and [string]::IsNullOrWhiteSpace($currentFix)) { return $false }
    if ([string]::IsNullOrWhiteSpace($previousBuild) -and [string]::IsNullOrWhiteSpace($previousFix)) { return $false }
    return $currentBuild -ne $previousBuild -or $currentFix -ne $previousFix
}

function New-ProbeRotationCandidateState {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Candidate,
        [AllowEmptyCollection()][object[]]$Evidence = @(),
        [Parameter(Mandatory)][bool]$ResetApplied
    )

    $pass = @($Evidence | Where-Object status -eq 'PASS')
    $inFlight = @($Evidence | Where-Object status -eq 'IN_FLIGHT')
    $failures = @($Evidence | Where-Object status -eq 'FAIL')
    $latest = Get-ProbeRotationLatestEvidence -Evidence $Evidence
    $status = 'ELIGIBLE'
    $reason = if ($ResetApplied) { 'explicit_catalog_reset' } else { 'unseen_candidate' }
    $retest = $false

    if ($pass.Count -gt 0) {
        $status = 'RETIRED'
        $reason = 'pass_or_completed_history'
    } elseif ($inFlight.Count -gt 0 -and $failures.Count -eq 0) {
        $status = 'QUARANTINED'
        $reason = 'attempt_in_flight'
    } elseif ($null -ne $latest -and $latest.status -eq 'FAIL') {
        $status = 'QUARANTINED'
        $reason = 'unchanged_failure_quarantine'
        if (Test-ProbeRotationIdentityChanged -Candidate $Candidate -Evidence $latest) {
            $sameCurrentFailure = @($failures | Where-Object {
                [string]$_.build_id -eq [string]$Candidate.build_id -and [string]$_.fix_id -eq [string]$Candidate.fix_id
            })
            $sameCurrentInFlight = @($inFlight | Where-Object {
                [string]$_.build_id -eq [string]$Candidate.build_id -and [string]$_.fix_id -eq [string]$Candidate.fix_id
            })
            if ($sameCurrentFailure.Count -eq 0 -and $sameCurrentInFlight.Count -eq 0) {
                $status = 'RETEST'
                $reason = 'new_fix_or_build_after_failure'
                $retest = $true
            } elseif ($sameCurrentInFlight.Count -gt 0 -and $sameCurrentFailure.Count -eq 0) {
                $status = 'QUARANTINED'
                $reason = 'retest_already_consumed_in_flight'
            }
        }
    }

    [pscustomobject][ordered]@{
        id = [string]$Candidate.id
        label = [string]$Candidate.label
        description = [string]$Candidate.description
        kind = [string]$Candidate.kind
        probe_id = [string]$Candidate.probe_id
        manifest_path = [string]$Candidate.manifest_path
        mechanics = @($Candidate.mechanics)
        priority = [int]$Candidate.priority
        build_id = [string]$Candidate.build_id
        fix_id = [string]$Candidate.fix_id
        reset_id = [string]$Candidate.reset_id
        history_count = $Evidence.Count
        unseen = $Evidence.Count -eq 0
        status = $status
        reason = $reason
        retest = $retest
        latest_history = $latest
        eligible = $status -in @('ELIGIBLE', 'RETEST')
        novel_mechanics = @()
        novelty_count = 0
    }
}

function New-ProbeRotationPlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Catalog,
        [ValidateRange(1,64)][int]$MaxCandidates = 2
    )

    $states = foreach ($candidate in @($Catalog.candidates)) {
        $evidence = @(Get-ProbeRotationCurrentHistory -Candidate $candidate)
        $resetApplied = ([string](Get-ProbeRotationProperty $candidate reset_id '') -ne '') -and $evidence.Count -eq 0 -and @($candidate.history).Count -gt 0
        New-ProbeRotationCandidateState -Candidate $candidate -Evidence $evidence -ResetApplied $resetApplied
    }

    $seenMechanics = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($state in @($states)) {
        if ($state.history_count -gt 0) {
            foreach ($mechanic in @($state.mechanics)) { $null = $seenMechanics.Add([string]$mechanic) }
        }
    }
    foreach ($state in @($states)) {
        $novel = @($state.mechanics | Where-Object { -not $seenMechanics.Contains([string]$_) })
        $state.novel_mechanics = @($novel)
        $state.novelty_count = $novel.Count
    }

    $eligible = @($states | Where-Object eligible)
    $ordered = @($eligible | Sort-Object `
        @{ Expression = { if ($_.unseen) { 0 } else { 1 } } }, `
        @{ Expression = { -[int]$_.novelty_count } }, `
        @{ Expression = { if ($_.status -eq 'ELIGIBLE') { 0 } else { 1 } } }, `
        @{ Expression = { -[int]$_.priority } }, `
        @{ Expression = { [string]$_.id } })
    $selected = @($ordered | Select-Object -First $MaxCandidates)

    [pscustomobject][ordered]@{
        schema = $script:ProbeRotationPlanSchema
        schema_version = 1
        mode = 'Plan'
        dry_run = $true
        generated_at_utc = [datetime]::UtcNow.ToString('o')
        catalog_path = [string]$Catalog.catalog_path
        selection = [ordered]@{
            max_candidates = $MaxCandidates
            candidate_ids = @($selected | ForEach-Object { [string]$_.id })
            rule = 'unseen_then_novel_mechanics_then_status_then_priority_then_id'
        }
        candidates = @($states)
        next_candidates = @($selected)
        safety = [ordered]@{
            bridge_calls = 0
            process_launches = 0
            database_writes = 0
            teleports = 0
            runtime_mutations = 0
        }
    }
}
