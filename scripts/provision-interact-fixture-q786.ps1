<#
.SYNOPSIS
    Provisions the two dedicated, fresh Horde characters used by the q786 interaction fixture.

.DESCRIPTION
    Dry-runs by default. -Apply requires the normal worldserver and AutoWoW bridge to be
    stopped. Account creation uses only WOW_ACCOUNT_PASSWORD from the environment and a
    transient local worldserver console. Direct character bootstrap is protected by a
    named MySQL lock and inserts only the exact character and homebind rows. The script
    never deletes or rewrites account/character data, never writes inventory or quest
    state, and never starts, stops, or restarts the normal server.

    Successful Apply adds only the two resolved GUIDs to AutoWow.FixtureGuids, enrolls
    only those GUIDs in autowow_league_member, and emits an exact JSON handoff manifest.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ManifestPath = '',
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$BridgePort = 18787,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Q786ProvisionSchema = 'autowow.q786.fixture-provision.definition.v1'
$script:Q786ManifestSchema = 'autowow.q786.fixture-provision.manifest.v1'
$script:Q786FixtureId = 'q786-dedicated-v1'
$script:Q786DatabaseLockName = 'autowow.q786.fixture-provision.v1'

function Get-Q786ProvisionDefinition {
    [CmdletBinding()]
    param()

    $members = @(
        [pscustomobject][ordered]@{
            ordinal = 1
            role = 'owner'
            account_name = 'AWQ786OWNER'
            character_name = 'Kolkarown'
            faction = 'Horde'
            race_name = 'Orc'
            race_id = 2
            class_name = 'Warrior'
            class_id = 1
            gender = 0
            bootstrap_level = 1
            fixture_init_level = 8
            fixture_init_spec_index = 0
            fixture_init_quality = 2
            position = [pscustomobject][ordered]@{
                area = 'Valley of Trials, Durotar'
                map = 1
                zone = 14
                x = -618.518
                y = -4251.67
                z = 38.718
                o = 0.0
            }
            league = [pscustomobject][ordered]@{
                team_id = $null
                affiliation = 'wayfarer'
                role = 'adventurer'
                class_plan = 'q786-owner'
                active = 1
            }
        },
        [pscustomobject][ordered]@{
            ordinal = 2
            role = 'helper'
            account_name = 'AWQ786HELPER'
            character_name = 'Kolkarhelp'
            faction = 'Horde'
            race_name = 'Orc'
            race_id = 2
            class_name = 'Shaman'
            class_id = 7
            gender = 0
            bootstrap_level = 1
            fixture_init_level = 7
            fixture_init_spec_index = 0
            fixture_init_quality = 2
            position = [pscustomobject][ordered]@{
                area = 'Valley of Trials, Durotar'
                map = 1
                zone = 14
                x = -618.518
                y = -4251.67
                z = 38.718
                o = 0.0
            }
            league = [pscustomobject][ordered]@{
                team_id = $null
                affiliation = 'wayfarer'
                role = 'adventurer'
                class_plan = 'q786-helper'
                active = 1
            }
        }
    )

    $definition = [pscustomobject][ordered]@{
        schema = $script:Q786ProvisionSchema
        schema_version = 1
        fixture_id = $script:Q786FixtureId
        quest_id = 786
        quest_title = 'Thwarting Kolkar Aggression'
        local_only = $true
        disposable = $true
        account_password_env = 'WOW_ACCOUNT_PASSWORD'
        database_lock = $script:Q786DatabaseLockName
        ordered_members = $members
    }
    Assert-Q786ProvisionDefinition -Definition $definition | Out-Null
    return $definition
}

function Assert-Q786ProvisionDefinition {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Definition)

    if ($Definition.schema -ne $script:Q786ProvisionSchema -or
        $Definition.fixture_id -ne $script:Q786FixtureId -or
        [int]$Definition.quest_id -ne 786 -or
        $Definition.local_only -ne $true -or
        $Definition.disposable -ne $true) {
        throw 'Unexpected q786 provisioning identity or scope.'
    }

    $members = @($Definition.ordered_members)
    if ($members.Count -ne 2) { throw "q786 provisioning requires exactly two members; found $($members.Count)." }
    if (($members.role -join ',') -ne 'owner,helper' -or ($members.ordinal -join ',') -ne '1,2') {
        throw 'q786 provisioning requires exact owner-then-helper order.'
    }
    if (($members.account_name -join ',') -ne 'AWQ786OWNER,AWQ786HELPER' -or
        ($members.character_name -join ',') -ne 'Kolkarown,Kolkarhelp') {
        throw 'q786 provisioning identities are immutable and exact.'
    }

    $validClassesByRace = @{
        2 = @(1, 3, 4, 7, 9)
        5 = @(1, 4, 5, 8, 9)
        6 = @(1, 3, 7, 11)
        8 = @(1, 3, 4, 5, 7, 8)
        10 = @(2, 3, 4, 5, 8, 9)
    }
    foreach ($member in $members) {
        if ($member.account_name -notmatch '^AWQ786(OWNER|HELPER)$') { throw "Unsafe account identity: $($member.account_name)" }
        if ($member.character_name -notmatch '^[A-Z][a-z]{2,11}$') { throw "Client-invalid character name: $($member.character_name)" }
        if ($member.faction -ne 'Horde' -or [int]$member.race_id -notin @(2, 5, 6, 8, 10)) {
            throw "q786 member $($member.character_name) is not a valid Horde race."
        }
        if ([int]$member.class_id -notin $validClassesByRace[[int]$member.race_id]) {
            throw "Wrath-invalid race/class pair for $($member.character_name)."
        }
        if ([int]$member.bootstrap_level -ne 1) { throw "q786 bootstrap must leave $($member.character_name) at level 1." }
        if ([int]$member.position.map -ne 1 -or [int]$member.position.zone -ne 14 -or
            [Math]::Abs([double]$member.position.x - (-618.518)) -gt 0.0001 -or
            [Math]::Abs([double]$member.position.y - (-4251.67)) -gt 0.0001 -or
            [Math]::Abs([double]$member.position.z - 38.718) -gt 0.0001) {
            throw "q786 member $($member.character_name) is not at the exact Durotar bootstrap position."
        }
        if ($null -ne $member.league.team_id -or $member.league.affiliation -ne 'wayfarer' -or
            $member.league.role -ne 'adventurer' -or [int]$member.league.active -ne 1 -or
            $member.league.class_plan -ne "q786-$($member.role)") {
            throw "q786 league enrollment is not exact for $($member.character_name)."
        }
    }
    return $true
}

