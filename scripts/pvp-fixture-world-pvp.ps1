[CmdletBinding()]
param(
    [ValidateSet('plan', 'provision', 'rally', 'start', 'stop', 'run', 'status', 'cleanup')]
    [string]$Action = 'plan',

    [ValidateSet('world-pvp-2v2', 'ladder-3v3', 'wsg-10v10')]
    [string]$Stage = 'world-pvp-2v2',

    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),

    [ValidatePattern('^[A-Za-z][A-Za-z0-9]{2,7}$')]
    [string]$FixtureId = 'awpvp1',

    [ValidateRange(1, 80)]
    [int]$Level = 80,

    [ValidateRange(1000, 60000)]
    [int]$BridgeTimeoutMs = 5000,

    [ValidateRange(5, 600)]
    [int]$WaitSeconds = 120,

    [ValidateRange(1, 20)]
    [int]$SampleCount = 3,

    [ValidateRange(0, 60)]
    [int]$SampleIntervalSeconds = 2,

    [string]$ReceiptPath = '',

    [switch]$Execute,

    [switch]$ConfirmFixtureCleanup
)

Set-StrictMode -Version Latest

$commonPath = Join-Path $PSScriptRoot 'Common.ps1'
$libraryPath = Join-Path $PSScriptRoot 'pvp-fixture-lib.ps1'
if (-not (Test-Path -LiteralPath $commonPath)) { throw "Missing AutoWoW common helper: $commonPath" }
if (-not (Test-Path -LiteralPath $libraryPath)) { throw "Missing PvP fixture library: $libraryPath" }
. $commonPath
. $libraryPath

$script:PvpFixtureMetrics = [ordered]@{
    bridge_commands = 0
    console_commands = 0
    db_read_statements = 0
    db_write_statements = 0
    db_rows_inserted = 0
    db_rows_updated = 0
    db_rows_deleted = 0
    unrelated_mutation_count = 0
    unsupported_stage_count = 0
}
$script:PvpFixtureReceiptState = $null
$script:PvpFixtureContext = $null

function Get-PvpFixtureFullPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $script:PvpFixtureContext.ServerRoot $Path))
}

function Assert-PvpFixtureReceiptPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $logsRoot = [System.IO.Path]::GetFullPath($script:PvpFixtureContext.LogsRoot).TrimEnd('\', '/')
    $prefix = $logsRoot + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Receipt path must stay under AutoWoW logs: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        throw "Refusing to overwrite an existing receipt: $fullPath"
    }
    return $fullPath
}

function Write-PvpFixtureReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Event,

        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Payload
    )

    if ($null -eq $script:PvpFixtureReceiptState) {
        throw 'Receipt writer has not been initialized.'
    }

    $script:PvpFixtureReceiptState.Sequence++
    $record = [ordered]@{
        schema = $script:PvpFixtureReceiptSchema
        schema_version = 1
        run_id = $script:PvpFixtureReceiptState.RunId
        sequence = $script:PvpFixtureReceiptState.Sequence
        timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
        fixture_id = $script:PvpFixtureContext.Definition.fixture_id
        action = $script:PvpFixtureContext.Action
        dry_run = (-not $script:PvpFixtureContext.Execute)
        execute = [bool]$script:PvpFixtureContext.Execute
        event = $Event
        payload = $Payload
    }
    $line = $record | ConvertTo-Json -Depth 24 -Compress
    if ($line -match '(?i)(password|mysql_pwd)\s*[:=]\s*[^,}\]]+') {
        throw 'Receipt payload appears to contain a secret-like field; refusing to write it.'
    }
    Add-Content -LiteralPath $script:PvpFixtureReceiptState.Path -Value $line -Encoding utf8
    Write-Output $line
}

function Add-PvpFixtureMetric {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [ValidateSet('bridge_commands', 'console_commands', 'db_read_statements', 'db_write_statements', 'db_rows_inserted', 'db_rows_updated', 'db_rows_deleted', 'unrelated_mutation_count', 'unsupported_stage_count')]
        [string]$Name,

        [int]$Amount = 1
    )

    $script:PvpFixtureMetrics[$Name] = [int]$script:PvpFixtureMetrics[$Name] + $Amount
}

function Get-PvpFixtureMetricsSnapshot {
    return [ordered]@{
        bridge_commands = [int]$script:PvpFixtureMetrics.bridge_commands
        console_commands = [int]$script:PvpFixtureMetrics.console_commands
        db_read_statements = [int]$script:PvpFixtureMetrics.db_read_statements
        db_write_statements = [int]$script:PvpFixtureMetrics.db_write_statements
        db_rows_inserted = [int]$script:PvpFixtureMetrics.db_rows_inserted
        db_rows_updated = [int]$script:PvpFixtureMetrics.db_rows_updated
        db_rows_deleted = [int]$script:PvpFixtureMetrics.db_rows_deleted
        unrelated_mutation_count = [int]$script:PvpFixtureMetrics.unrelated_mutation_count
        unsupported_stage_count = [int]$script:PvpFixtureMetrics.unsupported_stage_count
    }
}

function Get-PvpFixtureConfigConnection {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$ConfigPath,

        [Parameter(Mandatory)]
        [string]$Key
    )

    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        throw "Required configuration is missing: $ConfigPath"
    }
    $escapedKey = [regex]::Escape($Key)
    $line = @(Get-Content -LiteralPath $ConfigPath | Where-Object { $_ -match "^\s*$escapedKey\s*=" } | Select-Object -First 1)
    if ($line.Count -ne 1) { throw "Could not find $Key in $ConfigPath" }
    $value = (($line[0] -split '=', 2)[1]).Trim().Trim('"')
    $parts = @($value -split ';', 5)
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    $dbHost = $parts[0].Trim()
    Assert-LocalOnlyAddress -Address $dbHost
    $dbPort = 0
    if (-not [int]::TryParse($parts[1].Trim(), [ref]$dbPort) -or $dbPort -lt 1 -or $dbPort -gt 65535) {
        throw "$Key has an invalid local port."
    }
    foreach ($valuePart in @($parts[2], $parts[4])) {
        if ($valuePart -notmatch '^[A-Za-z0-9_.$-]+$') {
            throw "$Key contains an unsafe database connection token."
        }
    }
    return [pscustomobject]@{
        key = $Key
        host = $dbHost
        port = $dbPort
        user = $parts[2].Trim()
        password = $parts[3]
        database = $parts[4].Trim()
    }
}

function Get-PvpFixtureMysqlPath {
    $candidates = @(
        (Join-Path $script:PvpFixtureContext.ServerRoot 'third_party\mysql\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
    )
    $mysql = Get-FirstExistingPath -Candidates $candidates
    if (-not $mysql) { throw 'MySQL CLI was not found. Set up the existing AutoWoW third_party\mysql helper first.' }
    return $mysql
}

function Get-PvpFixtureDatabaseContext {
    $worldConfig = Join-Path $script:PvpFixtureContext.ServerRoot 'server\configs\worldserver.conf'
    $playerbotsConfig = Join-Path $script:PvpFixtureContext.ServerRoot 'server\configs\modules\playerbots.conf'
    return [pscustomobject]@{
        mysql = Get-PvpFixtureMysqlPath
        login = Get-PvpFixtureConfigConnection -ConfigPath $worldConfig -Key 'LoginDatabaseInfo'
        world = Get-PvpFixtureConfigConnection -ConfigPath $worldConfig -Key 'WorldDatabaseInfo'
        characters = Get-PvpFixtureConfigConnection -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
        playerbots = Get-PvpFixtureConfigConnection -ConfigPath $playerbotsConfig -Key 'PlayerbotsDatabaseInfo'
    }
}

function Invoke-PvpFixtureSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Database,

        [Parameter(Mandatory)]
        [string]$Sql,

        [switch]$Mutation
    )

    if (-not $script:PvpFixtureContext.Execute) {
        throw 'Database execution was requested without -Execute.'
    }
    if ($Mutation) { Add-PvpFixtureMetric -Name db_write_statements } else { Add-PvpFixtureMetric -Name db_read_statements }

    $oldPassword = $env:MYSQL_PWD
    $env:MYSQL_PWD = $Database.password
    try {
        $mysqlArgs = @(
            '--protocol=tcp',
            "--host=$($Database.host)",
            "--port=$($Database.port)",
            "--user=$($Database.user)",
            '--default-character-set=utf8mb4',
            '--batch',
            '--skip-column-names',
            "--database=$($Database.database)",
            "--execute=$Sql"
        )
        $raw = & $script:PvpFixtureContext.Database.mysql @mysqlArgs 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPassword) {
            Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue
        }
        else {
            $env:MYSQL_PWD = $oldPassword
        }
    }

    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object {
        $_ -and $_ -notmatch '^mysql: (Unknown OS character set|Switching to the default character set)'
    })
    if ($exitCode -ne 0) {
        $safeError = @($lines | Where-Object { $_ -notmatch '(?i)(password|MYSQL_PWD|;[^;]+;[^;]+;[^;]+;)' }) -join ' '
        if ([string]::IsNullOrWhiteSpace($safeError)) { $safeError = 'no database error text returned' }
        throw "PvP fixture database command failed: $safeError"
    }
    return $lines
}

function ConvertFrom-PvpFixtureTabRows {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [string[]]$Lines,

        [Parameter(Mandatory)]
        [string[]]$Columns
    )

    $rows = @()
    foreach ($line in $Lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        # PowerShell treats a negative max-substrings argument as a right-to-left
        # split request; -1 leaves this tab-delimited row unsplit. Regex.Split
        # preserves empty columns and has the expected cross-version behavior.
        $fields = @([regex]::Split($line, "`t"))
        if ($fields.Count -lt $Columns.Count) { continue }
        $row = [ordered]@{}
        for ($fieldIndex = 0; $fieldIndex -lt $Columns.Count; $fieldIndex++) {
            $row[$Columns[$fieldIndex]] = $fields[$fieldIndex]
        }
        $rows += [pscustomobject]$row
    }
    return @($rows)
}

