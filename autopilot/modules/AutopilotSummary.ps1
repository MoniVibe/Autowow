# AutoWoW Autopilot V1.2 - PROGRESS SUMMARY module (receipt-backed reporting).
# Dot-sourced AFTER AutopilotLib.ps1 into the same scope; composes its helpers
# (Get-AutopilotJobs, Get-AutopilotReceipts, ConvertTo-AutopilotHashtable,
# ConvertTo-AutopilotUtcDateTime, ...) and never redefines anything.
#
# Core rule: ONLY metrics backed by receipts or recorded observations are
# reported as facts. A metric with no evidence stream is unknown
# (value = $null, evidence = 'none', with a reason) - never guessed, never
# defaulted to a plausible value, never zero-as-fact. Nothing here implies
# live execution: sent vs would_send is always preserved.
#
# Fail closed: a malformed receipt ledger, or a receipt with an unknown
# schema/schema_version, is rejected with a typed reason and treated as
# unavailable evidence - never partially trusted.

Set-StrictMode -Version Latest

$script:AutopilotSummarySchema = 'autowow.autopilot.summary.v1'
$script:AutopilotSummarySchemaVersion = 1

function Get-AutopilotSummaryField {
    # Safe field access under StrictMode: absent or null fields return the
    # default. Returns the value plainly (arrays enumerate on output), so array
    # call sites MUST collect with @(...) - which every caller here does.
    param(
        [Parameter(Mandatory)][AllowNull()]$Table,
        [Parameter(Mandatory)][string]$Name,
        [AllowNull()]$Default = $null
    )
    $value = $null
    if ($Table -is [System.Collections.IDictionary]) {
        if ($Table.Contains($Name)) { $value = $Table[$Name] }
    }
    elseif ($Table -is [System.Management.Automation.PSCustomObject]) {
        $prop = $Table.PSObject.Properties[$Name]
        if ($prop) { $value = $prop.Value }
    }
    if ($null -eq $value) { $value = $Default }
    return $value
}

function New-AutopilotSummaryMetric {
    # Every metric carries its own evidence statement. evidence 'none' means
    # the value is unknown ($null), never zero-as-fact.
    param(
        [AllowNull()]$Value,
        [Parameter(Mandatory)][ValidateSet('receipts', 'none')][string]$Evidence,
        [string]$Note = ''
    )
    return [ordered]@{ value = $Value; evidence = $Evidence; note = $Note }
}

function Get-AutopilotSummaryReceiptSet {
    # Fail-closed receipt loader. A malformed JSONL ledger invalidates the whole
    # ledger with a typed reason (we cannot know which surviving lines to trust).
    # Individual receipts with an unknown schema or schema_version, or without a
    # parseable timestamp, are rejected and counted - never treated as evidence.
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId
    )
    $raw = @()
    try { $raw = @(Get-AutopilotReceipts -Paths $Paths -JobId $JobId) }
    catch {
        return [pscustomobject]@{
            Ok       = $false
            Reason   = "receipt ledger malformed (fail closed): $($_.Exception.Message)"
            Receipts = @()
            Rejected = 0
        }
    }
    $valid = @()
    $rejected = 0
    foreach ($record in $raw) {
        $r = ConvertTo-AutopilotHashtable $record
        if (-not ($r -is [System.Collections.IDictionary])) { $rejected++; continue }
        if (-not $r.Contains('schema') -or [string]$r.schema -ne $script:AutopilotReceiptSchema) { $rejected++; continue }
        $versionOk = $false
        if ($r.Contains('schema_version')) {
            try { $versionOk = ([int64]$r.schema_version -eq 1) } catch { $versionOk = $false }
        }
        if (-not $versionOk) { $rejected++; continue }
        if (-not $r.Contains('timestamp_utc') -or -not $r.Contains('event')) { $rejected++; continue }
        try { $null = ConvertTo-AutopilotUtcDateTime $r.timestamp_utc } catch { $rejected++; continue }
        $valid += , $r
    }
    return [pscustomobject]@{ Ok = $true; Reason = 'loaded'; Receipts = $valid; Rejected = $rejected }
}