function Get-Q786ProvisionPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Definition)

    Assert-Q786ProvisionDefinition -Definition $Definition | Out-Null
    return [pscustomobject][ordered]@{
        schema = 'autowow.q786.fixture-provision.plan.v1'
        schema_version = 1
        fixture_id = $Definition.fixture_id
        status = 'DRY_RUN'
        dry_run = $true
        apply = $false
        exact_scope = [pscustomobject][ordered]@{
            account_count = 2
            character_count = 2
            accounts = @($Definition.ordered_members | ForEach-Object { $_.account_name })
            characters = @($Definition.ordered_members | ForEach-Object { $_.character_name })
            faction = 'Horde'
            quest_id = 786
        }
        account_bootstrap = [pscustomobject][ordered]@{
            password_source = 'WOW_ACCOUNT_PASSWORD environment variable only'
            password_emitted_or_persisted = $false
            commands = @($Definition.ordered_members | ForEach-Object {
                ".account create $($_.account_name) <WOW_ACCOUNT_PASSWORD>"
            })
        }
        character_bootstrap = @($Definition.ordered_members | ForEach-Object {
            [pscustomobject][ordered]@{
                role = $_.role
                account_name = $_.account_name
                character_name = $_.character_name
                race = "$($_.race_name) ($($_.race_id))"
                class = "$($_.class_name) ($($_.class_id))"
                level_before_fixture_init = [int]$_.bootstrap_level
                position = $_.position
                inventory_rows = 0
                quest_log_rows = 0
                mutation = 'named-lock protected insert-if-absent into characters and character_homebind only'
            }
        })
        controlled_setup = @(
            'append only the two resolved GUIDs to AutoWow.FixtureGuids',
            'insert only the two resolved GUIDs into autowow_league_member',
            'emit one exact non-overwriting JSON manifest containing owner and helper GUIDs'
        )
        lifecycle = [pscustomobject][ordered]@{
            apply_requires_normal_worldserver_stopped = $true
            apply_requires_bridge_stopped = $true
            normal_server_stop_by_script = $false
            normal_server_start_or_restart_by_script = $false
            transient_account_console_only_if_accounts_are_missing = $true
            fixture_init_performed = $false
        }
        forbidden_actions = @(
            'account or character deletion',
            'existing account or character rewrite',
            'inventory write',
            'quest-state write',
            'normal server stop, start, or restart',
            'fixture-init',
            'unrelated config or league mutation'
        )
        live_mutations_executed = 0
    }
}

function ConvertTo-Q786SqlIdentifier {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Value)
    if ($Value -notmatch '^[A-Za-z0-9_]+$') { throw "Unsafe SQL identifier: $Value" }
    return ('`{0}`' -f $Value)
}

function ConvertTo-Q786SqlLiteral {
    [CmdletBinding()]
    param([AllowEmptyString()][string]$Value)
    return "'" + $Value.Replace("'", "''") + "'"
}

function ConvertTo-Q786SqlNumber {
    [CmdletBinding()]
    param([Parameter(Mandatory)][double]$Value)
    return $Value.ToString('0.###############', [Globalization.CultureInfo]::InvariantCulture)
}

function Assert-Q786LocalAddress {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Address)
    if ($Address.Trim().ToLowerInvariant() -notin @('127.0.0.1', 'localhost', '::1')) {
        throw "q786 provisioning refuses non-local endpoint $Address."
    }
}

function Get-Q786ConfigConnection {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigPath,
        [Parameter(Mandatory)][string]$Key
    )

    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { throw "Required configuration is missing: $ConfigPath" }
    $escapedKey = [regex]::Escape($Key)
    $lines = @(Get-Content -LiteralPath $ConfigPath | Where-Object { $_ -match "^\s*$escapedKey\s*=" })
    if ($lines.Count -ne 1) { throw "Expected exactly one $Key entry in $ConfigPath; found $($lines.Count)." }
    $value = (($lines[0] -split '=', 2)[1]).Trim().Trim('"')
    $parts = @($value -split ';', 5)
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    Assert-Q786LocalAddress -Address $parts[0].Trim()
    $port = 0
    if (-not [int]::TryParse($parts[1].Trim(), [ref]$port) -or $port -lt 1 -or $port -gt 65535) {
        throw "$Key has an invalid database port."
    }
    foreach ($safeToken in @($parts[2], $parts[4])) {
        if ($safeToken.Trim() -notmatch '^[A-Za-z0-9_.$-]+$') { throw "$Key contains an unsafe database token." }
    }
    return [pscustomobject]@{
        host = $parts[0].Trim()
        port = $port
        user = $parts[2].Trim()
        password = $parts[3]
        database = $parts[4].Trim()
    }
}

function Get-Q786DatabaseContext {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Root)

    $mysqlCandidates = @(
        (Join-Path $Root 'third_party\mysql\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
    )
    $mysql = @($mysqlCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1)
    if ($mysql.Count -ne 1) { throw 'MySQL CLI was not found.' }
    $worldConfig = Join-Path $Root 'server\configs\worldserver.conf'
    $playerbotsConfig = Join-Path $Root 'server\configs\modules\playerbots.conf'
    return [pscustomobject]@{
        mysql = $mysql[0]
        login = Get-Q786ConfigConnection -ConfigPath $worldConfig -Key 'LoginDatabaseInfo'
        characters = Get-Q786ConfigConnection -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
        playerbots = Get-Q786ConfigConnection -ConfigPath $playerbotsConfig -Key 'PlayerbotsDatabaseInfo'
    }
}

function Assert-Q786SqlSafety {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Sql,
        [Parameter(Mandatory)][ValidateSet('Read', 'CharacterInsert', 'LeagueEnrollment')][string]$Operation
    )

    if ($Sql -match '(?i)\b(DELETE|UPDATE|REPLACE|DROP|TRUNCATE|ALTER|CREATE|RENAME)\b') {
        throw "q786 $Operation SQL contains a forbidden rewrite or destructive statement."
    }
    switch ($Operation) {
        'Read' {
            if ($Sql.TrimStart() -notmatch '^(?i)SELECT\b' -or $Sql -match '(?i)\b(INSERT|SET|START\s+TRANSACTION|COMMIT)\b') {
                throw 'q786 read SQL is not read-only.'
            }
        }
        'CharacterInsert' {
            if ($Sql -match '(?i)\b(character_inventory|character_queststatus(?:_rewarded|_daily|_weekly|_monthly)?)\b') {
                throw 'q786 character bootstrap refuses inventory and quest-state writes.'
            }
            $insertTargets = @([regex]::Matches($Sql, '(?i)INSERT\s+INTO\s+[^.\s]+\.([A-Za-z0-9_]+)') |
                ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
            if (($insertTargets -join ',') -ne 'character_homebind,characters') {
                throw "q786 character bootstrap has unexpected insert targets: $($insertTargets -join ',')."
            }
        }
        'LeagueEnrollment' {
            $insertTargets = @([regex]::Matches($Sql, '(?i)INSERT\s+INTO\s+[^.\s]+\.([A-Za-z0-9_]+)') |
                ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
            if (($insertTargets -join ',') -ne 'autowow_league_member') {
                throw "q786 league setup has unexpected insert targets: $($insertTargets -join ',')."
            }
            if ($Sql -match '(?i)\b(character_inventory|character_queststatus)\b') {
                throw 'q786 league setup refuses inventory and quest-state writes.'
            }
        }
    }
    return $true
}

function Invoke-Q786Sql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$DatabaseContext,
        [Parameter(Mandatory)][psobject]$Database,
        [Parameter(Mandatory)][string]$Sql,
        [Parameter(Mandatory)][ValidateSet('Read', 'CharacterInsert', 'LeagueEnrollment')][string]$Operation,
        [Parameter(Mandatory)][switch]$ApplyConfirmed
    )

    if (-not $ApplyConfirmed) { throw 'q786 database access requires explicit -Apply.' }
    Assert-Q786SqlSafety -Sql $Sql -Operation $Operation | Out-Null
    $oldPassword = $env:MYSQL_PWD
    $env:MYSQL_PWD = $Database.password
    $raw = @()
    $exitCode = -1
    try {
        $arguments = @(
            '--protocol=tcp', "--host=$($Database.host)", "--port=$($Database.port)", "--user=$($Database.user)",
            '--default-character-set=utf8mb4', '--batch', '--skip-column-names', "--database=$($Database.database)", "--execute=$Sql"
        )
        $raw = @(& $DatabaseContext.mysql @arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPassword) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }
        else { $env:MYSQL_PWD = $oldPassword }
    }
    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object {
        $_ -and $_ -notmatch '^mysql: (Unknown OS character set|Switching to the default character set)'
    })
    if ($exitCode -ne 0) {
        throw "q786 database $Operation failed; database output was withheld to avoid credential disclosure."
    }
    return $lines
}

