[CmdletBinding()]
param(
    [ValidateSet('snapshot', 'delta', 'sql')]
    [string]$Action = 'snapshot',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BaselinePath = '',
    [string]$CurrentPath = '',
    [string]$OutputPath = '',
    [string]$CharactersDatabaseName = 'acore_characters',
    [string]$WorldDatabaseName = 'acore_world',
    [string]$LeagueDatabaseName = 'acore_playerbots',
    [string]$WorldServerConfigPath = '',
    [string]$PlayerbotsConfigPath = '',
    [string]$MySqlPath = '',
    [switch]$AsJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:PrimarySkills = [ordered]@{
    '164' = 'Blacksmithing'
    '165' = 'Leatherworking'
    '171' = 'Alchemy'
    '182' = 'Herbalism'
    '186' = 'Mining'
    '197' = 'Tailoring'
    '202' = 'Engineering'
    '333' = 'Enchanting'
    '393' = 'Skinning'
    '755' = 'Jewelcrafting'
    '773' = 'Inscription'
}

$script:SecondarySkills = [ordered]@{
    '129' = 'First Aid'
    '185' = 'Cooking'
    '356' = 'Fishing'
}

$script:GuildBankEventTypes = @{
    '1' = 'deposit_item'
    '2' = 'withdraw_item'
    '3' = 'move_item'
    '4' = 'deposit_money'
    '5' = 'withdraw_money'
    '6' = 'repair_money'
    '7' = 'move_item2'
    '8' = 'unknown'
    '9' = 'buy_slot'
}

function Get-OptionalProperty {
    param(
        [AllowNull()]
        [object]$InputObject,
        [Parameter(Mandatory)]
        [string]$Name,
        [AllowNull()]
        [object]$Default = $null
    )

    if ($null -eq $InputObject) {
        return $Default
    }

    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return $Default
    }

    return $property.Value
}

function ConvertTo-Int64OrZero {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace("$Value")) {
        return [int64]0
    }

    $number = [int64]0
    if ([int64]::TryParse("$Value", [Globalization.NumberStyles]::Integer, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }

    return [int64]0
}

function ConvertTo-NullableInt64 {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace("$Value")) {
        return $null
    }

    $number = [int64]0
    if ([int64]::TryParse("$Value", [Globalization.NumberStyles]::Integer, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }

    return $null
}

function Assert-SafeDatabaseIdentifier {
    param([Parameter(Mandatory)][string]$Name)

    if ($Name -notmatch '^[A-Za-z0-9_]+$') {
        throw "Unsafe database identifier: $Name"
    }
}

function Assert-ReadOnlySql {
    param([Parameter(Mandatory)][string]$Sql)

    $trimmed = $Sql.Trim()
    if ($trimmed -notmatch '^(?i:SELECT)\b') {
        throw 'Gathering telemetry accepts SELECT-only SQL.'
    }

    if ($trimmed -match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE)\b') {
        throw 'Mutating SQL keyword rejected by gathering telemetry.'
    }
}

function Get-ConfigValue {
    param(
        [Parameter(Mandatory)][string]$ConfigText,
        [Parameter(Mandatory)][string[]]$Keys
    )

    foreach ($key in $Keys) {
        $pattern = '(?im)^\s*' + [regex]::Escape($key) + '\s*=\s*(?<value>[^#\r\n]+)'
        $match = [regex]::Match($ConfigText, $pattern)
        if ($match.Success) {
            return $match.Groups['value'].Value.Trim().Trim('"')
        }
    }

    return $null
}

function ConvertTo-ConnectionInfo {
    param(
        [Parameter(Mandatory)][string]$Info,
        [Parameter(Mandatory)][string]$DatabaseOverride
    )

    $parts = @($Info.Trim().Trim('"').Split(';'))
    if ($parts.Count -lt 5) {
        throw 'DatabaseInfo must contain host;port;user;password;database.'
    }

    $hostName = $parts[0].Trim()
    $port = ConvertTo-Int64OrZero $parts[1].Trim()
    if ($port -le 0 -or $port -gt 65535) {
        throw "Invalid database port in DatabaseInfo: $($parts[1])"
    }

    $user = $parts[2].Trim()
    $password = $parts[3]
    $database = if ([string]::IsNullOrWhiteSpace($DatabaseOverride)) { $parts[$parts.Count - 1].Trim() } else { $DatabaseOverride }
    Assert-SafeDatabaseIdentifier -Name $database

    return [pscustomobject]@{
        Host = $hostName
        Port = [int]$port
        User = $user
        Password = $password
        Database = $database
    }
}

