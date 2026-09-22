<#
.SYNOPSIS
    Builds an isolated, minimal observer WoW working directory without modifying the source client.

.DESCRIPTION
    Root binaries and small metadata are copied. Immutable MPQ game data is hard-linked when source
    and destination share a volume, otherwise copied. WTF, Cache, Logs, Screenshots, Interface, and
    realmlist.wtf are private. Existing source files are never opened for write.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory)][string]$SourceClientDirectory,
    [string]$ObserverClientDirectory = ''
)

. (Join-Path $PSScriptRoot 'observer-automation-lib.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$source = (Resolve-Path -LiteralPath $SourceClientDirectory).Path.TrimEnd('\')
if ([string]::IsNullOrWhiteSpace($ObserverClientDirectory)) { $ObserverClientDirectory = Join-Path $ServerRoot 'work\observer-client' }
$destination = [IO.Path]::GetFullPath($ObserverClientDirectory).TrimEnd('\')
if ([string]::Equals($source, $destination, [StringComparison]::OrdinalIgnoreCase) -or
    $destination.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Observer client directory must be separate from and outside the source client.'
}
if (-not $destination.StartsWith($ServerRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Observer client directory must remain inside the AutoWoW workspace.'
}

$sourceWow = Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Name -in @('Wow.exe','WoW.exe') } | Select-Object -First 1
if (-not $sourceWow) { throw "No 3.3.5 Wow.exe found directly under $source" }
$sourceData = Join-Path $source 'Data'
if (-not (Test-Path -LiteralPath $sourceData -PathType Container)) { throw "Source client Data directory is missing: $sourceData" }

New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($privateName in @('WTF','Cache','Logs','Screenshots','Interface','Interface\AddOns','Data')) {
    $privatePath = Join-Path $destination $privateName
    if ((Test-Path -LiteralPath $privatePath) -and ((Get-Item -LiteralPath $privatePath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Private observer path must not be a junction or symlink: $privatePath"
    }
    New-Item -ItemType Directory -Path $privatePath -Force | Out-Null
}

# Copy only launch-time root files. The observer does not inherit the user's WTF/Cache/Logs/Interface.
$rootFiles = Get-ChildItem -LiteralPath $source -File -Force | Where-Object {
    $_.Extension -in @('.exe','.dll','.html')
}
foreach ($file in $rootFiles) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $destination $file.Name) -Force
}

$linked = 0
$copied = 0
$sameVolume = [string]::Equals([IO.Path]::GetPathRoot($sourceData), [IO.Path]::GetPathRoot($destination), [StringComparison]::OrdinalIgnoreCase)
foreach ($directory in Get-ChildItem -LiteralPath $sourceData -Directory -Recurse -Force) {
    $relative = [IO.Path]::GetRelativePath($sourceData, $directory.FullName)
    New-Item -ItemType Directory -Path (Join-Path (Join-Path $destination 'Data') $relative) -Force | Out-Null
}
foreach ($file in Get-ChildItem -LiteralPath $sourceData -File -Recurse -Force) {
    $relative = [IO.Path]::GetRelativePath($sourceData, $file.FullName)
    $target = Join-Path (Join-Path $destination 'Data') $relative
    $targetParent = Split-Path -Parent $target
    New-Item -ItemType Directory -Path $targetParent -Force | Out-Null

    if ($file.Extension -ieq '.mpq' -and $sameVolume) {
        if (Test-Path -LiteralPath $target) {
            if ((Get-Item -LiteralPath $target).Length -ne $file.Length) { throw "Existing observer MPQ does not match source length: $target" }
        }
        else {
            New-Item -ItemType HardLink -Path $target -Target $file.FullName | Out-Null
        }
        $linked++
    }
    else {
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        $copied++
    }
}

$privateRealmlists = @(Get-ChildItem -LiteralPath (Join-Path $destination 'Data') -Filter 'realmlist.wtf' -File -Recurse)
if ($privateRealmlists.Count -eq 0) {
    $localeDirectory = Get-ChildItem -LiteralPath (Join-Path $destination 'Data') -Directory | Where-Object { $_.Name -match '^[a-z]{2}[A-Z]{2}$' } | Select-Object -First 1
    if (-not $localeDirectory) { throw 'Could not identify a locale directory for private realmlist.wtf.' }
    $privateRealmlists = @((New-Item -ItemType File -Path (Join-Path $localeDirectory.FullName 'realmlist.wtf') -Force))
}
foreach ($realmlist in $privateRealmlists) {
    [IO.File]::WriteAllText($realmlist.FullName, "set realmlist 127.0.0.1`r`n", [Text.UTF8Encoding]::new($false))
}

$configPath = Join-Path $destination 'WTF\Config.wtf'
$config = @'
SET accountName "AUTOWATCH"
SET realmName "AutoWoW"
SET gxWindow "1"
SET gxMaximize "0"
SET gxResolution "800x600"
SET gxRefresh "60"
SET gxVSync "0"
SET maxFPS "20"
SET farclip "177"
SET groundEffectDensity "16"
SET groundEffectDist "40"
SET shadowLevel "0"
SET textureFilteringMode "0"
SET weatherDensity "0"
SET readTOS "1"
SET readEULA "1"
'@
[IO.File]::WriteAllText($configPath, $config.TrimStart() + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))

$observerWow = Join-Path $destination $sourceWow.Name
$manifestPath = Join-Path $destination 'observer-client-manifest.json'
$manifest = [pscustomobject][ordered]@{
    schema = 'autowow.observer.client.v1'
    prepared_utc = (Get-Date).ToUniversalTime().ToString('o')
    source_client = $source
    observer_client = $destination
    executable_path = $observerWow
    data_strategy = if ($sameVolume) { 'hardlink-mpq-copy-metadata' } else { 'copy-all-data-cross-volume' }
    linked_mpq_count = $linked
    copied_data_file_count = $copied
    private_directories = @('WTF','Cache','Logs','Screenshots','Interface')
    config_path = $configPath
    account_name_saved = $true
    password_saved = $false
    source_client_modified = $false
}
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
Write-Output "Observer client prepared without modifying the source client. Manifest: $manifestPath"
