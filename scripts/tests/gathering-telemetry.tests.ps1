Describe 'gathering-telemetry.ps1' {
    BeforeAll {
        $telemetry = Join-Path $PSScriptRoot '..\gathering-telemetry.ps1'
        $script:BaselinePath = Join-Path $TestDrive 'gathering-baseline.json'
        $script:CurrentPath = Join-Path $TestDrive 'gathering-current.json'
        $script:WorldConfigPath = Join-Path $TestDrive 'worldserver.conf'
        $script:PlayerbotsConfigPath = Join-Path $TestDrive 'playerbots.conf'
        $script:FakeMysqlPath = Join-Path $TestDrive 'mysql-fake.ps1'

        @'
WorldDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_world"
CharacterDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_characters"
'@ | Set-Content -LiteralPath $script:WorldConfigPath -Encoding utf8
        @'
PlayerbotsDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_playerbots"
'@ | Set-Content -LiteralPath $script:PlayerbotsConfigPath -Encoding utf8
        @'
function Write-Row {
    param([string[]]$Values)
    Write-Output ($Values -join [char]9)
}
$execute = @($args | Where-Object { "$_" -like '--execute=*' } | Select-Object -First 1)
if ($execute.Count -eq 0) { exit 0 }
$sql = $execute[0].Substring('--execute='.Length)
if ($sql -match 'autowow_league_member') {
    Write-Row @('101','northstar','Alliance','worker','Herbalism','Alchemy','1','NorthstarWorker','1','11','5','1000','1')
} elseif ($sql -match 'character_skills') {
    Write-Row @('101','171','5','75')
    Write-Row @('101','182','5','75')
} elseif ($sql -match 'character_inventory') {
    Write-Row @('101','5001','2447','4','Peacebloom','20','1','7')
} elseif ($sql -match 'mail_items') {
} elseif ($sql -match 'FROM .*\.mail WHERE') {
    Write-Row @('101','9','100','0','0','0')
} elseif ($sql -match 'guild_member') {
    Write-Row @('55','101','Northstar Guild','1000')
} elseif ($sql -match 'guild_bank_item') {
    Write-Row @('55','0','0','6001','2835','2','Rough Stone','10','1','7')
} elseif ($sql -match 'guild_bank_eventlog') {
    Write-Row @('55','1','0','1','101','2447','4','0','1710000000')
} elseif ($sql -match 'auctionhouse') {
    Write-Row @('1','7001','101','500','1710000000','0','0','300','5','2447','5','Peacebloom','7','20','1')
}
exit 0
'@ | Set-Content -LiteralPath $script:FakeMysqlPath -Encoding utf8

        [ordered]@{
            schema_version = 'gathering-telemetry-v0'
            collected_utc = '2026-07-14T10:00:00Z'
            skills = @(
                [ordered]@{ guid = 101; skill_id = 182; name = 'Herbalism'; kind = 'primary'; value = 5; max = 75 }
            )
            materials = @(
                [ordered]@{ channel = 'character_storage'; team = 'northstar'; owner_guid = 101; guild_id = $null; item_entry = 2447; item_name = 'Peacebloom'; count = 10; buy_price = 20; sell_price = 1 }
                [ordered]@{ channel = 'guild_bank'; team = 'ember'; owner_guid = $null; guild_id = 55; item_entry = 2835; item_name = 'Rough Stone'; count = 8; buy_price = 10; sell_price = 1 }
            )
            money = @(
                [ordered]@{ kind = 'character'; team = 'northstar'; guid = 101; guild_id = $null; money_copper = 1000 }
            )
            auctions = @(
                [ordered]@{ auction_id = 1; item_entry = 2447; count = 5; buyout_copper = 500; last_bid_copper = 0; owner_guid = 101 }
                [ordered]@{ auction_id = 3; item_entry = 2835; count = 2; buyout_copper = 200; last_bid_copper = 0; owner_guid = 202 }
            )
            guild_bank_events = @(
                [ordered]@{ guild_id = 55; log_guid = 1; tab_id = 0; event_type = 1; event_name = 'deposit_item'; item_or_money = 2835; item_stack_count = 8 }
            )
        } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $script:BaselinePath -Encoding utf8

        [ordered]@{
            schema_version = 'gathering-telemetry-v0'
            collected_utc = '2026-07-14T10:05:00Z'
            skills = @(
                [ordered]@{ guid = 101; skill_id = 182; name = 'Herbalism'; kind = 'primary'; value = 10; max = 75 }
            )
            materials = @(
                [ordered]@{ channel = 'character_storage'; team = 'northstar'; owner_guid = 101; guild_id = $null; item_entry = 2447; item_name = 'Peacebloom'; count = 13; buy_price = 20; sell_price = 1 }
                [ordered]@{ channel = 'guild_bank'; team = 'ember'; owner_guid = $null; guild_id = 55; item_entry = 2835; item_name = 'Rough Stone'; count = 6; buy_price = 10; sell_price = 1 }
            )
            money = @(
                [ordered]@{ kind = 'character'; team = 'northstar'; guid = 101; guild_id = $null; money_copper = 1100 }
            )
            auctions = @(
                [ordered]@{ auction_id = 1; item_entry = 2447; count = 5; buyout_copper = 500; last_bid_copper = 100; owner_guid = 101 }
                [ordered]@{ auction_id = 2; item_entry = 2447; count = 3; buyout_copper = 300; last_bid_copper = 0; owner_guid = 101 }
            )
            guild_bank_events = @(
                [ordered]@{ guild_id = 55; log_guid = 1; tab_id = 0; event_type = 1; event_name = 'deposit_item'; item_or_money = 2835; item_stack_count = 8 }
                [ordered]@{ guild_id = 55; log_guid = 2; tab_id = 0; event_type = 2; event_name = 'withdraw_item'; item_or_money = 2835; item_stack_count = 2 }
            )
        } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $script:CurrentPath -Encoding utf8
    }

    It 'computes read-only skill, material, money, auction, and guild-bank deltas' {
        $delta = (& $telemetry -Action delta -BaselinePath $script:BaselinePath -CurrentPath $script:CurrentPath -AsJson | Out-String) | ConvertFrom-Json

        $skill = $delta.skill_deltas | Where-Object { $_.guid -eq 101 -and $_.skill_id -eq 182 }
        $skill.value_delta | Should -Be 5

        $herb = $delta.material_deltas | Where-Object { $_.item_entry -eq 2447 -and $_.channel -eq 'character_storage' }
        $herb.delta | Should -Be 3
        $guild = $delta.material_deltas | Where-Object { $_.item_entry -eq 2835 -and $_.channel -eq 'guild_bank' }
        $guild.delta | Should -Be -2

        $money = $delta.money_deltas | Where-Object { $_.guid -eq 101 }
        $money.delta_money_copper | Should -Be 100
        @($delta.auction_deltas | Where-Object status -eq 'added').Count | Should -Be 1
        @($delta.auction_deltas | Where-Object status -eq 'removed').Count | Should -Be 1
        @($delta.auction_deltas | Where-Object status -eq 'changed').Count | Should -Be 1
        @($delta.guild_bank_event_deltas).Count | Should -Be 1
        $delta.vendor_observability.status | Should -BeExactly 'inferred_only'
        $delta.safety.db_writes | Should -Be 0
    }

    It 'exposes only SELECT queries in the database contract' {
        $contract = (& $telemetry -Action sql -CharactersDatabaseName acore_characters -WorldDatabaseName acore_world -LeagueDatabaseName acore_playerbots -AsJson | Out-String) | ConvertFrom-Json

        $contract.mutating_keywords_detected | Should -BeFalse
        @($contract.queries).Count | Should -BeGreaterThan 5
        foreach ($query in @($contract.queries)) {
            $query.sql | Should -Match '^\s*SELECT\b'
            $query.sql | Should -Not -Match '\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE)\b'
            $query.read_only | Should -BeTrue
        }
    }

    It 'maps a fake read-only snapshot into skill, material, money, auction, and guild-event rows' {
        $snapshot = (& $telemetry -Action snapshot -ServerRoot $TestDrive -WorldServerConfigPath $script:WorldConfigPath -PlayerbotsConfigPath $script:PlayerbotsConfigPath -MySqlPath $script:FakeMysqlPath -AsJson | Out-String) | ConvertFrom-Json

        $snapshot.read_only | Should -BeTrue
        @($snapshot.members).Count | Should -Be 1
        $snapshot.members[0].profession_source | Should -BeExactly 'league_member_metadata'
        @($snapshot.skills | Where-Object { $_.name -eq 'Herbalism' -and $_.kind -eq 'primary' }).Count | Should -Be 1
        @($snapshot.materials | Where-Object { $_.channel -eq 'character_storage' -and $_.item_entry -eq 2447 }).Count | Should -Be 1
        @($snapshot.materials | Where-Object { $_.channel -eq 'guild_bank' -and $_.item_entry -eq 2835 }).Count | Should -Be 1
        @($snapshot.money | Where-Object { $_.kind -eq 'pending_mail' }).Count | Should -Be 1
        @($snapshot.auctions).Count | Should -Be 1
        @($snapshot.guild_bank_events | Where-Object event_name -eq 'deposit_item').Count | Should -Be 1
        $snapshot.vendor_observability.status | Should -BeExactly 'inferred_only'
        $snapshot.safety.db_writes | Should -Be 0
    }
}