function Get-MySqlPath {
    if (-not [string]::IsNullOrWhiteSpace($MySqlPath)) {
        if (-not (Test-Path -LiteralPath $MySqlPath -PathType Leaf)) {
            throw "Configured mysql client not found: $MySqlPath"
        }
        return $MySqlPath
    }

    $candidates = @(
        (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
        (Join-Path $ServerRoot 'server\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe',
        'C:\Program Files\MariaDB 11.0\bin\mysql.exe'
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    $command = Get-Command mysql.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw 'mysql.exe was not found. Use the existing AutoWoW MySQL installation or pass a PATH-visible client.'
}

function Get-DatabaseConnections {
    if ([string]::IsNullOrWhiteSpace($WorldServerConfigPath)) {
        $WorldServerConfigPath = Join-Path $ServerRoot 'server\configs\worldserver.conf'
    }
    if ([string]::IsNullOrWhiteSpace($PlayerbotsConfigPath)) {
        $PlayerbotsConfigPath = Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
    }
    if (-not (Test-Path -LiteralPath $WorldServerConfigPath -PathType Leaf)) {
        throw "Worldserver config not found: $WorldServerConfigPath"
    }

    $worldText = Get-Content -LiteralPath $WorldServerConfigPath -Raw
    $playerbotsText = if (Test-Path -LiteralPath $PlayerbotsConfigPath -PathType Leaf) { Get-Content -LiteralPath $PlayerbotsConfigPath -Raw } else { '' }

    $characterInfo = Get-ConfigValue -ConfigText $worldText -Keys @('CharacterDatabaseInfo', 'CharacterDatabaseInfo')
    $worldInfo = Get-ConfigValue -ConfigText $worldText -Keys @('WorldDatabaseInfo')
    $leagueInfo = Get-ConfigValue -ConfigText $playerbotsText -Keys @('PlayerbotsDatabaseInfo', 'PlayerbotDatabaseInfo', 'AiPlayerbot.DatabaseInfo', 'PlayerbotDatabase')
    if ([string]::IsNullOrWhiteSpace($leagueInfo)) {
        $leagueInfo = $worldInfo
    }
    if ([string]::IsNullOrWhiteSpace($characterInfo) -or [string]::IsNullOrWhiteSpace($worldInfo) -or [string]::IsNullOrWhiteSpace($leagueInfo)) {
        throw 'Could not find CharacterDatabaseInfo, WorldDatabaseInfo, and a Playerbots database connection in the existing configs.'
    }

    return [ordered]@{
        characters = ConvertTo-ConnectionInfo -Info $characterInfo -DatabaseOverride $CharactersDatabaseName
        world = ConvertTo-ConnectionInfo -Info $worldInfo -DatabaseOverride $WorldDatabaseName
        league = ConvertTo-ConnectionInfo -Info $leagueInfo -DatabaseOverride $LeagueDatabaseName
    }
}

function Get-TabularRows {
    param(
        [AllowNull()][string]$Text,
        [Parameter(Mandatory)][string[]]$Headers
    )

    $rows = [System.Collections.Generic.List[object]]::new()
    if ([string]::IsNullOrWhiteSpace($Text)) {
        return @($rows)
    }

    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line -match '^(?i:warning|mysql:)') {
            continue
        }

        $parts = $line.Split([char]9)
        $row = [ordered]@{}
        for ($index = 0; $index -lt $Headers.Count; $index++) {
            $row[$Headers[$index]] = if ($index -lt $parts.Count) { $parts[$index] } else { '' }
        }
        [void]$rows.Add([pscustomobject]$row)
    }

    return @($rows)
}

function Invoke-ReadOnlySql {
    param(
        [Parameter(Mandatory)][pscustomobject]$Connection,
        [Parameter(Mandatory)][string]$Sql,
        [Parameter(Mandatory)][string[]]$Headers,
        [Parameter(Mandatory)][string]$MysqlPath
    )

    Assert-ReadOnlySql -Sql $Sql
    $oldPassword = [Environment]::GetEnvironmentVariable('MYSQL_PWD', 'Process')
    $hadPassword = $null -ne $oldPassword
    try {
        $env:MYSQL_PWD = $Connection.Password
        $arguments = @(
            '--protocol=tcp',
            "--host=$($Connection.Host)",
            "--port=$($Connection.Port)",
            "--user=$($Connection.User)",
            "--database=$($Connection.Database)",
            '--default-character-set=utf8mb4',
            '--batch',
            '--skip-column-names',
            "--execute=$Sql"
        )
        $output = & $MysqlPath @arguments 2>&1 | ForEach-Object { "$($_)" } | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "Read-only SQL failed for database $($Connection.Database) with exit code $LASTEXITCODE."
        }
        return @(Get-TabularRows -Text $output -Headers $Headers)
    } finally {
        if ($hadPassword) {
            $env:MYSQL_PWD = $oldPassword
        } else {
            Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue
        }
    }
}

function New-IdList {
    param([AllowNull()][object[]]$Ids)

    $numbers = @($Ids | ForEach-Object { ConvertTo-NullableInt64 $_ } | Where-Object { $null -ne $_ } | Select-Object -Unique)
    if ($numbers.Count -eq 0) {
        return '0'
    }
    return ($numbers -join ',')
}

