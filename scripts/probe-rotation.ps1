<#
.SYNOPSIS
    Read-only anti-treadmill planner for breadth-oriented Probe Lab lanes.
.DESCRIPTION
    Reads a rotation catalog whose history entries point to Probe Lab summary and
    receipt files. It classifies candidates and emits a machine-readable plan.
    This script never invokes the bridge, starts/stops a process, writes a DB, or
    mutates runtime state. An output file is written only when -OutputPath is explicit.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CatalogPath,
    [ValidateRange(1,64)][int]$MaxCandidates = 2,
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe-rotation-lib.ps1')

$catalog = Read-ProbeRotationCatalog -Path $CatalogPath
$plan = New-ProbeRotationPlan -Catalog $catalog -MaxCandidates $MaxCandidates
$json = $plan | ConvertTo-Json -Depth 50
if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $parent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding utf8NoBOM
}
$json
