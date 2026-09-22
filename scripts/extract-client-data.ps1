[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory = $true)][string]$WowClientDir,
    [string]$DataDir = '',
    [switch]$SkipMMaps
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$WowClientDir = (Resolve-Path -LiteralPath $WowClientDir).Path
if ([string]::IsNullOrWhiteSpace($DataDir)) { $DataDir = Join-Path $ServerRoot 'server\data' }
$DataDir = [System.IO.Path]::GetFullPath($DataDir)
Initialize-AutoWoWLayout -Root $ServerRoot
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null

$wowExe = Get-ChildItem -LiteralPath $WowClientDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in @('Wow.exe', 'WoW.exe', 'Wow-64.exe', 'WoW-64.exe') } |
    Select-Object -First 1
if (-not $wowExe) { throw "No WoW executable found directly under $WowClientDir" }
$fileVersion = $wowExe.VersionInfo.FileVersion
$productVersion = $wowExe.VersionInfo.ProductVersion
$binaryMarker = $false
if (($fileVersion -notmatch '12340') -and ($productVersion -notmatch '12340')) {
    $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($wowExe.FullName))
    $binaryMarker = $ascii.Contains('12340')
}
if (($fileVersion -notmatch '12340') -and ($productVersion -notmatch '12340') -and -not $binaryMarker) {
    throw "Client does not appear to be build 12340. FileVersion='$fileVersion'; ProductVersion='$productVersion'; BinaryMarker12340=$binaryMarker. No extraction was started."
}

$installRoot = Join-Path $ServerRoot 'server'
$extractor = Join-Path $installRoot 'map_extractor.exe'
$vmapExtractor = Join-Path $installRoot 'vmap4_extractor.exe'
$vmapAssembler = Join-Path $installRoot 'vmap4_assembler.exe'
$mmapsGenerator = Join-Path $installRoot 'mmaps_generator.exe'
foreach ($tool in @($extractor,$vmapExtractor,$vmapAssembler)) { if (-not (Test-Path -LiteralPath $tool)) { throw "Extractor missing: $tool" } }
if (-not $SkipMMaps -and -not (Test-Path -LiteralPath $mmapsGenerator)) { throw "MMaps generator missing: $mmapsGenerator" }

$workRoot = Join-Path $ServerRoot 'work\client-extraction'
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null
$sourceDataDir = Join-Path $WowClientDir 'Data'
if (-not (Test-Path -LiteralPath $sourceDataDir)) { throw "Client Data directory missing: $sourceDataDir" }
Assert-WowClientDataReady -ClientDir $WowClientDir
$expectedDataTarget = (Resolve-Path -LiteralPath $sourceDataDir).Path
$dataLink = Join-Path $workRoot 'Data'
if (Test-Path -LiteralPath $dataLink) {
    $item = Get-Item -LiteralPath $dataLink -Force
    $actualDataTarget = $null
    if ($item.Target) {
        try { $actualDataTarget = (Get-Item -LiteralPath $item.Target -Force).FullName } catch { $actualDataTarget = $null }
    }
    if ($item.LinkType -ne 'Junction' -or -not $actualDataTarget -or [System.IO.Path]::GetFullPath($actualDataTarget) -ne [System.IO.Path]::GetFullPath($expectedDataTarget)) {
        throw "Refusing to reuse extraction Data link with an unexpected target: $dataLink"
    }
}
else {
    New-Item -ItemType Junction -Path $dataLink -Target (Join-Path $WowClientDir 'Data') | Out-Null
}

function Copy-ExtractedFolder {
    param([string]$Name)
    $source = Join-Path $workRoot $Name
    if (-not (Test-Path -LiteralPath $source)) { throw "Extractor did not produce $source" }
    $destination = Join-Path $DataDir $Name
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

$mapLog = New-AutoWoWLogPath -Name 'extract-maps'
foreach ($folder in @('dbc','maps','cameras')) {
    $stale = Join-Path $workRoot $folder
    if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Recurse -Force }
}
Invoke-NativeLogged -FilePath $extractor -ArgumentList @() -WorkingDirectory $workRoot -LogPath $mapLog | Out-Null
foreach ($folder in @('dbc','maps','cameras')) { Copy-ExtractedFolder -Name $folder }

$vmapLog = New-AutoWoWLogPath -Name 'extract-vmaps'
foreach ($folder in @('Buildings','vmaps')) {
    $stale = Join-Path $workRoot $folder
    if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Recurse -Force }
}
Invoke-NativeLogged -FilePath $vmapExtractor -ArgumentList @() -WorkingDirectory $workRoot -LogPath $vmapLog | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $workRoot 'Buildings'))) { throw 'vmap4_extractor did not produce Buildings.' }
New-Item -ItemType Directory -Path (Join-Path $workRoot 'vmaps') -Force | Out-Null
Invoke-NativeLogged -FilePath $vmapAssembler -ArgumentList @('Buildings', 'vmaps') -WorkingDirectory $workRoot -LogPath (New-AutoWoWLogPath -Name 'assemble-vmaps') | Out-Null
Copy-ExtractedFolder -Name 'vmaps'
Remove-Item -LiteralPath (Join-Path $workRoot 'Buildings') -Recurse -Force

if (-not $SkipMMaps) {
    $mmapsLog = New-AutoWoWLogPath -Name 'extract-mmaps'
    $staleMmaps = Join-Path $workRoot 'mmaps'
    if (Test-Path -LiteralPath $staleMmaps) { Remove-Item -LiteralPath $staleMmaps -Recurse -Force }
    Invoke-NativeLogged -FilePath $mmapsGenerator -ArgumentList @('--config', (Join-Path $installRoot 'mmaps-config.yaml')) -WorkingDirectory $workRoot -LogPath $mmapsLog | Out-Null
    Copy-ExtractedFolder -Name 'mmaps'
}

$report = @"
# AutoWoW client-data extraction report

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

- Client: $WowClientDir
- WoW executable: $($wowExe.FullName)
- FileVersion: $fileVersion
- ProductVersion: $productVersion
- Binary marker `12340`: $binaryMarker
- Extraction work root: $workRoot
- Installed DataDir: $DataDir
- Original client modified: **No**; extraction used a junction to the original `Data` directory and wrote outputs under the separate work root.
- MMaps: $(if ($SkipMMaps) { 'skipped by request' } else { 'completed' })

Logs:

- Maps/DBC/cameras: $mapLog
- VMaps: $vmapLog
- MMaps: $(if ($SkipMMaps) { 'N/A' } else { $mmapsLog })
"@
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'EXTRACTION_REPORT.md'), $report, $utf8NoBom)
Write-Output "Client data extraction completed. Report: $(Join-Path $ServerRoot 'EXTRACTION_REPORT.md')"
