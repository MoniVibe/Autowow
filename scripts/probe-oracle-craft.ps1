[CmdletBinding()]
param(
    [ValidateRange(1, 4294967295)][uint32]$BotGuid = 101,
    [switch]$Execute,
    [ValidateRange(1, 30)][int]$TimeoutSeconds = 15,
    [ValidateRange(100, 5000)][int]$PollMilliseconds = 500,
    [string]$ReceiptPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
$stages = [System.Collections.Generic.List[object]]::new()

function Invoke-ControlJson {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('professioneconomy', 'craft', 'craft-status')][string]$Action,
        [uint32]$RecipeSpellId = 0
    )

    $arguments = @{ Action = $Action; BotGuid = $BotGuid }
    if ($Action -eq 'craft') { $arguments.RecipeSpellId = $RecipeSpellId }
    $raw = (& $control @arguments | Out-String).Trim()
    if ([string]::IsNullOrWhiteSpace($raw)) {
        throw "The bridge returned an empty response for $Action."
    }
    return $raw | ConvertFrom-Json
}

function Add-Stage {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][object]$Response
    )
    $stages.Add([pscustomobject][ordered]@{
            at_utc = [datetime]::UtcNow.ToString('o')
            name = $Name
            response = $Response
        })
}

function Emit-Receipt {
    param([Parameter(Mandatory = $true)][object]$Receipt)
    $json = $Receipt | ConvertTo-Json -Depth 12 -Compress
    if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) {
        $parent = Split-Path -Parent $ReceiptPath
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        Add-Content -LiteralPath $ReceiptPath -Value $json
    }
    Write-Output $json
}

$started = [datetime]::UtcNow
$receipt = [ordered]@{
    schema = 'autowow.oracle.craft-probe.v1'
    started_utc = $started.ToString('o')
    bot_guid = $BotGuid
    mode = if ($Execute) { 'execute_once' } else { 'read_only' }
    mutation_count = 0
    state = 'starting'
    stages = $stages
}

$economy = Invoke-ControlJson -Action professioneconomy
Add-Stage -Name 'professioneconomy' -Response $economy
if (-not $economy.ok) {
    $receipt.state = 'blocked'
    $receipt.reason = 'craft:profession_telemetry_unavailable'
    Emit-Receipt -Receipt $receipt
    exit 0
}

$candidates = @($economy.recipes.items |
    Where-Object {
        [bool]$_.immediately_craftable -and
        [bool]$_.deterministic_output -and
        [bool]$_.output_capacity
    } |
    Sort-Object { [uint32]$_.spell_id })

if ($candidates.Count -eq 0) {
    $receipt.state = 'blocked'
    $receipt.reason = 'craft:no_immediately_craftable_recipe'
    $receipt.observed_blocked_recipes = @($economy.recipes.items |
        Sort-Object { [uint32]$_.spell_id } |
        Select-Object -First 16 spell_id, name, block_reason, limiting_reagent_item_id, limiting_reagent_bag_count)
    Emit-Receipt -Receipt $receipt
    exit 0
}

$selected = $candidates[0]
$receipt.selected_recipe = [ordered]@{
    spell_id = [uint32]$selected.spell_id
    name = [string]$selected.name
    output_item_id = [uint32]$selected.output_item_id
    output_quantity = [uint32]$selected.output_quantity
    profession = [string]$selected.profession
}

if (-not $Execute) {
    $receipt.state = 'planned'
    $receipt.reason = 'craft:execute_switch_required'
    Emit-Receipt -Receipt $receipt
    exit 0
}

$cast = Invoke-ControlJson -Action craft -RecipeSpellId ([uint32]$selected.spell_id)
Add-Stage -Name 'craft' -Response $cast
$receipt.mutation_count = 1
if (-not $cast.ok) {
    $receipt.state = 'failed'
    $receipt.reason = [string]$cast.error
    Emit-Receipt -Receipt $receipt
    exit 1
}

$deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
do {
    Start-Sleep -Milliseconds $PollMilliseconds
    $status = Invoke-ControlJson -Action craft-status
    Add-Stage -Name 'craft-status' -Response $status
    if ($status.completed) {
        $receipt.state = 'completed'
        $receipt.proof = [string]$status.proof
        $receipt.completed = $true
        Emit-Receipt -Receipt $receipt
        exit 0
    }
    if (-not $status.pending) {
        $receipt.state = 'failed'
        $receipt.reason = [string]$status.error
        Emit-Receipt -Receipt $receipt
        exit 1
    }
} while ([datetime]::UtcNow -lt $deadline)

$receipt.state = 'failed'
$receipt.reason = 'craft:probe_timeout'
Emit-Receipt -Receipt $receipt
exit 1
