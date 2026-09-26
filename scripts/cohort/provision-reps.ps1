<#
.SYNOPSIS
    Create the 8 guild-rep characters (4 profession houses x 2 factions) on the existing cohort accounts.
.DESCRIPTION
    DRY-RUN BY DEFAULT: prints the console lines. -Apply runs them in a one-shot WSL worldserver, the same
    maintenance-window path as provision-cohort.ps1, so the world must be stopped.
    Characters are created the stock way: level 1, starting gear and spells, no grants. No accounts are created.
    After -Apply, the script prints the guid of each rep for the AutoWow.Guilds.Rep.<House>.<Team> keys.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [ValidateSet('reps','squad')][string]$Set = 'reps',
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$WorldserverBinary = '/root/autowow-advisor-t1-build/src/server/apps/worldserver',
    [string]$WorldserverConfig = '/root/p1runtime/worldserver.conf',
    [ValidateRange(30,900)][int]$ReadyTimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cohort-lib.ps1')

# role, team, account, race id, class id, gender, name
# reps: 1 per house per faction (warriors, level 1, capital hubs)
$sets = @{
    reps = @(
        @('Weavers','Alliance','AWPVP1A01',1,1,0,'Wevrin'),
        @('Smiths','Alliance','AWPVP1A01',1,1,0,'Smithal'),
        @('Tanners','Alliance','AWPVP1A01',1,1,0,'Tannoc'),
        @('Brewers','Alliance','AWPVP1A01',1,1,0,'Brewick'),
        @('Weavers','Horde','AWPVP1H01',2,1,0,'Zugweva'),
        @('Smiths','Horde','AWPVP1H01',2,1,0,'Kragsmit'),
        @('Tanners','Horde','AWPVP1H01',2,1,0,'Durtan'),
        @('Brewers','Horde','AWPVP1H01',2,1,0,'Gorbrew')
    )
    # squad: 5 gatherers per faction (dwarves / undead, mixed classes) + 1 Brewers artisan per faction
    squad = @(
        @('Squad','Alliance','AWPVP1A02',3,1,0,'Brannik'),
        @('Squad','Alliance','AWPVP1A02',3,2,0,'Thordal'),
        @('Squad','Alliance','AWPVP1A02',3,5,1,'Hilvara'),
        @('Squad','Alliance','AWPVP1A02',3,3,0,'Gromdur'),
        @('Squad','Alliance','AWPVP1A02',3,4,1,'Kellda'),
        @('Artisan.Brewers','Alliance','AWPVP1A03',4,11,1,'Aelmira'),
        @('Squad','Horde','AWPVP1H02',5,1,0,'Morthal'),
        @('Squad','Horde','AWPVP1H02',5,5,1,'Veskara'),
        @('Squad','Horde','AWPVP1H02',5,4,0,'Grimsel'),
        @('Squad','Horde','AWPVP1H02',5,8,0,'Dusmor'),
        @('Squad','Horde','AWPVP1H02',5,9,1,'Yzolde'),
        @('Artisan.Brewers','Horde','AWPVP1H03',6,11,0,'Tahnoka')
    )
}
$reps = $sets[$Set]
$lines = @($reps | ForEach-Object { ".autowow cohort create $($_[2]) $($_[3]) $($_[4]) $($_[5]) $($_[6])" })

if (-not $Apply) {
    'DRY RUN. Console lines:'
    $lines
    return
}

if (Get-Process -Name worldserver -ErrorAction SilentlyContinue) { throw 'A Windows worldserver is running.' }
$null = & wsl.exe -d $WslDistro -u root -- pgrep -x worldserver 2>$null
if ($LASTEXITCODE -eq 0) { throw 'A WSL worldserver is running; stop the soak first.' }
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
$log = Join-Path $logDir ("provision-{1}-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Set)
[System.IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($false)))
"log: $log"
$text -split "`n" | Where-Object { $_ -match 'cohort create:' }

$names = ($reps | ForEach-Object { "'$($_[6])'" }) -join ','
$db = Get-CohortDb
$guidOf = @{}
foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql "SELECT name, guid FROM characters WHERE name IN ($names)")) {
    $f = ([string]$line) -split "`t"
    if ($f.Count -ge 2) { $guidOf[$f[0]] = $f[1] }
}
foreach ($r in $reps) {
    $g = if ($guidOf.ContainsKey($r[6])) { $guidOf[$r[6]] } else { 'MISSING' }
    '{0}.{1} {2} guid={3}' -f $r[0], $r[1], $r[6], $g
}
