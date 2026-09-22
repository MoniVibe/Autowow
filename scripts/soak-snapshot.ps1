param(
    [Parameter(Mandatory = $true)][string]$OutPath,
    [uint32[]]$Guids = @()
)
# Read-only: one bridge 'list' call, flattened per-bot row. Appends one JSON line per call.
$ErrorActionPreference = 'Stop'
$raw = & (Join-Path $PSScriptRoot 'autowow-control.ps1') list 2>&1 | Select-Object -Last 1
$r = $raw | ConvertFrom-Json
$bots = @($r.bots)
if ($Guids.Count) { $bots = @($bots | Where-Object { $Guids -contains [uint32]$_.guid }) }
$rows = foreach ($b in $bots) {
    [ordered]@{
        guid = $b.guid; name = $b.name; level = $b.progress.level; xp = $b.progress.xp
        copper = $b.progress.money_copper; map = $b.position.map; zone = $b.position.zone
        x = $b.position.x; y = $b.position.y; z = $b.position.z
        alive = $b.alive; combat = $b.combat; state = $b.state
    }
}
$line = [ordered]@{ utc = (Get-Date).ToUniversalTime().ToString('o'); count = $bots.Count; bots = @($rows) }
$dir = Split-Path -Parent $OutPath
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
Add-Content -Path $OutPath -Value ($line | ConvertTo-Json -Depth 4 -Compress) -Encoding ASCII
$line