function ConvertFrom-Q786TabRows {
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [Parameter(Mandatory)][string[]]$Columns
    )
    $rows = @()
    foreach ($line in $Lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $fields = @([regex]::Split($line, "`t"))
        if ($fields.Count -ne $Columns.Count) { throw "Unexpected q786 database row width: $line" }
        $row = [ordered]@{}
        for ($index = 0; $index -lt $Columns.Count; $index++) { $row[$Columns[$index]] = $fields[$index] }
        $rows += [pscustomobject]$row
    }
    return @($rows)
}

function Get-Q786WorldserverProcesses {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Root)

    $serverPrefix = [IO.Path]::GetFullPath((Join-Path $Root 'server')).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $matches = @()
    foreach ($candidate in @(Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $candidate.MainModule.FileName } catch { }
        if ([string]::IsNullOrWhiteSpace($path)) {
            throw "Could not prove the executable path for worldserver process $($candidate.Id); provisioning fails closed."
        }
        if ([IO.Path]::GetFullPath($path).StartsWith($serverPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            $matches += $candidate
        }
    }
    return @($matches)
}

function Test-Q786ProvisionBridgeReady {
    [CmdletBinding()]
    param(
        [ValidateSet('127.0.0.1')][string]$HostName = '127.0.0.1',
        [ValidateRange(1, 65535)][int]$Port = 18787,
        [ValidateRange(100, 2000)][int]$TimeoutMs = 400
    )
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync($HostName, $Port)
        return ($connect.Wait($TimeoutMs) -and $client.Connected)
    }
    catch { return $false }
    finally { $client.Dispose() }
}

function Assert-Q786ProvisioningOffline {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Root,
        [ValidateSet('127.0.0.1')][string]$HostName = '127.0.0.1',
        [ValidateRange(1, 65535)][int]$Port = 18787
    )
    if (@(Get-Q786WorldserverProcesses -Root $Root).Count -ne 0) {
        throw 'q786 provisioning requires the normal worldserver to be stopped; this script will not stop or restart it.'
    }
    if (Test-Q786ProvisionBridgeReady -HostName $HostName -Port $Port) {
        throw 'q786 provisioning requires the AutoWoW bridge to be stopped; this script will not stop or restart its server.'
    }
    return $true
}

function Invoke-Q786AccountBootstrap {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object[]]$MissingMembers,
        [Parameter(Mandatory)][string]$Root,
        [ValidateSet('127.0.0.1')][string]$HostName = '127.0.0.1',
        [ValidateRange(1, 65535)][int]$Port = 18787,
        [Parameter(Mandatory)][switch]$ApplyConfirmed
    )

    if (-not $ApplyConfirmed) { throw 'q786 account creation requires explicit -Apply.' }
    $members = @($MissingMembers)
    if ($members.Count -lt 1 -or $members.Count -gt 2 -or
        @($members.account_name | Sort-Object -Unique).Count -ne $members.Count -or
        @($members | Where-Object {
            "$($_.role)|$($_.account_name)|$($_.character_name)" -notin @(
                'owner|AWQ786OWNER|Kolkarown',
                'helper|AWQ786HELPER|Kolkarhelp'
            )
        }).Count -ne 0) {
        throw 'q786 account bootstrap received an out-of-scope identity.'
    }
    Assert-Q786ProvisioningOffline -Root $Root -HostName $HostName -Port $Port | Out-Null
    $password = $env:WOW_ACCOUNT_PASSWORD
    if ([string]::IsNullOrWhiteSpace($password)) {
        throw 'WOW_ACCOUNT_PASSWORD must be set in the environment for q786 account bootstrap; it is never accepted as a parameter.'
    }
    if ($password -match '\s' -or $password.Length -lt 3 -or $password.Length -gt 16) {
        throw 'WOW_ACCOUNT_PASSWORD must be 3-16 characters with no whitespace (the WoW 3.3.5 client limit).'
    }

    $serverDirectory = Join-Path $Root 'server'
    $worldserver = Join-Path $serverDirectory 'worldserver.exe'
    $worldConfig = Join-Path $serverDirectory 'configs\worldserver.conf'
    if (-not (Test-Path -LiteralPath $worldserver -PathType Leaf) -or
        -not (Test-Path -LiteralPath $worldConfig -PathType Leaf)) {
        throw 'Transient q786 account bootstrap requires the existing worldserver.exe and worldserver.conf.'
    }

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $worldserver
    $startInfo.Arguments = '-c "' + $worldConfig + '"'
    $startInfo.WorkingDirectory = $serverDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $exitCode = $null
    if (-not $process.Start()) { throw 'Could not start the transient q786 account-bootstrap console.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    try {
        $ready = $false
        $readyWatch = [Diagnostics.Stopwatch]::StartNew()
        while ($readyWatch.Elapsed.TotalSeconds -lt 150) {
            if ($process.HasExited) { throw 'Transient q786 account-bootstrap worldserver exited before its local bridge became ready.' }
            if (Test-Q786ProvisionBridgeReady -HostName $HostName -Port $Port -TimeoutMs 500) { $ready = $true; break }
            Start-Sleep -Milliseconds 500
        }
        if (-not $ready) { throw 'Transient q786 account-bootstrap worldserver did not become ready within 150 seconds.' }
        Start-Sleep -Seconds 2
        foreach ($member in $members) {
            [void]$process.StandardInput.WriteLine(".account create $($member.account_name) $password")
        }
        [void]$process.StandardInput.Flush()
        Start-Sleep -Seconds 5
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(180000)) {
            try { $process.Kill() } catch { }
            throw 'Transient q786 account-bootstrap worldserver did not exit after stdin closed.'
        }
        $exitCode = $process.ExitCode
        $null = $stdoutTask.Result
        $null = $stderrTask.Result
    }
    finally {
        if (-not $process.HasExited) { try { $process.Kill() } catch { } }
        $process.Dispose()
    }
    if ($null -eq $exitCode -or $exitCode -ne 0) {
        throw 'Transient q786 account bootstrap failed; console output was not persisted to avoid secret disclosure.'
    }
    Assert-Q786ProvisioningOffline -Root $Root -HostName $HostName -Port $Port | Out-Null
    return [pscustomobject][ordered]@{
        account_names = @($members | ForEach-Object { $_.account_name })
        commands = @($members | ForEach-Object { ".account create $($_.account_name) <WOW_ACCOUNT_PASSWORD>" })
        password_emitted_or_persisted = $false
        transient_console_used = $true
        transient_console_exit_code = $exitCode
        normal_server_stopped_or_restarted = $false
    }
}