function New-ReadOnlyQuerySet {
    param(
        [Parameter(Mandatory)][string]$CharactersDb,
        [Parameter(Mandatory)][string]$WorldDb,
        [Parameter(Mandatory)][string]$LeagueDb,
        [AllowNull()][object[]]$MemberGuids,
        [AllowNull()][object[]]$GuildIds
    )

    Assert-SafeDatabaseIdentifier -Name $CharactersDb
    Assert-SafeDatabaseIdentifier -Name $WorldDb
    Assert-SafeDatabaseIdentifier -Name $LeagueDb
    $guidList = New-IdList -Ids $MemberGuids
    $guildList = New-IdList -Ids $GuildIds
    $skillList = (@($script:PrimarySkills.Keys + $script:SecondarySkills.Keys) | Sort-Object -Unique) -join ','

    return [ordered]@{
        members = [ordered]@{
            database = 'league'
            headers = @('guid', 'team', 'affiliation', 'role', 'profession_one', 'profession_two', 'active', 'name', 'race', 'class', 'level', 'money', 'online')
            sql = "SELECT m.character_guid AS guid, COALESCE(m.team_id,''), m.affiliation, m.role, m.profession_one, m.profession_two, m.active, COALESCE(c.name,''), COALESCE(c.race,0), COALESCE(c.class,0), COALESCE(c.level,0), COALESCE(c.money,0), COALESCE(c.online,0) FROM $LeagueDb.autowow_league_member m LEFT JOIN $CharactersDb.characters c ON c.guid = m.character_guid ORDER BY m.team_id, m.character_guid"
        }
        skills = [ordered]@{
            database = 'characters'
            headers = @('guid', 'skill_id', 'value', 'max_value')
            sql = "SELECT guid, skill, value, max FROM $CharactersDb.character_skills WHERE guid IN ($guidList) AND skill IN ($skillList) ORDER BY guid, skill"
        }
        character_materials = [ordered]@{
            database = 'characters'
            headers = @('guid', 'item_guid', 'item_entry', 'count', 'item_name', 'buy_price', 'sell_price', 'item_class')
            sql = "SELECT ci.guid, ci.item AS item_guid, ii.itemEntry AS item_entry, COALESCE(ii.count,1), COALESCE(it.name,''), COALESCE(it.BuyPrice,0), COALESCE(it.SellPrice,0), COALESCE(it.class,0) FROM $CharactersDb.character_inventory ci JOIN $CharactersDb.item_instance ii ON ii.guid = ci.item JOIN $WorldDb.item_template it ON it.entry = ii.itemEntry WHERE ci.guid IN ($guidList) AND it.class = 7 ORDER BY ci.guid, ii.itemEntry"
        }
        mail_materials = [ordered]@{
            database = 'characters'
            headers = @('guid', 'mail_id', 'item_guid', 'item_entry', 'count', 'item_name', 'buy_price', 'sell_price', 'item_class')
            sql = "SELECT mi.receiver AS guid, mi.mail_id, mi.item_guid, ii.itemEntry AS item_entry, COALESCE(ii.count,1), COALESCE(it.name,''), COALESCE(it.BuyPrice,0), COALESCE(it.SellPrice,0), COALESCE(it.class,0) FROM $CharactersDb.mail_items mi JOIN $CharactersDb.mail m ON m.id = mi.mail_id JOIN $CharactersDb.item_instance ii ON ii.guid = mi.item_guid JOIN $WorldDb.item_template it ON it.entry = ii.itemEntry WHERE mi.receiver IN ($guidList) AND it.class = 7 ORDER BY mi.receiver, ii.itemEntry"
        }
        mail_money = [ordered]@{
            database = 'characters'
            headers = @('guid', 'mail_id', 'money', 'cod', 'expire_time', 'deliver_time')
            sql = "SELECT receiver AS guid, id AS mail_id, COALESCE(money,0), COALESCE(cod,0), expire_time, deliver_time FROM $CharactersDb.mail WHERE receiver IN ($guidList) AND money > 0 ORDER BY receiver, id"
        }
        guild_memberships = [ordered]@{
            database = 'characters'
            headers = @('guild_id', 'guid', 'guild_name', 'bank_money')
            sql = "SELECT gm.guildid AS guild_id, gm.guid, COALESCE(g.name,''), COALESCE(g.BankMoney,0) FROM $CharactersDb.guild_member gm JOIN $CharactersDb.guild g ON g.guildid = gm.guildid WHERE gm.guid IN ($guidList) ORDER BY gm.guildid, gm.guid"
        }
        guild_materials = [ordered]@{
            database = 'characters'
            headers = @('guild_id', 'tab_id', 'slot_id', 'item_guid', 'item_entry', 'count', 'item_name', 'buy_price', 'sell_price', 'item_class')
            sql = "SELECT gbi.guildid AS guild_id, gbi.TabId AS tab_id, gbi.SlotId AS slot_id, gbi.item_guid, ii.itemEntry AS item_entry, COALESCE(ii.count,1), COALESCE(it.name,''), COALESCE(it.BuyPrice,0), COALESCE(it.SellPrice,0), COALESCE(it.class,0) FROM $CharactersDb.guild_bank_item gbi JOIN $CharactersDb.item_instance ii ON ii.guid = gbi.item_guid JOIN $WorldDb.item_template it ON it.entry = ii.itemEntry WHERE gbi.guildid IN ($guildList) AND it.class = 7 ORDER BY gbi.guildid, gbi.TabId, gbi.SlotId"
        }
        guild_bank_events = [ordered]@{
            database = 'characters'
            headers = @('guild_id', 'log_guid', 'tab_id', 'event_type', 'player_guid', 'item_or_money', 'item_stack_count', 'dest_tab_id', 'timestamp')
            sql = "SELECT guildid AS guild_id, LogGuid AS log_guid, TabId AS tab_id, EventType AS event_type, PlayerGuid AS player_guid, ItemOrMoney AS item_or_money, ItemStackCount AS item_stack_count, DestTabId AS dest_tab_id, TimeStamp AS timestamp FROM $CharactersDb.guild_bank_eventlog WHERE guildid IN ($guildList) ORDER BY guildid, LogGuid"
        }
        auctions = [ordered]@{
            database = 'characters'
            headers = @('auction_id', 'item_guid', 'item_owner', 'buyout_price', 'auction_time', 'bid_guid', 'last_bid', 'start_bid', 'deposit', 'item_entry', 'count', 'item_name', 'item_class', 'buy_price', 'sell_price')
            sql = "SELECT a.id AS auction_id, a.itemguid AS item_guid, a.itemowner AS item_owner, COALESCE(a.buyoutprice,0), a.time AS auction_time, COALESCE(a.buyguid,0), COALESCE(a.lastbid,0), COALESCE(a.startbid,0), COALESCE(a.deposit,0), ii.itemEntry AS item_entry, COALESCE(ii.count,1), COALESCE(it.name,''), COALESCE(it.class,0), COALESCE(it.BuyPrice,0), COALESCE(it.SellPrice,0) FROM $CharactersDb.auctionhouse a JOIN $CharactersDb.item_instance ii ON ii.guid = a.itemguid JOIN $WorldDb.item_template it ON it.entry = ii.itemEntry WHERE (a.itemowner IN ($guidList) OR a.buyguid IN ($guidList)) AND it.class = 7 ORDER BY a.id"
        }
    }
}

