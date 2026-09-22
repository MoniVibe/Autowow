<#
    QuestLogSource.ps1 — read-only quest-log source for Quest Director V1.

    Get-NormalizedQuestLog returns a normalized quest array for a bot:
      id, title, status, is_complete, quest_type, suggested_players, src_item,
      capability_class, supported, objectives[ {kind, entry, required, current, done} ]

    Primary path: the loopback bridge `questlog <guid>` order (live world-thread QuestStatusData).
    Fallback (when the running worldserver predates the questlog endpoint): SELECT-only reads of
    character_queststatus joined to quest_template. Both paths are strictly read-only — no INSERT/
    UPDATE/DELETE and no quest-state mutation. Requires QuestDirectorLib.ps1 to be dot-sourced first.
#>

# --- Bridge (direct loopback TCP; does not touch the shared control script) ---------------------
function Send-Bridge {
    param(
        [Parameter(Mandatory = $true)][string]$Request,
        [string]$BridgeHost = '127.0.0.1',
        [int]$Port = 18787,
        [int]$TimeoutMs = 4000
    )
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        if (-not $client.ConnectAsync($BridgeHost, $Port).Wait($TimeoutMs) -or -not $client.Connected) {
            throw "Could not connect to the AutoWow bridge at ${BridgeHost}:$Port."
        }
        $stream = $client.GetStream(); $stream.ReadTimeout = $TimeoutMs
        $writer = [System.IO.StreamWriter]::new($stream); $writer.NewLine = "`n"
        $writer.WriteLine($Request); $writer.Flush()
        $reader = [System.IO.StreamReader]::new($stream)
        $line = $reader.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) { throw 'AutoWow bridge returned an empty response.' }
        return $line
    }
    finally { $client.Dispose() }
}

