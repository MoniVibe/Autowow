<#
.SYNOPSIS
    Cohort snapshot every IntervalSeconds into OutPath (meant for start-detached.ps1 so it outlives the caller).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutPath,
    [string]$Guids = ((62955..63004) -join ','),
    [int]$IntervalSeconds = 300,
    [int]$Count = 288
)
$guidList = @($Guids -split ',' | ForEach-Object { [uint32]$_ })
for ($i = 0; $i -lt $Count; $i++) {
    try { & (Join-Path $PSScriptRoot 'soak-snapshot.ps1') -OutPath $OutPath -Guids $guidList | Out-Null } catch {}
    Start-Sleep -Seconds $IntervalSeconds
}