function ConvertTo-AutopilotHistoryDetail {
    # Human-readable one-liner for a schema-validated receipt (ordered hashtable).
    # Wording preserves the honesty invariants: would-send is never rendered as
    # sent, and blocked states carry their typed code.
    param([Parameter(Mandatory)]$Receipt)
    $r = $Receipt
    $event = [string](Get-AutopilotSummaryField $r 'event' '?')
    switch ($event) {
        'job_created' {
            $kind = [string](Get-AutopilotSummaryField $r 'kind' '?')
            $mode = [string](Get-AutopilotSummaryField $r 'execution_mode' '?')
            $priority = [string](Get-AutopilotSummaryField $r 'priority' '?')
            return ('created: {0} job ({1}), priority {2}' -f $kind, $mode, $priority)
        }
        'job_transition' {
            $from = [string](Get-AutopilotSummaryField $r 'from' '?')
            $to = [string](Get-AutopilotSummaryField $r 'to' '?')
            $reason = [string](Get-AutopilotSummaryField $r 'reason' '')
            if ($to -eq 'Blocked') {
                $blocked = Get-AutopilotSummaryField $r 'blocked' $null
                $code = 'unknown-code'
                if ($blocked -is [System.Collections.IDictionary] -and $blocked.Contains('code')) { $code = [string]$blocked.code }
                return ('blocked: {0} ({1} -> Blocked)' -f $code, $from)
            }
            if ($reason) { return ('{0} -> {1} ({2})' -f $from, $to, $reason) }
            return ('{0} -> {1}' -f $from, $to)
        }
        'command' {
            $cmd = Get-AutopilotSummaryField $r 'command' $null
            $wire = [string](Get-AutopilotSummaryField $cmd 'wire' '?')
            $sent = [bool](Get-AutopilotSummaryField $cmd 'sent' $false)
            if ($sent) { return ('sent: {0}' -f $wire) }
            return ('would-send: {0}' -f $wire)
        }
        'command_deduplicated' {
            return ('deduplicated: {0}' -f [string](Get-AutopilotSummaryField $r 'wire' '?'))
        }
        'progress_observed' {
            $current = [string](Get-AutopilotSummaryField $r 'current' '?')
            $required = [string](Get-AutopilotSummaryField $r 'required' '?')
            $questId = [string](Get-AutopilotSummaryField $r 'quest_id' '?')
            $line = 'objective {0}/{1} quest {2}' -f $current, $required, $questId
            if ([bool](Get-AutopilotSummaryField $r 'rewarded' $false)) { $line += ' (rewarded)' }
            return $line
        }
        'ownership_acquired' {
            $guids = @(@(Get-AutopilotSummaryField $r 'guids' @()) | ForEach-Object { [string]$_ }) -join ','
            return ('ownership acquired: guid(s) {0} until {1}' -f $guids, [string](Get-AutopilotSummaryField $r 'expires_at' '?'))
        }
        'ownership_renewed' {
            $guids = @(@(Get-AutopilotSummaryField $r 'guids' @()) | ForEach-Object { [string]$_ }) -join ','
            return ('ownership renewed: guid(s) {0} until {1}' -f $guids, [string](Get-AutopilotSummaryField $r 'expires_at' '?'))
        }
        'ownership_released' {
            $guids = @(@(Get-AutopilotSummaryField $r 'guids' @()) | ForEach-Object { [string]$_ }) -join ','
            return ('ownership released: guid(s) {0} ({1})' -f $guids, [string](Get-AutopilotSummaryField $r 'reason' '?'))
        }
        'ownership_conflict' {
            return ('ownership conflict: guid {0} owned by {1} (job {2}) until {3}' -f
                [string](Get-AutopilotSummaryField $r 'guid' '?'),
                [string](Get-AutopilotSummaryField $r 'owner_instance' '?'),
                [string](Get-AutopilotSummaryField $r 'owner_job' '?'),
                [string](Get-AutopilotSummaryField $r 'owner_expires' '?'))
        }
        'plan_selected' {
            return ('plan selected: leader guid {0}' -f [string](Get-AutopilotSummaryField $r 'leader_guid' '?'))
        }
        'travel_delegated' {
            return ('travel delegated: {0}' -f [string](Get-AutopilotSummaryField $r 'reason' 'native execution resolves travel'))
        }
        'enrollment_proposed' {
            $caps = @(@(Get-AutopilotSummaryField $r 'required_capabilities' @()) | ForEach-Object { [string]$_ }) -join ','
            $guids = @(@(Get-AutopilotSummaryField $r 'guids' @()) | ForEach-Object { [string]$_ }) -join ','
            return ('enrollment proposed: {0} for guid(s) {1}' -f $caps, $guids)
        }
        'job_reconciled' {
            return ('reconciled: phase {0} (corrected={1})' -f
                [string](Get-AutopilotSummaryField $r 'phase' '?'),
                [string](Get-AutopilotSummaryField $r 'corrected_from_ledger' '?'))
        }
        'job_deduplicated' {
            return ('duplicate submission refused: {0}' -f [string](Get-AutopilotSummaryField $r 'duplicate_job_id' '?'))
        }
        default { return $event }
    }
}

