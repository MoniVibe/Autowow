[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory = $true)][string]$WowClientDir,
    [string]$ClientCopyDir = '',
    [switch]$RefreshExistingCopy
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$WowClientDir = (Resolve-Path -LiteralPath $WowClientDir).Path
if ([string]::IsNullOrWhiteSpace($ClientCopyDir)) { $ClientCopyDir = Join-Path $ServerRoot 'work\wow-client' }
$clientSourceFull = [System.IO.Path]::GetFullPath($WowClientDir).TrimEnd('\')
$clientCopyFull = [System.IO.Path]::GetFullPath($ClientCopyDir).TrimEnd('\')
if ($clientSourceFull -eq $clientCopyFull -or $clientCopyFull.StartsWith($clientSourceFull + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'The client working copy must not be the original client directory or a child of it.'
}

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
    throw "Client does not appear to be build 12340. FileVersion='$fileVersion'; ProductVersion='$productVersion'; BinaryMarker12340=$binaryMarker. No copy was started."
}

if (Test-Path -LiteralPath $clientCopyFull) {
    if ($RefreshExistingCopy) {
        if (-not $clientCopyFull.StartsWith($ServerRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'RefreshExistingCopy refuses to remove a client copy outside the AutoWoW workspace.'
        }
        Remove-Item -LiteralPath $clientCopyFull -Recurse -Force
        New-Item -ItemType Directory -Path $clientCopyFull -Force | Out-Null
        Get-ChildItem -LiteralPath $WowClientDir -Force | Copy-Item -Destination $clientCopyFull -Recurse -Force
    }
    else {
    $existingExe = Get-ChildItem -LiteralPath $clientCopyFull -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @('Wow.exe', 'WoW.exe', 'Wow-64.exe', 'WoW-64.exe') } |
        Select-Object -First 1
    if (-not $existingExe) { throw "Client copy destination exists but does not look like a completed client: $clientCopyFull" }
    Write-Output "Reusing existing client working copy: $clientCopyFull"
    }
}
else {
    New-Item -ItemType Directory -Path $clientCopyFull -Force | Out-Null
    Get-ChildItem -LiteralPath $WowClientDir -Force | Copy-Item -Destination $clientCopyFull -Recurse -Force
}
Assert-WowClientDataReady -ClientDir $clientCopyFull

$realmlist = Get-ChildItem -LiteralPath (Join-Path $clientCopyFull 'Data') -Filter 'realmlist.wtf' -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $realmlist) { throw "No realmlist.wtf found under $clientCopyFull\Data" }
$backup = $realmlist.FullName + '.bootstrap.original'
if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $realmlist.FullName -Destination $backup }
$lines = [System.Collections.Generic.List[string]]::new()
foreach ($line in [System.IO.File]::ReadAllLines($realmlist.FullName)) { $lines.Add($line) }
$changed = $false
for ($index = 0; $index -lt $lines.Count; $index++) {
    if ($lines[$index] -match '^\s*set\s+realmlist\s+') {
        $lines[$index] = 'set realmlist 127.0.0.1'
        $changed = $true
    }
}
if (-not $changed) { $lines.Add('set realmlist 127.0.0.1') }
[System.IO.File]::WriteAllLines($realmlist.FullName, $lines, [System.Text.UTF8Encoding]::new($false))

$report = @(
    '# AutoWoW client working-copy report',
    '',
    ('Generated: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')),
    '',
    ('- Original client: {0}' -f $WowClientDir),
    ('- Working copy: {0}' -f $clientCopyFull),
    ('- WoW executable: {0}' -f $wowExe.FullName),
    ('- FileVersion: {0}' -f $fileVersion),
    ('- ProductVersion: {0}' -f $productVersion),
    ('- Binary marker 12340: {0}' -f $binaryMarker),
    ('- Realmlist edited: {0}' -f $realmlist.FullName),
    ('- Original realmlist backup: {0}' -f $backup),
    '- Original client modified: **No**',
    '- Local realm: `set realmlist 127.0.0.1`'
) -join [Environment]::NewLine
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'CLIENT_COPY_REPORT.md'), $report, [System.Text.UTF8Encoding]::new($false))
Write-Output "Client working copy prepared at $clientCopyFull. Report: $(Join-Path $ServerRoot 'CLIENT_COPY_REPORT.md')"
