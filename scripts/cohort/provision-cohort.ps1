<#
.SYNOPSIS
    Provision the persistent bot cohort: accounts, level-1 characters, league enrollment.
.DESCRIPTION
    DRY-RUN BY DEFAULT: validates the manifest, takes a read-only inventory (SELECT only) and
    prints the exact actions. -Apply performs them. Idempotent: existing accounts, characters
    and enrollments are verified and skipped; any conflict (name owned elsewhere, race/class
    mismatch, foreign team) aborts before anything is written.

    Transport:
      * Accounts + characters: worldserver console lines fed to a ONE-SHOT WSL worldserver
        (same pattern as create-account.ps1). Needs a maintenance window: -Apply refuses while
        any worldserver runs. Character creation uses the console command named by
        -CharacterCreateCommand; stock AzerothCore has no headless create-character command,
        so this requires the module command described in docs\COHORT_PLAN.md (section 2).
      * Enrollment: autowow_league_team / autowow_league_member rows (same table + pattern as
        oracle-race-campaign.ps1 / provision-gather-lane.ps1). Safe while the world runs.

    Secrets: the cohort account password comes only from $env:COHORT_ACCOUNT_PASSWORD; DB
    passwords come from the existing config connection strings. Neither is printed; console
    output is redacted before it is written to disk.
    Characters are created exactly as the stock create path makes them: level 1, starting
    gear and spells only. No gear, gold, level or spell grants.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$Offline,
    [string[]]$Id = @(),
    [ValidateSet('Both','Alliance','Horde')][string]$Faction = 'Both',
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'cohort-manifest.json'),
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$WorldserverBinary = '/root/autowow-advisor-t1-build/src/server/apps/worldserver',
    [string]$WorldserverConfig = '/root/p1runtime/worldserver.conf',
    [string]$CharacterCreateCommand = '.autowow cohort create {account} {race} {class} {gender} {name}',
    [ValidateRange(30,900)][int]$ReadyTimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cohort-lib.ps1')

if ($Apply -and $Offline) { throw '-Apply needs the read-only inventory; drop -Offline.' }
$manifest = Get-CohortManifest -Path $ManifestPath
$entries = @(Select-CohortEntries -Manifest $manifest -Id $Id -Faction $Faction)
if ($entries.Count -eq 0) { throw 'No active manifest entries selected.' }

function Get-ConsoleLines {
    param([object[]]$Rows, [switch]$Redacted)
    $pw = if ($Redacted) { '<COHORT_ACCOUNT_PASSWORD>' } else { $env:COHORT_ACCOUNT_PASSWORD }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($acct in @($Rows | Where-Object { $_.state -eq 'missing_account' } | ForEach-Object { $_.account } | Sort-Object -Unique)) {
        $lines.Add(".account create $acct $pw")
    }
    foreach ($r in @($Rows | Where-Object { $_.state -in @('missing_account','missing_character') })) {
        $e = @($entries | Where-Object { $_.id -eq $r.id })[0]
        $lines.Add($CharacterCreateCommand.Replace('{account}', [string]$e.account).Replace('{race}', [string]$e.race_id).
            Replace('{class}', [string]$e.class_id).Replace('{gender}', [string]$e.gender).Replace('{name}', [string]$e.name))
    }
    return $lines
}

function Get-EnrollmentSql {
    param([object[]]$Rows)
    $sql = New-Object System.Text.StringBuilder
    foreach ($t in @($manifest.teams)) {
        [void]$sql.AppendLine("INSERT INTO autowow_league_team (team_id,display_name,faction,roster_cap,worker_cap,treasury_copper) VALUES ($(ConvertTo-CohortSqlLiteral $t.team_id),$(ConvertTo-CohortSqlLiteral $t.display_name),$(ConvertTo-CohortSqlLiteral $t.faction),200,0,0) ON DUPLICATE KEY UPDATE display_name=VALUES(display_name);")
    }
    foreach ($r in @($Rows | Where-Object { $_.guid -and -not $_.enrolled })) {
        $e = @($entries | Where-Object { $_.id -eq $r.id })[0]
        # INSERT IGNORE never steals a row enrolled elsewhere; the verification pass reports that as a conflict.
        [void]$sql.AppendLine("INSERT IGNORE INTO autowow_league_member (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active) VALUES ($($r.guid),$(ConvertTo-CohortSqlLiteral $r.team_id),'guild','adventurer',$(ConvertTo-CohortSqlLiteral ([string]$e.class).ToLowerInvariant()),'','',0);")
    }
    return $sql.ToString()
}

function Test-WorldRunning {
    if (Get-Process -Name worldserver -ErrorAction SilentlyContinue) { return $true }
    $null = & wsl.exe -d $WslDistro -u root -- pgrep -x worldserver 2>$null
    return ($LASTEXITCODE -eq 0)
}

