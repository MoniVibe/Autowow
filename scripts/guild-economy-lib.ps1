# In-memory attribution ledger over participating bot wallets and inventory.
# This is not a WoW guild bank or escrow: assets remain owned by characters
# until a completed native trade receipt proves settlement.

Set-StrictMode -Version Latest

function Get-GuildEconomyItemTotals {
    param(
        [Parameter(Mandatory)]
        $GuildItems
    )

    $totals = [System.Collections.Generic.Dictionary[long, long]]::new()
    foreach ($items in $GuildItems.Values) {
        foreach ($entry in $items.GetEnumerator()) {
            $itemId = [long]$entry.Key
            if (-not $totals.ContainsKey($itemId)) {
                $totals[$itemId] = 0L
            }
            $totals[$itemId] += [long]$entry.Value
        }
    }

    return $totals
}

function New-GuildEconomyLedger {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [hashtable]$Guilds
    )

    $balances = [System.Collections.Generic.Dictionary[long, long]]::new()
    $inventory = [System.Collections.Generic.Dictionary[long, object]]::new()

    foreach ($entry in $Guilds.GetEnumerator()) {
        $guildId = [long]$entry.Key
        if ($guildId -le 0) {
            throw 'Guild IDs must be positive database IDs.'
        }

        $copper = [long]$entry.Value.Copper
        if ($copper -lt 0) {
            throw "Guild $guildId has negative copper."
        }
        $balances[$guildId] = $copper

        $items = [System.Collections.Generic.Dictionary[long, long]]::new()
        if ($null -ne $entry.Value.Items) {
            foreach ($item in $entry.Value.Items.GetEnumerator()) {
                $quantity = [long]$item.Value
                if ($quantity -lt 0) {
                    throw "Guild $guildId item $($item.Key) has negative quantity."
                }
                $items[[long]$item.Key] = $quantity
            }
        }
        $inventory[$guildId] = $items
    }

    return [pscustomobject]@{
        GuildCopper = $balances
        GuildItems = $inventory
        Contracts = @{}
        Receipts = @{}
        Journal = [System.Collections.Generic.List[object]]::new()
        InitialCopper = [long](($balances.Values | Measure-Object -Sum).Sum)
        InitialItems = Get-GuildEconomyItemTotals -GuildItems $inventory
    }
}

function Add-GuildEconomyContract {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Ledger,
        [Parameter(Mandatory)] [string]$ContractId,
        [Parameter(Mandatory)] [long]$ProducerGuildId,
        [Parameter(Mandatory)] [long]$ConsumerGuildId,
        [Parameter(Mandatory)] [long]$ProducerTraderGuid,
        [Parameter(Mandatory)] [long]$ConsumerTraderGuid,
        [Parameter(Mandatory)] [long]$ItemId,
        [Parameter(Mandatory)] [long]$ItemGuid,
        [Parameter(Mandatory)] [long]$Quantity,
        [Parameter(Mandatory)] [long]$PriceCopper
    )

    if ($Ledger.Contracts.ContainsKey($ContractId)) {
        throw "Contract '$ContractId' already exists."
    }
    if ($ProducerGuildId -eq $ConsumerGuildId) {
        throw 'Producer and consumer guilds must differ.'
    }
    if (-not $Ledger.GuildCopper.ContainsKey($ProducerGuildId) -or
        -not $Ledger.GuildCopper.ContainsKey($ConsumerGuildId)) {
        throw 'Contracts may reference only guild IDs registered in the ledger.'
    }
    if ($ProducerTraderGuid -le 0 -or
        $ConsumerTraderGuid -le 0 -or
        $ProducerTraderGuid -eq $ConsumerTraderGuid) {
        throw 'Trader GUIDs must be distinct positive IDs.'
    }
    if ($ItemId -le 0 -or $ItemGuid -le 0 -or $Quantity -le 0 -or $PriceCopper -lt 0) {
        throw 'Item, item GUID, quantity, or price is invalid.'
    }

    $contract = [pscustomobject]@{
        ContractId = $ContractId
        ProducerGuildId = $ProducerGuildId
        ConsumerGuildId = $ConsumerGuildId
        ProducerTraderGuid = $ProducerTraderGuid
        ConsumerTraderGuid = $ConsumerTraderGuid
        ItemId = $ItemId
        ItemGuid = $ItemGuid
        Quantity = $Quantity
        PriceCopper = $PriceCopper
        State = 'offer'
    }
    $Ledger.Contracts[$ContractId] = $contract

    return $contract
}

