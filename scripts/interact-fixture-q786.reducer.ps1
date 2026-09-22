<#
    Pure reducer for the durable direct-GO receipt ledger emitted by questobjective/acceptance.
    It performs no I/O and issues no bridge requests, so Pester can exercise the live-proof contract
    without a server. The native ledger is cumulative; repeated snapshots are ignored after their
    highest already-consumed sequence.
#>

function Get-Q786ReceiptField {
    param(
        [Parameter(Mandatory = $true)][object]$Receipt,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $property = $Receipt.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        throw "q786 direct-GO receipt is missing required field '$Name'."
    }
    return $property.Value
}

function Get-Q786ExpectedEntryForSlot {
    param([Parameter(Mandatory = $true)][uint32]$Slot)

    switch ($Slot) {
        0 { return [uint32]3189 }
        1 { return [uint32]3190 }
        2 { return [uint32]3192 }
        default { throw "q786 direct-GO receipt has invalid objective slot $Slot." }
    }
}

function Get-Q786MaxDirectGoReceiptSequence {
    param([AllowNull()][object[]]$Receipts = @())

    $items = @($Receipts | Where-Object { $null -ne $_ })
    [uint64]$previous = 0
    $havePrevious = $false
    foreach ($receipt in $items) {
        [uint64]$sequence = Get-Q786ReceiptField -Receipt $receipt -Name 'sequence'
        if ($sequence -eq 0) { throw 'q786 direct-GO receipt sequence must be positive.' }
        if ($havePrevious -and $sequence -le $previous) {
            throw "Direct-GO receipt ledger is not strictly increasing: $sequence followed $previous."
        }
        $previous = $sequence
        $havePrevious = $true
    }
    return $previous
}

function New-Q786DirectGoProofState {
    param([uint64]$BaselineSequence = 0)

    $entries = [ordered]@{}
    foreach ($entry in @([uint32]3189, [uint32]3190)) {
        $entries[[string]$entry] = [pscustomobject][ordered]@{
            credited = $false
            sequence = [uint64]0
            slot = [uint32]0
            entry = $entry
            guid = [uint64]0
            before = [uint32]0
            after = [uint32]0
        }
    }

    return [pscustomobject][ordered]@{
        baseline_sequence = $BaselineSequence
        last_sequence = $BaselineSequence
        receipt_count = [uint32]0
        entries = $entries
    }
}

function Update-Q786DirectGoProof {
    param(
        [Parameter(Mandatory = $true)][object]$State,
        [AllowNull()][object[]]$Receipts = @()
    )

    $items = @($Receipts | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) {
        if ([uint64]$State.last_sequence -gt [uint64]$State.baseline_sequence) {
            throw "Direct-GO receipt ledger disappeared after sequence $($State.last_sequence)."
        }
        return
    }

    [uint64]$maxSequence = Get-Q786MaxDirectGoReceiptSequence -Receipts $items
    if ($maxSequence -lt [uint64]$State.last_sequence) {
        throw "Direct-GO receipt ledger regressed from $($State.last_sequence) to $maxSequence."
    }

    foreach ($receipt in $items) {
        [uint64]$sequence = Get-Q786ReceiptField -Receipt $receipt -Name 'sequence'
        if ($sequence -le [uint64]$State.last_sequence) { continue }

        [uint32]$questId = Get-Q786ReceiptField -Receipt $receipt -Name 'quest_id'
        if ($questId -ne 786) {
            $State.last_sequence = $sequence
            continue
        }

        [uint32]$slot = Get-Q786ReceiptField -Receipt $receipt -Name 'objective_slot'
        [uint32]$entry = Get-Q786ReceiptField -Receipt $receipt -Name 'entry'
        [uint64]$guid = Get-Q786ReceiptField -Receipt $receipt -Name 'guid'
        [uint32]$before = Get-Q786ReceiptField -Receipt $receipt -Name 'before'
        [uint32]$after = Get-Q786ReceiptField -Receipt $receipt -Name 'after'
        [uint32]$expectedEntry = Get-Q786ExpectedEntryForSlot -Slot $slot

        if ($entry -ne $expectedEntry) {
            throw "q786 direct-GO receipt slot $slot expected entry $expectedEntry, observed $entry."
        }
        if ($guid -eq 0) { throw "q786 direct-GO receipt for entry $entry has an empty GUID." }
        if ($before -gt 1 -or $after -gt 1 -or $after -lt $before) {
            throw "q786 direct-GO receipt for entry $entry has invalid counter transition $before->$after."
        }

        $key = [string]$entry
        if ($State.entries.Contains($key) -and $after -gt $before) {
            $proofEntry = $State.entries[$key]
            if ([bool]$proofEntry.credited) {
                throw "q786 direct-GO entry $entry emitted more than one successful credit receipt."
            }
            $proofEntry.credited = $true
            $proofEntry.sequence = $sequence
            $proofEntry.slot = $slot
            $proofEntry.guid = $guid
            $proofEntry.before = $before
            $proofEntry.after = $after
        }

        $State.receipt_count = [uint32]$State.receipt_count + 1
        $State.last_sequence = $sequence
    }
}

function Test-Q786DirectGoProofComplete {
    param([Parameter(Mandatory = $true)][object]$State)

    foreach ($entry in $State.entries.Values) {
        if (-not [bool]$entry.credited) { return $false }
    }
    return $true
}

function Get-Q786DirectGoProofView {
    param([Parameter(Mandatory = $true)][object]$State)

    $entries = [ordered]@{}
    foreach ($key in $State.entries.Keys) {
        $entry = $State.entries[$key]
        $entries[$key] = [pscustomobject][ordered]@{
            credited = [bool]$entry.credited
            sequence = [uint64]$entry.sequence
            slot = [uint32]$entry.slot
            entry = [uint32]$entry.entry
            guid = [uint64]$entry.guid
            before = [uint32]$entry.before
            after = [uint32]$entry.after
        }
    }

    return [pscustomobject][ordered]@{
        baseline_sequence = [uint64]$State.baseline_sequence
        last_sequence = [uint64]$State.last_sequence
        receipt_count = [uint32]$State.receipt_count
        complete = Test-Q786DirectGoProofComplete -State $State
        entries = $entries
    }
}