function Invoke-QuerySet {
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$QuerySet,
        [Parameter(Mandatory)][System.Collections.IDictionary]$Connections,
        [Parameter(Mandatory)][string]$MysqlPath
    )

    $result = [ordered]@{}
    foreach ($queryName in $QuerySet.Keys) {
        $query = $QuerySet[$queryName]
        $connection = $Connections[$query.database]
        $result[$queryName] = @(Invoke-ReadOnlySql -Connection $connection -Sql $query.sql -Headers $query.headers -MysqlPath $MysqlPath)
    }
    return $result
}

function Get-TeamForGuid {
    param([AllowNull()][object]$Guid, [hashtable]$TeamByGuid)
    if ($null -eq $Guid) { return $null }
    $key = "$Guid"
    if ($TeamByGuid.ContainsKey($key)) { return $TeamByGuid[$key] }
    return $null
}

function Get-TeamForGuild {
    param([AllowNull()][object]$GuildId, [hashtable]$TeamByGuild)
    if ($null -eq $GuildId) { return $null }
    $key = "$GuildId"
    if ($TeamByGuild.ContainsKey($key)) { return $TeamByGuild[$key] }
    return $null
}

function Convert-SkillRows {
    param([object[]]$Rows)

    $converted = [System.Collections.Generic.List[object]]::new()
    foreach ($row in @($Rows)) {
        $skillId = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'skill_id')
        $name = $null
        $kind = 'other'
        $skillKey = "$skillId"
        if ($script:PrimarySkills.Contains($skillKey)) {
            $name = $script:PrimarySkills[$skillKey]
            $kind = 'primary'
        } elseif ($script:SecondarySkills.Contains($skillKey)) {
            $name = $script:SecondarySkills[$skillKey]
            $kind = 'secondary'
        }
        if ($null -eq $name) { continue }
        [void]$converted.Add([ordered]@{
            guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guid')
            skill_id = [int]$skillId
            name = $name
            kind = $kind
            value = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'value')
            max = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'max_value' -Default (Get-OptionalProperty -InputObject $row -Name 'max'))
        })
    }
    return @($converted)
}

function New-MaterialRow {
    param(
        [Parameter(Mandatory)][string]$Channel,
        [AllowNull()][object]$Team,
        [AllowNull()][object]$OwnerGuid,
        [AllowNull()][object]$GuildId,
        [Parameter(Mandatory)][object]$Row
    )

    return [ordered]@{
        channel = $Channel
        team = if ($null -eq $Team) { $null } else { "$Team" }
        owner_guid = if ($null -eq $OwnerGuid) { $null } else { ConvertTo-Int64OrZero $OwnerGuid }
        guild_id = if ($null -eq $GuildId) { $null } else { ConvertTo-Int64OrZero $GuildId }
        item_guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Row -Name 'item_guid')
        item_entry = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Row -Name 'item_entry')
        item_name = "$((Get-OptionalProperty -InputObject $Row -Name 'item_name' -Default ''))"
        count = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Row -Name 'count')
        buy_price = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Row -Name 'buy_price')
        sell_price = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Row -Name 'sell_price')
        source_classification = 'trade_good_candidate'
    }
}

function Convert-UnixTimestamp {
    param([AllowNull()][object]$Value)
    $number = ConvertTo-NullableInt64 $Value
    if ($null -eq $number -or $number -le 0) { return $null }
    try { return [DateTimeOffset]::FromUnixTimeSeconds($number).UtcDateTime.ToString('o') } catch { return "$Value" }
}