function Get-Q786FixtureRows {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$DatabaseContext,
        [Parameter(Mandatory)][psobject]$Definition,
        [Parameter(Mandatory)][switch]$ApplyConfirmed
    )

    if (-not $ApplyConfirmed) { throw 'q786 fixture read access requires explicit -Apply.' }
    Assert-Q786ProvisionDefinition -Definition $Definition | Out-Null
    $accountList = (($Definition.ordered_members | ForEach-Object { ConvertTo-Q786SqlLiteral -Value $_.account_name }) -join ',')
    $characterList = (($Definition.ordered_members | ForEach-Object { ConvertTo-Q786SqlLiteral -Value $_.character_name }) -join ',')
    $accountSql = "SELECT id,username FROM account WHERE username IN ($accountList) ORDER BY username;"
    $accountLines = @(Invoke-Q786Sql -DatabaseContext $DatabaseContext -Database $DatabaseContext.login -Sql $accountSql -Operation Read -ApplyConfirmed:$ApplyConfirmed)
    $accountRows = @(ConvertFrom-Q786TabRows -Lines $accountLines -Columns @('id', 'username'))
    $accountIds = @($accountRows | ForEach-Object {
        if ([string]$_.id -notmatch '^\d+$' -or [int64]$_.id -le 0) { throw "Invalid account id for $($_.username)." }
        [int64]$_.id
    })
    $ownedPredicate = if ($accountIds.Count -gt 0) { " OR c.account IN ($($accountIds -join ','))" } else { '' }
    $charactersDb = ConvertTo-Q786SqlIdentifier -Value $DatabaseContext.characters.database
    $characterSql = @"
SELECT c.guid,c.account,c.name,c.race,c.class,c.gender,c.level,c.online,c.map,
       c.position_x,c.position_y,c.position_z,c.orientation,
       COALESCE(h.mapId,-1),COALESCE(h.zoneId,-1),COALESCE(h.posX,0),COALESCE(h.posY,0),COALESCE(h.posZ,0),
       (SELECT COUNT(*) FROM $charactersDb.character_inventory i WHERE i.guid=c.guid),
       (SELECT COUNT(*) FROM $charactersDb.character_queststatus q WHERE q.guid=c.guid),
       (SELECT COUNT(*) FROM $charactersDb.character_queststatus_rewarded r WHERE r.guid=c.guid)
FROM $charactersDb.characters c
LEFT JOIN $charactersDb.character_homebind h ON h.guid=c.guid
WHERE c.name IN ($characterList)$ownedPredicate
ORDER BY c.account,c.guid;
"@
    $characterLines = @(Invoke-Q786Sql -DatabaseContext $DatabaseContext -Database $DatabaseContext.characters -Sql $characterSql -Operation Read -ApplyConfirmed:$ApplyConfirmed)
    $characterRows = @(ConvertFrom-Q786TabRows -Lines $characterLines -Columns @(
        'guid', 'account', 'name', 'race', 'class', 'gender', 'level', 'online', 'map',
        'x', 'y', 'z', 'o', 'home_map', 'home_zone', 'home_x', 'home_y', 'home_z',
        'inventory_count', 'questlog_count', 'rewarded_count'
    ))
    return [pscustomobject]@{ accounts = $accountRows; characters = $characterRows }
}

