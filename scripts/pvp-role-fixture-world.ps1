<#
.SYNOPSIS
    Provisions and normalizes the isolated role-balanced AutoWoW 10v10 WSG fixture.

.DESCRIPTION
    Dry-runs by default. -Apply is required for local account creation, exact character
    bootstrap, bot activation, and per-member fixture-init. The script never builds,
    restarts, stops, or reconfigures the normal server and never dispatches a WSG queue.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ReportPath = '',
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
    [ValidateRange(10, 300)][int]$ActivationTimeoutSeconds = 120,
    [ValidateRange(1, 5)][int]$PollSeconds = 2,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonPath = Join-Path $PSScriptRoot 'Common.ps1'
$roleFixtureLibrary = Join-Path $PSScriptRoot 'pvp-role-fixture-lib.ps1'
$controlPath = Join-Path $PSScriptRoot 'autowow-control.ps1'
foreach ($requiredPath in @($commonPath, $roleFixtureLibrary, $controlPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) { throw "Missing PvP role fixture dependency: $requiredPath" }
}
. $commonPath
. $roleFixtureLibrary

$definition = Get-PvpRoleFixtureDefinition
$plan = Get-PvpRoleFixturePlan -Definition $definition
if (-not $Apply) {
    [pscustomobject][ordered]@{
        schema = 'autowow.pvp.role-fixture.run.v1'
        schema_version = 1
        fixture_id = $definition.fixture_id
        status = 'DRY_RUN'
        dry_run = $true
        apply = $false
        local_only = $true
        definition = $definition
        plan = $plan
        live_mutations_executed = 0
    } | ConvertTo-Json -Depth 24
    return
}

function Get-PvpRoleFixtureConfigConnection {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigPath,
        [Parameter(Mandatory)][string]$Key
    )

    if (-not (Test-Path -LiteralPath $ConfigPath)) { throw "Required configuration is missing: $ConfigPath" }
    $escapedKey = [regex]::Escape($Key)
    $line = @(Get-Content -LiteralPath $ConfigPath | Where-Object { $_ -match "^\s*$escapedKey\s*=" } | Select-Object -First 1)
    if ($line.Count -ne 1) { throw "Could not find $Key in $ConfigPath" }
    $value = (($line[0] -split '=', 2)[1]).Trim().Trim('"')
    $parts = @($value -split ';', 5)
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    $dbPort = 0
    Assert-LocalOnlyAddress -Address $parts[0].Trim()
    if (-not [int]::TryParse($parts[1].Trim(), [ref]$dbPort) -or $dbPort -lt 1 -or $dbPort -gt 65535) {
        throw "$Key has an invalid local database port."
    }
    foreach ($safeToken in @($parts[2], $parts[4])) {
        if ($safeToken.Trim() -notmatch '^[A-Za-z0-9_.$-]+$') { throw "$Key contains an unsafe database token." }
    }
    return [pscustomobject]@{
        host = $parts[0].Trim()
        port = $dbPort
        user = $parts[2].Trim()
        password = $parts[3]
        database = $parts[4].Trim()
    }
}

function Get-PvpRoleFixtureDatabaseContext {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Root)

    $mysql = Get-FirstExistingPath -Candidates @(
        (Join-Path $Root 'third_party\mysql\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
    )
    if (-not $mysql) { throw 'MySQL CLI was not found.' }
    $worldConfig = Join-Path $Root 'server\configs\worldserver.conf'
    return [pscustomobject]@{
        mysql = $mysql
        login = Get-PvpRoleFixtureConfigConnection -ConfigPath $worldConfig -Key 'LoginDatabaseInfo'
        characters = Get-PvpRoleFixtureConfigConnection -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
    }
}

function Invoke-PvpRoleFixtureSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$DatabaseContext,
        [Parameter(Mandatory)][psobject]$Database,
        [Parameter(Mandatory)][string]$Sql,
        [switch]$Mutation,
        [Parameter(Mandatory)][switch]$ApplyConfirmed
    )

    if (-not $ApplyConfirmed) { throw 'Database access requires the explicit -Apply guard.' }
    $oldPassword = $env:MYSQL_PWD
    $raw = @()
    $exitCode = -1
    $env:MYSQL_PWD = $Database.password
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
        $safeError = @($lines | Where-Object { $_ -notmatch '(?i)(password|MYSQL_PWD|;[^;]+;[^;]+;[^;]+;)' }) -join ' '
        if ([string]::IsNullOrWhiteSpace($safeError)) { $safeError = 'no safe database error text returned' }
        $kind = if ($Mutation) { 'mutation' } else { 'query' }
        throw "PvP role fixture database $kind failed: $safeError"
    }
    return $lines
}

