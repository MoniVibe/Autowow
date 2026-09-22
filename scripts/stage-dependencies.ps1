[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$SkipOpenSSL,
    [switch]$SkipBoost,
    [switch]$SkipMySQL
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$thirdParty = Join-Path $ServerRoot 'third_party'
$downloads = Join-Path $thirdParty 'downloads'
New-Item -ItemType Directory -Path $downloads -Force | Out-Null
$logPath = New-AutoWoWLogPath -Name 'stage-dependencies'

function Download-And-Verify {
    param(
        [string]$Url,
        [string]$Destination,
        [string]$Algorithm,
        [string]$ExpectedHash
    )

    if (Test-Path -LiteralPath $Destination) {
        $current = (Get-FileHash -LiteralPath $Destination -Algorithm $Algorithm).Hash.ToLowerInvariant()
        if ($current -eq $ExpectedHash.ToLowerInvariant()) {
            Write-Output "Using verified existing file $Destination"
            return
        }
        Remove-Item -LiteralPath $Destination -Force
    }

    Write-Output "Downloading $Url"
    $client = [System.Net.WebClient]::new()
    try { $client.DownloadFile($Url, $Destination) }
    finally { $client.Dispose() }
    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm $Algorithm).Hash.ToLowerInvariant()
    if ($actual -ne $ExpectedHash.ToLowerInvariant()) {
        Remove-Item -LiteralPath $Destination -Force
        throw "Hash mismatch for $Destination. Expected $ExpectedHash, got $actual"
    }
}

if (-not $SkipMySQL) {
    $mysqlZip = Join-Path $downloads 'mysql-8.4.10-winx64.zip'
    Download-And-Verify -Url 'https://cdn.mysql.com/Downloads/MySQL-8.4/mysql-8.4.10-winx64.zip' -Destination $mysqlZip -Algorithm MD5 -ExpectedHash '150f12262df6ac88d43862a0e683eb81'
    $mysqlStage = Join-Path $thirdParty 'mysql'
    $mysqlTemp = Join-Path $thirdParty 'tmp-mysql'
    if (-not (Test-Path -LiteralPath (Join-Path $mysqlStage 'bin\mysqld.exe'))) {
        if (Test-Path -LiteralPath $mysqlTemp) { Remove-Item -LiteralPath $mysqlTemp -Recurse -Force }
        if (Test-Path -LiteralPath $mysqlStage) { Remove-Item -LiteralPath $mysqlStage -Recurse -Force }
        Expand-Archive -LiteralPath $mysqlZip -DestinationPath $mysqlTemp -Force
        $mysqlTop = Get-ChildItem -LiteralPath $mysqlTemp -Directory | Select-Object -First 1
        if (-not $mysqlTop) { throw 'MySQL ZIP did not contain a top-level directory.' }
        New-Item -ItemType Directory -Path $mysqlStage -Force | Out-Null
        Get-ChildItem -LiteralPath $mysqlTop.FullName -Force | Copy-Item -Destination $mysqlStage -Recurse -Force
        Remove-Item -LiteralPath $mysqlTemp -Recurse -Force
    }
    Write-Output "MySQL staged at $mysqlStage"
}