function Get-AutopilotHistory {
    <#
    Chronological receipt-backed history for one character GUID across every job
    where the GUID appears in characters.assignedGuids (preferred evidence of
    actual involvement) or characters.eligibleGuids. Entries are sorted
    ascending by ConvertTo-AutopilotUtcDateTime (seq as a deterministic
    tie-breaker) and optionally filtered by -Since. An unreadable ledger is
    surfaced as a 'receipts_unavailable' entry - never silently dropped, never
    invented.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][int64]$Guid,
        [datetime]$Since
    )
    $sinceUtc = $null
    if ($PSBoundParameters.ContainsKey('Since')) { $sinceUtc = ConvertTo-AutopilotUtcDateTime $Since }
    $rows = [System.Collections.Generic.List[object]]::new()

    foreach ($job in @(Get-AutopilotJobs -Paths $Paths -IncludeTerminal)) {
        $characters = Get-AutopilotSummaryField $job 'characters' ([ordered]@{})
        $assigned = @(@(Get-AutopilotSummaryField $characters 'assignedGuids' @()) | ForEach-Object { [int64]$_ })
        $eligible = @(@(Get-AutopilotSummaryField $characters 'eligibleGuids' @()) | ForEach-Object { [int64]$_ })
        if (($assigned -notcontains $Guid) -and ($eligible -notcontains $Guid)) { continue }
        $jobId = [string]$job.jobId

        $set = Get-AutopilotSummaryReceiptSet -Paths $Paths -JobId $jobId
        if (-not $set.Ok) {
            # Honest failure: place the fail-closed marker at the job snapshot's
            # updatedAt (its only trustworthy timestamp); if even that is
            # unparseable the job cannot be placed on a timeline at all.
            $stampRaw = Get-AutopilotSummaryField $job 'updatedAt' $null
            $time = $null
            if ($null -ne $stampRaw) {
                try { $time = ConvertTo-AutopilotUtcDateTime $stampRaw } catch { $time = $null }
            }
            if ($null -eq $time) { continue }
            $rows.Add([pscustomobject]@{
                    Time  = $time; JobId = $jobId; Seq = [int64]::MaxValue
                    Entry = [ordered]@{
                        timestamp_utc = ConvertTo-AutopilotTimestamp -Time $time
                        jobId         = $jobId
                        event         = 'receipts_unavailable'
                        detail        = $set.Reason
                    }
                })
            continue
        }
        foreach ($r in $set.Receipts) {
            $time = ConvertTo-AutopilotUtcDateTime $r.timestamp_utc
            $rows.Add([pscustomobject]@{
                    Time  = $time
                    JobId = $jobId
                    Seq   = [int64](Get-AutopilotSummaryField $r 'seq' 0)
                    Entry = [ordered]@{
                        timestamp_utc = ConvertTo-AutopilotTimestamp -Time $time
                        jobId         = $jobId
                        event         = [string]$r.event
                        detail        = ConvertTo-AutopilotHistoryDetail -Receipt $r
                    }
                })
        }
    }

    $filtered = if ($null -ne $sinceUtc) { @($rows | Where-Object { $_.Time -ge $sinceUtc }) } else { @($rows) }
    $sorted = @($filtered | Sort-Object -Property Time, JobId, Seq)
    return , @($sorted | ForEach-Object { $_.Entry })
}

