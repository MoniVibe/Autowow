Describe 'guild-economy-lib.ps1 native trade ledger' {
    BeforeAll { . (Join-Path $PSScriptRoot '..\guild-economy-lib.ps1') }
    BeforeEach {
        $script:l = New-GuildEconomyLedger @{
            101 = @{ Copper = 100; Items = @{ 42 = 10 } }
            202 = @{ Copper = 1000; Items = @{ 42 = 1 } }
        }
        Add-GuildEconomyContract $script:l c1 101 202 1001 2002 42 424242 4 300 | Out-Null
        $script:base = @{
            ProducerGuildId = 101
            ConsumerGuildId = 202
            ProducerTraderGuid = 1001
            ConsumerTraderGuid = 2002
            ItemId = 42
            ItemGuid = 424242
            Quantity = 4
            PriceCopper = 300
        }
        $script:trade = $script:base + @{
            ReceiptId = 'trade-1'
            EvidenceKind = 'native_trade_receipt'
            CompletionStatus = 'completed'
            ProducerCopperBefore = 100
            ProducerCopperAfter = 400
            ConsumerCopperBefore = 1000
            ConsumerCopperAfter = 700
            ProducerItemBefore = 10
            ProducerItemAfter = 6
            ConsumerItemBefore = 1
            ConsumerItemAfter = 5
        }
    }
    It 'reserves logically without moving assets then settles one atomic native trade' {
        Invoke-GuildEconomyEvent $l c1 reserved ($base + @{
            ReceiptId = 'reservation-1'
            EvidenceKind = 'reservation'
        }) | Out-Null
        $l.GuildCopper[202] | Should -Be 1000
        $l.GuildItems[101][42] | Should -Be 10
        Invoke-GuildEconomyEvent $l c1 settled $trade | Out-Null
        $l.Contracts.c1.State | Should -Be settled
        $l.GuildCopper[101] | Should -Be 400
        $l.GuildCopper[202] | Should -Be 700
        $l.GuildItems[101][42] | Should -Be 6
        $l.GuildItems[202][42] | Should -Be 5
        Assert-GuildEconomyConservation $l | Should -BeTrue
    }
    It 'makes exact receipt replay idempotent and rejects conflicting reuse' {
        Invoke-GuildEconomyEvent $l c1 reserved ($base + @{ ReceiptId = 'reservation-1' }) | Out-Null
        Invoke-GuildEconomyEvent $l c1 settled $trade | Out-Null
        (Invoke-GuildEconomyEvent $l c1 settled $trade).Duplicate | Should -BeTrue
        $conflict = $trade.Clone()
        $conflict.ProducerCopperAfter = 401
        { Invoke-GuildEconomyEvent $l c1 settled $conflict } | Should -Throw '*different evidence*'
        $l.Journal.Count | Should -Be 2
    }
    It 'rejects snapshots, incomplete trades, and unrelated deltas' {
        Invoke-GuildEconomyEvent $l c1 reserved ($base + @{ ReceiptId = 'reservation-1' }) | Out-Null
        $snapshot = $trade.Clone()
        $snapshot.ReceiptId = 's'
        $snapshot.EvidenceKind = 'snapshot'
        { Invoke-GuildEconomyEvent $l c1 settled $snapshot } | Should -Throw '*completed native trade receipt*'
        $incomplete = $trade.Clone()
        $incomplete.ReceiptId = 'i'
        $incomplete.CompletionStatus = 'pending'
        { Invoke-GuildEconomyEvent $l c1 settled $incomplete } | Should -Throw '*completed native trade receipt*'
        $delta = $trade.Clone()
        $delta.ReceiptId = 'd'
        $delta.ConsumerCopperAfter = 699
        { Invoke-GuildEconomyEvent $l c1 settled $delta } | Should -Throw '*exact atomic*'
    }
    It 'matches guilds, trader GUIDs, item GUID, and contract quantities' {
        Invoke-GuildEconomyEvent $l c1 reserved ($base + @{ ReceiptId = 'reservation-1' }) | Out-Null
        $identityFields = @(
            'ProducerGuildId',
            'ConsumerGuildId',
            'ProducerTraderGuid',
            'ConsumerTraderGuid',
            'ItemGuid',
            'Quantity'
        )
        foreach ($field in $identityFields) {
            $bad = $trade.Clone()
            $bad.ReceiptId = "bad-$field"
            $bad[$field] = [long]$bad[$field] + 1
            { Invoke-GuildEconomyEvent $l c1 settled $bad } | Should -Throw '*does not match*'
        }
    }
    It 'rejects insufficient funds and inventory before applying a receipt' {
        Invoke-GuildEconomyEvent $l c1 reserved ($base + @{ ReceiptId = 'reservation-1' }) | Out-Null
        $l.GuildCopper[202] = 200
        { Invoke-GuildEconomyEvent $l c1 settled $trade } | Should -Throw '*insufficient funds*'
        $l.GuildCopper[202] = 1000
        $l.GuildItems[101][42] = 2
        { Invoke-GuildEconomyEvent $l c1 settled $trade } | Should -Throw '*insufficient inventory*'
    }
    It 'fails or cancels without asset mutation and leaves terminal state' {
        Invoke-GuildEconomyEvent $l c1 reserved ($base + @{ ReceiptId = 'reservation-1' }) | Out-Null
        Invoke-GuildEconomyEvent $l c1 failed ($base + @{
            ReceiptId = 'failure-1'
            Reason = 'native_trade_failed'
        }) | Out-Null
        $l.GuildCopper[202] | Should -Be 1000
        $l.GuildItems[101][42] | Should -Be 10
        $l.Contracts.c1.State | Should -Be failed
        { Invoke-GuildEconomyEvent $l c1 settled $trade } | Should -Throw '*invalid*'
        $l2 = New-GuildEconomyLedger @{
            101 = @{ Copper = 100; Items = @{ 42 = 10 } }
            202 = @{ Copper = 1000; Items = @{ 42 = 1 } }
        }
        Add-GuildEconomyContract $l2 c2 101 202 1001 2002 42 424242 4 300 | Out-Null
        Invoke-GuildEconomyEvent $l2 c2 cancelled ($base + @{ ReceiptId = 'cancel-1' }) | Out-Null
        $l2.GuildCopper[202] | Should -Be 1000
        $l2.Contracts.c2.State | Should -Be cancelled
    }
    It 'detects direct conservation corruption' {
        $l.GuildCopper[101]++
        { Assert-GuildEconomyConservation $l } | Should -Throw '*Copper conservation failed*'
    }
}