function Resolve-Q786FixtureManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Definition,
        [AllowEmptyCollection()][object[]]$AccountRows = @(),
        [AllowEmptyCollection()][object[]]$CharacterRows = @()
    )

    Assert-Q786ProvisionDefinition -Definition $Definition | Out-Null
    $expectedByAccount = @{}
    $expectedByName = @{}
    foreach ($member in @($Definition.ordered_members)) {
        $expectedByAccount[$member.account_name] = $member
        $expectedByName[$member.character_name] = $member
    }

    $accountByName = @{}
    $accountNameById = @{}
    foreach ($row in @($AccountRows)) {
        $username = [string]$row.username
        if (-not $expectedByAccount.ContainsKey($username)) { throw "Out-of-scope account returned for q786: $username" }
        if ([string]$row.id -notmatch '^\d+$' -or [int64]$row.id -le 0) { throw "Invalid q786 account id for $username." }
        if ($accountByName.ContainsKey($username) -or $accountNameById.ContainsKey([string]$row.id)) {
            throw "Duplicate q786 account identity returned for $username."
        }
        $accountByName[$username] = $row
        $accountNameById[[string]$row.id] = $username
    }

    $charactersByAccount = @{}
    $characterByName = @{}
    $seenGuids = @{}
    foreach ($row in @($CharacterRows)) {
        $accountId = [string]$row.account
        $name = [string]$row.name
        if ([string]$row.guid -notmatch '^\d+$' -or [int64]$row.guid -le 0) { throw "Invalid q786 character GUID for $name." }
        if ($seenGuids.ContainsKey([string]$row.guid)) { throw "Duplicate q786 GUID returned: $($row.guid)." }
        $seenGuids[[string]$row.guid] = $true
        if (-not $charactersByAccount.ContainsKey($accountId)) { $charactersByAccount[$accountId] = @() }
        $charactersByAccount[$accountId] += $row

        if (-not $expectedByName.ContainsKey($name)) {
            if ($accountNameById.ContainsKey($accountId)) {
                throw "Dedicated q786 account $($accountNameById[$accountId]) owns out-of-scope character $name."
            }
            throw "Manifest query returned unrelated character $name."
        }
        if ($characterByName.ContainsKey($name)) { throw "Duplicate q786 character name returned: $name." }
        $characterByName[$name] = $row
        $expected = $expectedByName[$name]
        if (-not $accountByName.ContainsKey($expected.account_name)) {
            throw "q786 character collision: $name exists while dedicated account $($expected.account_name) is absent."
        }
        if ([int64]$row.account -ne [int64]$accountByName[$expected.account_name].id) {
            throw "q786 character collision: $name belongs to another account."
        }
    }

    $missingAccounts = New-Object Collections.Generic.List[object]
    $missingCharacters = New-Object Collections.Generic.List[object]
    $resolved = New-Object Collections.Generic.List[object]
    foreach ($member in @($Definition.ordered_members | Sort-Object ordinal)) {
        if (-not $accountByName.ContainsKey($member.account_name)) {
            $missingAccounts.Add($member)
            $missingCharacters.Add($member)
            continue
        }
        $accountId = [string]$accountByName[$member.account_name].id
        $owned = @(if ($charactersByAccount.ContainsKey($accountId)) { $charactersByAccount[$accountId] })
        if ($owned.Count -gt 1) { throw "Dedicated q786 account $($member.account_name) owns more than one character." }
        $exact = @($owned | Where-Object name -eq $member.character_name)
        if ($exact.Count -eq 0) {
            if ($owned.Count -eq 1) { throw "Dedicated q786 account $($member.account_name) owns non-fixture character $($owned[0].name)." }
            $missingCharacters.Add($member)
            continue
        }
        if ($exact.Count -ne 1) { throw "Could not resolve exactly one q786 character $($member.character_name)." }
        $row = $exact[0]
        if ([int]$row.race -ne [int]$member.race_id -or [int]$row.class -ne [int]$member.class_id -or
            [int]$row.gender -ne [int]$member.gender) {
            throw "q786 character $($member.character_name) has unexpected race/class/gender and will not be rewritten."
        }
        if ([int]$row.level -ne 1 -or [int]$row.online -ne 0) {
            throw "q786 character $($member.character_name) is not fresh level 1 and offline; it will not be rewritten."
        }
        if ([int]$row.map -ne [int]$member.position.map -or
            [Math]::Abs([double]$row.x - [double]$member.position.x) -gt 0.01 -or
            [Math]::Abs([double]$row.y - [double]$member.position.y) -gt 0.01 -or
            [Math]::Abs([double]$row.z - [double]$member.position.z) -gt 0.01 -or
            [Math]::Abs([double]$row.o - [double]$member.position.o) -gt 0.01) {
            throw "q786 character $($member.character_name) is not at the exact Durotar bootstrap position."
        }
        if ([int]$row.home_map -ne [int]$member.position.map -or [int]$row.home_zone -ne [int]$member.position.zone -or
            [Math]::Abs([double]$row.home_x - [double]$member.position.x) -gt 0.01 -or
            [Math]::Abs([double]$row.home_y - [double]$member.position.y) -gt 0.01 -or
            [Math]::Abs([double]$row.home_z - [double]$member.position.z) -gt 0.01) {
            throw "q786 character $($member.character_name) has an unexpected homebind and will not be rewritten."
        }
        if ([int]$row.inventory_count -ne 0 -or [int]$row.questlog_count -ne 0 -or [int]$row.rewarded_count -ne 0) {
            throw "q786 character $($member.character_name) is not fresh: inventory or quest state is non-empty."
        }
        $resolved.Add([pscustomobject][ordered]@{
            ordinal = [int]$member.ordinal
            role = $member.role
            account_name = $member.account_name
            account_id = [int64]$accountByName[$member.account_name].id
            character_name = $member.character_name
            character_guid = [uint32]$row.guid
            faction = 'Horde'
            race_name = $member.race_name
            race_id = [int]$row.race
            class_name = $member.class_name
            class_id = [int]$row.class
            level_before_fixture_init = [int]$row.level
            fixture_init_level = [int]$member.fixture_init_level
            fixture_init_spec_index = [int]$member.fixture_init_spec_index
            fixture_init_quality = [int]$member.fixture_init_quality
            map = [int]$row.map
            zone = [int]$member.position.zone
            x = [double]$row.x
            y = [double]$row.y
            z = [double]$row.z
            o = [double]$row.o
            inventory_rows = [int]$row.inventory_count
            quest_log_rows = [int]$row.questlog_count
            rewarded_quest_rows = [int]$row.rewarded_count
            fresh_before_fixture_init = $true
            league = $member.league
        })
    }

    return [pscustomobject][ordered]@{
        complete = ($missingAccounts.Count -eq 0 -and $missingCharacters.Count -eq 0 -and $resolved.Count -eq 2)
        missing_accounts = $missingAccounts.ToArray()
        missing_characters = $missingCharacters.ToArray()
        resolved_members = @($resolved | Sort-Object ordinal)
        ordered_guids = @($resolved | Sort-Object ordinal | ForEach-Object { [uint32]$_.character_guid })
    }
}

function New-Q786CharacterInsertSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$CharactersDatabaseName,
        [Parameter(Mandatory)][ValidateRange(1, [int]::MaxValue)][int]$AccountId,
        [Parameter(Mandatory)][psobject]$Member
    )

    $identity = "$($Member.role)|$($Member.account_name)|$($Member.character_name)|$([int]$Member.race_id)|$([int]$Member.class_id)"
    if ($identity -notin @(
        'owner|AWQ786OWNER|Kolkarown|2|1',
        'helper|AWQ786HELPER|Kolkarhelp|2|7'
    )) {
        throw 'q786 character insert received an out-of-scope identity.'
    }
    $database = ConvertTo-Q786SqlIdentifier -Value $CharactersDatabaseName
    $name = ConvertTo-Q786SqlLiteral -Value $Member.character_name
    $lockName = ConvertTo-Q786SqlLiteral -Value $script:Q786DatabaseLockName
    $x = ConvertTo-Q786SqlNumber -Value ([double]$Member.position.x)
    $y = ConvertTo-Q786SqlNumber -Value ([double]$Member.position.y)
    $z = ConvertTo-Q786SqlNumber -Value ([double]$Member.position.z)
    $o = ConvertTo-Q786SqlNumber -Value ([double]$Member.position.o)
    $sql = @"
SELECT GET_LOCK($lockName, 30) INTO @q786_fixture_lock;
START TRANSACTION;
SELECT COALESCE(MAX(guid), 0) + 1 INTO @q786_fixture_guid FROM $database.characters;
INSERT INTO $database.characters
    (guid,account,name,race,class,gender,level,map,position_x,position_y,position_z,orientation,taximask,cinematic,health,innTriggerId,at_login)
SELECT @q786_fixture_guid,$AccountId,$name,$([int]$Member.race_id),$([int]$Member.class_id),$([int]$Member.gender),
       1,$([int]$Member.position.map),$x,$y,$z,$o,'',1,1,0,0
WHERE @q786_fixture_lock=1
  AND NOT EXISTS (
      SELECT 1 FROM $database.characters
      WHERE name=$name OR account=$AccountId OR guid=@q786_fixture_guid
  );