function Ensure-PvpFixtureExactWsgTemplate {
    [CmdletBinding()]
    param()

    $read = {
        $lines = @(Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.world -Sql `
            'SELECT ID,MinPlayersPerTeam,MaxPlayersPerTeam FROM battleground_template WHERE ID=2;')
        return @(ConvertFrom-PvpFixtureTabRows -Lines $lines -Columns @('id', 'min_players', 'max_players'))
    }

    $rows = @(& $read)
    if ($rows.Count -ne 1 -or [int]$rows[0].id -ne 2 -or [int]$rows[0].max_players -ne 10) {
        throw 'WSG fixture requires battleground_template ID 2 with MaxPlayersPerTeam=10.'
    }
    $previousMinimum = [int]$rows[0].min_players
    if ($previousMinimum -ne 10) {
        $null = Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.world -Mutation -Sql `
            'UPDATE battleground_template SET MinPlayersPerTeam=10 WHERE ID=2 AND MaxPlayersPerTeam=10;'
        $rows = @(& $read)
    }
    if ($rows.Count -ne 1 -or [int]$rows[0].min_players -ne 10 -or [int]$rows[0].max_players -ne 10) {
        throw 'WSG fixture could not verify the exact 10v10 battleground template.'
    }

    Write-PvpFixtureReceipt -Event 'wsg_template_verified' -Payload @{
        battleground_template_id = 2
        previous_min_players_per_team = $previousMinimum
        min_players_per_team = 10
        max_players_per_team = 10
        restart_required = ($previousMinimum -ne 10)
    } | Out-Null
}

function Get-PvpFixtureAccountAndCharacterRows {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition
    )

    $names = @($Definition.accounts | ForEach-Object account_name)
    $nameSql = (($names | ForEach-Object { ConvertTo-PvpFixtureSqlLiteral -Value $_ }) -join ',')
    $accountSql = "SELECT id,username FROM account WHERE username IN ($nameSql) ORDER BY username;"
    $accountLines = @(Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.login -Sql $accountSql)
    $accountRows = ConvertFrom-PvpFixtureTabRows -Lines $accountLines -Columns @('id', 'username')
    if ($null -ne $script:PvpFixtureReceiptState) {
        Write-PvpFixtureReceipt -Event 'account_query_diagnostic' -Payload @{
            expected_count = $names.Count
            raw_line_count = $accountLines.Count
            parsed_row_count = @($accountRows).Count
            first_line_shape = if ($accountLines.Count) { $accountLines[0] -replace "`t", '<TAB>' } else { '' }
        } | Out-Null
    }
    $accountIds = @($accountRows | ForEach-Object { if ($_.id -match '^\d+$') { [int]$_.id } })

    $accountPredicate = if ($accountIds.Count -gt 0) { " OR account IN ($($accountIds -join ','))" } else { '' }
    $characterSql = @"
SELECT guid,account,name,race,class,gender,level,online,map,position_x,position_y,position_z,orientation
FROM $((ConvertTo-PvpFixtureSqlIdentifier -Value $script:PvpFixtureContext.Database.characters.database)).characters
WHERE name IN ($nameSql)$accountPredicate
ORDER BY account,guid;
"@
    $characterRows = ConvertFrom-PvpFixtureTabRows -Lines @(Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.characters -Sql $characterSql) -Columns @('guid', 'account', 'name', 'race', 'class', 'gender', 'level', 'online', 'map', 'position_x', 'position_y', 'position_z', 'orientation')

    return [pscustomobject]@{
        accounts = @($accountRows)
        characters = @($characterRows)
    }
}

function Assert-PvpFixtureAccountAndCharacterScope {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition,

        [Parameter(Mandatory)]
        [psobject]$Rows
    )

    $expectedByAccount = @{}
    $expectedByCharacter = @{}
    foreach ($member in @($Definition.accounts)) {
        $expectedByAccount[$member.account_name] = $member
        $expectedByCharacter[$member.character_name] = $member
    }

    $accountsByName = @{}
    foreach ($account in @($Rows.accounts)) {
        if ($accountsByName.ContainsKey($account.username)) { throw "Duplicate account row returned for $($account.username)." }
        $accountsByName[$account.username] = $account
    }
    $charactersByAccount = @{}
    foreach ($character in @($Rows.characters)) {
        $accountId = [int]$character.account
        if (-not $charactersByAccount.ContainsKey($accountId)) { $charactersByAccount[$accountId] = @() }
        $charactersByAccount[$accountId] += $character
        if ($expectedByCharacter.ContainsKey($character.name)) {
            $expected = $expectedByCharacter[$character.name]
            if (-not $accountsByName.ContainsKey($expected.account_name)) {
                throw "Fixture character name collision: $($character.name) exists while its exact fixture account is absent."
            }
            if ($accountId -ne [int]$accountsByName[$expected.account_name].id) {
                throw "Fixture character name collision: $($character.name) belongs to another account."
            }
            if ([int]$character.race -ne [int]$expected.race -or [int]$character.class -ne [int]$expected.class) {
                throw "Fixture character $($character.name) has an unexpected race/class; refusing to rewrite it."
            }
            if ([int]$character.level -ne [int]$expected.level) {
                throw "Fixture character $($character.name) is level $($character.level), expected $($expected.level); refusing to normalize an existing character."
            }
        }
    }

    $missingAccounts = @()
    $missingCharacters = @()
    $characterByName = @{}
    foreach ($member in @($Definition.accounts)) {
        if (-not $accountsByName.ContainsKey($member.account_name)) {
            $missingAccounts += $member
            continue
        }
        $accountId = [int]$accountsByName[$member.account_name].id
        # Array subexpressions inside an if branch are unrolled again when the
        # branch result is assigned. Wrap the whole conditional so one row does
        # not become a scalar object without a strict-mode Count property.
        $accountCharacters = @(if ($charactersByAccount.ContainsKey($accountId)) { $charactersByAccount[$accountId] })
        if ($accountCharacters.Count -gt 1) {
            throw "Disposable fixture account $($member.account_name) already owns unrelated characters; refusing to mutate or delete it."
        }
        $matching = @($accountCharacters | Where-Object name -eq $member.character_name)
        if ($matching.Count -eq 0) {
            if ($accountCharacters.Count -eq 1) {
                throw "Disposable fixture account $($member.account_name) owns a non-fixture character; refusing to reuse it."
            }
            $missingCharacters += $member
        }
        else {
            $characterByName[$member.character_name] = $matching[0]
        }
    }

    return [pscustomobject]@{
        accounts_by_name = $accountsByName
        characters_by_name = $characterByName
        missing_accounts = @($missingAccounts)
        missing_characters = @($missingCharacters)
    }
}

function Get-PvpFixturePlayerbotsSchemaState {
    $db = ConvertTo-PvpFixtureSqlIdentifier -Value $script:PvpFixtureContext.Database.playerbots.database
    $sql = "SELECT table_name FROM information_schema.tables WHERE table_schema=$(ConvertTo-PvpFixtureSqlLiteral -Value $script:PvpFixtureContext.Database.playerbots.database) AND table_name IN ('autowow_league_team','autowow_league_member') ORDER BY table_name;"
    $rows = @(Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.playerbots -Sql $sql)
    $names = @($rows | Where-Object { $_ -and $_ -notmatch "\t" } | ForEach-Object { $_.Trim() })
    if ('autowow_league_team' -notin $names -or 'autowow_league_member' -notin $names) {
        throw 'Required existing autowow league team/member tables are missing; refusing to create or alter deployment schema.'
    }
    return $true
}

function Get-PvpFixtureMembershipRows {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition,

        [Parameter(Mandatory)]
        [hashtable]$CharacterGuidByName
    )

    $db = ConvertTo-PvpFixtureSqlIdentifier -Value $script:PvpFixtureContext.Database.playerbots.database
    $teamIds = (($Definition.teams | ForEach-Object { ConvertTo-PvpFixtureSqlLiteral -Value $_.team_id }) -join ',')
    $guids = (($CharacterGuidByName.Values | ForEach-Object { [int]$_ }) -join ',')
    $teamSql = "SELECT team_id,display_name,faction,roster_cap,worker_cap FROM $db.autowow_league_team WHERE team_id IN ($teamIds) ORDER BY team_id;"
    $memberSql = "SELECT character_guid,team_id,active,retired_at FROM $db.autowow_league_member WHERE character_guid IN ($guids) ORDER BY character_guid;"
    $teams = ConvertFrom-PvpFixtureTabRows -Lines @(Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.playerbots -Sql $teamSql) -Columns @('team_id', 'display_name', 'faction', 'roster_cap', 'worker_cap')
    $members = ConvertFrom-PvpFixtureTabRows -Lines @(Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.playerbots -Sql $memberSql) -Columns @('character_guid', 'team_id', 'active', 'retired_at')
    return [pscustomobject]@{ teams = @($teams); members = @($members) }
}