if (-not $SkipBoost) {
    $boostZip = Join-Path $downloads 'boost_1_83_0.zip'
    if (-not (Test-Path -LiteralPath $boostZip)) {
        Write-Output 'Downloading Boost 1.83 source from the official Boost archive.'
        $client = [System.Net.WebClient]::new()
        try { $client.DownloadFile('https://archives.boost.io/release/1.83.0/source/boost_1_83_0.zip', $boostZip) }
        finally { $client.Dispose() }
    }
    $boostSource = Join-Path $thirdParty 'src\boost_1_83_0'
    $boostInstall = Join-Path $thirdParty 'boost\1.83.0'
    $boostHeader = Join-Path $boostInstall 'include\boost\version.hpp'
    $boostHeaderVersioned = Join-Path $boostInstall 'include\boost-1_83\boost\version.hpp'
    if (-not (Test-Path -LiteralPath $boostHeader) -and -not (Test-Path -LiteralPath $boostHeaderVersioned)) {
        $boostTemp = Join-Path $thirdParty 'tmp-boost'
        if (Test-Path -LiteralPath $boostTemp) { Remove-Item -LiteralPath $boostTemp -Recurse -Force }
        if (Test-Path -LiteralPath $boostSource) { Remove-Item -LiteralPath $boostSource -Recurse -Force }
        Expand-Archive -LiteralPath $boostZip -DestinationPath $boostTemp -Force
        $boostTop = Get-ChildItem -LiteralPath $boostTemp -Directory | Select-Object -First 1
        if (-not $boostTop) { throw 'Boost ZIP did not contain a top-level directory.' }
        New-Item -ItemType Directory -Path (Split-Path -Parent $boostSource) -Force | Out-Null
        Move-Item -LiteralPath $boostTop.FullName -Destination $boostSource
        Remove-Item -LiteralPath $boostTemp -Recurse -Force
        New-Item -ItemType Directory -Path (Split-Path -Parent $boostInstall) -Force | Out-Null

        $vswhere = Get-FirstExistingPath -Candidates @('C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe','C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe')
        if (-not $vswhere) { throw 'vswhere.exe is required to build Boost with the installed MSVC.' }
        $vsPath = (& $vswhere -latest -products * -property installationPath).Trim()
        $vsDevCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
        if (-not (Test-Path -LiteralPath $vsDevCmd)) { throw "VsDevCmd.bat not found: $vsDevCmd" }

        $vcToolsRoot = Join-Path $vsPath 'VC\Tools\MSVC'
        $vcTools = Get-ChildItem -LiteralPath $vcToolsRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if (-not $vcTools) { throw "Could not locate MSVC tools under $vcToolsRoot" }
        $clPath = Join-Path $vcTools.FullName 'bin\Hostx64\x64\cl.exe'
        $vcvarsall = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
        if (-not (Test-Path -LiteralPath $clPath) -or -not (Test-Path -LiteralPath $vcvarsall)) {
            throw "Could not locate the expected VS x64 compiler or vcvarsall.bat under $vsPath"
        }
        $userConfig = Join-Path $boostSource 'user-config.jam'
        $clJam = $clPath.Replace('\', '/')
        $vcvarsJam = $vcvarsall.Replace('\', '/')
        $userConfigText = 'using msvc : 14.3 : "{0}" : <setup>"{1}" ;' -f $clJam, $vcvarsJam
        [System.IO.File]::WriteAllText($userConfig, $userConfigText + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
        $boostCommand = 'call "' + $vsDevCmd + '" -arch=x64 -host_arch=x64 && call bootstrap.bat && b2.exe --user-config=user-config.jam --build-dir=bin.v2-fixed --with-filesystem --with-program_options --with-iostreams --with-regex --with-thread address-model=64 architecture=x86 link=static runtime-link=shared variant=release threading=multi --prefix="' + $boostInstall + '" -j4 install'
        Invoke-NativeLogged -FilePath $env:ComSpec -ArgumentList @('/d','/s','/c',$boostCommand) -WorkingDirectory $boostSource -LogPath (New-AutoWoWLogPath -Name 'boost-build') | Out-Null
    }
    Write-Output "Boost 1.83 staged at $boostInstall"
}

if (-not $SkipOpenSSL) {
    $opensslInstaller = Join-Path $downloads 'Win64OpenSSL-3_6_3.exe'
    Download-And-Verify -Url 'https://slproweb.com/download/Win64OpenSSL-3_6_3.exe' -Destination $opensslInstaller -Algorithm SHA256 -ExpectedHash 'ffdf6934beaf37546b291ddc70f5788d23b7bf28e7312071240ee8c5439b73e4'
    $opensslRoot = Join-Path $thirdParty 'openssl'
    if (-not (Test-Path -LiteralPath (Join-Path $opensslRoot 'include\openssl\ssl.h'))) {
        if (Test-Path -LiteralPath $opensslRoot) { Remove-Item -LiteralPath $opensslRoot -Recurse -Force }
        New-Item -ItemType Directory -Path $opensslRoot -Force | Out-Null
        Invoke-NativeLogged -FilePath $opensslInstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR=' + $opensslRoot)) -WorkingDirectory $thirdParty -LogPath (New-AutoWoWLogPath -Name 'openssl-install') | Out-Null
    }
    if (-not (Test-Path -LiteralPath (Join-Path $opensslRoot 'include\openssl\ssl.h'))) { throw "OpenSSL installer did not produce expected headers under $opensslRoot" }
    Write-Output "Full Win64 OpenSSL 3.6.3 staged at $opensslRoot"
}

$summary = @(
    '# Dependency staging report',
    '',
    ("Generated: {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')),
    '',
    ("- MySQL: {0}" -f $(if ($SkipMySQL) { 'not requested' } else { 'MySQL Community Server 8.4.10 ZIP; MD5 verified against the official download page' })),
    ("- Boost: {0}" -f $(if ($SkipBoost) { 'not requested' } else { 'Boost 1.83 source built with MSVC x64; build log is under logs' })),
    ("- OpenSSL: {0}" -f $(if ($SkipOpenSSL) { 'not requested' } else { 'full Win64 OpenSSL 3.6.3 installer; SHA-256 verified against the publisher hash JSON' })),
    '',
    'Environment for build.ps1:',
    '',
    '```powershell',
    ("`$env:Boost_ROOT = '{0}'" -f (Join-Path $thirdParty 'boost\1.83.0')),
    ("`$env:MYSQL_ROOT_DIR = '{0}'" -f (Join-Path $thirdParty 'mysql')),
    ("`$env:OPENSSL_ROOT_DIR = '{0}'" -f (Join-Path $thirdParty 'openssl')),
    '```'
) -join [Environment]::NewLine
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'DEPENDENCY_REPORT.md'), $summary, $utf8NoBom)
Write-Output "Dependency staging completed. Report: $(Join-Path $ServerRoot 'DEPENDENCY_REPORT.md')"