function New-Snapshot {
    $connections = Get-DatabaseConnections
    $mysqlPath = Get-MySqlPath
    $initialQueries = New-ReadOnlyQuerySet -CharactersDb $CharactersDatabaseName -WorldDb $WorldDatabaseName -LeagueDb $LeagueDatabaseName -MemberGuids @(0) -GuildIds @(0)
    $memberRows = @(Invoke-ReadOnlySql -Connection $connections.league -Sql $initialQueries.members.sql -Headers $initialQueries.members.headers -MysqlPath $mysqlPath)
    $memberGuids = @($memberRows | ForEach-Object { ConvertTo-NullableInt64 (Get-OptionalProperty -InputObject $_ -Name 'guid') } | Where-Object { $null -ne $_ } | Select-Object -Unique)
    $querySet = New-ReadOnlyQuerySet -CharactersDb $CharactersDatabaseName -WorldDb $WorldDatabaseName -LeagueDb $LeagueDatabaseName -MemberGuids $memberGuids -GuildIds @(0)
    $skillRows = @(Invoke-ReadOnlySql -Connection $connections.characters -Sql $querySet.skills.sql -Headers $querySet.skills.headers -MysqlPath $mysqlPath)
    $membershipRows = @(Invoke-ReadOnlySql -Connection $connections.characters -Sql $querySet.guild_memberships.sql -Headers $querySet.guild_memberships.headers -MysqlPath $mysqlPath)
    $guildIds = @($membershipRows | ForEach-Object { ConvertTo-NullableInt64 (Get-OptionalProperty -InputObject $_ -Name 'guild_id') } | Where-Object { $null -ne $_ } | Select-Object -Unique)
    $querySet = New-ReadOnlyQuerySet -CharactersDb $CharactersDatabaseName -WorldDb $WorldDatabaseName -LeagueDb $LeagueDatabaseName -MemberGuids $memberGuids -GuildIds $guildIds
    $queryResults = Invoke-QuerySet -QuerySet $querySet -Connections $connections -MysqlPath $mysqlPath

    $teamByGuid = @{}
    $members = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $memberRows) {
        $guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guid')
        $team = "$((Get-OptionalProperty -InputObject $row -Name 'team' -Default ''))"
        if (-not [string]::IsNullOrWhiteSpace($team)) {
            $teamByGuid["$guid"] = $team
        }
        $declared = @(
            "$((Get-OptionalProperty -InputObject $row -Name 'profession_one' -Default ''))"
            "$((Get-OptionalProperty -InputObject $row -Name 'profession_two' -Default ''))"
        ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
        [void]$members.Add([ordered]@{
            guid = $guid
            team = $team
            affiliation = "$((Get-OptionalProperty -InputObject $row -Name 'affiliation' -Default ''))"
            role = "$((Get-OptionalProperty -InputObject $row -Name 'role' -Default ''))"
            name = "$((Get-OptionalProperty -InputObject $row -Name 'name' -Default ''))"
            level = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'level')
            money_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'money')
            online = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'online')
            active = "$((Get-OptionalProperty -InputObject $row -Name 'active' -Default ''))"
            declared_professions = @($declared)
            profession_source = 'league_member_metadata'
        })
    }

    $teamByGuild = @{}
    $guilds = [System.Collections.Generic.List[object]]::new()
    $seenGuilds = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($row in $membershipRows) {
        $guildId = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guild_id')
        $guildKey = "$guildId"
        $guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guid')
        $team = Get-TeamForGuid -Guid $guid -TeamByGuid $teamByGuid
        if ($null -ne $team -and -not $teamByGuild.ContainsKey($guildKey)) { $teamByGuild[$guildKey] = $team }
        if ($seenGuilds.Add($guildKey)) {
            [void]$guilds.Add([ordered]@{
                guild_id = $guildId
                team = $team
                name = "$((Get-OptionalProperty -InputObject $row -Name 'guild_name' -Default ''))"
                bank_money_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'bank_money')
            })
        }
    }

    $skills = Convert-SkillRows -Rows $skillRows
    $materials = [System.Collections.Generic.List[object]]::new()
    foreach ($row in @($queryResults.character_materials)) {
        $guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guid')
        [void]$materials.Add((New-MaterialRow -Channel 'character_storage' -Team (Get-TeamForGuid -Guid $guid -TeamByGuid $teamByGuid) -OwnerGuid $guid -GuildId $null -Row $row))
    }
    foreach ($row in @($queryResults.mail_materials)) {
        $guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guid')
        [void]$materials.Add((New-MaterialRow -Channel 'pending_mail' -Team (Get-TeamForGuid -Guid $guid -TeamByGuid $teamByGuid) -OwnerGuid $guid -GuildId $null -Row $row))
    }
    foreach ($row in @($queryResults.guild_materials)) {
        $guildId = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guild_id')
        [void]$materials.Add((New-MaterialRow -Channel 'guild_bank' -Team (Get-TeamForGuild -GuildId $guildId -TeamByGuild $teamByGuild) -OwnerGuid $null -GuildId $guildId -Row $row))
    }

    $money = [System.Collections.Generic.List[object]]::new()
    foreach ($member in $members) {
        [void]$money.Add([ordered]@{ kind = 'character'; team = $member.team; guid = $member.guid; guild_id = $null; money_copper = $member.money_copper })
    }
    foreach ($row in @($queryResults.mail_money)) {
        $guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guid')
        [void]$money.Add([ordered]@{ kind = 'pending_mail'; team = Get-TeamForGuid -Guid $guid -TeamByGuid $teamByGuid; guid = $guid; guild_id = $null; mail_id = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'mail_id'); money_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'money'); cod_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'cod') })
    }
    foreach ($guild in $guilds) {
        [void]$money.Add([ordered]@{ kind = 'guild_bank'; team = $guild.team; guid = $null; guild_id = $guild.guild_id; money_copper = $guild.bank_money_copper })
    }

    $auctions = [System.Collections.Generic.List[object]]::new()
    foreach ($row in @($queryResults.auctions)) {
        $owner = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'item_owner')
        $bidder = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'bid_guid')
        [void]$auctions.Add([ordered]@{
            auction_id = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'auction_id')
            item_guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'item_guid')
            item_entry = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'item_entry')
            item_name = "$((Get-OptionalProperty -InputObject $row -Name 'item_name' -Default ''))"
            count = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'count')
            owner_guid = $owner
            owner_team = Get-TeamForGuid -Guid $owner -TeamByGuid $teamByGuid
            bidder_guid = $bidder
            bidder_team = Get-TeamForGuid -Guid $bidder -TeamByGuid $teamByGuid
            buyout_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'buyout_price')
            last_bid_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'last_bid')
            start_bid_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'start_bid')
            deposit_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'deposit')
            auction_time = "$((Get-OptionalProperty -InputObject $row -Name 'auction_time' -Default ''))"
            source_classification = 'current_auction_state_only'
        })
    }

    $guildBankEvents = [System.Collections.Generic.List[object]]::new()
    foreach ($row in @($queryResults.guild_bank_events)) {
        $eventType = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'event_type')
        $guildId = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guild_id')
        [void]$guildBankEvents.Add([ordered]@{
            guild_id = $guildId
            team = Get-TeamForGuild -GuildId $guildId -TeamByGuild $teamByGuild
            log_guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'log_guid')
            tab_id = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'tab_id')
            event_type = $eventType
            event_name = if ($script:GuildBankEventTypes.ContainsKey("$eventType")) { $script:GuildBankEventTypes["$eventType"] } else { 'unknown' }
            player_guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'player_guid')
            item_or_money = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'item_or_money')
            item_stack_count = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'item_stack_count')
            dest_tab_id = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'dest_tab_id')
            timestamp = Convert-UnixTimestamp (Get-OptionalProperty -InputObject $row -Name 'timestamp')
        })
    }

    return [ordered]@{
        schema_version = 'gathering-telemetry-v0'
        mode = 'read_only_snapshot'
        collected_utc = [DateTime]::UtcNow.ToString('o')
        read_only = $true
        members = @($members)
        skills = @($skills)
        materials = @($materials)
        money = @($money)
        auctions = @($auctions)
        guilds = @($guilds)
        guild_bank_events = @($guildBankEvents)
        vendor_observability = [ordered]@{
            status = 'inferred_only'
            direct_transaction_journal = $false
            basis = @('item_template.BuyPrice', 'item_template.SellPrice', 'character money deltas', 'inventory/material deltas')
            warning = 'The characters database does not expose an exact vendor transaction journal; vendor flow attribution is never asserted from one snapshot.'
        }
        limitations = @(
            'character_skills proves ranks but not that a profession was learned through this lane',
            'auctionhouse is current-state exposure; completed settlement history is inferred from later money/mail/material deltas',
            'guild_bank_eventlog is read-only and bounded by the server retention window',
            'vendor transactions are not journaled in the current schema'
        )
        query_contract = @($querySet.Keys | ForEach-Object { [ordered]@{ name = $_; database = $querySet[$_].database; sql = $querySet[$_].sql; read_only = $true } })
        safety = [ordered]@{ db_writes = 0; character_state_writes = 0; live_activation = $false }
    }
}