SET @q786_character_inserted=ROW_COUNT();
INSERT INTO $database.character_homebind (guid,mapId,zoneId,posX,posY,posZ)
SELECT @q786_fixture_guid,$([int]$Member.position.map),$([int]$Member.position.zone),$x,$y,$z
WHERE @q786_fixture_lock=1 AND @q786_character_inserted=1
  AND EXISTS (
      SELECT 1 FROM $database.characters
      WHERE guid=@q786_fixture_guid AND account=$AccountId AND name=$name
  )
  AND NOT EXISTS (SELECT 1 FROM $database.character_homebind WHERE guid=@q786_fixture_guid);
SELECT @q786_fixture_guid,@q786_character_inserted,@q786_fixture_lock;
COMMIT;
SELECT RELEASE_LOCK($lockName);
"@
    Assert-Q786SqlSafety -Sql $sql -Operation CharacterInsert | Out-Null
    return $sql
}

function Get-Q786FixtureGuidConfigUpdate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Content,
        [Parameter(Mandatory)][uint32[]]$RequiredGuids
    )

    $required = @($RequiredGuids | Sort-Object -Unique)
    if ($required.Count -ne 2 -or @($required | Where-Object { $_ -eq 0 }).Count -ne 0) {
        throw 'AutoWow.FixtureGuids setup requires exactly two distinct resolved q786 GUIDs.'
    }
    $pattern = '^(?<prefix>\s*AutoWow\.FixtureGuids\s*=\s*")(?<value>[^"\r\n]*)(?<suffix>"[^\r\n]*)(?=\r?$)'
    $matches = [regex]::Matches($Content, $pattern, [Text.RegularExpressions.RegexOptions]::Multiline)
    if ($matches.Count -ne 1) { throw "Expected exactly one AutoWow.FixtureGuids setting; found $($matches.Count)." }
    $match = $matches[0]
    $before = New-Object Collections.Generic.List[uint32]
    foreach ($token in @($match.Groups['value'].Value -split '[,\s]+' | Where-Object { $_ })) {
        $parsed = 0L
        if (-not [int64]::TryParse($token, [ref]$parsed) -or $parsed -le 0 -or $parsed -gt [uint32]::MaxValue) {
            throw "Invalid GUID token in AutoWow.FixtureGuids: $token"
        }
        if ([uint32]$parsed -notin $before) { $before.Add([uint32]$parsed) }
    }
    $after = New-Object Collections.Generic.List[uint32]
    foreach ($guid in $before) { $after.Add($guid) }
    $added = New-Object Collections.Generic.List[uint32]
    foreach ($guid in @($RequiredGuids)) {
        if ($guid -notin $after) { $after.Add($guid); $added.Add($guid) }
    }
    $newLine = $match.Groups['prefix'].Value + ($after -join ',') + $match.Groups['suffix'].Value
    $newContent = $Content.Substring(0, $match.Index) + $newLine + $Content.Substring($match.Index + $match.Length)
    return [pscustomobject][ordered]@{
        content = $newContent
        before_guids = $before.ToArray()
        after_guids = $after.ToArray()
        added_guids = $added.ToArray()
        changed = ($added.Count -gt 0)
    }
}

function Set-Q786FixtureGuidConfig {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigPath,
        [Parameter(Mandatory)][uint32[]]$RequiredGuids,
        [Parameter(Mandatory)][string]$Root,
        [ValidateSet('127.0.0.1')][string]$HostName = '127.0.0.1',
        [ValidateRange(1, 65535)][int]$Port = 18787,
        [Parameter(Mandatory)][switch]$ApplyConfirmed
    )

    if (-not $ApplyConfirmed) { throw 'AutoWow.FixtureGuids setup requires explicit -Apply.' }
    Assert-Q786ProvisioningOffline -Root $Root -HostName $HostName -Port $Port | Out-Null
    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { throw "Missing playerbots config: $ConfigPath" }
    $beforeContent = [IO.File]::ReadAllText($ConfigPath)
    $update = Get-Q786FixtureGuidConfigUpdate -Content $beforeContent -RequiredGuids $RequiredGuids
    if ($update.changed) {
        if ([IO.File]::ReadAllText($ConfigPath) -cne $beforeContent) {
            throw 'playerbots.conf changed during q786 preflight; refusing a stale config write.'
        }
        [IO.File]::WriteAllText($ConfigPath, $update.content, [Text.UTF8Encoding]::new($false))
    }
    $verified = Get-Q786FixtureGuidConfigUpdate -Content ([IO.File]::ReadAllText($ConfigPath)) -RequiredGuids $RequiredGuids
    if ($verified.changed -or ($verified.after_guids -join ',') -ne ($update.after_guids -join ',')) {
        throw 'AutoWow.FixtureGuids post-write verification failed.'
    }
    return [pscustomobject][ordered]@{
        config_path = $ConfigPath
        required_guids = @($RequiredGuids)
        added_guids = @($update.added_guids)
        configured_guids = @($verified.after_guids)
        config_mutated = [bool]$update.changed
        only_resolved_guids_added = (@($update.added_guids | Where-Object { $_ -notin $RequiredGuids }).Count -eq 0)
    }
}

function Get-Q786LeagueRows {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$DatabaseContext,
        [Parameter(Mandatory)][uint32[]]$Guids,
        [Parameter(Mandatory)][switch]$ApplyConfirmed
    )
    if (-not $ApplyConfirmed) { throw 'q786 league read access requires explicit -Apply.' }
    $exactGuids = @($Guids | Sort-Object -Unique)
    if ($exactGuids.Count -ne 2) { throw 'q786 league query requires exactly two resolved GUIDs.' }
    $database = ConvertTo-Q786SqlIdentifier -Value $DatabaseContext.playerbots.database
    $sql = @"
SELECT character_guid,COALESCE(team_id,''),affiliation,role,class_plan,active,COALESCE(DATE_FORMAT(retired_at,'%Y-%m-%dT%H:%i:%s'),'')
FROM $database.autowow_league_member
WHERE character_guid IN ($($exactGuids -join ','))
ORDER BY character_guid;
"@
    $lines = @(Invoke-Q786Sql -DatabaseContext $DatabaseContext -Database $DatabaseContext.playerbots -Sql $sql -Operation Read -ApplyConfirmed:$ApplyConfirmed)
    return @(ConvertFrom-Q786TabRows -Lines $lines -Columns @('character_guid', 'team_id', 'affiliation', 'role', 'class_plan', 'active', 'retired_at'))
}

function Assert-Q786LeagueRows {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows,
        [Parameter(Mandatory)][object[]]$ResolvedMembers,
        [switch]$RequireComplete
    )
    $members = @($ResolvedMembers)
    if ($members.Count -ne 2) { throw 'q786 league validation requires exactly two resolved members.' }
    $expectedByGuid = @{}
    foreach ($member in $members) { $expectedByGuid[[string]$member.character_guid] = $member }
    $seen = @{}
    foreach ($row in @($Rows)) {
        $guid = [string]$row.character_guid
        if (-not $expectedByGuid.ContainsKey($guid)) { throw "q786 league query returned out-of-scope GUID $guid." }
        if ($seen.ContainsKey($guid)) { throw "q786 league query returned duplicate GUID $guid." }
        $seen[$guid] = $true
        $member = $expectedByGuid[$guid]
        if ([string]$row.team_id -ne '' -or [string]$row.affiliation -ne 'wayfarer' -or
            [string]$row.role -ne 'adventurer' -or [string]$row.class_plan -ne "q786-$($member.role)" -or
            [int]$row.active -ne 1 -or -not [string]::IsNullOrEmpty([string]$row.retired_at)) {
            throw "GUID $guid already has non-q786 league state and will not be rewritten."
        }
    }
    if ($RequireComplete -and $seen.Count -ne 2) { throw "q786 league enrollment is incomplete; found $($seen.Count) exact rows." }
    return $true
}