function Assert-PvpFixtureMembershipScope {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition,

        [Parameter(Mandatory)]
        [hashtable]$CharacterGuidByName,

        [Parameter(Mandatory)]
        [psobject]$Rows
    )

    foreach ($team in @($Definition.teams)) {
        $existing = @($Rows.teams | Where-Object team_id -eq $team.team_id)
        if ($existing.Count -gt 1) { throw "Duplicate fixture team rows returned for $($team.team_id)." }
        if ($existing.Count -eq 1) {
            if ($existing[0].display_name -ne $team.display_name -or $existing[0].faction -ne $team.faction) {
                throw "Existing team row $($team.team_id) is not the expected disposable fixture team."
            }
        }
    }

    foreach ($member in @($Definition.accounts)) {
        $guid = [int]$CharacterGuidByName[$member.character_name]
        $existing = @($Rows.members | Where-Object { [int]$_.character_guid -eq $guid })
        if ($existing.Count -gt 1) { throw "Duplicate league member rows returned for GUID $guid." }
        if ($existing.Count -eq 1) {
            $expectedTeam = (Get-PvpFixtureTeamForMember -Definition $Definition -Member $member).team_id
            if ($existing[0].team_id -ne $expectedTeam) {
                throw "Character GUID $guid is already assigned to another league team; refusing to take ownership."
            }
        }
    }
    return $true
}

function Invoke-PvpFixtureAccountConsoleBatch {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject[]]$MissingAccounts,

        [ValidateSet('create', 'delete')]
        [string]$Mode = 'create'
    )

    $password = ''
    if ($Mode -eq 'create') {
        $password = $env:WOW_ACCOUNT_PASSWORD
        if ([string]::IsNullOrWhiteSpace($password)) {
            throw 'WOW_ACCOUNT_PASSWORD must be set in the environment for -Execute account provisioning; it is never accepted as a command-line argument.'
        }
        if ($password -match '\s' -or $password.Length -lt 3 -or $password.Length -gt 16) {
            throw 'WOW_ACCOUNT_PASSWORD must be 3-16 characters with no whitespace (the 3.3.5 client limit).'
        }
    }

    $serverDirectory = Join-Path $script:PvpFixtureContext.ServerRoot 'server'
    $worldserver = Join-Path $serverDirectory 'worldserver.exe'
    $worldConfig = Join-Path $serverDirectory 'configs\worldserver.conf'
    if (-not (Test-Path -LiteralPath $worldserver) -or -not (Test-Path -LiteralPath $worldConfig)) {
        throw 'Temporary account provisioning requires the existing worldserver.exe and worldserver.conf.'
    }
    $running = @(Get-PvpFixtureWorldserverProcesses)
    if ($running.Count -gt 0) {
        throw 'Refusing temporary console provisioning while the normal worldserver is running.'
    }

    $stdoutPath = Join-Path $script:PvpFixtureContext.LogsRoot ("pvp-fixture-$($script:PvpFixtureContext.Definition.tag)-$($script:PvpFixtureContext.RunId)-console.stdout.log")
    $stderrPath = Join-Path $script:PvpFixtureContext.LogsRoot ("pvp-fixture-$($script:PvpFixtureContext.Definition.tag)-$($script:PvpFixtureContext.RunId)-console.stderr.log")
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $worldserver
    $startInfo.Arguments = '-c "' + $worldConfig + '"'
    $startInfo.WorkingDirectory = $serverDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw 'Could not start the temporary worldserver console process.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $exitCode = $null
    try {
        $ready = $false
        $readyWatch = [System.Diagnostics.Stopwatch]::StartNew()
        while ($readyWatch.Elapsed.TotalSeconds -lt 150) {
            if ($process.HasExited) {
                throw 'Temporary account provisioning worldserver exited before the AutoWow bridge became ready.'
            }

            $client = [System.Net.Sockets.TcpClient]::new()
            try {
                $connect = $client.ConnectAsync('127.0.0.1', 18787)
                if ($connect.Wait(500) -and $client.Connected) {
                    $ready = $true
                    break
                }
            }
            catch { }
            finally { $client.Dispose() }
            Start-Sleep -Milliseconds 500
        }
        if (-not $ready) {
            throw 'Temporary account provisioning worldserver did not become ready within 150 seconds.'
        }

        # The bridge is opened at the end of world initialization. Give the
        # console command loop one more tick before feeding the atomic batch.
        Start-Sleep -Seconds 2
        Write-PvpFixtureReceipt -Event 'account_console_ready' -Payload @{
            readiness_surface = 'AutoWow bridge 127.0.0.1:18787'
            elapsed_seconds = [math]::Round($readyWatch.Elapsed.TotalSeconds, 3)
        } | Out-Null
        foreach ($account in $MissingAccounts) {
            $command = if ($Mode -eq 'create') { ".account create $($account.account_name) $password" } else { ".account delete $($account.account_name)" }
            $receiptEvent = if ($Mode -eq 'create') { 'account_create_command' } else { 'account_delete_command' }
            [void]$process.StandardInput.WriteLine($command)
            Add-PvpFixtureMetric -Name console_commands
            Write-PvpFixtureReceipt -Event $receiptEvent -Payload @{
                account_name = $account.account_name
                command = if ($Mode -eq 'create') { ".account create $($account.account_name) <WOW_ACCOUNT_PASSWORD>" } else { ".account delete $($account.account_name)" }
                scope = 'exact disposable fixture account'
            } | Out-Null
        }
        [void]$process.StandardInput.Flush()
        Start-Sleep -Seconds 5
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(180000)) {
            try { $process.Kill() } catch { }
            throw 'Temporary account provisioning worldserver did not exit after stdin close.'
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
    }
    finally {
        if ($process.HasExited) { $exitCode = $process.ExitCode }
        if (-not $process.HasExited) {
            try { $process.Kill() } catch { }
        }
        $process.Dispose()
    }

    $safeStdout = if ($password) { $stdout -replace [regex]::Escape($password), '<redacted>' } else { $stdout }
    $safeStderr = if ($password) { $stderr -replace [regex]::Escape($password), '<redacted>' } else { $stderr }
    [System.IO.File]::WriteAllText($stdoutPath, $safeStdout, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($stderrPath, $safeStderr, [System.Text.UTF8Encoding]::new($false))
    if ($null -eq $exitCode) { throw "Temporary account provisioning worldserver did not provide an exit code; see $stdoutPath and $stderrPath" }
    if ($exitCode -ne 0) {
        throw "Temporary account provisioning worldserver exited with code $exitCode; see $stdoutPath and $stderrPath"
    }
    Write-PvpFixtureReceipt -Event 'account_console_batch_completed' -Payload @{
        mode = $Mode
        account_count = $MissingAccounts.Count
        stdout_log = $stdoutPath
        stderr_log = $stderrPath
        exit_code = $exitCode
    } | Out-Null
}

function Invoke-PvpFixtureProvisioning {
    [CmdletBinding()]
    param()

    if (-not $script:PvpFixtureContext.Execute) { throw 'Provisioning requires -Execute.' }
    $definition = $script:PvpFixtureContext.Definition
    $script:PvpFixtureContext.Database = Get-PvpFixtureDatabaseContext
    Ensure-PvpFixtureExactWsgTemplate
    $rows = Get-PvpFixtureAccountAndCharacterRows -Definition $definition
    $scope = Assert-PvpFixtureAccountAndCharacterScope -Definition $definition -Rows $rows
    Write-PvpFixtureReceipt -Event 'account_character_preflight' -Payload @{
        existing_account_count = @($rows.accounts).Count
        existing_character_count = @($rows.characters).Count
        missing_account_count = @($scope.missing_accounts).Count
        missing_character_count = @($scope.missing_characters).Count
        unrelated_mutation_count = 0
    } | Out-Null

    if (@($scope.missing_accounts).Count -gt 0) {
        Invoke-PvpFixtureAccountConsoleBatch -MissingAccounts @($scope.missing_accounts)
        $rows = Get-PvpFixtureAccountAndCharacterRows -Definition $definition
        $scope = Assert-PvpFixtureAccountAndCharacterScope -Definition $definition -Rows $rows
        if (@($scope.missing_accounts).Count -gt 0) { throw 'Account console batch completed but one or more exact fixture accounts are still missing.' }
    }

    foreach ($member in @($scope.missing_characters)) {
        $accountId = [int]$scope.accounts_by_name[$member.account_name].id
        $sql = New-PvpFixtureCharacterInsertSql -CharactersDatabaseName $script:PvpFixtureContext.Database.characters.database -AccountId $accountId -Member $member
        Write-PvpFixtureReceipt -Event 'character_insert_sql' -Payload @{
            account_name = $member.account_name
            character_name = $member.character_name
            account_id = $accountId
            scope = "exact account=$accountId and name=$($member.character_name)"
            mutation = 'insert-if-absent'
        } | Out-Null
        $null = Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.characters -Sql $sql -Mutation
    }

    $rows = Get-PvpFixtureAccountAndCharacterRows -Definition $definition
    $scope = Assert-PvpFixtureAccountAndCharacterScope -Definition $definition -Rows $rows
    if (@($scope.missing_characters).Count -gt 0) { throw 'Character insert batch completed but one or more exact fixture characters are still missing.' }

    $characterGuidByName = @{}
    foreach ($member in @($definition.accounts)) {
        $characterGuidByName[$member.character_name] = [int]$scope.characters_by_name[$member.character_name].guid
    }
    Get-PvpFixturePlayerbotsSchemaState | Out-Null
    $membershipRows = Get-PvpFixtureMembershipRows -Definition $definition -CharacterGuidByName $characterGuidByName
    Assert-PvpFixtureMembershipScope -Definition $definition -CharacterGuidByName $characterGuidByName -Rows $membershipRows | Out-Null
    $membershipSql = New-PvpFixtureTeamMembershipSql -PlayerbotsDatabaseName $script:PvpFixtureContext.Database.playerbots.database -Definition $definition -CharacterGuidByName $characterGuidByName
    Write-PvpFixtureReceipt -Event 'team_membership_sql' -Payload @{
        team_ids = @($definition.teams | ForEach-Object team_id)
        member_guids = @($characterGuidByName.Values | ForEach-Object { [int]$_ })
        mutation = 'exact fixture team ids and exact fixture character_guid values only'
    } | Out-Null
    $null = Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.playerbots -Sql $membershipSql -Mutation

    $membershipRows = Get-PvpFixtureMembershipRows -Definition $definition -CharacterGuidByName $characterGuidByName
    Assert-PvpFixtureMembershipScope -Definition $definition -CharacterGuidByName $characterGuidByName -Rows $membershipRows | Out-Null
    $expectedMemberCount = @($definition.accounts).Count
    $actualMemberCount = @($membershipRows.members).Count
    if ($actualMemberCount -ne $expectedMemberCount) { throw "Fixture team membership verification found $actualMemberCount rows; expected $expectedMemberCount." }

    $levels = @($scope.characters_by_name.Values | ForEach-Object { [int]$_.level } | Sort-Object -Unique)
    if ($levels.Count -ne 1 -or $levels[0] -ne [int]$definition.level) { throw 'Fixture equal-level verification failed.' }
    Write-PvpFixtureReceipt -Event 'fixture_roster_verified' -Payload @{
        account_count = @($rows.accounts).Count
        character_count = @($scope.characters_by_name.Keys).Count
        alliance_count = @($definition.accounts | Where-Object faction -eq 'Alliance').Count
        horde_count = @($definition.accounts | Where-Object faction -eq 'Horde').Count
        level = [int]$definition.level
        member_count = $actualMemberCount
        character_guid_by_name = $characterGuidByName
    } | Out-Null
    Write-PvpFixtureReceipt -Event 'equal_level_verified' -Payload @{
        expected_level = [int]$definition.level
        distinct_levels = $levels
        character_count = @($scope.characters_by_name.Keys).Count
    } | Out-Null
    Write-PvpFixtureReceipt -Event 'team_membership_verified' -Payload @{
        team_ids = @($definition.teams | ForEach-Object team_id)
        member_count = $actualMemberCount
        member_guids = @($characterGuidByName.Values | ForEach-Object { [int]$_ })
    } | Out-Null

    $script:PvpFixtureContext.CharacterGuidByName = $characterGuidByName
    return [pscustomobject]@{
        account_rows = $rows.accounts
        character_rows = $scope.characters_by_name
        character_guid_by_name = $characterGuidByName
        membership_rows = $membershipRows
    }
}

