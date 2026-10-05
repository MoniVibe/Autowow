<#
.SYNOPSIS
    Create the combat-lab characters (Lab<Class>) on the owner's account and print the lab config block.
.DESCRIPTION
    DRY-RUN BY DEFAULT: prints the plan (free slots, characters, spec, item-level cap from the cohort median) and
    the console lines. -Apply runs them in a one-shot WSL worldserver (same maintenance-window path as
    cohort/provision-reps.ps1), so the world must be stopped. No accounts are created; existing characters
    (Flacid) are never touched.
    Characters are created stock (level 1). The owner finishes each one in game with `.autowow lab kit` (level,
    solo spec, gear capped at the cohort median item level), which needs the printed AutoWow.Lab.* config.
    Order is fixed (owner ruling 2026-10-05): rogue, warrior, shaman, druid, mage, priest, warlock, paladin are
    required; hunter only if a slot is still free (else the owner's hunter class stands in); no death knight
    (the cohort has none). The script refuses to apply when the required characters do not fit the free slots.
    PowerShell 5.1, ASCII only.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [string]$Account = 'SHONHAY',
    [ValidateRange(10,80)][int]$Level = 75,
    [ValidateRange(0,5)][int]$Quality = 2,
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$WorldserverBinary = '/root/autowow-advisor-t1-build/src/server/apps/worldserver',
    [string]$WorldserverConfig = '/root/p1runtime/worldserver.conf',
    [ValidateRange(30,900)][int]$ReadyTimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\cohort\cohort-lib.ps1')

$CharactersPerRealm = 10
# name (as the server normalizes it), class id, race id (Alliance, like the owner's night elf), gender, spec, required
$plan = @(
    @('Labrogue',   4, 1, 0, 'Combat',      $true),
    @('Labwarrior', 1, 1, 0, 'Arms',        $true),
    @('Labshaman',  7, 11, 0, 'Enhancement', $true),
    @('Labdruid',   11, 4, 0, 'Feral cat',   $true),
    @('Labmage',    8, 1, 1, 'Frost',       $true),
    @('Labpriest',  5, 1, 1, 'Shadow',      $true),
    @('Labwarlock', 9, 1, 0, 'Affliction',  $true),
    @('Labpaladin', 2, 1, 0, 'Retribution', $true),
    @('Labhunter',  3, 3, 0, 'Beast Mastery', $false)
)

$db = Get-CohortDb
$worldParts = Get-CohortDbParts -ConfigPath $script:CohortWorldConfig -Key 'WorldDatabaseInfo'
$acct = $Account.ToUpperInvariant()
if ($acct -cnotmatch '^[A-Z0-9_]{3,16}$') { throw "Account '$Account' is not a plain account name." }

$idLine = @(Invoke-CohortSql -DbInfo $db -Parts $db.auth -Sql "SELECT id FROM account WHERE username = '$acct'")
if ($idLine.Count -ne 1) { throw "Account $acct not found (this script never creates accounts)." }
$accountId = [int]$idLine[0]

$existing = @{}
foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql "SELECT name, class, level FROM characters WHERE account = $accountId")) {
    $f = ([string]$line) -split "`t"
    if ($f.Count -ge 3) { $existing[$f[0]] = @([int]$f[1], [int]$f[2]) }
}
$free = $CharactersPerRealm - $existing.Count