function ConvertFrom-PvpRoleFixtureTabRows {
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [Parameter(Mandatory)][string[]]$Columns
    )

    $rows = @()
    foreach ($line in $Lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $fields = @([regex]::Split($line, "`t"))
        if ($fields.Count -lt $Columns.Count) { continue }
        $row = [ordered]@{}
        for ($fieldIndex = 0; $fieldIndex -lt $Columns.Count; $fieldIndex++) { $row[$Columns[$fieldIndex]] = $fields[$fieldIndex] }
        $rows += [pscustomobject]$row
    }
    return @($rows)
}

function Get-PvpRoleFixtureRows {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$DatabaseContext,
        [Parameter(Mandatory)][psobject]$Definition
    )

    $accountNames = @($Definition.ordered_roster.account_name)
    $characterNames = @($Definition.ordered_roster.character_name)
    $accountList = (($accountNames | ForEach-Object { ConvertTo-PvpFixtureSqlLiteral -Value $_ }) -join ',')
    $characterList = (($characterNames | ForEach-Object { ConvertTo-PvpFixtureSqlLiteral -Value $_ }) -join ',')
    $accountLines = @(Invoke-PvpRoleFixtureSql -DatabaseContext $DatabaseContext -Database $DatabaseContext.login -ApplyConfirmed -Sql `
        "SELECT id,username FROM account WHERE username IN ($accountList) ORDER BY username;")
    $accountRows = @(ConvertFrom-PvpRoleFixtureTabRows -Lines $accountLines -Columns @('id', 'username'))
    $accountIds = @($accountRows | Where-Object id -Match '^\d+$' | ForEach-Object { [int64]$_.id })
    $ownedAccountPredicate = if ($accountIds.Count -gt 0) { " OR account IN ($($accountIds -join ','))" } else { '' }
    $charactersDatabase = ConvertTo-PvpFixtureSqlIdentifier -Value $DatabaseContext.characters.database
    $characterSql = @"
SELECT guid,account,name,race,class,gender,level,online,map
FROM $charactersDatabase.characters
WHERE name IN ($characterList)$ownedAccountPredicate
ORDER BY account,guid;
"@
    $characterLines = @(Invoke-PvpRoleFixtureSql -DatabaseContext $DatabaseContext -Database $DatabaseContext.characters -ApplyConfirmed -Sql $characterSql)
    $characterRows = @(ConvertFrom-PvpRoleFixtureTabRows -Lines $characterLines -Columns @('guid', 'account', 'name', 'race', 'class', 'gender', 'level', 'online', 'map'))
    return [pscustomobject]@{ accounts = $accountRows; characters = $characterRows }
}

function Get-PvpRoleFixtureWorldserverProcesses {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Root)

    $serverPrefix = [System.IO.Path]::GetFullPath((Join-Path $Root 'server')).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $matches = @()
    foreach ($candidate in @(Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $candidate.MainModule.FileName } catch { }
        if ([string]::IsNullOrWhiteSpace($path)) {
            throw "Could not prove the executable path for worldserver process $($candidate.Id)."
        }
        if ([System.IO.Path]::GetFullPath($path).StartsWith($serverPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $matches += $candidate
        }
    }
    return @($matches)
}

function Invoke-PvpRoleFixtureAccountBootstrap {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object[]]$MissingAccounts,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$LogsRoot,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][switch]$ApplyConfirmed
    )

    if (-not $ApplyConfirmed) { throw 'Account creation requires the explicit -Apply guard.' }
    if (@(Get-PvpRoleFixtureWorldserverProcesses -Root $Root).Count -ne 0 -or (Test-PvpRoleFixtureBridgeReady)) {
        throw 'Missing fixture accounts cannot be created while the normal worldserver is running; no restart or stop is performed by this lane.'
    }
    $password = $env:WOW_ACCOUNT_PASSWORD
    if ([string]::IsNullOrWhiteSpace($password)) {
        throw 'WOW_ACCOUNT_PASSWORD must be set in the environment for account bootstrap; it is never accepted as a parameter.'
    }
    if ($password -match '\s' -or $password.Length -lt 3 -or $password.Length -gt 16) {
        throw 'WOW_ACCOUNT_PASSWORD must be 3-16 characters with no whitespace (the WoW 3.3.5 client limit).'
    }

    $serverDirectory = Join-Path $Root 'server'
    $worldserver = Join-Path $serverDirectory 'worldserver.exe'
    $worldConfig = Join-Path $serverDirectory 'configs\worldserver.conf'
    if (-not (Test-Path -LiteralPath $worldserver) -or -not (Test-Path -LiteralPath $worldConfig)) {
        throw 'Transient account bootstrap requires the existing worldserver.exe and worldserver.conf.'
    }

    $stdoutPath = Join-Path $LogsRoot "pvp-role-fixture-$RunId-console.stdout.log"
    $stderrPath = Join-Path $LogsRoot "pvp-role-fixture-$RunId-console.stderr.log"
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $worldserver
    $startInfo.Arguments = '-c "' + $worldConfig + '"'
    $startInfo.WorkingDirectory = $serverDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $stdout = ''
    $stderr = ''
    $exitCode = $null
    if (-not $process.Start()) { throw 'Could not start the transient account-bootstrap console.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    try {
        $ready = $false
        $readyWatch = [System.Diagnostics.Stopwatch]::StartNew()
        while ($readyWatch.Elapsed.TotalSeconds -lt 150) {
            if ($process.HasExited) { throw 'Transient account-bootstrap worldserver exited before its local bridge became ready.' }
            $client = [System.Net.Sockets.TcpClient]::new()
            try {
                $connect = $client.ConnectAsync($BridgeHost, $Port)
                if ($connect.Wait(500) -and $client.Connected) { $ready = $true; break }
            }
            catch { }
            finally { $client.Dispose() }
            Start-Sleep -Milliseconds 500
        }
        if (-not $ready) { throw 'Transient account-bootstrap worldserver did not become ready within 150 seconds.' }
        Start-Sleep -Seconds 2
        foreach ($account in @($MissingAccounts)) {
            [void]$process.StandardInput.WriteLine(".account create $($account.account_name) $password")
        }
        [void]$process.StandardInput.Flush()
        Start-Sleep -Seconds 5
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(180000)) {
            try { $process.Kill() } catch { }
            throw 'Transient account-bootstrap worldserver did not exit after stdin closed.'
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        $exitCode = $process.ExitCode
    }
    finally {
        if (-not $process.HasExited) { try { $process.Kill() } catch { } }
        $process.Dispose()
    }

    $safeStdout = $stdout -replace [regex]::Escape($password), '<redacted>'
    $safeStderr = $stderr -replace [regex]::Escape($password), '<redacted>'
    [System.IO.File]::WriteAllText($stdoutPath, $safeStdout, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($stderrPath, $safeStderr, [System.Text.UTF8Encoding]::new($false))
    if ($null -eq $exitCode -or $exitCode -ne 0) {
        throw "Transient account bootstrap failed; inspect redacted logs $stdoutPath and $stderrPath"
    }
    return [pscustomobject][ordered]@{
        account_names = @($MissingAccounts.account_name)
        commands = @($MissingAccounts | ForEach-Object { ".account create $($_.account_name) <WOW_ACCOUNT_PASSWORD>" })
        stdout_log = $stdoutPath
        stderr_log = $stderrPath
        exit_code = $exitCode
        normal_server_restarted = $false
    }
}

function Get-PvpRoleFixtureConfiguredGuids {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$ConfigPath)

    if (-not (Test-Path -LiteralPath $ConfigPath)) { throw "Missing playerbots configuration: $ConfigPath" }
    $line = @(Get-Content -LiteralPath $ConfigPath | Where-Object { $_ -match '^\s*AutoWow\.FixtureGuids\s*=' } | Select-Object -First 1)
    if ($line.Count -ne 1) { throw 'AutoWow.FixtureGuids is missing from playerbots.conf.' }
    $value = (($line[0] -split '=', 2)[1]).Trim().Trim('"')
    $guids = New-Object System.Collections.Generic.List[uint32]
    foreach ($token in @($value -split '[,\s]+' | Where-Object { $_ })) {
        $guid = 0L
        if (-not [int64]::TryParse($token, [ref]$guid) -or $guid -le 0 -or $guid -gt [uint32]::MaxValue) {
            throw "Invalid GUID token in AutoWow.FixtureGuids: $token"
        }
        if ([uint32]$guid -notin $guids) { $guids.Add([uint32]$guid) }
    }
    return $guids.ToArray()
}

function Get-PvpRoleFixtureProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string]$Name
    )
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Invoke-PvpRoleFixtureControl {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'activate', 'fixture-init', 'fixture-status')][string]$Action,
        [uint32]$Guid = 0,
        [int]$Level = 80,
        [int]$SpecIndex = 0,
        [int]$Quality = 3
    )

    $raw = switch ($Action) {
        'list' { & $controlPath -Action list -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'activate' { & $controlPath -Action activate -BotGuid $Guid -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'fixture-init' { & $controlPath -Action fixture-init -BotGuid $Guid -Level $Level -SpecIndex $SpecIndex -Quality $Quality -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'fixture-status' { & $controlPath -Action fixture-status -BotGuid $Guid -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
    }
    $jsonLines = @($raw | ForEach-Object { [string]$_ } | Where-Object { $_ -match '^\s*\{' })
    if ($jsonLines.Count -eq 0) { throw "Control action $Action returned no JSON response." }
    try { $response = $jsonLines[-1] | ConvertFrom-Json } catch { throw "Control action $Action returned malformed JSON." }
    $ok = Get-PvpRoleFixtureProperty -Object $response -Name 'ok'
    if ($null -ne $ok -and -not [bool]$ok) {
        throw "Control action $Action failed: $([string](Get-PvpRoleFixtureProperty -Object $response -Name 'error'))"
    }
    return $response
}

function Get-PvpRoleFixtureOnlineGuids {
    $response = Invoke-PvpRoleFixtureControl -Action list
    $bots = @(Get-PvpRoleFixtureProperty -Object $response -Name 'bots')
    return @($bots | ForEach-Object { [uint32](Get-PvpRoleFixtureProperty -Object $_ -Name 'guid') })
}

function Test-PvpRoleFixtureBridgeReady {
    try {
        $null = Invoke-PvpRoleFixtureControl -Action list
        return $true
    }
    catch {
        return $false
    }
}

function Save-PvpRoleFixtureReport {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$Report,
        [Parameter(Mandatory)][string]$Path
    )
    $json = $Report | ConvertTo-Json -Depth 30
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
    return $json
}

$resolvedRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
$logsRoot = Join-Path $resolvedRoot 'logs'
if (-not (Test-Path -LiteralPath $logsRoot)) { New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null }
$runId = "PVPROLE-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
if ([string]::IsNullOrWhiteSpace($ReportPath)) { $ReportPath = Join-Path $logsRoot "pvp-role-fixture-$runId.json" }
elseif (-not [System.IO.Path]::IsPathRooted($ReportPath)) { $ReportPath = Join-Path $logsRoot $ReportPath }
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$logsPrefix = [System.IO.Path]::GetFullPath($logsRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $ReportPath.StartsWith($logsPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "ReportPath must stay under AutoWoW logs: $ReportPath"
}
if (Test-Path -LiteralPath $ReportPath) { throw "Refusing to overwrite an existing report: $ReportPath" }

$report = [ordered]@{
    schema = 'autowow.pvp.role-fixture.run.v1'
    schema_version = 1
    run_id = $runId
    fixture_id = $definition.fixture_id
    started_at_utc = [datetime]::UtcNow.ToString('o')
    completed_at_utc = $null
    status = 'APPLYING'
    dry_run = $false
    apply = $true
    local_only = $true
    report_path = $ReportPath
    definition = $definition
    preflight = $null
    account_bootstrap = $null
    character_bootstrap = @()
    manifest = $null
    fixture_allowlist = $null
    post_login_fixture_init = $null
    wsg_handoff = $null
    server_lifecycle = [ordered]@{ normal_server_restart = $false; normal_server_stop = $false; build = $false; config_write = $false }
    errors = @()
}

try {
    $databaseContext = Get-PvpRoleFixtureDatabaseContext -Root $resolvedRoot
    $rows = Get-PvpRoleFixtureRows -DatabaseContext $databaseContext -Definition $definition
    $manifest = Resolve-PvpRoleFixtureManifest -Definition $definition -AccountRows $rows.accounts -CharacterRows $rows.characters
    $report.preflight = [ordered]@{
        existing_accounts = @($rows.accounts).Count
        existing_characters = @($rows.characters).Count
        missing_accounts = @($manifest.missing_accounts | ForEach-Object { $_.account_name })
        missing_characters = @($manifest.missing_characters | ForEach-Object { $_.character_name })
    }

    if (@($manifest.missing_accounts).Count -gt 0) {
        $report.account_bootstrap = Invoke-PvpRoleFixtureAccountBootstrap -MissingAccounts @($manifest.missing_accounts) `
            -Root $resolvedRoot -LogsRoot $logsRoot -RunId $runId -ApplyConfirmed
        $rows = Get-PvpRoleFixtureRows -DatabaseContext $databaseContext -Definition $definition
        $manifest = Resolve-PvpRoleFixtureManifest -Definition $definition -AccountRows $rows.accounts -CharacterRows $rows.characters
        if (@($manifest.missing_accounts).Count -gt 0) { throw 'Exact account bootstrap completed but fixture accounts are still missing.' }
    }

    if (@($manifest.missing_characters).Count -gt 0) {
        if (@(Get-PvpRoleFixtureWorldserverProcesses -Root $resolvedRoot).Count -ne 0 -or (Test-PvpRoleFixtureBridgeReady)) {
            throw 'Direct character bootstrap is allowed only while the normal worldserver is stopped; this lane will not stop or restart it.'
        }
        foreach ($member in @($manifest.missing_characters | Sort-Object ordinal)) {
            $accountRow = @($rows.accounts | Where-Object username -eq $member.account_name)
            if ($accountRow.Count -ne 1) { throw "Could not resolve one exact account row for $($member.account_name)." }
            $sql = New-PvpRoleFixtureCharacterInsertSql -CharactersDatabaseName $databaseContext.characters.database `
                -AccountId ([int]$accountRow[0].id) -Member $member
            $null = Invoke-PvpRoleFixtureSql -DatabaseContext $databaseContext -Database $databaseContext.characters `
                -Sql $sql -Mutation -ApplyConfirmed
            $report.character_bootstrap += [ordered]@{
                account_name = $member.account_name
                character_name = $member.character_name
                race_id = [int]$member.race_id
                class_id = [int]$member.class_id
                mutation = 'exact named-lock protected insert-if-absent'
            }
        }
        $rows = Get-PvpRoleFixtureRows -DatabaseContext $databaseContext -Definition $definition
        $manifest = Resolve-PvpRoleFixtureManifest -Definition $definition -AccountRows $rows.accounts -CharacterRows $rows.characters
    }
    if (-not $manifest.complete) { throw 'Idempotent manifest resolution did not produce the exact 20-member fixture.' }
    $report.manifest = $manifest

    $configuredGuids = @(Get-PvpRoleFixtureConfiguredGuids -ConfigPath (Join-Path $resolvedRoot 'server\configs\modules\playerbots.conf'))
    $missingAllowlistGuids = @($manifest.ordered_wsg_guids | Where-Object { [uint32]$_ -notin $configuredGuids })
    $report.fixture_allowlist = [ordered]@{
        required_guids = @($manifest.ordered_wsg_guids)
        missing_guids = @($missingAllowlistGuids)
        complete = ($missingAllowlistGuids.Count -eq 0)
        config_mutated = $false
    }
    $report.wsg_handoff = [ordered]@{
        ordered_wsg_guids = @($manifest.ordered_wsg_guids)
        alliance_guids = @($manifest.ordered_wsg_guids | Select-Object -First 10)
        horde_guids = @($manifest.ordered_wsg_guids | Select-Object -Skip 10)
        queue_dispatched = $false
        warning = 'Do not pass this roster through a single-spec normalizer; preserve each member spec_index from this manifest.'
    }
    if ($missingAllowlistGuids.Count -gt 0) {
        $report.status = 'PROVISIONED_PENDING_FIXTURE_ALLOWLIST'
        $report.post_login_fixture_init = [ordered]@{
            status = 'NOT_RUN'
            reason = 'resolved GUIDs are not all in the existing fixture allowlist; this lane does not edit config or restart the server'
        }
        $report.completed_at_utc = [datetime]::UtcNow.ToString('o')
        Save-PvpRoleFixtureReport -Report $report -Path $ReportPath
        return
    }

    $worldservers = @(Get-PvpRoleFixtureWorldserverProcesses -Root $resolvedRoot)
    $bridgeReady = Test-PvpRoleFixtureBridgeReady
    if (-not $bridgeReady) {
        $report.status = 'PROVISIONED_PENDING_SERVER_LOGIN_INIT'
        $report.post_login_fixture_init = [ordered]@{
            status = 'NOT_RUN'
            reason = 'the local AutoWoW bridge is not reachable; this lane does not start or restart the server'
        }
        $report.completed_at_utc = [datetime]::UtcNow.ToString('o')
        Save-PvpRoleFixtureReport -Report $report -Path $ReportPath
        return
    }
    if ($worldservers.Count -gt 1) { throw "Expected at most one Windows worldserver for fixture-init; found $($worldservers.Count)." }

    foreach ($resolvedMember in @($manifest.resolved_members)) {
        Invoke-PvpRoleFixtureControl -Action activate -Guid ([uint32]$resolvedMember.character_guid) | Out-Null
    }
    $activationDeadline = (Get-Date).AddSeconds($ActivationTimeoutSeconds)
    $missingOnline = @($manifest.ordered_wsg_guids)
    do {
        $onlineGuids = @(Get-PvpRoleFixtureOnlineGuids)
        $missingOnline = @($manifest.ordered_wsg_guids | Where-Object { [uint32]$_ -notin $onlineGuids })
        if ($missingOnline.Count -eq 0) { break }
        Start-Sleep -Seconds $PollSeconds
    } while ((Get-Date) -lt $activationDeadline)
    if ($missingOnline.Count -ne 0) { throw "Timed out waiting for exact fixture logins: $($missingOnline -join ', ')" }

    $definitionByName = @{}
    foreach ($member in @($definition.ordered_roster)) { $definitionByName[$member.character_name] = $member }
    $evidenceRows = New-Object System.Collections.Generic.List[object]
    foreach ($resolvedMember in @($manifest.resolved_members)) {
        $member = $definitionByName[$resolvedMember.character_name]
        Invoke-PvpRoleFixtureControl -Action fixture-init -Guid ([uint32]$resolvedMember.character_guid) `
            -Level ([int]$member.level) -SpecIndex ([int]$member.spec_index) -Quality ([int]$member.gear_quality) | Out-Null
        $status = Invoke-PvpRoleFixtureControl -Action fixture-status -Guid ([uint32]$resolvedMember.character_guid)
        $class = Get-PvpRoleFixtureProperty -Object $status -Name 'class'
        $spec = Get-PvpRoleFixtureProperty -Object $status -Name 'spec'
        $role = Get-PvpRoleFixtureProperty -Object $status -Name 'role'
        $gear = Get-PvpRoleFixtureProperty -Object $status -Name 'gear'
        $qualityCounts = Get-PvpRoleFixtureProperty -Object $gear -Name 'quality_counts'
        $evidenceRows.Add([pscustomobject][ordered]@{
            character_name = $member.character_name
            character_guid = [uint32]$resolvedMember.character_guid
            class_id = [int](Get-PvpRoleFixtureProperty -Object $class -Name 'id')
            level = [int](Get-PvpRoleFixtureProperty -Object $status -Name 'level')
            spec_index = [int](Get-PvpRoleFixtureProperty -Object $spec -Name 'stored_index')
            spec_name = [string](Get-PvpRoleFixtureProperty -Object $spec -Name 'name')
            combat_role = [string](Get-PvpRoleFixtureProperty -Object $role -Name 'combat')
            equipped_slots = [int](Get-PvpRoleFixtureProperty -Object $gear -Name 'equipped_slots')
            expected_quality_slots = [int](Get-PvpRoleFixtureProperty -Object $qualityCounts -Name $definition.gear_quality_name)
            other_quality_slots = [int](Get-PvpRoleFixtureProperty -Object $gear -Name 'other_quality_slots')
        })
    }

    $loadoutEvidence = Test-PvpRoleFixtureLoadoutEvidence -Definition $definition -EvidenceRows $evidenceRows.ToArray()
    $report.post_login_fixture_init = $loadoutEvidence
    if ($loadoutEvidence.status -ne 'PASS') { throw "Post-login fixture-init validation failed: $($loadoutEvidence.reasons -join ', ')" }
    $report.status = 'READY'
    $report.completed_at_utc = [datetime]::UtcNow.ToString('o')
    Save-PvpRoleFixtureReport -Report $report -Path $ReportPath
}
catch {
    $report.status = 'FAILED'
    $report.completed_at_utc = [datetime]::UtcNow.ToString('o')
    $report.errors = @($report.errors) + $_.Exception.Message
    try { Save-PvpRoleFixtureReport -Report $report -Path $ReportPath | Out-Null } catch { }
    throw
}