function New-Q786LeagueEnrollmentSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$PlayerbotsDatabaseName,
        [Parameter(Mandatory)][object[]]$Members
    )
    $pending = @($Members | Sort-Object ordinal)
    if ($pending.Count -lt 1 -or $pending.Count -gt 2 -or
        @($pending | Where-Object { $_.role -notin @('owner', 'helper') -or [uint32]$_.character_guid -eq 0 }).Count -ne 0) {
        throw 'q786 league insertion received an out-of-scope member.'
    }
    foreach ($member in $pending) {
        $identity = "$($member.role)|$($member.account_name)|$($member.character_name)|$([int]$member.race_id)|$([int]$member.class_id)"
        if ($identity -notin @(
            'owner|AWQ786OWNER|Kolkarown|2|1',
            'helper|AWQ786HELPER|Kolkarhelp|2|7'
        )) {
            throw 'q786 league insertion received a mismatched dedicated identity.'
        }
    }
    $database = ConvertTo-Q786SqlIdentifier -Value $PlayerbotsDatabaseName
    $lockName = ConvertTo-Q786SqlLiteral -Value $script:Q786DatabaseLockName
    $lines = New-Object Collections.Generic.List[string]
    $lines.Add("SELECT GET_LOCK($lockName, 30) INTO @q786_fixture_lock;")
    $lines.Add('START TRANSACTION;')
    $lines.Add('SET @q786_league_inserted=0;')
    foreach ($member in $pending) {
        $classPlan = ConvertTo-Q786SqlLiteral -Value "q786-$($member.role)"
        $lines.Add("INSERT INTO $database.autowow_league_member (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active,retired_at)")
        $lines.Add("SELECT $([uint32]$member.character_guid),NULL,'wayfarer','adventurer',$classPlan,'','',1,NULL")
        $lines.Add("WHERE @q786_fixture_lock=1 AND NOT EXISTS (SELECT 1 FROM $database.autowow_league_member WHERE character_guid=$([uint32]$member.character_guid));")
        $lines.Add('SET @q786_league_inserted=@q786_league_inserted+ROW_COUNT();')
    }
    $lines.Add('SELECT @q786_league_inserted,@q786_fixture_lock;')
    $lines.Add('COMMIT;')
    $lines.Add("SELECT RELEASE_LOCK($lockName);")
    $sql = $lines -join [Environment]::NewLine
    Assert-Q786SqlSafety -Sql $sql -Operation LeagueEnrollment | Out-Null
    return $sql
}

function New-Q786ReadyManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Definition,
        [Parameter(Mandatory)][psobject]$ResolvedManifest,
        [Parameter(Mandatory)][psobject]$FixtureGuidSetup,
        [Parameter(Mandatory)][object[]]$LeagueRows,
        [Parameter(Mandatory)][psobject]$AccountBootstrap,
        [Parameter(Mandatory)][string]$OutputPath
    )
    if (-not $ResolvedManifest.complete -or @($ResolvedManifest.resolved_members).Count -ne 2 -or
        @($ResolvedManifest.ordered_guids).Count -ne 2) {
        throw 'Cannot emit q786 manifest without exactly two fresh resolved members.'
    }
    Assert-Q786LeagueRows -Rows $LeagueRows -ResolvedMembers $ResolvedManifest.resolved_members -RequireComplete | Out-Null
    $owner = @($ResolvedManifest.resolved_members | Where-Object role -eq 'owner')
    $helper = @($ResolvedManifest.resolved_members | Where-Object role -eq 'helper')
    if ($owner.Count -ne 1 -or $helper.Count -ne 1) { throw 'q786 manifest requires one exact owner and helper.' }
    return [pscustomobject][ordered]@{
        schema = $script:Q786ManifestSchema
        schema_version = 1
        fixture_id = $Definition.fixture_id
        status = 'READY_FOR_Q786_FIXTURE_INIT'
        generated_at_utc = [datetime]::UtcNow.ToString('o')
        manifest_path = $OutputPath
        quest = [pscustomobject][ordered]@{ id = 786; title = $Definition.quest_title }
        owner_guid = [uint32]$owner[0].character_guid
        helper_guid = [uint32]$helper[0].character_guid
        ordered_guids = @([uint32]$owner[0].character_guid, [uint32]$helper[0].character_guid)
        members = @($ResolvedManifest.resolved_members)
        freshness = [pscustomobject][ordered]@{
            level_one_before_fixture_init = $true
            inventory_rows = 0
            quest_log_rows = 0
            rewarded_quest_rows = 0
            validated_members = 2
        }
        account_bootstrap = $AccountBootstrap
        fixture_allowlist = $FixtureGuidSetup
        league_enrollment = [pscustomobject][ordered]@{
            table = 'autowow_league_member'
            exact_guid_count = @($LeagueRows).Count
            character_guids = @($LeagueRows | Sort-Object { [uint32]$_.character_guid } | ForEach-Object { [uint32]$_.character_guid })
            unrelated_rows_mutated = 0
        }
        mutation_receipt = [pscustomobject][ordered]@{
            account_or_character_deletes = 0
            existing_account_or_character_rewrites = 0
            inventory_writes = 0
            quest_state_writes = 0
            fixture_init_calls = 0
            normal_server_stops = 0
            normal_server_starts_or_restarts = 0
        }
        next_action = 'Start the combined build normally, then pass owner_guid and helper_guid to the q786 interaction fixture for fixture-init and proof.'
    }
}

$definition = Get-Q786ProvisionDefinition
$plan = Get-Q786ProvisionPlan -Definition $definition
if (-not $Apply) {
    $plan | ConvertTo-Json -Depth 20
    return
}

$resolvedRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
$logsRoot = [IO.Path]::GetFullPath((Join-Path $resolvedRoot 'logs'))
if ([string]::IsNullOrWhiteSpace($ManifestPath)) { $ManifestPath = Join-Path $logsRoot 'q786-fixture-manifest.json' }
elseif (-not [IO.Path]::IsPathRooted($ManifestPath)) { $ManifestPath = Join-Path $logsRoot $ManifestPath }
$ManifestPath = [IO.Path]::GetFullPath($ManifestPath)
$logsPrefix = $logsRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $ManifestPath.StartsWith($logsPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "ManifestPath must stay under AutoWoW logs: $ManifestPath"
}
if (Test-Path -LiteralPath $ManifestPath) { throw "Refusing to overwrite an existing q786 manifest: $ManifestPath" }