# --- Read-only MySQL (SELECT only), credentials from worldserver.conf, never emitted ------------
function New-QuestDbContext {
    param([Parameter(Mandatory = $true)][string]$ServerRoot)
    $worldConfig = Join-Path $ServerRoot 'server\configs\worldserver.conf'
    if (-not (Test-Path -LiteralPath $worldConfig)) { return $null }

    $parse = {
        param($key)
        $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($key)
        $line = (Select-String -LiteralPath $worldConfig -Pattern $pattern | Select-Object -First 1).Line
        if (-not $line) { return $null }
        $parts = (($line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
        if ($parts.Count -ne 5) { return $null }
        return $parts
    }
    $charInfo = & $parse 'CharacterDatabaseInfo'
    $worldInfo = & $parse 'WorldDatabaseInfo'
    if (-not $charInfo -or -not $worldInfo) { return $null }

    $mysql = $null
    foreach ($cand in @(
            (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
            'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
            'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe')) {
        if (Test-Path -LiteralPath $cand) { $mysql = (Resolve-Path -LiteralPath $cand).Path; break }
    }
    if (-not $mysql) { return $null }

    return [pscustomobject]@{
        Mysql = $mysql; DbHost = $charInfo[0]; Port = $charInfo[1]; User = $charInfo[2]
        Pass = $charInfo[3]; CharDb = $charInfo[4]; WorldDb = $worldInfo[4]
    }
}

function Invoke-QuestDbSelect {
    param([Parameter(Mandatory = $true)][object]$Context, [Parameter(Mandatory = $true)][string]$Sql)
    $trimmed = $Sql.TrimStart()
    if ($trimmed -notmatch '^(?i)SELECT\b') { throw 'Refusing non-SELECT statement in read-only quest source.' }
    if ($trimmed -match '(?i)\b(INSERT|UPDATE|DELETE|REPLACE|DROP|ALTER|CREATE|TRUNCATE|GRANT|REVOKE|SET|CALL|LOAD|RENAME|LOCK|UNLOCK)\b') {
        throw 'Refusing statement containing a mutation keyword.'
    }
    $oldPwd = $env:MYSQL_PWD; $env:MYSQL_PWD = $Context.Pass
    try {
        $raw = & $Context.Mysql --protocol=tcp "--host=$($Context.DbHost)" "--port=$($Context.Port)" "--user=$($Context.User)" `
            --batch --skip-column-names --execute=$Sql 2>&1
        $exit = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
    }
    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object { $_ -notmatch '^mysql:' -and $_ -ne '' })
    if ($exit -ne 0) { throw 'Read-only quest query failed. Connection details were not emitted.' }
    return $lines
}

function ConvertTo-NormalizedFromDb {
    param([object]$Context, [uint32]$Guid)
    $mob = @(); for ($i = 1; $i -le 4; $i++) { $mob += "qs.mobcount$i" }
    $itm = @(); for ($i = 1; $i -le 6; $i++) { $itm += "qs.itemcount$i" }
    $rng = @(); for ($i = 1; $i -le 4; $i++) { $rng += "qt.RequiredNpcOrGo$i" }
    $rngc = @(); for ($i = 1; $i -le 4; $i++) { $rngc += "qt.RequiredNpcOrGoCount$i" }
    $rit = @(); for ($i = 1; $i -le 6; $i++) { $rit += "qt.RequiredItemId$i" }
    $ritc = @(); for ($i = 1; $i -le 6; $i++) { $ritc += "qt.RequiredItemCount$i" }
    # LogTitle placed last so a stray value cannot shift the fixed-width numeric columns.
    $cols = @('qs.quest', 'qs.status') + $mob + $itm + @('qt.QuestType', 'qt.QuestLevel', 'qt.StartItem') + $rng + $rngc + $rit + $ritc + @('qt.LogTitle')
    $sql = "SELECT " + ($cols -join ', ') +
        " FROM $($Context.CharDb).character_queststatus qs" +
        " JOIN $($Context.WorldDb).quest_template qt ON qt.ID = qs.quest" +
        " WHERE qs.guid = $([uint32]$Guid) AND qs.status IN (1,3,5)"

    $quests = @()
    foreach ($line in (Invoke-QuestDbSelect -Context $Context -Sql $sql)) {
        $c = $line -split "`t"
        if ($c.Count -lt 35) { continue }
        $questId = [int]$c[0]; $status = [int]$c[1]
        $mobc = @($c[2..5] | ForEach-Object { [int]$_ })
        $itemc = @($c[6..11] | ForEach-Object { [int]$_ })
        $questType = [int]$c[12]; $questLevel = [int]$c[13]; $startItem = [int]$c[14]
        $reqNpc = @($c[15..18] | ForEach-Object { [int]$_ })
        $reqNpcC = @($c[19..22] | ForEach-Object { [int]$_ })
        $reqItemId = @($c[23..28] | ForEach-Object { [int]$_ })
        $reqItemC = @($c[29..34] | ForEach-Object { [int]$_ })
        $title = if ($c.Count -ge 36) { [string]$c[35] } else { '' }

        $objectives = @(); $hasNpc = $false; $hasGo = $false; $hasItem = $false
        for ($i = 0; $i -lt 4; $i++) {
            if ($reqNpc[$i] -ne 0 -and $reqNpcC[$i] -gt 0) {
                if ($reqNpc[$i] -gt 0) { $hasNpc = $true; $kind = 'npc'; $entry = $reqNpc[$i] }
                else { $hasGo = $true; $kind = 'gameobject'; $entry = - $reqNpc[$i] }
                $objectives += [pscustomobject]@{ kind = $kind; entry = $entry; required = $reqNpcC[$i]; current = $mobc[$i]; done = ($mobc[$i] -ge $reqNpcC[$i]) }
            }
        }
        for ($i = 0; $i -lt 6; $i++) {
            if ($reqItemId[$i] -gt 0 -and $reqItemC[$i] -gt 0) {
                $hasItem = $true
                $objectives += [pscustomobject]@{ kind = 'item'; entry = $reqItemId[$i]; required = $reqItemC[$i]; current = $itemc[$i]; done = ($itemc[$i] -ge $reqItemC[$i]) }
            }
        }
        $isComplete = ($status -eq 1)
        # SuggestedPlayers is not a WotLK quest_template column; rely on QuestType for group detection.
        $class = Get-CapabilityClass -QuestType $questType -SuggestedPlayers 0 -SrcItem $startItem -HasNpc $hasNpc -HasGo $hasGo -HasItem $hasItem -IsComplete $isComplete
        $quests += [pscustomobject]@{
            id = $questId; title = $title; status = $status; is_complete = $isComplete
            quest_type = $questType; level = $questLevel; suggested_players = 0; src_item = $startItem
            capability_class = $class; supported = (Test-CapabilitySupported $class); objectives = $objectives
            source = 'db'
        }
    }
    return , $quests
}

function ConvertTo-NormalizedFromBridge {
    param([object]$Parsed)
    $quests = @()
    foreach ($q in @($Parsed.quests)) {
        $objectives = @()
        foreach ($o in @($q.objectives)) {
            $objectives += [pscustomobject]@{ kind = [string]$o.kind; entry = [int]$o.entry; required = [int]$o.required; current = [int]$o.current; done = [bool]$o.done }
        }
        $quests += [pscustomobject]@{
            id = [int]$q.id; title = [string]$q.title; status = [int]$q.status; is_complete = [bool]$q.is_complete
            quest_type = [int]$q.quest_type; level = [int]$q.level; suggested_players = [int]$q.suggested_players; src_item = 0
            capability_class = [string]$q.capability_class; supported = [bool]$q.supported; objectives = $objectives
            source = 'bridge'
        }
    }
    return , $quests
}

# Returns @{ quests = <array>; source = 'bridge'|'db'; note = '' }
function Get-NormalizedQuestLog {
    param([Parameter(Mandatory = $true)][uint32]$Guid, [object]$DbContext = $null)
    try {
        $resp = Send-Bridge -Request "questlog $Guid" | ConvertFrom-Json
        if ($resp.ok) {
            return @{ quests = (ConvertTo-NormalizedFromBridge -Parsed $resp); source = 'bridge'; note = '' }
        }
        if ($resp.error -notmatch 'unknown command') {
            return @{ quests = @(); source = 'bridge'; note = [string]$resp.error }
        }
    }
    catch { }

    if ($null -ne $DbContext) {
        return @{ quests = (ConvertTo-NormalizedFromDb -Context $DbContext -Guid $Guid); source = 'db'; note = 'questlog endpoint absent; using read-only DB counters (save-lagged)' }
    }
    return @{ quests = @(); source = 'none'; note = 'questlog endpoint absent and no DB context' }
}