function Read-JsonFile {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Telemetry JSON file not found: $Path"
    }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Get-ObjectArray {
    param([AllowNull()][object]$Object, [Parameter(Mandatory)][string]$PropertyName)
    $value = Get-OptionalProperty -InputObject $Object -Name $PropertyName -Default @()
    if ($null -eq $value) { return @() }
    return @($value)
}

function Get-MaterialMap {
    param([AllowNull()][object]$Snapshot)
    $map = @{}
    foreach ($row in @(Get-ObjectArray -Object $Snapshot -PropertyName 'materials')) {
        $key = '{0}|{1}|{2}|{3}|{4}' -f (Get-OptionalProperty -InputObject $row -Name 'channel' -Default ''), (Get-OptionalProperty -InputObject $row -Name 'team' -Default ''), (Get-OptionalProperty -InputObject $row -Name 'owner_guid' -Default ''), (Get-OptionalProperty -InputObject $row -Name 'guild_id' -Default ''), (Get-OptionalProperty -InputObject $row -Name 'item_entry' -Default 0)
        if (-not $map.ContainsKey($key)) {
            $map[$key] = [ordered]@{
                channel = Get-OptionalProperty -InputObject $row -Name 'channel' -Default ''
                team = Get-OptionalProperty -InputObject $row -Name 'team'
                owner_guid = Get-OptionalProperty -InputObject $row -Name 'owner_guid'
                guild_id = Get-OptionalProperty -InputObject $row -Name 'guild_id'
                item_entry = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'item_entry')
                item_name = Get-OptionalProperty -InputObject $row -Name 'item_name' -Default ''
                count = [int64]0
                buy_price = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'buy_price')
                sell_price = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'sell_price')
            }
        }
        $map[$key].count += ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'count')
    }
    return $map
}

function Get-SkillMap {
    param([AllowNull()][object]$Snapshot)
    $map = @{}
    foreach ($row in @(Get-ObjectArray -Object $Snapshot -PropertyName 'skills')) {
        $key = '{0}|{1}' -f (Get-OptionalProperty -InputObject $row -Name 'guid' -Default 0), (Get-OptionalProperty -InputObject $row -Name 'skill_id' -Default (Get-OptionalProperty -InputObject $row -Name 'skill' -Default 0))
        $map[$key] = [ordered]@{
            guid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'guid')
            skill_id = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'skill_id' -Default (Get-OptionalProperty -InputObject $row -Name 'skill'))
            name = Get-OptionalProperty -InputObject $row -Name 'name' -Default ''
            kind = Get-OptionalProperty -InputObject $row -Name 'kind' -Default ''
            value = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'value')
            max = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'max' -Default (Get-OptionalProperty -InputObject $row -Name 'max_value'))
        }
    }
    return $map
}