function Invoke-OneShotConsole {
    param([string[]]$Lines)
    if (Test-WorldRunning) { throw 'A worldserver is running. Console provisioning needs a maintenance window (stop the soak first); enrollment-only runs do not.' }
    $relay = Join-Path $script:CohortServerRoot 'work\phase1-wsl-runtime\mysql-relay.json'
    if (-not (Test-Path -LiteralPath $relay)) { throw "The WSL MySQL relay is not running ($relay). Start scripts\phase1-wsl-mysql-relay.ps1 first." }
    $logDir = Join-Path $script:CohortServerRoot 'logs\cohort'
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'wsl.exe'
    $psi.Arguments = "-d $WslDistro -u root -- $WorldserverBinary -c $WorldserverConfig"
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out = $proc.StandardOutput.ReadToEndAsync(); $err = $proc.StandardError.ReadToEndAsync()
    try {
        # The bridge listener opens once the world has finished loading; use it as the ready signal.
        $deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
        $ready = $false
        while (-not $ready -and -not $proc.HasExited -and (Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 2
            $tcp = New-Object System.Net.Sockets.TcpClient
            try { $ready = $tcp.ConnectAsync('127.0.0.1', 18787).Wait(1000) -and $tcp.Connected } catch { $ready = $false } finally { $tcp.Dispose() }
        }
        if (-not $ready) { throw 'One-shot worldserver did not become ready.' }
        foreach ($l in $Lines) { $proc.StandardInput.WriteLine($l); $proc.StandardInput.Flush(); Start-Sleep -Milliseconds 300 }
        Start-Sleep -Seconds 20   # let async character saves drain before shutdown
    }
    finally {
        # Console EOF triggers AzerothCore's normal shutdown path (saves are flushed).
        $proc.StandardInput.Close()
        if (-not $proc.WaitForExit(300000)) { $proc.Kill(); Write-Warning 'One-shot worldserver was killed after 300s.' }
    }
    $pw = $env:COHORT_ACCOUNT_PASSWORD
    $text = ($out.Result + "`n--- stderr ---`n" + $err.Result).Replace($pw, '<redacted>')
    $log = Join-Path $logDir "provision-console-$stamp.log"
    [System.IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($false)))
    return $log
}

# ---- inventory -------------------------------------------------------------------------
if ($Offline) {
    $rows = @($entries | ForEach-Object {
        [pscustomobject][ordered]@{ id = $_.id; name = $_.name; account = $_.account; account_id = $null; guid = $null; level = $null
            state = 'missing_account'; detail = 'offline: assumed missing'; enrolled = $false; team_id = (Get-CohortTeamId -Faction $_.faction); control_arm = $_.control_arm }
    })
    $plan = [ordered]@{
        mode = 'dry-run-offline (no database access; everything assumed missing)'
        entries = $entries.Count
        console_lines_if_all_missing = @(Get-ConsoleLines -Rows $rows -Redacted)
        enrollment = 'SQL is generated after characters resolve to GUIDs (online dry-run shows it).'
    }
    $plan | ConvertTo-Json -Depth 5
    return
}

$db = Get-CohortDb
$inv = Get-CohortInventory -DbInfo $db -Entries $entries
$conflicts = @($inv.rows | Where-Object { $_.state -eq 'conflict' })
$consoleRows = @($inv.rows | Where-Object { $_.state -in @('missing_account','missing_character') })
$plan = [ordered]@{
    mode = if ($Apply) { 'apply' } else { 'dry-run' }
    entries = $entries.Count
    ok = @($inv.rows | Where-Object { $_.state -eq 'ok' }).Count
    missing_accounts = @($inv.rows | Where-Object { $_.state -eq 'missing_account' } | ForEach-Object { $_.account } | Sort-Object -Unique)
    missing_characters = @($consoleRows | ForEach-Object { $_.id })
    conflicts = @($conflicts | ForEach-Object { "$($_.id) $($_.name): $($_.detail)" })
    console_lines = @(Get-ConsoleLines -Rows $inv.rows -Redacted)
    enrollment_sql = (Get-EnrollmentSql -Rows $inv.rows)
}
if (-not $Apply) { $plan | ConvertTo-Json -Depth 5; return }

# ---- apply -------------------------------------------------------------------------------
if ($conflicts.Count) { throw ("Conflicts; nothing was written:`n - " + ($plan.conflicts -join "`n - ")) }
if ($consoleRows.Count) {
    $pw = $env:COHORT_ACCOUNT_PASSWORD
    if (@($inv.rows | Where-Object { $_.state -eq 'missing_account' }).Count -and
        ([string]::IsNullOrWhiteSpace($pw) -or $pw.Length -lt 3 -or $pw.Length -gt 16 -or $pw -match '\s')) {
        throw 'Set COHORT_ACCOUNT_PASSWORD (3-16 chars, no whitespace) in the process environment.'
    }
    $plan['console_log'] = Invoke-OneShotConsole -Lines @(Get-ConsoleLines -Rows $inv.rows)
    $inv = Get-CohortInventory -DbInfo $db -Entries $entries
}
$enroll = Get-EnrollmentSql -Rows $inv.rows
Invoke-CohortSql -DbInfo $db -Parts $db.playerbots -Sql $enroll -Write | Out-Null
$inv = Get-CohortInventory -DbInfo $db -Entries $entries
$plan['final'] = @($inv.rows | Select-Object id, name, guid, level, state, enrolled, control_arm, detail)
$bad = @($inv.rows | Where-Object { $_.state -ne 'ok' -or -not $_.enrolled })
$plan['verified'] = ($bad.Count -eq 0)
$plan | ConvertTo-Json -Depth 5
if ($bad.Count) { throw ("Provisioning incomplete for: " + (($bad | ForEach-Object { "$($_.id)=$($_.state)" }) -join ', ')) }
