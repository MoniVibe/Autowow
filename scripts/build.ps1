[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('RelWithDebInfo','Release','Debug')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1,16)][int]$Parallel = 4
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$coreRoot = Join-Path $ServerRoot 'azerothcore-wotlk'
$buildRoot = Join-Path $ServerRoot 'build'
$installRoot = Join-Path $ServerRoot 'server'
Initialize-AutoWoWLayout -Root $ServerRoot

if (-not (Test-Path -LiteralPath (Join-Path $coreRoot '.git'))) { throw "Core checkout missing: $coreRoot" }
if (-not (Test-Path -LiteralPath (Join-Path $coreRoot 'modules\mod-playerbots\.git'))) { throw 'Playerbots module checkout is missing.' }

$boostRoot = Get-FirstExistingPath -Candidates @(
    $env:Boost_ROOT,
    $env:BOOST_ROOT,
    (Join-Path $ServerRoot 'third_party\boost\1.83.0'),
    (Join-Path $ServerRoot 'third_party\boost\boost_1_83_0')
)
$mysqlRoot = Get-FirstExistingPath -Candidates @(
    $env:MYSQL_ROOT_DIR,
    (Join-Path $ServerRoot 'third_party\mysql'),
    'C:\Program Files\MySQL\MySQL Server 8.4',
    'C:\Program Files\MySQL\MySQL Server 8.0'
)
$opensslRoot = Get-FirstExistingPath -Candidates @(
    $env:OPENSSL_ROOT_DIR,
    (Join-Path $ServerRoot 'third_party\openssl'),
    'C:\Program Files\OpenSSL-Win64',
    'C:\Program Files\OpenSSL'
)

$missing = [System.Collections.Generic.List[string]]::new()
if (-not $boostRoot) { $missing.Add('Boost_ROOT / Boost 1.83') }
if (-not $mysqlRoot) { $missing.Add('MYSQL_ROOT_DIR / MySQL 8.4 development files') }
if (-not $opensslRoot) { $missing.Add('OPENSSL_ROOT_DIR / full Win64 OpenSSL 3.x') }
if ($boostRoot -and -not (Test-Path -LiteralPath (Join-Path $boostRoot 'boost\version.hpp')) -and -not (Test-Path -LiteralPath (Join-Path $boostRoot 'include\boost-1_83\boost\version.hpp'))) { $missing.Add("Boost headers under $boostRoot") }
if ($mysqlRoot -and -not (Test-Path -LiteralPath (Join-Path $mysqlRoot 'include\mysql.h'))) { $missing.Add("MySQL headers under $mysqlRoot") }
if ($mysqlRoot -and -not (Test-Path -LiteralPath (Join-Path $mysqlRoot 'lib\libmysql.lib'))) { $missing.Add("MySQL library under $mysqlRoot") }
if ($opensslRoot -and -not (Test-Path -LiteralPath (Join-Path $opensslRoot 'include\openssl\ssl.h'))) { $missing.Add("OpenSSL headers under $opensslRoot") }
if ($missing.Count) { throw ("Build prerequisites are missing:`n - " + ($missing -join "`n - ")) }

$cmake = (Get-Command cmake -ErrorAction Stop).Source
$configureLog = New-AutoWoWLogPath -Name 'cmake-configure'
$buildLog = New-AutoWoWLogPath -Name 'cmake-build'
$installLog = New-AutoWoWLogPath -Name 'cmake-install'

$cmakeArgs = @(
    '-Wno-dev',
    '-S', $coreRoot,
    '-B', $buildRoot,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DCMAKE_CONFIGURATION_TYPES=$Configuration",
    "-DCMAKE_INSTALL_PREFIX=$installRoot",
    '-DAPPS_BUILD=all',
    '-DTOOLS_BUILD=all',
    '-DSCRIPTS=static',
    '-DMODULES=static',
    '-DBUILD_TESTING=OFF',
    '-DTOOL_CONFIG_MERGER=OFF',
    "-DBoost_ROOT=$boostRoot",
    '-DBoost_NO_SYSTEM_PATHS=ON',
    "-DMYSQL_ROOT_DIR=$mysqlRoot",
    "-DOPENSSL_ROOT_DIR=$opensslRoot"
)

Invoke-NativeLogged -FilePath $cmake -ArgumentList $cmakeArgs -WorkingDirectory $ServerRoot -LogPath $configureLog | Out-Null
$oldClMpCount = $env:CL_MPCount
$env:CL_MPCount = "$Parallel"
try {
    Invoke-NativeLogged -FilePath $cmake -ArgumentList @('--build', $buildRoot, '--config', $Configuration, '--parallel', "$Parallel") -WorkingDirectory $ServerRoot -LogPath $buildLog | Out-Null
}
finally {
    if ($null -eq $oldClMpCount) { Remove-Item Env:CL_MPCount -ErrorAction SilentlyContinue } else { $env:CL_MPCount = $oldClMpCount }
}
Invoke-NativeLogged -FilePath $cmake -ArgumentList @('--install', $buildRoot, '--config', $Configuration) -WorkingDirectory $ServerRoot -LogPath $installLog | Out-Null

$expected = @('authserver.exe','worldserver.exe','map_extractor.exe','vmap4_extractor.exe','vmap4_assembler.exe','mmaps_generator.exe','dbimport.exe')
$missingBinaries = @($expected | Where-Object { -not (Test-Path -LiteralPath (Join-Path $installRoot $_)) })
if ($missingBinaries.Count) { throw "Install completed but expected binaries are missing: $($missingBinaries -join ', ')" }

$runtimeDlls = @(
    @{ Name = 'libmysql.dll'; Source = Join-Path $mysqlRoot 'lib\libmysql.dll' },
    @{ Name = 'libcrypto-3-x64.dll'; Source = Join-Path $opensslRoot 'bin\libcrypto-3-x64.dll' },
    @{ Name = 'libssl-3-x64.dll'; Source = Join-Path $opensslRoot 'bin\libssl-3-x64.dll' },
    @{ Name = 'legacy.dll'; Source = Join-Path $opensslRoot 'bin\legacy.dll' }
)
foreach ($runtimeDll in $runtimeDlls) {
    if (-not (Test-Path -LiteralPath $runtimeDll.Source)) { throw "Required runtime DLL is missing: $($runtimeDll.Source)" }
    Copy-Item -LiteralPath $runtimeDll.Source -Destination (Join-Path $installRoot $runtimeDll.Name) -Force
}

$report = @"
# AutoWoW build report

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

- Core SHA: $((& git -C $coreRoot rev-parse HEAD).Trim())
- Playerbots SHA: $((& git -C (Join-Path $coreRoot 'modules\mod-playerbots') rev-parse HEAD).Trim())
- Generator: Visual Studio 17 2022, x64
- Configuration: $Configuration
- Parallel build limit: $Parallel
- Boost root: $boostRoot
- MySQL root: $mysqlRoot
- OpenSSL root: $opensslRoot
- Applications: authserver, worldserver
- Tools: map_extractor, vmap4_extractor, vmap4_assembler, mmaps_generator, dbimport
- Install root: $installRoot
- Runtime DLLs: $($runtimeDlls.Name -join ', ')

Logs:

- Configure: $configureLog
- Build: $buildLog
- Install: $installLog
"@
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'BUILD_REPORT.md'), $report, $utf8NoBom)
Write-Output "Build and install completed. Report: $(Join-Path $ServerRoot 'BUILD_REPORT.md')"
