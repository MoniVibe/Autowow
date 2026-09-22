[CmdletBinding()]
param(
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
}

function Get-OptionalProperty {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][object]$Default = $null
    )
    if ($null -eq $InputObject) { return $Default }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function Assert-Equal {
    param([AllowNull()][object]$Actual, [AllowNull()][object]$Expected, [string]$Label)
    if ([string]$Actual -ne [string]$Expected) {
        throw "[$Label] expected '$Expected', got '$Actual'"
    }
}

$library = Join-Path $Root 'scripts\oracle-race-campaign-lib.ps1'
if (-not (Test-Path -LiteralPath $library)) { throw "missing library: $library" }
. $library

$definition = Get-OracleRaceSeedByGuid -Guid 236
$missing = Get-OracleProfessionPlanAssessment -Definition $definition -Telemetry ([pscustomobject]@{
    professions = @(
        [pscustomobject]@{ name = 'tailoring' }
    )
})
Assert-Equal $missing.status 'not_realized' 'Nolliano missing Herbalism'
Assert-Equal (@($missing.missing) -join ',') 'herbalism' 'Nolliano missing list'

$realized = Get-OracleProfessionPlanAssessment -Definition $definition -Telemetry ([pscustomobject]@{
    professions = @(
        [pscustomobject]@{ name = 'Herbalism' }
        [pscustomobject]@{ name = 'TAILORING' }
    )
})
Assert-Equal $realized.status 'realized' 'case-insensitive realized plan'
Assert-Equal @($realized.missing).Count 0 'realized missing list'

$unavailable = Get-OracleProfessionPlanAssessment -Definition $definition -Telemetry $null
Assert-Equal $unavailable.status 'unavailable' 'missing telemetry is not proof'

$campaign = Join-Path $Root 'scripts\oracle-race-campaign.ps1'
$tokens = $null
$errors = $null
[System.Management.Automation.Language.Parser]::ParseFile($campaign, [ref]$tokens, [ref]$errors) | Out-Null
if (@($errors).Count -ne 0) { throw "campaign script parse failed: $(@($errors) -join '; ')" }

Write-Output 'oracle-profession-economy contract: PASS'