# Cohort median equipped item level per class (cohort accounts from the manifest, bots L67-78; shirt/tabard skipped).
$manifest = Get-CohortManifest
$cohortAccounts = (@($manifest.entries | ForEach-Object { [string]$_.account } | Sort-Object -Unique) | ForEach-Object { ConvertTo-CohortSqlLiteral $_ }) -join ','
$cohortIds = (@(Invoke-CohortSql -DbInfo $db -Parts $db.auth -Sql "SELECT id FROM account WHERE username IN ($cohortAccounts)")) -join ','
$equipped = @()
if ($cohortIds) {
    $equipped = @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql ("SELECT c.class, c.guid, ii.itemEntry FROM characters c " +
        "JOIN character_inventory ci ON ci.guid = c.guid AND ci.bag = 0 AND ci.slot < 19 AND ci.slot NOT IN (3,18) " +
        "JOIN item_instance ii ON ii.guid = ci.item WHERE c.account IN ($cohortIds) AND c.level BETWEEN 67 AND 78"))
}
$itemLevel = @{}
$entries = @($equipped | ForEach-Object { (([string]$_) -split "`t")[2] } | Sort-Object -Unique)
if ($entries.Count) {
    foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $worldParts -Sql "SELECT entry, ItemLevel FROM item_template WHERE entry IN ($($entries -join ','))")) {
        $f = ([string]$line) -split "`t"; $itemLevel[$f[0]] = [int]$f[1]
    }
}
$perChar = @{}
foreach ($line in $equipped) {
    $f = ([string]$line) -split "`t"
    $key = "$($f[0]):$($f[1])"
    if (-not $perChar.ContainsKey($key)) { $perChar[$key] = New-Object System.Collections.Generic.List[int] }
    if ($itemLevel.ContainsKey($f[2])) { $perChar[$key].Add($itemLevel[$f[2]]) }
}
$classMeans = @{}
foreach ($k in $perChar.Keys) {
    $cls = [int](($k -split ':')[0]); $v = $perChar[$k]
    if ($v.Count -eq 0) { continue }
    if (-not $classMeans.ContainsKey($cls)) { $classMeans[$cls] = New-Object System.Collections.Generic.List[double] }
    $classMeans[$cls].Add(($v | Measure-Object -Average).Average)
}
function Get-Median([double[]]$xs) {
    $s = @($xs | Sort-Object); $n = $s.Count
    if ($n -eq 0) { return 0 }
    if ($n % 2) { return $s[($n - 1) / 2] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}
$ilvl = @{}
foreach ($cls in $classMeans.Keys) { $ilvl[$cls] = [int][math]::Round((Get-Median $classMeans[$cls].ToArray())) }

# Slot plan: skip names that already exist on the account; required first, hunter only into a spare slot.
$create = New-Object System.Collections.Generic.List[object]
$skipped = New-Object System.Collections.Generic.List[string]
$slots = $free
foreach ($p in $plan) {
    if ($existing.ContainsKey($p[0])) {
        if ($existing[$p[0]][0] -ne $p[1]) { throw "$($p[0]) exists on $acct with another class; resolve by hand." }
        continue
    }
    if ($slots -gt 0) { $create.Add($p); $slots-- }
    elseif ($p[5]) { throw "Refused: $acct has $free free character slot(s) (of $CharactersPerRealm), not enough for the required lab set. Nothing applied." }
    else { $skipped.Add($p[0]) }
}

"Account $acct (id $accountId): $($existing.Count) character(s) [$(@($existing.Keys) -join ', ')], $free free slot(s)."
'Plan (created at level 1; finish in game with .autowow lab kit):'
'{0,-11} {1,-6} {2,-5} {3,-14} {4,-6} {5}' -f 'name', 'class', 'race', 'spec', 'level', 'ilvl_cap (cohort median)'
foreach ($p in $plan) {
    $state = if ($existing.ContainsKey($p[0])) { 'exists' } elseif (@($create | Where-Object { $_[0] -eq $p[0] }).Count) { 'create' } else { 'SKIP no slot' }
    $cap = if ($ilvl.ContainsKey([int]$p[1])) { $ilvl[[int]$p[1]] } else { 'none (no L67-78 cohort data)' }
    '{0,-11} {1,-6} {2,-5} {3,-14} {4,-6} {5}  [{6}]' -f $p[0], $p[1], $p[2], $p[4], $Level, $cap, $state
}
if ($skipped.Count) { "Not created (no free slot): $($skipped -join ', ') -- use the owner's own character of that class." }
''
'Config for the lab world (playerbots.conf; the kit reads these, nothing else changes):'
'AutoWow.Lab.Enable = 1'
'AutoWow.Lab.Trace = 1'
"AutoWow.Lab.Account = $accountId"
"AutoWow.Lab.Level = $Level"
"AutoWow.Lab.Quality = $Quality"
'AutoWow.Lab.KitIlvl = "{0}"' -f ((@($ilvl.Keys | Sort-Object) | ForEach-Object { "${_}:$($ilvl[$_])" }) -join ',')
''

$lines = @($create | ForEach-Object { ".autowow cohort create $acct $($_[2]) $($_[1]) $($_[3]) $($_[0])" })
if (-not $Apply) {
    'DRY RUN. Console lines:'
    if ($lines.Count) { $lines } else { '(nothing to create)' }
    return
}
if ($lines.Count -eq 0) { 'Nothing to create.'; return }

if (Get-Process -Name worldserver -ErrorAction SilentlyContinue) { throw 'A Windows worldserver is running.' }
$null = & wsl.exe -d $WslDistro -u root -- pgrep -x worldserver 2>$null
if ($LASTEXITCODE -eq 0) { throw 'A WSL worldserver is running; stop the world first.' }
$relay = Join-Path $script:CohortServerRoot 'work\phase1-wsl-runtime\mysql-relay.json'
if (-not (Test-Path -LiteralPath $relay)) { throw "The WSL MySQL relay is not running ($relay)." }

$logDir = Join-Path $script:CohortServerRoot 'logs\cohort'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'wsl.exe'
$psi.Arguments = "-d $WslDistro -u root -- $WorldserverBinary -c $WorldserverConfig"
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
$psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
$proc = [System.Diagnostics.Process]::Start($psi)
$out = $proc.StandardOutput.ReadToEndAsync(); $err = $proc.StandardError.ReadToEndAsync()
try {
    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
    $ready = $false
    while (-not $ready -and -not $proc.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $tcp = New-Object System.Net.Sockets.TcpClient
        try { $ready = $tcp.ConnectAsync('127.0.0.1', 18787).Wait(1000) -and $tcp.Connected } catch { $ready = $false } finally { $tcp.Dispose() }
    }
    if (-not $ready) { throw 'One-shot worldserver did not become ready.' }
    foreach ($l in $lines) { $proc.StandardInput.WriteLine($l); $proc.StandardInput.Flush(); Start-Sleep -Milliseconds 300 }
    Start-Sleep -Seconds 20
    $proc.StandardInput.WriteLine('.server shutdown 1'); $proc.StandardInput.Flush()
}
finally {
    $proc.StandardInput.Close()
    if (-not $proc.WaitForExit(300000)) { $proc.Kill(); Write-Warning 'One-shot worldserver was killed after 300s.' }
}
$text = $out.Result + "`n--- stderr ---`n" + $err.Result
$log = Join-Path $logDir ("provision-lab-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
[System.IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($false)))
"log: $log"
$text -split "`n" | Where-Object { $_ -match 'cohort create:' }
foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql "SELECT name, guid, class, level FROM characters WHERE account = $accountId ORDER BY guid")) {
    $f = ([string]$line) -split "`t"
    '{0} guid={1} class={2} level={3}' -f $f[0], $f[1], $f[2], $f[3]
}