function Get-AutopilotSummaryData {
    <#
    Receipt-backed progress summary. Every metric is an ordered
    {value, evidence('receipts'|'none'), note}. Metrics without a deployed
    evidence stream (crafting, deaths, pvp, levels, ...) report value=$null
    with evidence='none' - unknown is unknown, never zero.

    -Since filters the counted receipt events (not the job snapshots and not
     blocked-duration intervals, which need their full entering/leaving pairs).
    -Campaign keeps only jobs whose owner.campaign matches; the controller
     ledger is controller-wide and is reported with that caveat.
    -Now anchors open Blocked intervals (defaults to the real UTC now).
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [datetime]$Since,
        [string]$Campaign,
        [datetime]$Now
    )
    $nowUtc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $sinceUtc = $null
    if ($PSBoundParameters.ContainsKey('Since')) { $sinceUtc = ConvertTo-AutopilotUtcDateTime $Since }

    $jobs = @(Get-AutopilotJobs -Paths $Paths -IncludeTerminal)
    if ($Campaign) {
        $jobs = @($jobs | Where-Object {
                $owner = Get-AutopilotSummaryField $_ 'owner' ([ordered]@{})
                [string](Get-AutopilotSummaryField $owner 'campaign' '') -eq $Campaign
            })
    }

    # Load every considered job's ledger, failing closed per ledger.
    $invalidLedgers = @()
    $rejectedReceipts = 0
    $ledgerReceipts = [ordered]@{}
    foreach ($job in $jobs) {
        $jobId = [string]$job.jobId
        $set = Get-AutopilotSummaryReceiptSet -Paths $Paths -JobId $jobId
        if (-not $set.Ok) {
            $invalidLedgers += , ([ordered]@{ jobId = $jobId; reason = $set.Reason })
            continue
        }
        $rejectedReceipts += [int]$set.Rejected
        $ledgerReceipts[$jobId] = @($set.Receipts)
    }
    $validLedgerCount = @($ledgerReceipts.Keys).Count
    # Receipt-derived counts are facts only while at least one considered ledger
    # is readable (or there are no jobs at all, where zero events is a fact of
    # the empty store). All-ledgers-invalid means those metrics are unknown.
    $receiptEvidence = ($jobs.Count -eq 0) -or ($validLedgerCount -gt 0)
    $caveat = ''
    if (@($invalidLedgers).Count -gt 0) { $caveat += ('; {0} job ledger(s) invalid (fail closed) and excluded' -f @($invalidLedgers).Count) }
    if ($rejectedReceipts -gt 0) { $caveat += ('; {0} receipt(s) with unknown schema rejected' -f $rejectedReceipts) }
    $sinceNote = if ($null -ne $sinceUtc) { '; since ' + (ConvertTo-AutopilotTimestamp -Time $sinceUtc) } else { '' }
    $noEvidenceReason = ('all {0} considered job ledger(s) are invalid (fail closed); counts unknown' -f $jobs.Count)

    # Flattened Since-filtered event stream for the count metrics.
    $countedReceipts = @()
    foreach ($jobId in @($ledgerReceipts.Keys)) {
        foreach ($r in $ledgerReceipts[$jobId]) {
            if ($null -ne $sinceUtc -and (ConvertTo-AutopilotUtcDateTime $r.timestamp_utc) -lt $sinceUtc) { continue }
            $countedReceipts += , $r
        }
    }

    # --- job-document metrics (ledger-backed snapshots) ---------------------
    $phaseMap = [ordered]@{}
    foreach ($phase in $script:AutopilotPhases) {
        $count = @($jobs | Where-Object { [string](Get-AutopilotSummaryField $_ 'phase' '') -eq $phase }).Count
        if ($count -gt 0) { $phaseMap[$phase] = $count }
    }
    $completedJobs = @($jobs | Where-Object { [string](Get-AutopilotSummaryField $_ 'phase' '') -eq 'Completed' })

    $metJobIds = @()
    foreach ($job in $completedJobs) {
        $criteria = Get-AutopilotSummaryField (Get-AutopilotSummaryField $job 'successCriteria' ([ordered]@{})) 'counters' ([ordered]@{})
        $progressCounters = Get-AutopilotSummaryField (Get-AutopilotSummaryField $job 'progress' ([ordered]@{})) 'counters' ([ordered]@{})
        if (-not ($criteria -is [System.Collections.IDictionary])) { continue }
        $keys = @($criteria.Keys)
        if ($keys.Count -eq 0) { continue }
        $allMet = $true
        foreach ($key in $keys) {
            $target = [int64]$criteria[$key]
            $achieved = [int64]0
            if ($progressCounters -is [System.Collections.IDictionary] -and $progressCounters.Contains($key)) { $achieved = [int64]$progressCounters[$key] }
            if ($achieved -lt $target) { $allMet = $false; break }
        }
        if ($allMet) { $metJobIds += [string]$job.jobId }
    }

    # --- receipt-count metrics ----------------------------------------------
    $turnIns = @($countedReceipts | Where-Object {
            [string]$_.event -eq 'progress_observed' -and [bool](Get-AutopilotSummaryField $_ 'rewarded' $false)
        }).Count
    $progressEvents = @($countedReceipts | Where-Object { [string]$_.event -eq 'progress_observed' }).Count

    $gatherReceipts = @($countedReceipts | Where-Object { [string]$_.event -eq 'progress_observed_gather' })
    $materialsGathered = if ($receiptEvidence -and $gatherReceipts.Count -gt 0) {
        $sum = [int64]0
        foreach ($g in $gatherReceipts) {
            foreach ($fieldName in @('count', 'gathered', 'current')) {
                $v = Get-AutopilotSummaryField $g $fieldName $null
                if ($null -ne $v) { $sum += [int64]$v; break }
            }
        }
        New-AutopilotSummaryMetric -Value $sum -Evidence 'receipts' -Note ('summed over {0} progress_observed_gather receipt(s){1}{2}' -f $gatherReceipts.Count, $sinceNote, $caveat)
    }
    else {
        New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no progress_observed_gather receipts recorded'
    }

    $deathEvents = @($countedReceipts | Where-Object { [string]$_.event -in @('death', 'death_observed') })
    $deaths = if ($receiptEvidence -and $deathEvents.Count -gt 0) {
        New-AutopilotSummaryMetric -Value $deathEvents.Count -Evidence 'receipts' -Note ('death events receipted{0}{1}' -f $sinceNote, $caveat)
    }
    else {
        New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no death events receipted'
    }
    $recoveryEvents = @($countedReceipts | Where-Object { [string]$_.event -in @('recovery', 'recovery_observed') })
    $recoveries = if ($receiptEvidence -and $recoveryEvents.Count -gt 0) {
        New-AutopilotSummaryMetric -Value $recoveryEvents.Count -Evidence 'receipts' -Note ('recovery events receipted{0}{1}' -f $sinceNote, $caveat)
    }
    else {
        New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no recovery events receipted'
    }

    # --- blocked durations (entering/leaving Blocked in the transition ledger)
    $blockedMetric = if (-not $receiptEvidence) {
        New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note $noEvidenceReason
    }
    else {
        $durations = [ordered]@{}
        foreach ($jobId in @($ledgerReceipts.Keys)) {
            $transitions = @($ledgerReceipts[$jobId] | Where-Object { [string]$_.event -eq 'job_transition' } |
                    Sort-Object -Property @{Expression = { [int64](Get-AutopilotSummaryField $_ 'seq' 0) } })
            $openCode = $null
            $openStart = $null
            foreach ($t in $transitions) {
                $to = [string](Get-AutopilotSummaryField $t 'to' '')
                $from = [string](Get-AutopilotSummaryField $t 'from' '')
                if ($to -eq 'Blocked') {
                    $blocked = Get-AutopilotSummaryField $t 'blocked' $null
                    $openCode = 'unknown-code'
                    if ($blocked -is [System.Collections.IDictionary] -and $blocked.Contains('code')) { $openCode = [string]$blocked.code }
                    $openStart = ConvertTo-AutopilotUtcDateTime $t.timestamp_utc
                }
                elseif ($from -eq 'Blocked' -and $null -ne $openStart) {
                    $end = ConvertTo-AutopilotUtcDateTime $t.timestamp_utc
                    $seconds = [math]::Max(0, ($end - $openStart).TotalSeconds)
                    $existing = if ($durations.Contains($openCode)) { [double]$durations[$openCode] } else { 0.0 }
                    $durations[$openCode] = $existing + $seconds
                    $openCode = $null
                    $openStart = $null
                }
            }
            if ($null -ne $openStart) {
                # Still Blocked at ledger end: accrue to the -Now anchor.
                $seconds = [math]::Max(0, ($nowUtc - $openStart).TotalSeconds)
                $existing = if ($durations.Contains($openCode)) { [double]$durations[$openCode] } else { 0.0 }
                $durations[$openCode] = $existing + $seconds
            }
        }
        foreach ($key in @($durations.Keys)) { $durations[$key] = [math]::Round([double]$durations[$key], 1) }
        New-AutopilotSummaryMetric -Value $durations -Evidence 'receipts' -Note ('total seconds per blocked code from job_transition receipts; open Blocked intervals accrue to {0}; Since not applied to durations{1}' -f (ConvertTo-AutopilotTimestamp -Time $nowUtc), $caveat)
    }

    # --- conflicts / commands / controller ----------------------------------
    $conflictsMetric = if ($receiptEvidence) {
        $conflictCount = @($countedReceipts | Where-Object { [string]$_.event -eq 'ownership_conflict' }).Count
        New-AutopilotSummaryMetric -Value $conflictCount -Evidence 'receipts' -Note ('ownership_conflict receipts across considered jobs{0}{1}' -f $sinceNote, $caveat)
    }
    else {
        New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note $noEvidenceReason
    }

    $commandsMetric = if ($receiptEvidence) {
        $commandReceipts = @($countedReceipts | Where-Object { [string]$_.event -eq 'command' })
        $sentCount = @($commandReceipts | Where-Object {
                $cmd = Get-AutopilotSummaryField $_ 'command' $null
                [bool](Get-AutopilotSummaryField $cmd 'sent' $false)
            }).Count
        $wouldSendCount = @($commandReceipts | Where-Object {
                $cmd = Get-AutopilotSummaryField $_ 'command' $null
                [bool](Get-AutopilotSummaryField $cmd 'would_send' $false)
            }).Count
        $dedupCount = @($countedReceipts | Where-Object { [string]$_.event -eq 'command_deduplicated' }).Count
        New-AutopilotSummaryMetric -Value ([ordered]@{
                total        = $commandReceipts.Count
                sent         = $sentCount
                wouldSend    = $wouldSendCount
                deduplicated = $dedupCount
            }) -Evidence 'receipts' -Note ('command receipts; would-send entries are dry-run rehearsals, never live execution{0}{1}' -f $sinceNote, $caveat)
    }
    else {
        New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note $noEvidenceReason
    }

    $controllerSet = Get-AutopilotSummaryReceiptSet -Paths $Paths -JobId 'controller'
    $controllerMetric = if ($controllerSet.Ok) {
        $controllerReceipts = @($controllerSet.Receipts)
        if ($null -ne $sinceUtc) {
            $controllerReceipts = @($controllerReceipts | Where-Object { (ConvertTo-AutopilotUtcDateTime $_.timestamp_utc) -ge $sinceUtc })
        }
        New-AutopilotSummaryMetric -Value ([ordered]@{
                starts             = @($controllerReceipts | Where-Object { [string]$_.event -eq 'controller_started' }).Count
                staleLockRecoveries = @($controllerReceipts | Where-Object { [string]$_.event -eq 'stale_lock_recovered' }).Count
                reconciliations    = @($controllerReceipts | Where-Object { [string]$_.event -eq 'reconciliation_completed' }).Count
            }) -Evidence 'receipts' -Note ('controller ledger is controller-wide; the campaign filter does not apply to it{0}' -f $sinceNote)
    }
    else {
        New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note $controllerSet.Reason
    }

    # --- assemble ------------------------------------------------------------
    return [ordered]@{
        schema           = $script:AutopilotSummarySchema
        schema_version   = $script:AutopilotSummarySchemaVersion
        generated_at_utc = ConvertTo-AutopilotTimestamp -Time $nowUtc
        filters          = [ordered]@{
            campaign = $(if ($Campaign) { $Campaign } else { $null })
            since    = $(if ($null -ne $sinceUtc) { ConvertTo-AutopilotTimestamp -Time $sinceUtc } else { $null })
        }
        jobsConsidered   = @($jobs | ForEach-Object { [string]$_.jobId })
        invalidLedgers   = @($invalidLedgers)

        jobsByPhase      = (New-AutopilotSummaryMetric -Value $phaseMap -Evidence 'receipts' -Note 'job documents (ledger-backed snapshots)')
        completedJobs    = (New-AutopilotSummaryMetric -Value $completedJobs.Count -Evidence 'receipts' -Note 'jobs in phase Completed')
        completedObjectives = (New-AutopilotSummaryMetric -Value @($metJobIds).Count -Evidence 'receipts' -Note $(if (@($metJobIds).Count -gt 0) { 'jobIds: ' + ($metJobIds -join ', ') } else { 'no Completed jobs with all successCriteria counters met' }))
        questTurnIns     = $(if ($receiptEvidence) {
                New-AutopilotSummaryMetric -Value $turnIns -Evidence 'receipts' -Note ('progress_observed receipts with rewarded=true{0}{1}' -f $sinceNote, $caveat)
            }
            else { New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note $noEvidenceReason })
        questObjectiveProgressEvents = $(if ($receiptEvidence) {
                New-AutopilotSummaryMetric -Value $progressEvents -Evidence 'receipts' -Note ('progress_observed receipts{0}{1}' -f $sinceNote, $caveat)
            }
            else { New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note $noEvidenceReason })
        materialsGathered = $materialsGathered
        itemsCrafted     = (New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no crafting receipt stream is deployed yet')
        bossAttempts     = (New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no encounter receipt stream is deployed yet')
        bossKills        = (New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no encounter receipt stream is deployed yet')
        deaths           = $deaths
        recoveries       = $recoveries
        pvpMatches       = (New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no pvp receipt stream is deployed yet')
        levelsGained     = (New-AutopilotSummaryMetric -Value $null -Evidence 'none' -Note 'no evidence stream: no level observations are receipted yet')
        blockedDurationsByCode = $blockedMetric
        ownershipConflicts = $conflictsMetric
        commands         = $commandsMetric
        controller       = $controllerMetric
    }
}

function Format-AutopilotSummary {
    <#
    Renders Get-AutopilotSummaryData output. -AsJson emits the document as
    JSON; otherwise a concise human text where every metric without an evidence
    stream renders as 'unknown (no evidence stream)' - never 0.
    #>
    param(
        [Parameter(Mandatory)]$Data,
        [switch]$AsJson
    )
    $data = ConvertTo-AutopilotHashtable $Data
    if ($AsJson) { return ($data | ConvertTo-Json -Depth 30) }

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('AutoWoW Autopilot progress summary (receipt-backed)')
    $lines.Add('generated: ' + [string](Get-AutopilotSummaryField $data 'generated_at_utc' 'unknown'))
    $filters = Get-AutopilotSummaryField $data 'filters' ([ordered]@{})
    $campaign = Get-AutopilotSummaryField $filters 'campaign' $null
    $since = Get-AutopilotSummaryField $filters 'since' $null
    $lines.Add(('filters: campaign={0}, since={1}' -f
            $(if ($null -ne $campaign) { [string]$campaign } else { '(all)' }),
            $(if ($null -ne $since) { [string]$since } else { '(none)' })))
    $jobsConsidered = @(Get-AutopilotSummaryField $data 'jobsConsidered' @())
    $lines.Add('jobs considered: ' + $jobsConsidered.Count)
    $invalid = @(Get-AutopilotSummaryField $data 'invalidLedgers' @())
    if ($invalid.Count -gt 0) {
        $parts = @($invalid | ForEach-Object {
                '{0}: {1}' -f [string](Get-AutopilotSummaryField $_ 'jobId' '?'), [string](Get-AutopilotSummaryField $_ 'reason' '?')
            })
        $lines.Add('invalid ledgers (fail closed, excluded): ' + ($parts -join ' | '))
    }

    $metricKeys = @(
        'jobsByPhase', 'completedJobs', 'completedObjectives', 'questTurnIns',
        'questObjectiveProgressEvents', 'materialsGathered', 'itemsCrafted',
        'bossAttempts', 'bossKills', 'deaths', 'recoveries', 'pvpMatches',
        'levelsGained', 'blockedDurationsByCode', 'ownershipConflicts',
        'commands', 'controller'
    )
    foreach ($key in $metricKeys) {
        if (-not ($data -is [System.Collections.IDictionary]) -or -not $data.Contains($key)) { continue }
        $metric = $data[$key]
        $value = Get-AutopilotSummaryField $metric 'value' $null
        $evidence = [string](Get-AutopilotSummaryField $metric 'evidence' 'none')
        $note = [string](Get-AutopilotSummaryField $metric 'note' '')
        if ($null -eq $value -or $evidence -eq 'none') {
            $line = '{0}: unknown (no evidence stream)' -f $key
            if ($note) { $line += ' - ' + $note }
            $lines.Add($line)
            continue
        }
        if ($value -is [System.Collections.IDictionary]) {
            $pairs = @(foreach ($k in $value.Keys) {
                    if ($key -eq 'blockedDurationsByCode') { '{0}={1}s' -f $k, $value[$k] }
                    else { '{0}={1}' -f $k, $value[$k] }
                })
            $rendered = if ($pairs.Count -gt 0) { $pairs -join ', ' } else { '(none)' }
            $lines.Add(('{0}: {1}' -f $key, $rendered))
        }
        else {
            $line = '{0}: {1}' -f $key, $value
            if ($key -eq 'completedObjectives' -and $note) { $line += ' (' + $note + ')' }
            $lines.Add($line)
        }
    }
    $lines.Add('note: sent=0 means no live command was issued; would-send entries are dry-run rehearsals, not live execution.')
    return ($lines -join [Environment]::NewLine)
}