function Get-MoneyMap {
    param([AllowNull()][object]$Snapshot)
    $map = @{}
    foreach ($row in @(Get-ObjectArray -Object $Snapshot -PropertyName 'money')) {
        $key = '{0}|{1}|{2}|{3}' -f (Get-OptionalProperty -InputObject $row -Name 'kind' -Default ''), (Get-OptionalProperty -InputObject $row -Name 'team' -Default ''), (Get-OptionalProperty -InputObject $row -Name 'guid' -Default ''), (Get-OptionalProperty -InputObject $row -Name 'guild_id' -Default '')
        $map[$key] = [ordered]@{
            kind = Get-OptionalProperty -InputObject $row -Name 'kind' -Default ''
            team = Get-OptionalProperty -InputObject $row -Name 'team'
            guid = Get-OptionalProperty -InputObject $row -Name 'guid'
            guild_id = Get-OptionalProperty -InputObject $row -Name 'guild_id'
            money_copper = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $row -Name 'money_copper' -Default (Get-OptionalProperty -InputObject $row -Name 'money'))
        }
    }
    return $map
}

function Get-AuctionMap {
    param([AllowNull()][object]$Snapshot)
    $map = @{}
    foreach ($row in @(Get-ObjectArray -Object $Snapshot -PropertyName 'auctions')) {
        $key = "$(Get-OptionalProperty -InputObject $row -Name 'auction_id' -Default 0)"
        $map[$key] = $row
    }
    return $map
}

function Get-EventMap {
    param([AllowNull()][object]$Snapshot)
    $map = @{}
    foreach ($row in @(Get-ObjectArray -Object $Snapshot -PropertyName 'guild_bank_events')) {
        $key = '{0}|{1}|{2}' -f (Get-OptionalProperty -InputObject $row -Name 'guild_id' -Default 0), (Get-OptionalProperty -InputObject $row -Name 'log_guid' -Default 0), (Get-OptionalProperty -InputObject $row -Name 'tab_id' -Default 0)
        $map[$key] = $row
    }
    return $map
}

function Get-UnionKeys {
    param([hashtable]$Left, [hashtable]$Right)
    return @($Left.Keys + $Right.Keys | Select-Object -Unique)
}

function ConvertTo-ComparableJson {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return '' }
    return ($Value | ConvertTo-Json -Depth 10 -Compress)
}

function New-Delta {
    if ([string]::IsNullOrWhiteSpace($BaselinePath) -or [string]::IsNullOrWhiteSpace($CurrentPath)) {
        throw 'Delta action requires -BaselinePath and -CurrentPath.'
    }

    $baseline = Read-JsonFile -Path $BaselinePath
    $current = Read-JsonFile -Path $CurrentPath
    $baselineMaterials = Get-MaterialMap -Snapshot $baseline
    $currentMaterials = Get-MaterialMap -Snapshot $current
    $materialDeltas = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @(Get-UnionKeys -Left $baselineMaterials -Right $currentMaterials)) {
        $before = if ($baselineMaterials.ContainsKey($key)) { $baselineMaterials[$key] } else { $null }
        $after = if ($currentMaterials.ContainsKey($key)) { $currentMaterials[$key] } else { $null }
        $beforeCount = if ($null -eq $before) { [int64]0 } else { $before.count }
        $afterCount = if ($null -eq $after) { [int64]0 } else { $after.count }
        $delta = [int64]$afterCount - [int64]$beforeCount
        if ($delta -eq 0) { continue }
        $source = if ($null -ne $after) { $after } else { $before }
        [void]$materialDeltas.Add([ordered]@{
            key = $key
            channel = $source.channel
            team = $source.team
            owner_guid = $source.owner_guid
            guild_id = $source.guild_id
            item_entry = $source.item_entry
            item_name = $source.item_name
            before_count = $beforeCount
            after_count = $afterCount
            delta = $delta
            direction = if ($delta -gt 0) { 'gained' } else { 'spent_or_removed' }
            source_classification = 'trade_good_candidate'
            vendor_value_reference = [ordered]@{ buy_price = $source.buy_price; sell_price = $source.sell_price }
        })
    }

    $baselineSkills = Get-SkillMap -Snapshot $baseline
    $currentSkills = Get-SkillMap -Snapshot $current
    $skillDeltas = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @(Get-UnionKeys -Left $baselineSkills -Right $currentSkills)) {
        $before = if ($baselineSkills.ContainsKey($key)) { $baselineSkills[$key] } else { $null }
        $after = if ($currentSkills.ContainsKey($key)) { $currentSkills[$key] } else { $null }
        $beforeValue = if ($null -eq $before) { [int64]0 } else { $before.value }
        $afterValue = if ($null -eq $after) { [int64]0 } else { $after.value }
        $beforeMax = if ($null -eq $before) { [int64]0 } else { $before.max }
        $afterMax = if ($null -eq $after) { [int64]0 } else { $after.max }
        if ($beforeValue -eq $afterValue -and $beforeMax -eq $afterMax) { continue }
        $source = if ($null -ne $after) { $after } else { $before }
        [void]$skillDeltas.Add([ordered]@{
            key = $key
            guid = $source.guid
            skill_id = $source.skill_id
            name = $source.name
            kind = $source.kind
            before_value = $beforeValue
            after_value = $afterValue
            value_delta = [int64]$afterValue - [int64]$beforeValue
            before_max = $beforeMax
            after_max = $afterMax
            max_delta = [int64]$afterMax - [int64]$beforeMax
        })
    }

    $baselineMoney = Get-MoneyMap -Snapshot $baseline
    $currentMoney = Get-MoneyMap -Snapshot $current
    $moneyDeltas = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @(Get-UnionKeys -Left $baselineMoney -Right $currentMoney)) {
        $before = if ($baselineMoney.ContainsKey($key)) { $baselineMoney[$key] } else { $null }
        $after = if ($currentMoney.ContainsKey($key)) { $currentMoney[$key] } else { $null }
        $beforeMoney = if ($null -eq $before) { [int64]0 } else { $before.money_copper }
        $afterMoney = if ($null -eq $after) { [int64]0 } else { $after.money_copper }
        $delta = [int64]$afterMoney - [int64]$beforeMoney
        if ($delta -eq 0) { continue }
        $source = if ($null -ne $after) { $after } else { $before }
        [void]$moneyDeltas.Add([ordered]@{
            key = $key
            kind = $source.kind
            team = $source.team
            guid = $source.guid
            guild_id = $source.guild_id
            before_money_copper = $beforeMoney
            after_money_copper = $afterMoney
            delta_money_copper = $delta
        })
    }

    $baselineAuctions = Get-AuctionMap -Snapshot $baseline
    $currentAuctions = Get-AuctionMap -Snapshot $current
    $auctionDeltas = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @(Get-UnionKeys -Left $baselineAuctions -Right $currentAuctions)) {
        $before = if ($baselineAuctions.ContainsKey($key)) { $baselineAuctions[$key] } else { $null }
        $after = if ($currentAuctions.ContainsKey($key)) { $currentAuctions[$key] } else { $null }
        $status = if ($null -eq $before) { 'added' } elseif ($null -eq $after) { 'removed' } elseif ((ConvertTo-ComparableJson $before) -ne (ConvertTo-ComparableJson $after)) { 'changed' } else { 'unchanged' }
        if ($status -eq 'unchanged') { continue }
        [void]$auctionDeltas.Add([ordered]@{
            auction_id = ConvertTo-Int64OrZero $key
            status = $status
            before = $before
            after = $after
            settlement_inference = 'none_from_this_delta_alone'
        })
    }

    $baselineEvents = Get-EventMap -Snapshot $baseline
    $currentEvents = Get-EventMap -Snapshot $current
    $eventDeltas = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @(Get-UnionKeys -Left $baselineEvents -Right $currentEvents)) {
        if (-not $currentEvents.ContainsKey($key) -or $baselineEvents.ContainsKey($key)) { continue }
        [void]$eventDeltas.Add([ordered]@{ key = $key; status = 'new'; event = $currentEvents[$key] })
    }

    return [ordered]@{
        schema_version = 'gathering-telemetry-v0'
        mode = 'read_only_delta'
        read_only = $true
        baseline_collected_utc = Get-OptionalProperty -InputObject $baseline -Name 'collected_utc'
        current_collected_utc = Get-OptionalProperty -InputObject $current -Name 'collected_utc'
        material_deltas = @($materialDeltas)
        skill_deltas = @($skillDeltas)
        money_deltas = @($moneyDeltas)
        auction_deltas = @($auctionDeltas)
        guild_bank_event_deltas = @($eventDeltas)
        vendor_observability = [ordered]@{
            status = 'inferred_only'
            direct_transaction_journal = $false
            warning = 'Money and material deltas can suggest vendor activity, but cannot prove a vendor transaction without a transaction journal.'
        }
        summary = [ordered]@{
            material_rows = $materialDeltas.Count
            skill_rows = $skillDeltas.Count
            money_rows = $moneyDeltas.Count
            auction_rows = $auctionDeltas.Count
            new_guild_bank_events = $eventDeltas.Count
        }
        safety = [ordered]@{ db_writes = 0; character_state_writes = 0; live_activation = $false }
    }
}

