[CmdletBinding()]
param(
    [ValidateSet('initialize','validate','status','snapshot')][string]$Action = 'status',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$LeaguePath = '',
    [string]$Label = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$leagueRoot = Join-Path $ServerRoot 'leagues'
$templatePath = Join-Path $leagueRoot 'autowow-league.template.json'
if ([string]::IsNullOrWhiteSpace($LeaguePath)) { $LeaguePath = Join-Path $leagueRoot 'autowow-league.json' }

function Get-LeagueConfig {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw "League manifest not found: $Path. Run with -Action initialize first." }
    try { return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json) }
    catch { throw "League manifest is invalid JSON: $Path. $($_.Exception.Message)" }
}

function Test-LeagueConfig {
    param([object]$League)
    $errors = [System.Collections.Generic.List[string]]::new()
    if ($League.formatVersion -ne 1) { $errors.Add('formatVersion must be 1.') }
    if ([string]::IsNullOrWhiteSpace($League.leagueName)) { $errors.Add('leagueName is required.') }
    if ([int]$League.teamSize -lt 2 -or [int]$League.teamSize -gt 40) { $errors.Add('teamSize must be between 2 and 40.') }
    $teams = @($League.teams)
    if ($teams.Count -ne 2) { $errors.Add('Exactly two teams are required.') }
    $ids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $allAccounts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($team in $teams) {
        if ([string]::IsNullOrWhiteSpace($team.id) -or -not $ids.Add([string]$team.id)) { $errors.Add('Each team needs a unique id.') }
        if ($team.faction -notin @('Alliance','Horde')) { $errors.Add("Team '$($team.id)' must use Alliance or Horde.") }
        if ([string]::IsNullOrWhiteSpace($team.captainAccount)) { $errors.Add("Team '$($team.id)' needs a captainAccount.") }
        elseif ($team.captainAccount -notmatch '^[A-Z0-9_]{3,16}$') { $errors.Add("Team '$($team.id)' captainAccount must be 3-16 uppercase letters, digits, or underscores.") }
        $accounts = @($team.rosterAccounts)
        if ($accounts.Count -lt 1) { $errors.Add("Team '$($team.id)' needs at least one roster account.") }
        $captainSlots = if ($League.humanCaptainsIncluded) { 1 } else { 0 }
        $botSlotsNeeded = [int]$League.teamSize - $captainSlots
        if (($accounts.Count * 10) -lt $botSlotsNeeded) { $errors.Add("Team '$($team.id)' has only $($accounts.Count * 10) character slots for $botSlotsNeeded required bot slots.") }
        foreach ($account in $accounts) {
            if ([string]::IsNullOrWhiteSpace([string]$account)) { $errors.Add("Team '$($team.id)' has a blank roster account.") }
            elseif ($account -notmatch '^[A-Z0-9_]{3,16}$') { $errors.Add("Roster account '$account' must be 3-16 uppercase letters, digits, or underscores.") }
            elseif (-not $allAccounts.Add([string]$account)) { $errors.Add("Roster account '$account' is used by more than one team.") }
        }
    }
    return $errors
}

function Get-LiveLeagueStatus {
    param([object]$League)
    $control = Join-Path $PSScriptRoot 'autowow-control.ps1'
    if (-not (Test-Path -LiteralPath $control)) { throw "Bridge client is missing: $control" }
    $raw = & $control -Action list
    $bridge = (@($raw | ForEach-Object { $_.ToString() }) | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $bridge.ok) { throw "Bridge status failed: $($bridge.error)" }
    $bots = @($bridge.bots)
    $worldPidPath = Join-Path $ServerRoot 'worldserver.pid'
    $world = $null
    if (Test-Path -LiteralPath $worldPidPath) {
        try { $world = Get-Process -Id ([int](Get-Content -LiteralPath $worldPidPath -Raw).Trim()) -ErrorAction Stop } catch { }
    }
    if (-not $world) { throw 'worldserver is not running.' }
    $available = (Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue
    $maps = [ordered]@{}
    foreach ($group in ($bots | Group-Object { $_.position.map } | Sort-Object Name)) { $maps[[string]$group.Name] = $group.Count }
    return [ordered]@{
        timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
        league = $League.leagueName
        team_size = [int]$League.teamSize
        online_bots = $bots.Count
        alive_bots = @($bots | Where-Object alive).Count
        dead_bots = @($bots | Where-Object { -not $_.alive }).Count
        combat_bots = @($bots | Where-Object combat).Count
        map_population = $maps
        world_private_gib = [math]::Round($world.PrivateMemorySize64 / 1GB, 2)
        world_working_gib = [math]::Round($world.WorkingSet64 / 1GB, 2)
        world_cpu_seconds = [math]::Round($world.CPU, 2)
        available_memory_mib = [math]::Round($available, 0)
        recommended_next_tier = [int]$League.performanceGuardrails.nextPopulationTier
        performance_gate = [ordered]@{
            minimum_available_memory_mib = [int]$League.performanceGuardrails.minimumAvailableMemoryMiB
            maximum_world_private_gib = [int]$League.performanceGuardrails.maximumWorldPrivateGiB
            within_memory_limit = ($available -ge [int]$League.performanceGuardrails.minimumAvailableMemoryMiB)
            within_world_memory_limit = (($world.PrivateMemorySize64 / 1GB) -le [double]$League.performanceGuardrails.maximumWorldPrivateGiB)
        }
    }
}

switch ($Action) {
    'initialize' {
        if (-not (Test-Path -LiteralPath $templatePath)) { throw "League template is missing: $templatePath" }
        if (Test-Path -LiteralPath $LeaguePath) { throw "Refusing to overwrite existing league manifest: $LeaguePath" }
        Copy-Item -LiteralPath $templatePath -Destination $LeaguePath
        Write-Output "Created league manifest: $LeaguePath"
    }
    'validate' {
        $league = Get-LeagueConfig -Path $LeaguePath
        $errors = @(Test-LeagueConfig -League $league)
        if ($errors.Count) { throw ("League manifest validation failed:`n - " + ($errors -join "`n - ")) }
        Write-Output "League manifest is valid: $LeaguePath"
    }
    'status' {
        $league = Get-LeagueConfig -Path $LeaguePath
        $errors = @(Test-LeagueConfig -League $league)
        if ($errors.Count) { throw ("League manifest validation failed:`n - " + ($errors -join "`n - ")) }
        Get-LiveLeagueStatus -League $league | ConvertTo-Json -Depth 6
    }
    'snapshot' {
        $league = Get-LeagueConfig -Path $LeaguePath
        $errors = @(Test-LeagueConfig -League $league)
        if ($errors.Count) { throw ("League manifest validation failed:`n - " + ($errors -join "`n - ")) }
        $status = Get-LiveLeagueStatus -League $league
        $resultRoot = Join-Path $leagueRoot 'results'
        New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
        $safeLabel = if ([string]::IsNullOrWhiteSpace($Label)) { 'snapshot' } else { ($Label -replace '[^A-Za-z0-9_.-]', '_') }
        $path = Join-Path $resultRoot ('{0}-{1}-{2}.json' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $safeLabel, $status.online_bots)
        [System.IO.File]::WriteAllText($path, ($status | ConvertTo-Json -Depth 6), [System.Text.UTF8Encoding]::new($false))
        Write-Output "League snapshot: $path"
    }
}
