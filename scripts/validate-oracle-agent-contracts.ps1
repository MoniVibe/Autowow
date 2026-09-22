[CmdletBinding()]
param(
    [string]$ContractPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'contracts\oracle-agent-contracts.v1.json'),
    [switch]$AsJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'oracle-controlled-agent-lib.ps1')

$result = Test-OracleControlledAgentCatalog -Catalog (Get-OracleControlledAgentCatalog -Path $ContractPath)
if ($AsJson) {
    $result | ConvertTo-Json -Depth 12
} else {
    if ($result.valid) {
        Write-Output "PASS: $($result.contract_count) contracts and $($result.doctrine_count) doctrines validated."
    } else {
        Write-Output "FAIL: $(@($result.errors).Count) validation errors."
        @($result.errors) | ForEach-Object { Write-Output "- $_" }
    }
}
if (-not $result.valid) { exit 1 }