function Get-GuildEconomyEvidenceSignature {
    param(
        [Parameter(Mandatory)]
        [hashtable]$Evidence
    )

    return (($Evidence.Keys | Sort-Object | ForEach-Object {
        "$_=$($Evidence[$_])"
    }) -join '|')
}

function Assert-GuildEconomyContractFields {
    param(
        [Parameter(Mandatory)] $Contract,
        [Parameter(Mandatory)] [hashtable]$Evidence
    )

    $contractFields = @(
        'ProducerGuildId',
        'ConsumerGuildId',
        'ProducerTraderGuid',
        'ConsumerTraderGuid',
        'ItemId',
        'ItemGuid',
        'Quantity',
        'PriceCopper'
    )
    foreach ($name in $contractFields) {
        if (-not $Evidence.ContainsKey($name)) {
            throw "Evidence is missing '$name'."
        }
        if ([long]$Evidence[$name] -ne [long]$Contract.$name) {
            throw "Evidence '$name' does not match the contract."
        }
    }
}

function Invoke-GuildEconomyEvent {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Ledger,
        [Parameter(Mandatory)] [string]$ContractId,
        [Parameter(Mandatory)]
        [ValidateSet('reserved', 'settled', 'failed', 'cancelled')]
        [string]$Event,
        [Parameter(Mandatory)] [hashtable]$Evidence
    )

    if (-not $Ledger.Contracts.ContainsKey($ContractId)) {
        throw "Unknown contract '$ContractId'."
    }
    if (-not $Evidence.ContainsKey('ReceiptId') -or
        [string]::IsNullOrWhiteSpace([string]$Evidence.ReceiptId)) {
        throw 'ReceiptId is required.'
    }

    $contract = $Ledger.Contracts[$ContractId]
    Assert-GuildEconomyContractFields -Contract $contract -Evidence $Evidence
    $receiptId = [string]$Evidence.ReceiptId
    $signature = Get-GuildEconomyEvidenceSignature -Evidence $Evidence

    if ($Ledger.Receipts.ContainsKey($receiptId)) {
        $prior = $Ledger.Receipts[$receiptId]
        if ($prior.ContractId -ne $ContractId -or
            $prior.Event -ne $Event -or
            $prior.EvidenceSignature -ne $signature) {
            throw "Receipt '$receiptId' was already used with different evidence."
        }
        return [pscustomobject]@{
            Applied = $false
            Duplicate = $true
            State = $contract.State
            ReceiptId = $receiptId
        }
    }

    $allowedTransitions = @{
        offer = @('reserved', 'failed', 'cancelled')
        reserved = @('settled', 'failed', 'cancelled')
    }
    if (-not $allowedTransitions.ContainsKey($contract.State) -or
        $Event -notin $allowedTransitions[$contract.State]) {
        throw "Transition '$($contract.State)' -> '$Event' is invalid."
    }

    $producerGuildId = [long]$contract.ProducerGuildId
    $consumerGuildId = [long]$contract.ConsumerGuildId
    $itemId = [long]$contract.ItemId
    $quantity = [long]$contract.Quantity
    $price = [long]$contract.PriceCopper
    $producerItems = if ($Ledger.GuildItems[$producerGuildId].ContainsKey($itemId)) {
        [long]$Ledger.GuildItems[$producerGuildId][$itemId]
    } else {
        0L
    }
    $consumerItems = if ($Ledger.GuildItems[$consumerGuildId].ContainsKey($itemId)) {
        [long]$Ledger.GuildItems[$consumerGuildId][$itemId]
    } else {
        0L
    }

    if ($Event -eq 'settled') {
        $receiptFields = @(
            'EvidenceKind',
            'CompletionStatus',
            'ProducerCopperBefore',
            'ProducerCopperAfter',
            'ConsumerCopperBefore',
            'ConsumerCopperAfter',
            'ProducerItemBefore',
            'ProducerItemAfter',
            'ConsumerItemBefore',
            'ConsumerItemAfter'
        )
        foreach ($name in $receiptFields) {
            if (-not $Evidence.ContainsKey($name)) {
                throw "Native trade receipt is missing '$name'."
            }
        }
        if ($Evidence.EvidenceKind -ne 'native_trade_receipt' -or
            $Evidence.CompletionStatus -ne 'completed') {
            throw 'Only a completed native trade receipt can settle a contract.'
        }
        if ($Ledger.GuildCopper[$consumerGuildId] -lt $price) {
            throw 'Consumer guild has insufficient funds.'
        }
        if ($producerItems -lt $quantity) {
            throw 'Producer guild has insufficient inventory.'
        }

        $validCopper = (
            [long]$Evidence.ProducerCopperBefore -eq $Ledger.GuildCopper[$producerGuildId] -and
            [long]$Evidence.ProducerCopperAfter -eq ($Ledger.GuildCopper[$producerGuildId] + $price) -and
            [long]$Evidence.ConsumerCopperBefore -eq $Ledger.GuildCopper[$consumerGuildId] -and
            [long]$Evidence.ConsumerCopperAfter -eq ($Ledger.GuildCopper[$consumerGuildId] - $price)
        )
        $validItems = (
            [long]$Evidence.ProducerItemBefore -eq $producerItems -and
            [long]$Evidence.ProducerItemAfter -eq ($producerItems - $quantity) -and
            [long]$Evidence.ConsumerItemBefore -eq $consumerItems -and
            [long]$Evidence.ConsumerItemAfter -eq ($consumerItems + $quantity)
        )
        if (-not $validCopper -or -not $validItems) {
            throw 'Receipt does not prove the exact atomic item and copper trade.'
        }

        $Ledger.GuildCopper[$producerGuildId] += $price
        $Ledger.GuildCopper[$consumerGuildId] -= $price
        $Ledger.GuildItems[$producerGuildId][$itemId] = $producerItems - $quantity
        $Ledger.GuildItems[$consumerGuildId][$itemId] = $consumerItems + $quantity
    } elseif ($Evidence.ContainsKey('EvidenceKind') -and
        $Evidence.EvidenceKind -in @('snapshot', 'bridge_ok', 'native_trade_receipt')) {
        throw "Evidence kind '$($Evidence.EvidenceKind)' cannot prove the '$Event' state decision."
    }

    $contract.State = $Event
    $record = [pscustomobject]@{
        ReceiptId = $receiptId
        ContractId = $ContractId
        Event = $Event
        EvidenceSignature = $signature
    }
    $Ledger.Receipts[$receiptId] = $record
    [void]$Ledger.Journal.Add($record)
    Assert-GuildEconomyConservation -Ledger $Ledger | Out-Null

    return [pscustomobject]@{
        Applied = $true
        Duplicate = $false
        State = $contract.State
        ReceiptId = $receiptId
    }
}

function Assert-GuildEconomyConservation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        $Ledger
    )

    $copper = [long](($Ledger.GuildCopper.Values | Measure-Object -Sum).Sum)
    if ($copper -ne [long]$Ledger.InitialCopper) {
        throw "Copper conservation failed: expected $($Ledger.InitialCopper), observed $copper."
    }

    $items = Get-GuildEconomyItemTotals -GuildItems $Ledger.GuildItems
    $itemIds = @($Ledger.InitialItems.Keys + $items.Keys | Select-Object -Unique)
    foreach ($itemId in $itemIds) {
        $before = if ($Ledger.InitialItems.ContainsKey($itemId)) {
            [long]$Ledger.InitialItems[$itemId]
        } else {
            0L
        }
        $after = if ($items.ContainsKey($itemId)) {
            [long]$items[$itemId]
        } else {
            0L
        }
        if ($before -ne $after) {
            throw "Item $itemId conservation failed: expected $before, observed $after."
        }
    }

    return $true
}