# Every provisioning mutation is gated by this offline assertion. The account helper
# temporarily starts only the exact console process it owns; after it exits, this guard
# is re-established before direct SQL, config, or league mutation.
Assert-Q786ProvisioningOffline -Root $resolvedRoot -HostName $BridgeHost -Port $BridgePort | Out-Null
$databaseContext = Get-Q786DatabaseContext -Root $resolvedRoot
$rows = Get-Q786FixtureRows -DatabaseContext $databaseContext -Definition $definition -ApplyConfirmed
$resolved = Resolve-Q786FixtureManifest -Definition $definition -AccountRows $rows.accounts -CharacterRows $rows.characters
$accountBootstrap = [pscustomobject][ordered]@{
    account_names = @()
    commands = @()
    password_emitted_or_persisted = $false
    transient_console_used = $false
    normal_server_stopped_or_restarted = $false
}

if (@($resolved.missing_accounts).Count -gt 0) {
    $accountBootstrap = Invoke-Q786AccountBootstrap -MissingMembers @($resolved.missing_accounts) -Root $resolvedRoot `
        -HostName $BridgeHost -Port $BridgePort -ApplyConfirmed
    $rows = Get-Q786FixtureRows -DatabaseContext $databaseContext -Definition $definition -ApplyConfirmed
    $resolved = Resolve-Q786FixtureManifest -Definition $definition -AccountRows $rows.accounts -CharacterRows $rows.characters
    if (@($resolved.missing_accounts).Count -ne 0) { throw 'Exact q786 account bootstrap completed but dedicated accounts are still missing.' }
}

if (@($resolved.missing_characters).Count -gt 0) {
    foreach ($member in @($resolved.missing_characters | Sort-Object ordinal)) {
        Assert-Q786ProvisioningOffline -Root $resolvedRoot -HostName $BridgeHost -Port $BridgePort | Out-Null
        $accountRow = @($rows.accounts | Where-Object username -eq $member.account_name)
        if ($accountRow.Count -ne 1) { throw "Could not resolve one exact q786 account row for $($member.account_name)." }
        $sql = New-Q786CharacterInsertSql -CharactersDatabaseName $databaseContext.characters.database `
            -AccountId ([int]$accountRow[0].id) -Member $member
        $outcome = @(Invoke-Q786Sql -DatabaseContext $databaseContext -Database $databaseContext.characters `
            -Sql $sql -Operation CharacterInsert -ApplyConfirmed | Where-Object { $_ -match '^\d+\t\d+\t\d+$' })
        if ($outcome.Count -ne 1) { throw "q786 character insert returned no exact lock/row receipt for $($member.character_name)." }
        $fields = @($outcome[0] -split "`t")
        if ([int]$fields[1] -ne 1 -or [int]$fields[2] -ne 1) {
            throw "q786 character insert did not acquire its lock and insert exactly one row for $($member.character_name)."
        }
    }
    $rows = Get-Q786FixtureRows -DatabaseContext $databaseContext -Definition $definition -ApplyConfirmed
    $resolved = Resolve-Q786FixtureManifest -Definition $definition -AccountRows $rows.accounts -CharacterRows $rows.characters
}
if (-not $resolved.complete) { throw 'q786 resolution did not produce exactly two fresh dedicated characters.' }

$resolvedGuids = [uint32[]]@($resolved.ordered_guids)
$playerbotsConfig = Join-Path $resolvedRoot 'server\configs\modules\playerbots.conf'
Assert-Q786ProvisioningOffline -Root $resolvedRoot -HostName $BridgeHost -Port $BridgePort | Out-Null
$fixtureGuidSetup = Set-Q786FixtureGuidConfig -ConfigPath $playerbotsConfig -RequiredGuids $resolvedGuids `
    -Root $resolvedRoot -HostName $BridgeHost -Port $BridgePort -ApplyConfirmed

$leagueRows = @(Get-Q786LeagueRows -DatabaseContext $databaseContext -Guids $resolvedGuids -ApplyConfirmed)
Assert-Q786LeagueRows -Rows $leagueRows -ResolvedMembers $resolved.resolved_members | Out-Null
$existingLeagueGuids = @($leagueRows | ForEach-Object { [uint32]$_.character_guid })
$missingLeagueMembers = @($resolved.resolved_members | Where-Object { [uint32]$_.character_guid -notin $existingLeagueGuids })
if ($missingLeagueMembers.Count -gt 0) {
    Assert-Q786ProvisioningOffline -Root $resolvedRoot -HostName $BridgeHost -Port $BridgePort | Out-Null
    $leagueSql = New-Q786LeagueEnrollmentSql -PlayerbotsDatabaseName $databaseContext.playerbots.database -Members $missingLeagueMembers
    $leagueOutcome = @(Invoke-Q786Sql -DatabaseContext $databaseContext -Database $databaseContext.playerbots `
        -Sql $leagueSql -Operation LeagueEnrollment -ApplyConfirmed | Where-Object { $_ -match '^\d+\t\d+$' })
    if ($leagueOutcome.Count -ne 1) { throw 'q786 league enrollment returned no exact lock/row receipt.' }
    $leagueFields = @($leagueOutcome[0] -split "`t")
    if ([int]$leagueFields[0] -ne $missingLeagueMembers.Count -or [int]$leagueFields[1] -ne 1) {
        throw 'q786 league enrollment did not acquire its lock and insert exactly the missing resolved GUIDs.'
    }
}
$leagueRows = @(Get-Q786LeagueRows -DatabaseContext $databaseContext -Guids $resolvedGuids -ApplyConfirmed)
Assert-Q786LeagueRows -Rows $leagueRows -ResolvedMembers $resolved.resolved_members -RequireComplete | Out-Null

$readyManifest = New-Q786ReadyManifest -Definition $definition -ResolvedManifest $resolved `
    -FixtureGuidSetup $fixtureGuidSetup -LeagueRows $leagueRows -AccountBootstrap $accountBootstrap -OutputPath $ManifestPath
$json = $readyManifest | ConvertTo-Json -Depth 24
$secretCandidates = @($env:WOW_ACCOUNT_PASSWORD, $databaseContext.login.password, $databaseContext.characters.password, $databaseContext.playerbots.password) |
    Where-Object { -not [string]::IsNullOrEmpty([string]$_) }
foreach ($secret in $secretCandidates) {
    if ($json.Contains([string]$secret)) { throw 'q786 manifest secret-safety check failed; manifest was not written.' }
}
$manifestDirectory = Split-Path -Parent $ManifestPath
if (-not (Test-Path -LiteralPath $manifestDirectory)) { New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null }
if (Test-Path -LiteralPath $ManifestPath) { throw "Refusing a raced overwrite of q786 manifest: $ManifestPath" }
[IO.File]::WriteAllText($ManifestPath, $json, [Text.UTF8Encoding]::new($false))
$json