function Get-PvpFixtureWorldserverProcesses {
    [CmdletBinding()]
    param()

    $serverRoot = [System.IO.Path]::GetFullPath((Join-Path $script:PvpFixtureContext.ServerRoot 'server')).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $processes = @()
    foreach ($candidate in @(Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $candidate.MainModule.FileName } catch { }
        if ([string]::IsNullOrWhiteSpace($path)) {
            throw "Could not prove the path of worldserver process $($candidate.Id); refusing to treat it as local fixture infrastructure."
        }
        $fullPath = [System.IO.Path]::GetFullPath($path)
        if ($fullPath.StartsWith($serverRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            $processes += $candidate
        }
    }

    # The isolated Phase 1 binary runs inside WSL, so it is not visible as a Windows process named
    # worldserver. Prove that topology from the generated runtime manifest, the live wsl.exe wrapper,
    # the exact Linux executable path, and the loopback bridge listener. Callers use this result only
    # as a running/not-running guard; they never terminate the returned process object.
    if ($processes.Count -eq 0) {
        $statePath = Join-Path $script:PvpFixtureContext.ServerRoot 'work\phase1-wsl-runtime\runtime-processes.json'
        if (Test-Path -LiteralPath $statePath) {
            try {
                $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
                $wrapper = Get-Process -Id ([int]$state.wsl_wrapper_pid) -ErrorAction Stop
                if ($wrapper.ProcessName -eq 'wsl' -and -not [string]::IsNullOrWhiteSpace([string]$state.distro) -and
                    -not [string]::IsNullOrWhiteSpace([string]$state.binary)) {
                    $linuxPids = @(& wsl.exe -d ([string]$state.distro) -u root -- pgrep -x worldserver 2>$null)
                    foreach ($linuxProcessId in $linuxPids) {
                        if ([string]$linuxProcessId -notmatch '^\d+$') { continue }
                        $resolvedExe = (& wsl.exe -d ([string]$state.distro) -u root -- readlink -f "/proc/$linuxProcessId/exe" 2>$null)
                        $bridgeListening = $null -ne (Get-NetTCPConnection -State Listen -LocalPort 18787 -ErrorAction SilentlyContinue |
                            Select-Object -First 1)
                        if ($LASTEXITCODE -eq 0 -and ([string]$resolvedExe).Trim() -eq [string]$state.binary -and $bridgeListening) {
                            $processes += [pscustomobject]@{
                                Id = [int]$linuxProcessId
                                ProcessName = 'worldserver'
                                Platform = 'wsl'
                                Path = [string]$state.binary
                            }
                            break
                        }
                    }
                }
            }
            catch {
                # A stale or malformed WSL state file is not proof of a running server.
            }
        }
    }
    return @($processes)
}

function Get-PvpFixtureProperty {
    [CmdletBinding()]
    param(
        [AllowNull()]
        [object]$Object,

        [Parameter(Mandatory)]
        [string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Invoke-PvpFixtureBridgeAction {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [ValidateSet('list', 'activate', 'deactivate', 'party', 'rally', 'route', 'snapshot', 'pause', 'resume', 'engage')]
        [string]$BridgeAction,

        [uint32]$BotGuid = 0,

        [uint32[]]$MemberGuids = @(),

        [string]$Destination = '',

        [uint32]$TargetPlayerGuid = 0
    )

    if ($BridgeAction -notin (Get-PvpFixtureSafeBridgeActions)) {
        throw "Bridge action $BridgeAction is outside the fixture allowlist."
    }
    if ($BridgeAction -ne 'list' -and $BotGuid -eq 0) {
        throw "Bridge action $BridgeAction requires an exact fixture BotGuid."
    }
    if ($BridgeAction -eq 'party' -and $MemberGuids.Count -lt 1) {
        throw 'Party formation requires at least one exact fixture member GUID.'
    }
    if ($BridgeAction -eq 'route' -and [string]::IsNullOrWhiteSpace($Destination)) {
        throw 'Route staging requires an exact named destination.'
    }

    $bridgeScript = Join-Path $script:PvpFixtureContext.ServerRoot 'scripts\autowow-control.ps1'
    if (-not (Test-Path -LiteralPath $bridgeScript)) { throw "Existing bridge helper is missing: $bridgeScript" }
    $raw = switch ($BridgeAction) {
        'list' { & $bridgeScript -Action list -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'activate' { & $bridgeScript -Action activate -BotGuid $BotGuid -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'deactivate' { & $bridgeScript -Action deactivate -BotGuid $BotGuid -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'party' { & $bridgeScript -Action party -BotGuid $BotGuid -MemberGuid $MemberGuids -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'rally' { & $bridgeScript -Action rally -BotGuid $BotGuid -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'route' { & $bridgeScript -Action route -BotGuid $BotGuid -Destination $Destination -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'snapshot' { & $bridgeScript -Action snapshot -BotGuid $BotGuid -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'pause' { & $bridgeScript -Action pause -BotGuid $BotGuid -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'resume' { & $bridgeScript -Action resume -BotGuid $BotGuid -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
        'engage' { & $bridgeScript -Action engage -BotGuid $BotGuid -TargetPlayerGuid $TargetPlayerGuid -BridgeHost '127.0.0.1' -Port $script:PvpFixtureContext.BridgePort -TimeoutMs $script:PvpFixtureContext.BridgeTimeoutMs }
    }
    Add-PvpFixtureMetric -Name bridge_commands
    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object { $_ -match '^\s*\{' })
    if ($lines.Count -eq 0) { throw "Bridge action $BridgeAction returned no JSON response." }
    $jsonLine = $lines | Select-Object -Last 1
    try { $response = $jsonLine | ConvertFrom-Json } catch { throw "Bridge action $BridgeAction returned malformed JSON." }
    Write-PvpFixtureReceipt -Event 'bridge_command' -Payload @{
        bridge_action = $BridgeAction
        bot_guid = if ($BotGuid -eq 0) { $null } else { [uint32]$BotGuid }
        member_guids = @($MemberGuids | ForEach-Object { [uint32]$_ })
        destination = if ([string]::IsNullOrWhiteSpace($Destination)) { $null } else { $Destination }
        target_player_guid = if ($TargetPlayerGuid -eq 0) { $null } else { [uint32]$TargetPlayerGuid }
        host = '127.0.0.1'
        port = [int]$script:PvpFixtureContext.BridgePort
        response = $response
    } | Out-Null
    $ok = Get-PvpFixtureProperty -Object $response -Name 'ok'
    if ($null -ne $ok -and -not [bool]$ok) {
        $errorText = [string](Get-PvpFixtureProperty -Object $response -Name 'error')
        if ([string]::IsNullOrWhiteSpace($errorText)) { $errorText = 'bridge returned ok=false' }
        throw "Bridge action $BridgeAction failed: $errorText"
    }
    return $response
}

function Get-PvpFixtureBridgeBots {
    $response = Invoke-PvpFixtureBridgeAction -BridgeAction list
    $bots = Get-PvpFixtureProperty -Object $response -Name 'bots'
    # Preserve an actual object[] even when the bridge reports zero bots. PowerShell otherwise
    # unrolls the empty return value to $null, which breaks the first activation pass.
    if ($null -eq $bots) { return ,([object[]]@()) }
    return ,([object[]]@($bots))
}

function Get-PvpFixtureBridgeBot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [object[]]$Bots,

        [Parameter(Mandatory)]
        [uint32]$Guid
    )

    return @($Bots | Where-Object { [uint32](Get-PvpFixtureProperty -Object $_ -Name 'guid') -eq $Guid } | Select-Object -First 1)
}

function Get-PvpFixtureStage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$StageId
    )

    $stage = @($script:PvpFixtureContext.Definition.stages | Where-Object stage_id -eq $StageId)
    if ($stage.Count -ne 1) { throw "Unknown fixture stage: $StageId" }
    return $stage[0]
}

function Get-PvpFixtureStageStates {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Stage
    )

    if ($null -eq $script:PvpFixtureContext.CharacterGuidByName) {
        throw 'Character GUID mapping is not loaded; provision or status must run first.'
    }
    $members = Get-PvpFixtureStageMembers -Definition $script:PvpFixtureContext.Definition -Stage $Stage
    $states = @()
    foreach ($side in @('alliance', 'horde')) {
        foreach ($member in @($members.$side)) {
            if (-not $script:PvpFixtureContext.CharacterGuidByName.ContainsKey($member.character_name)) {
                throw "No exact character GUID is known for $($member.character_name)."
            }
            $states += [pscustomobject]@{
                side = $side
                member = $member
                guid = [uint32]$script:PvpFixtureContext.CharacterGuidByName[$member.character_name]
                team = Get-PvpFixtureTeamForMember -Definition $script:PvpFixtureContext.Definition -Member $member
            }
        }
    }
    return @($states)
}

function Ensure-PvpFixtureBotsOnline {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [object[]]$States
    )

    $bots = Get-PvpFixtureBridgeBots
    foreach ($state in $States) {
        $bot = @(Get-PvpFixtureBridgeBot -Bots $bots -Guid $state.guid)
        if ($bot.Count -eq 0) {
            Invoke-PvpFixtureBridgeAction -BridgeAction activate -BotGuid $state.guid | Out-Null
        }
    }
    $deadline = (Get-Date).AddSeconds($script:PvpFixtureContext.WaitSeconds)
    do {
        $bots = Get-PvpFixtureBridgeBots
        $missing = @($States | Where-Object { @(Get-PvpFixtureBridgeBot -Bots $bots -Guid $_.guid).Count -eq 0 })
        if ($missing.Count -eq 0) {
            Write-PvpFixtureReceipt -Event 'stage_bots_online_verified' -Payload @{
                requested_count = $States.Count
                online_count = $States.Count
                guids = @($States | ForEach-Object { [uint32]$_.guid })
            } | Out-Null
            return $bots
        }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for exact fixture bots to become online: $(@($missing | ForEach-Object { $_.member.character_name }) -join ', ')"
}

function Get-PvpFixtureGroupMembersCount {
    [CmdletBinding()]
    param([AllowNull()][object]$Bot)

    $group = Get-PvpFixtureProperty -Object $Bot -Name 'group'
    $value = Get-PvpFixtureProperty -Object $group -Name 'members'
    if ($null -eq $value -or $value -notmatch '^\d+$') { return 0 }
    return [int]$value
}

function Get-PvpFixtureGroupLeaderGuid {
    [CmdletBinding()]
    param([AllowNull()][object]$Bot)

    $group = Get-PvpFixtureProperty -Object $Bot -Name 'group'
    $value = Get-PvpFixtureProperty -Object $group -Name 'leader_guid'
    if ($null -eq $value -or $value -notmatch '^\d+$') { return [uint32]0 }
    return [uint32]$value
}

function Ensure-PvpFixtureParty {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [object[]]$States,

        [Parameter(Mandatory)]
        [object[]]$Bots
    )

    $leader = @($States | Select-Object -First 1)[0]
    $leaderBot = @(Get-PvpFixtureBridgeBot -Bots $Bots -Guid $leader.guid)
    if ($leaderBot.Count -ne 1) { throw "Fixture party leader $($leader.member.character_name) is not online." }
    $expectedCount = $States.Count
    $groupCounts = @($States | ForEach-Object {
        $bot = @(Get-PvpFixtureBridgeBot -Bots $Bots -Guid $_.guid)
        if ($bot.Count -ne 1) { throw "Fixture party member $($_.member.character_name) is not online." }
        [pscustomobject]@{ state = $_; count = Get-PvpFixtureGroupMembersCount -Bot $bot[0]; leader_guid = Get-PvpFixtureGroupLeaderGuid -Bot $bot[0] }
    })
    $alreadyExact = @($groupCounts | Where-Object { $_.count -eq $expectedCount -and $_.leader_guid -eq $leader.guid }).Count -eq $expectedCount
    $allUngrouped = @($groupCounts | Where-Object count -eq 0).Count -eq $expectedCount
    if (-not $alreadyExact -and -not $allUngrouped) {
        throw "Fixture party has a partial or foreign group; refusing to disband or reuse it."
    }
    if ($allUngrouped) {
        $otherGuids = @($States | Select-Object -Skip 1 | ForEach-Object { [uint32]$_.guid })
        Invoke-PvpFixtureBridgeAction -BridgeAction party -BotGuid $leader.guid -MemberGuids $otherGuids | Out-Null
        Start-Sleep -Milliseconds 500
        $after = Get-PvpFixtureBridgeBots
        $verified = @($States | Where-Object {
            $bot = @(Get-PvpFixtureBridgeBot -Bots $after -Guid $_.guid)
            $bot.Count -eq 1 -and (Get-PvpFixtureGroupMembersCount -Bot $bot[0]) -eq $expectedCount -and (Get-PvpFixtureGroupLeaderGuid -Bot $bot[0]) -eq $leader.guid
        }).Count
        if ($verified -ne $expectedCount) { throw "Bridge party formation did not verify all $expectedCount exact fixture members." }
        return $after
    }
    return $Bots
}

function Get-PvpFixturePoint {
    [CmdletBinding()]
    param([AllowNull()][object]$Bot)

    $position = Get-PvpFixtureProperty -Object $Bot -Name 'position'
    if ($null -eq $position) { return $null }
    $map = Get-PvpFixtureProperty -Object $position -Name 'map'
    $x = Get-PvpFixtureProperty -Object $position -Name 'x'
    $y = Get-PvpFixtureProperty -Object $position -Name 'y'
    $z = Get-PvpFixtureProperty -Object $position -Name 'z'
    $o = Get-PvpFixtureProperty -Object $position -Name 'o'
    if ($null -eq $map -or $null -eq $x -or $null -eq $y -or $null -eq $z) { return $null }
    return [pscustomobject]@{ map = [int]$map; x = [double]$x; y = [double]$y; z = [double]$z; o = if ($null -eq $o) { 0.0 } else { [double]$o } }
}

function Get-PvpFixtureDistance {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Left,
        [AllowNull()][object]$Right
    )

    if ($null -eq $Left -or $null -eq $Right -or [int]$Left.map -ne [int]$Right.map) { return [double]::PositiveInfinity }
    $dx = [double]$Left.x - [double]$Right.x
    $dy = [double]$Left.y - [double]$Right.y
    $dz = [double]$Left.z - [double]$Right.z
    return [math]::Sqrt(($dx * $dx) + ($dy * $dy) + ($dz * $dz))
}

function Get-PvpFixtureBotLevel {
    [CmdletBinding()]
    param([AllowNull()][object]$Bot)

    $progress = Get-PvpFixtureProperty -Object $Bot -Name 'progress'
    $level = Get-PvpFixtureProperty -Object $progress -Name 'level'
    if ($null -eq $level) { $level = Get-PvpFixtureProperty -Object $Bot -Name 'level' }
    if ($null -eq $level -or $level -notmatch '^\d+$') { return -1 }
    return [int]$level
}

function Get-PvpFixtureTargetGuid {
    [CmdletBinding()]
    param([AllowNull()][object]$Bot)

    $target = Get-PvpFixtureProperty -Object $Bot -Name 'target'
    $guid = Get-PvpFixtureProperty -Object $target -Name 'guid'
    if ($null -eq $guid -or $guid -notmatch '^\d+$') { return [uint32]0 }
    return [uint32]$guid
}

function Set-PvpFixtureActiveMembership {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [object[]]$States,

        [Parameter(Mandatory)]
        [bool]$Active
    )

    $guids = @($States | ForEach-Object { [int]$_.guid } | Sort-Object -Unique)
    if ($guids.Count -eq 0) { return }
    $teamIds = @($States | ForEach-Object { $_.team.team_id } | Sort-Object -Unique)
    $sql = New-PvpFixtureActiveMembershipSql -PlayerbotsDatabaseName $script:PvpFixtureContext.Database.playerbots.database -CharacterGuids $guids -Active $Active -TeamIds $teamIds
    $null = Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.playerbots -Sql $sql -Mutation
    Write-PvpFixtureReceipt -Event 'fixture_active_membership_update' -Payload @{
        active = $Active
        character_guids = $guids
        team_ids = $teamIds
        mutation = 'exact character_guid IN list; no other league rows are addressable'
    } | Out-Null
}

function Ensure-PvpFixtureProvisionedForControl {
    if (-not $script:PvpFixtureContext.Execute) { throw 'Live bridge control requires -Execute.' }
    if ($null -eq $script:PvpFixtureContext.CharacterGuidByName) {
        Invoke-PvpFixtureProvisioning | Out-Null
    }
}

function Get-PvpFixtureTelemetryPayload {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Stage,

        [Parameter(Mandatory)]
        [object[]]$States,

        [Parameter(Mandatory)]
        [object[]]$Bots,

        [Parameter(Mandatory)]
        [string]$Phase
    )

    $opposingGuids = @{}
    foreach ($state in $States) {
        $opposingGuids[[uint32]$state.guid] = $state.side
    }
    $leaderPointBySide = @{}
    foreach ($side in @('alliance', 'horde')) {
        $leaderState = @($States | Where-Object side -eq $side | Select-Object -First 1)
        if ($leaderState.Count -eq 1) {
            $leaderBot = @(Get-PvpFixtureBridgeBot -Bots $Bots -Guid $leaderState[0].guid)
            $leaderPointBySide[$side] = if ($leaderBot.Count -eq 1) { Get-PvpFixturePoint -Bot $leaderBot[0] } else { $null }
        }
        else { $leaderPointBySide[$side] = $null }
    }

    $botTelemetry = @()
    $sameLevelCount = 0
    $onlineCount = 0
    $aliveCount = 0
    $combatCount = 0
    $opposingTargetCount = 0
    $rallyDistances = @()
    foreach ($state in $States) {
        $bot = @(Get-PvpFixtureBridgeBot -Bots $Bots -Guid $state.guid)
        $online = $bot.Count -eq 1
        $botValue = if ($online) { $bot[0] } else { $null }
        $level = Get-PvpFixtureBotLevel -Bot $botValue
        if ($level -eq [int]$script:PvpFixtureContext.Definition.level) { $sameLevelCount++ }
        if ($online) { $onlineCount++ }
        $alive = if ($online) { [bool](Get-PvpFixtureProperty -Object $botValue -Name 'alive') } else { $false }
        if ($alive) { $aliveCount++ }
        $combat = if ($online) { [bool](Get-PvpFixtureProperty -Object $botValue -Name 'combat') } else { $false }
        if ($combat) { $combatCount++ }
        $point = Get-PvpFixturePoint -Bot $botValue
        $leaderPoint = $leaderPointBySide[$state.side]
        $rallyDistance = if ($null -ne $point -and $null -ne $leaderPoint) { Get-PvpFixtureDistance -Left $point -Right $leaderPoint } else { [double]::PositiveInfinity }
        if (-not [double]::IsInfinity($rallyDistance) -and -not [double]::IsNaN($rallyDistance)) { $rallyDistances += $rallyDistance }
        $targetGuid = Get-PvpFixtureTargetGuid -Bot $botValue
        $targetObserved = $targetGuid -ne 0 -and $opposingGuids.ContainsKey($targetGuid) -and $opposingGuids[$targetGuid] -ne $state.side
        if ($targetObserved) { $opposingTargetCount++ }
        $health = Get-PvpFixtureProperty -Object (Get-PvpFixtureProperty -Object $botValue -Name 'health') -Name 'pct'
        $target = Get-PvpFixtureProperty -Object $botValue -Name 'target'
        $position = Get-PvpFixtureProperty -Object $botValue -Name 'position'
        $botTelemetry += [ordered]@{
            stage_id = $Stage.stage_id
            phase = $Phase
            fixture_id = $script:PvpFixtureContext.Definition.fixture_id
            team_id = $state.team.team_id
            guid = [uint32]$state.guid
            character_name = $state.member.character_name
            online = $online
            alive = $alive
            combat = $combat
            paused = if ($online) { [bool](Get-PvpFixtureProperty -Object $botValue -Name 'paused') } else { $false }
            level = $level
            map = if ($null -eq $point) { $null } else { $point.map }
            position_x = if ($null -eq $point) { $null } else { $point.x }
            position_y = if ($null -eq $point) { $null } else { $point.y }
            position_z = if ($null -eq $point) { $null } else { $point.z }
            position_o = if ($null -eq $point) { $null } else { $point.o }
            health_pct = if ($null -eq $health) { $null } else { [double]$health }
            target_guid = if ($targetGuid -eq 0) { $null } else { [uint32]$targetGuid }
            target_name = if ($null -eq $target) { $null } else { Get-PvpFixtureProperty -Object $target -Name 'name' }
            target_health_pct = if ($null -eq $target) { $null } else { Get-PvpFixtureProperty -Object $target -Name 'health_pct' }
            group_members = if ($online) { Get-PvpFixtureGroupMembersCount -Bot $botValue } else { 0 }
            group_leader_guid = if ($online) { $leaderGuid = Get-PvpFixtureGroupLeaderGuid -Bot $botValue; if ($leaderGuid -eq 0) { $null } else { [uint32]$leaderGuid } } else { $null }
            rally_distance = if (-not [double]::IsInfinity($rallyDistance) -and -not [double]::IsNaN($rallyDistance)) { [math]::Round($rallyDistance, 3) } else { $null }
            opposing_target_observed = $targetObserved
        }
    }

    $maxRallyDistance = if ($rallyDistances.Count -gt 0) { [math]::Round((($rallyDistances | Measure-Object -Maximum).Maximum), 3) } else { $null }
    return [ordered]@{
        stage_id = $Stage.stage_id
        phase = $Phase
        fixture_id = $script:PvpFixtureContext.Definition.fixture_id
        samples = $botTelemetry
        aggregates = [ordered]@{
            online_stage_bots = $onlineCount
            alive_stage_bots = $aliveCount
            combat_stage_bots = $combatCount
            same_level_count = $sameLevelCount
            opposing_target_count = $opposingTargetCount
            rally_max_distance = $maxRallyDistance
            team_population = [ordered]@{
                alliance = @($States | Where-Object side -eq 'alliance').Count
                horde = @($States | Where-Object side -eq 'horde').Count
            }
        }
    }
}

function Invoke-PvpFixtureStageRally {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Stage
    )

    if ($Stage.kind -eq 'warsong_gulch') {
        throw "WSG 10v10 uses the dedicated $($Stage.proof_harness) atomic-roster harness."
    }

    if (-not [bool]$Stage.supported_start) {
        Add-PvpFixtureMetric -Name unsupported_stage_count
        Write-PvpFixtureReceipt -Event 'stage_blocked' -Payload @{
            stage_id = $Stage.stage_id
            status = 'blocked_unsupported_surface'
            reason = $Stage.start_surface
            mutation_performed = $false
        } | Out-Null
        return [pscustomobject]@{ status = 'blocked'; stage_id = $Stage.stage_id }
    }

    Ensure-PvpFixtureProvisionedForControl
    $states = Get-PvpFixtureStageStates -Stage $Stage
    $bots = Ensure-PvpFixtureBotsOnline -States $states
    foreach ($side in @('alliance', 'horde')) {
        $sideStates = @($states | Where-Object side -eq $side)
        $bots = Ensure-PvpFixtureParty -States $sideStates -Bots $bots
        $leader = @($sideStates | Select-Object -First 1)[0]
        if ($Stage.PSObject.Properties['rally_route'] -and -not [string]::IsNullOrWhiteSpace([string]$Stage.rally_route)) {
            Invoke-PvpFixtureBridgeAction -BridgeAction route -BotGuid $leader.guid -Destination ([string]$Stage.rally_route) | Out-Null
            Start-Sleep -Milliseconds 500
        }
        Invoke-PvpFixtureBridgeAction -BridgeAction rally -BotGuid $leader.guid | Out-Null
        Start-Sleep -Milliseconds 500
        $bots = Get-PvpFixtureBridgeBots
    }

    $bots = Get-PvpFixtureBridgeBots
    $telemetry = Get-PvpFixtureTelemetryPayload -Stage $Stage -States $states -Bots $bots -Phase 'rally'
    $maxDistance = $telemetry.aggregates.rally_max_distance
    if ($null -eq $maxDistance -or [double]$maxDistance -gt 0.5) {
        throw "Rally verification failed: maximum same-party distance was $maxDistance, expected <= 0.5."
    }
    $levels = @($telemetry.samples | ForEach-Object level | Sort-Object -Unique)
    if ($levels.Count -ne 1 -or $levels[0] -ne [int]$script:PvpFixtureContext.Definition.level) {
        throw 'Rally telemetry failed equal-level verification.'
    }
    if (@($telemetry.samples | Where-Object { $_.map -ne [int]$Stage.rally_anchor.map }).Count -gt 0) {
        throw "Rally telemetry found a bot outside the deterministic world anchor map $($Stage.rally_anchor.map)."
    }
    Set-PvpFixtureActiveMembership -States $states -Active $true
    Write-PvpFixtureReceipt -Event 'telemetry_sample' -Payload $telemetry | Out-Null
    Write-PvpFixtureReceipt -Event 'stage_rallied' -Payload @{
        stage_id = $Stage.stage_id
        status = 'verified'
        group_size_per_side = [int]$Stage.players_per_side
        rally_max_distance = $maxDistance
        map = [int]$Stage.rally_anchor.map
        active_member_guids = @($states | ForEach-Object { [uint32]$_.guid })
    } | Out-Null
    return [pscustomobject]@{ status = 'rallied'; stage_id = $Stage.stage_id; states = $states; bots = $bots }
}

function Invoke-PvpFixtureStageStop {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Stage
    )

    if ($Stage.kind -eq 'warsong_gulch') {
        throw "WSG 10v10 uses the dedicated $($Stage.proof_harness) atomic leave and cleanup path."
    }

    if (-not [bool]$Stage.supported_stop) {
        Add-PvpFixtureMetric -Name unsupported_stage_count
        Write-PvpFixtureReceipt -Event 'stage_blocked' -Payload @{
            stage_id = $Stage.stage_id
            status = 'blocked_unsupported_surface'
            reason = $Stage.stop_surface
            mutation_performed = $false
        } | Out-Null
        return [pscustomobject]@{ status = 'blocked'; stage_id = $Stage.stage_id }
    }

    Ensure-PvpFixtureProvisionedForControl
    $states = Get-PvpFixtureStageStates -Stage $Stage
    $bots = Get-PvpFixtureBridgeBots
    $onlineStates = @($states | Where-Object { @(Get-PvpFixtureBridgeBot -Bots $bots -Guid $_.guid).Count -eq 1 })
    foreach ($state in $onlineStates) {
        Invoke-PvpFixtureBridgeAction -BridgeAction pause -BotGuid $state.guid | Out-Null
    }
    if ($onlineStates.Count -gt 0) { Start-Sleep -Milliseconds 500 }
    $bots = Get-PvpFixtureBridgeBots
    $unpaused = @($onlineStates | Where-Object {
        $bot = @(Get-PvpFixtureBridgeBot -Bots $bots -Guid $_.guid)
        $bot.Count -eq 1 -and -not [bool](Get-PvpFixtureProperty -Object $bot[0] -Name 'paused')
    })
    if ($unpaused.Count -gt 0) {
        throw "Stop verification failed: bridge did not report paused=true for $(@($unpaused | ForEach-Object { $_.member.character_name }) -join ', ')"
    }
    foreach ($state in $onlineStates) {
        if (@(Get-PvpFixtureBridgeBot -Bots $bots -Guid $state.guid).Count -eq 1) {
            Invoke-PvpFixtureBridgeAction -BridgeAction deactivate -BotGuid $state.guid | Out-Null
        }
    }

    $deadline = (Get-Date).AddSeconds($script:PvpFixtureContext.WaitSeconds)
    do {
        $bots = Get-PvpFixtureBridgeBots
        $remaining = @($states | Where-Object { @(Get-PvpFixtureBridgeBot -Bots $bots -Guid $_.guid).Count -eq 1 })
        if ($remaining.Count -eq 0) { break }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    if ($remaining.Count -ne 0) {
        throw "Stop verification failed; exact fixture bots still online: $(@($remaining | ForEach-Object { $_.member.character_name }) -join ', ')"
    }

    Set-PvpFixtureActiveMembership -States $states -Active $false
    Write-PvpFixtureReceipt -Event 'stage_stopped' -Payload @{
        stage_id = $Stage.stage_id
        status = 'verified'
        paused_stage_bots = $onlineStates.Count - $unpaused.Count
        deactivated_stage_bots = $onlineStates.Count
        online_stage_bots = 0
        stop_surface = $Stage.stop_surface
    } | Out-Null
    return [pscustomobject]@{ status = 'stopped'; stage_id = $Stage.stage_id }
}

function Invoke-PvpFixtureStageStart {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Stage
    )

    if ($Stage.kind -eq 'warsong_gulch') {
        throw "WSG 10v10 uses the dedicated $($Stage.proof_harness) atomic queue and telemetry path."
    }

    if (-not [bool]$Stage.supported_start) {
        Add-PvpFixtureMetric -Name unsupported_stage_count
        Write-PvpFixtureReceipt -Event 'stage_blocked' -Payload @{
            stage_id = $Stage.stage_id
            status = 'blocked_unsupported_surface'
            reason = $Stage.start_surface
            stop_surface = $Stage.stop_surface
            mutation_performed = $false
        } | Out-Null
        return [pscustomobject]@{ status = 'blocked'; stage_id = $Stage.stage_id }
    }

    $rally = Invoke-PvpFixtureStageRally -Stage $Stage
    if ($rally.status -ne 'rallied') { throw "World PvP start cannot continue after rally status $($rally.status)." }
    $states = @($rally.states)
    foreach ($side in @('alliance', 'horde')) {
        $leader = @($states | Where-Object side -eq $side | Select-Object -First 1)[0]
        $opposingSide = if ($side -eq 'alliance') { 'horde' } else { 'alliance' }
        $targetLeader = @($states | Where-Object side -eq $opposingSide | Select-Object -First 1)[0]
        Invoke-PvpFixtureBridgeAction -BridgeAction engage -BotGuid $leader.guid `
            -TargetPlayerGuid $targetLeader.guid | Out-Null
    }
    Write-PvpFixtureReceipt -Event 'stage_started' -Payload @{
        stage_id = $Stage.stage_id
        status = 'dispatched'
        start_surface = $Stage.start_surface
        leader_guids = @($states | Group-Object side | ForEach-Object { [uint32](@($_.Group | Select-Object -First 1)[0].guid) })
    } | Out-Null

    $samplePayloads = @()
    $maxOpposingTargets = 0
    for ($sampleIndex = 1; $sampleIndex -le $script:PvpFixtureContext.SampleCount; $sampleIndex++) {
        $bots = Get-PvpFixtureBridgeBots
        $telemetry = Get-PvpFixtureTelemetryPayload -Stage $Stage -States $states -Bots $bots -Phase "start-sample-$sampleIndex"
        $samplePayloads += $telemetry
        $opposingTargets = [int]$telemetry.aggregates.opposing_target_count
        if ($opposingTargets -gt $maxOpposingTargets) { $maxOpposingTargets = $opposingTargets }
        Write-PvpFixtureReceipt -Event 'telemetry_sample' -Payload $telemetry | Out-Null
        if ($sampleIndex -lt $script:PvpFixtureContext.SampleCount -and $script:PvpFixtureContext.SampleIntervalSeconds -gt 0) {
            Start-Sleep -Seconds $script:PvpFixtureContext.SampleIntervalSeconds
        }
    }

    $accepted = $maxOpposingTargets -ge 1
    Write-PvpFixtureReceipt -Event 'stage_acceptance' -Payload @{
        stage_id = $Stage.stage_id
        status = if ($accepted) { 'accepted' } else { 'not_accepted' }
        opposing_target_observed = $maxOpposingTargets
        required = 'opposing_target_guid_observed >= 1'
        sample_count = $samplePayloads.Count
    } | Out-Null
    if (-not $accepted) {
        Invoke-PvpFixtureStageStop -Stage $Stage | Out-Null
        throw 'World PvP start was dispatched but acceptance telemetry did not observe an opposing fixture target GUID.'
    }
    return [pscustomobject]@{ status = 'accepted'; stage_id = $Stage.stage_id; opposing_target_count = $maxOpposingTargets }
}

function Invoke-PvpFixtureStatus {
    [CmdletBinding()]
    param()

    if (-not $script:PvpFixtureContext.Execute) { throw 'Status execution requires -Execute when it queries live local services.' }
    $script:PvpFixtureContext.Database = Get-PvpFixtureDatabaseContext
    $rows = Get-PvpFixtureAccountAndCharacterRows -Definition $script:PvpFixtureContext.Definition
    $scope = Assert-PvpFixtureAccountAndCharacterScope -Definition $script:PvpFixtureContext.Definition -Rows $rows
    $status = [ordered]@{
        account_count = @($rows.accounts).Count
        character_count = @($scope.characters_by_name.Keys).Count
        missing_account_count = @($scope.missing_accounts).Count
        missing_character_count = @($scope.missing_characters).Count
        bridge_status = 'not_queried'
    }
    if ($scope.characters_by_name.Count -eq @($script:PvpFixtureContext.Definition.accounts).Count) {
        $mapping = @{}
        foreach ($name in $scope.characters_by_name.Keys) { $mapping[$name] = [int]$scope.characters_by_name[$name].guid }
        $script:PvpFixtureContext.CharacterGuidByName = $mapping
        $membershipRows = Get-PvpFixtureMembershipRows -Definition $script:PvpFixtureContext.Definition -CharacterGuidByName $mapping
        $status.team_count = @($membershipRows.teams).Count
        $status.member_count = @($membershipRows.members).Count
        if (@(Get-PvpFixtureWorldserverProcesses).Count -gt 0) {
            try {
                $bots = Get-PvpFixtureBridgeBots
                $fixtureGuids = @($mapping.Values | ForEach-Object { [uint32]$_ })
                $fixtureBots = @($bots | Where-Object { $fixtureGuids -contains [uint32](Get-PvpFixtureProperty -Object $_ -Name 'guid') })
                $status.bridge_status = 'queried'
                $status.online_fixture_bot_count = $fixtureBots.Count
                $status.online_fixture_bot_guids = @($fixtureBots | ForEach-Object { [uint32](Get-PvpFixtureProperty -Object $_ -Name 'guid') })
            }
            catch {
                $status.bridge_status = 'error'
                $status.bridge_error = $_.Exception.Message
            }
        }
    }
    Write-PvpFixtureReceipt -Event 'status_snapshot' -Payload $status | Out-Null
    return $status
}

function Invoke-PvpFixtureCleanup {
    [CmdletBinding()]
    param()

    if (-not $script:PvpFixtureContext.Execute) { throw 'Cleanup requires -Execute.' }
    if (-not $script:PvpFixtureContext.ConfirmFixtureCleanup) {
        throw 'Cleanup requires -ConfirmFixtureCleanup in addition to -Execute.'
    }
    $definition = $script:PvpFixtureContext.Definition
    $script:PvpFixtureContext.Database = Get-PvpFixtureDatabaseContext
    $rows = Get-PvpFixtureAccountAndCharacterRows -Definition $definition
    $scope = Assert-PvpFixtureAccountAndCharacterScope -Definition $definition -Rows $rows
    if (@($scope.missing_accounts).Count -ne 0 -or @($scope.missing_characters).Count -ne 0) {
        throw 'Cleanup requires all ten exact fixture accounts and characters to exist; refusing partial cleanup.'
    }

    $mapping = @{}
    foreach ($name in $scope.characters_by_name.Keys) { $mapping[$name] = [int]$scope.characters_by_name[$name].guid }
    $script:PvpFixtureContext.CharacterGuidByName = $mapping
    Get-PvpFixturePlayerbotsSchemaState | Out-Null
    $membershipRows = Get-PvpFixtureMembershipRows -Definition $definition -CharacterGuidByName $mapping
    Assert-PvpFixtureMembershipScope -Definition $definition -CharacterGuidByName $mapping -Rows $membershipRows | Out-Null
    if (@($membershipRows.members).Count -ne 20 -or @($membershipRows.teams).Count -ne 2) {
        throw 'Cleanup scope did not resolve exactly two fixture teams and twenty fixture member rows.'
    }

    if (@(Get-PvpFixtureWorldserverProcesses).Count -gt 0) {
        $bots = Get-PvpFixtureBridgeBots
        $fixtureGuids = @($mapping.Values | ForEach-Object { [uint32]$_ })
        $online = @($bots | Where-Object { $fixtureGuids -contains [uint32](Get-PvpFixtureProperty -Object $_ -Name 'guid') })
        if ($online.Count -gt 0) {
            throw 'Cleanup refuses to delete fixture rows while exact fixture bots are online; stop the fixture first.'
        }
    }

    $guids = @($mapping.Values | ForEach-Object { [int]$_ })
    $teamIds = @($definition.teams | ForEach-Object team_id)
    Write-PvpFixtureReceipt -Event 'cleanup_scope_verified' -Payload @{
        account_names = @($definition.accounts | ForEach-Object account_name)
        character_names = @($definition.accounts | ForEach-Object character_name)
        character_guids = $guids
        team_ids = $teamIds
        unrelated_mutation_count = 0
    } | Out-Null
    $cleanupSql = New-PvpFixtureCleanupSql -PlayerbotsDatabaseName $script:PvpFixtureContext.Database.playerbots.database -CharactersDatabaseName $script:PvpFixtureContext.Database.characters.database -CharacterGuids $guids -TeamIds $teamIds
    Write-PvpFixtureReceipt -Event 'cleanup_sql' -Payload @{
        mutation = 'exact fixture team ids, exact fixture character_guid values, and no other predicates'
        character_guids = $guids
        team_ids = $teamIds
    } | Out-Null
    $null = Invoke-PvpFixtureSql -Database $script:PvpFixtureContext.Database.playerbots -Sql $cleanupSql -Mutation
    Invoke-PvpFixtureAccountConsoleBatch -MissingAccounts @($definition.accounts) -Mode delete

    $after = Get-PvpFixtureAccountAndCharacterRows -Definition $definition
    if (@($after.accounts).Count -ne 0 -or @($after.characters).Count -ne 0) {
        throw 'Cleanup verification found one or more exact fixture accounts or characters still present.'
    }
    Write-PvpFixtureReceipt -Event 'cleanup_completed' -Payload @{
        status = 'verified'
        deleted_account_count = 20
        deleted_character_count = 20
        deleted_member_count = 20
        deleted_team_count = 2
        unrelated_mutation_count = 0
    } | Out-Null
}

function Invoke-PvpFixtureRun {
    [CmdletBinding()]
    param()

    Ensure-PvpFixtureProvisionedForControl
    if (@(Get-PvpFixtureWorldserverProcesses).Count -eq 0) {
        throw 'Run requires the already-running local worldserver; the fixture never starts deployment processes.'
    }
    $worldStage = Get-PvpFixtureStage -StageId 'world-pvp-2v2'
    $startError = $null
    try {
        $null = Invoke-PvpFixtureStageStart -Stage $worldStage
    }
    catch {
        $startError = $_
    }
    finally {
        try { $null = Invoke-PvpFixtureStageStop -Stage $worldStage } catch { if ($null -eq $startError) { $startError = $_ } }
    }
    if ($null -ne $startError) { throw $startError }

    $ladderStage = Get-PvpFixtureStage -StageId 'ladder-3v3'
    $wsgStage = Get-PvpFixtureStage -StageId 'wsg-10v10'
    $ladderResult = Invoke-PvpFixtureStageStart -Stage $ladderStage
    $wsgResult = [pscustomobject]@{ status = 'dedicated_harness_required'; proof_harness = $wsgStage.proof_harness }
    return [pscustomobject]@{
        world_pvp = 'accepted_and_stopped'
        ladder_3v3 = $ladderResult.status
        wsg_10v10 = $wsgResult.status
    }
}

function Initialize-PvpFixtureContext {
    [CmdletBinding()]
    param()

    $resolvedRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
    $logsRoot = Join-Path $resolvedRoot 'logs'
    if (-not (Test-Path -LiteralPath $logsRoot)) {
        New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null
    }
    $definition = Get-PvpFixtureDefinition -FixtureId $FixtureId -Level $Level
    Assert-PvpFixtureDefinition -Definition $definition | Out-Null
    $runId = "$($definition.tag)-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
    $context = [pscustomobject]@{
        ServerRoot = $resolvedRoot
        LogsRoot = $logsRoot
        Definition = $definition
        Action = $Action
        Stage = $Stage
        Execute = [bool]$Execute
        ConfirmFixtureCleanup = [bool]$ConfirmFixtureCleanup
        BridgePort = 18787
        BridgeTimeoutMs = $BridgeTimeoutMs
        WaitSeconds = $WaitSeconds
        SampleCount = $SampleCount
        SampleIntervalSeconds = $SampleIntervalSeconds
        RunId = $runId
        Database = $null
        CharacterGuidByName = $null
    }
    $script:PvpFixtureContext = $context
    $path = if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
        Join-Path $logsRoot "pvp-fixture-$($definition.tag)-$runId.jsonl"
    }
    else {
        Get-PvpFixtureFullPath -Path $ReceiptPath
    }
    $path = Assert-PvpFixtureReceiptPath -Path $path
    $script:PvpFixtureReceiptState = [pscustomobject]@{ Path = $path; RunId = $runId; Sequence = 0 }

    Write-PvpFixtureReceipt -Event 'run_started' -Payload @{
        status = 'started'
        local_only = $true
        execute = [bool]$Execute
        action = $Action
        stage = $Stage
        server_root = $resolvedRoot
        receipt_path = $path
    } | Out-Null
    Write-PvpFixtureReceipt -Event 'fixture_definition' -Payload @{
        fixture_id = $definition.fixture_id
        tag = $definition.tag
        level = [int]$definition.level
        account_names = @($definition.accounts | ForEach-Object account_name)
        character_names = @($definition.accounts | ForEach-Object character_name)
        team_ids = @($definition.teams | ForEach-Object team_id)
        stage_order = @($definition.stages | Sort-Object ordinal | ForEach-Object stage_id)
        local_only = $true
    } | Out-Null
    Write-PvpFixtureReceipt -Event 'acceptance_contract' -Payload (Get-PvpFixtureAcceptanceContract -Definition $definition)
    Write-PvpFixtureReceipt -Event 'planned_execution' -Payload (Get-PvpFixturePlan -Definition $definition) | Out-Null
}

try {
    Initialize-PvpFixtureContext
    $readOnlyPlan = $Action -eq 'plan' -or -not $Execute
    if ($readOnlyPlan) {
        Write-PvpFixtureReceipt -Event 'dry_run_guard' -Payload @{
            status = 'not_executed'
            reason = if ($Action -eq 'plan') { 'plan action never mutates or connects' } else { 'explicit -Execute switch was not supplied' }
            requested_action = $Action
            requested_stage = $Stage
            would_execute = $Action -ne 'plan'
        } | Out-Null
        Write-PvpFixtureReceipt -Event 'run_completed' -Payload @{
            status = if ($Action -eq 'plan') { 'plan_only' } else { 'dry_run_only' }
            live_execution = $false
            acceptance_status = 'not_run'
            metrics = Get-PvpFixtureMetricsSnapshot
        }
        exit 0
    }

    switch ($Action) {
        'provision' { Invoke-PvpFixtureProvisioning | Out-Null }
        'status' { Invoke-PvpFixtureStatus | Out-Null }
        'rally' {
            $rallyStage = Get-PvpFixtureStage -StageId $Stage
            if ([bool]$rallyStage.supported_start) { Ensure-PvpFixtureProvisionedForControl }
            Invoke-PvpFixtureStageRally -Stage $rallyStage | Out-Null
        }
        'start' {
            $startStage = Get-PvpFixtureStage -StageId $Stage
            if ([bool]$startStage.supported_start) { Ensure-PvpFixtureProvisionedForControl }
            Invoke-PvpFixtureStageStart -Stage $startStage | Out-Null
        }
        'stop' {
            $stopStage = Get-PvpFixtureStage -StageId $Stage
            if ([bool]$stopStage.supported_stop) { Ensure-PvpFixtureProvisionedForControl }
            Invoke-PvpFixtureStageStop -Stage $stopStage | Out-Null
        }
        'run' { Invoke-PvpFixtureRun | Out-Null }
        'cleanup' { Invoke-PvpFixtureCleanup }
        default { throw "Unhandled fixture action: $Action" }
    }
    Write-PvpFixtureReceipt -Event 'run_completed' -Payload @{
        status = if ($Action -in @('rally', 'start', 'stop') -and $Stage -eq 'ladder-3v3') { 'blocked_unsupported_surface' } else { 'completed' }
        live_execution = $true
        acceptance_status = if ($Action -eq 'run') { 'world_pvp_then_explicit_stage_blocks' } else { 'action_completed' }
        metrics = Get-PvpFixtureMetricsSnapshot
    } | Out-Null
    Write-Output (Get-Content -LiteralPath $script:PvpFixtureReceiptState.Path | Select-Object -Last 1)
}
catch {
    $message = $_.Exception.Message
    if ($null -ne $script:PvpFixtureReceiptState) {
        try {
            Write-PvpFixtureReceipt -Event 'run_failed' -Payload @{
                status = 'failed_closed'
                error = $message
                metrics = Get-PvpFixtureMetricsSnapshot
            } | Out-Null
            Write-PvpFixtureReceipt -Event 'run_completed' -Payload @{
                status = 'failed_closed'
                live_execution = [bool]$Execute
                acceptance_status = 'failed_closed'
                metrics = Get-PvpFixtureMetricsSnapshot
            } | Out-Null
        }
        catch { }
    }
    throw
}