function Complete-Result {
    param([Parameter(Mandatory)][object]$Result)

    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        $parent = Split-Path -Parent $OutputPath
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            [void](New-Item -ItemType Directory -Path $parent -Force)
        }
        $Result | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $OutputPath -Encoding utf8
    }

    if ($AsJson) {
        $Result | ConvertTo-Json -Depth 15 -Compress
        exit 0
    }

    $mode = Get-OptionalProperty -InputObject $Result -Name 'mode' -Default 'result'
    $materialCount = @(Get-ObjectArray -Object $Result -PropertyName 'material_deltas').Count
    $skillCount = @(Get-ObjectArray -Object $Result -PropertyName 'skill_deltas').Count
    $moneyCount = @(Get-ObjectArray -Object $Result -PropertyName 'money_deltas').Count
    $auctionCount = @(Get-ObjectArray -Object $Result -PropertyName 'auction_deltas').Count
    Write-Output ("Gathering telemetry {0}: read-only; material deltas {1}; skill deltas {2}; money deltas {3}; auction deltas {4}." -f $mode, $materialCount, $skillCount, $moneyCount, $auctionCount)
}

if ($Action -eq 'sql') {
    $querySet = New-ReadOnlyQuerySet -CharactersDb $CharactersDatabaseName -WorldDb $WorldDatabaseName -LeagueDb $LeagueDatabaseName -MemberGuids @(0) -GuildIds @(0)
    $queryContract = @($querySet.Keys | ForEach-Object { [ordered]@{ name = $_; database = $querySet[$_].database; sql = $querySet[$_].sql; read_only = $true } })
    $result = [ordered]@{
        schema_version = 'gathering-telemetry-v0'
        mode = 'read_only_sql_contract'
        read_only = $true
        mutating_keywords_detected = (@($queryContract | Where-Object { $_.sql -match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE)\b' }).Count -gt 0)
        queries = $queryContract
        safety = [ordered]@{ db_writes = 0; character_state_writes = 0; live_activation = $false }
    }
    Complete-Result -Result $result
    exit 0
}

if ($Action -eq 'delta') {
    Complete-Result -Result (New-Delta)
    exit 0
}

Complete-Result -Result (New-Snapshot)
