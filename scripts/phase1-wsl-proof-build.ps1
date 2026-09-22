<#
.SYNOPSIS
    Configure, build, and verify the isolated /root/p1core proof binary.

.DESCRIPTION
    This script owns only the Ubuntu /root/p1core source/build tree. Plan is the
    default and performs no WSL calls. Build requires -Apply, configures the
    exact fail-closed profile, builds only unit_tests and worldserver with the
    RelWithDebInfo optimizer disabled in the isolated CMake cache, and writes a hash/config
    manifest. ValidateStartupLogs is read-only and ties fresh startup logs to
    that manifest and the current isolated WSL worldserver hash.

    It never invokes the Windows build, starts a server, stops a server, or
    installs/deploys an artifact.
#>
[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Build', 'ValidateStartupLogs')]
    [string]$Action = 'Plan',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$BuildManifestPath = '',
    [string]$StartupLogPath = '',
    [string]$ErrorLogPath = '',
    [string]$ValidationReceiptPath = '',
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Phase1ProofSchema = 'autowow.phase1.wsl-proof-build.v1'
$script:Phase1ValidationSchema = 'autowow.phase1.wsl-proof-startup-validation.v1'
$script:Phase1SourceRoot = '/root/p1core'
$script:Phase1BuildRoot = '/root/p1core/build'
$script:Phase1WorldserverPath = '/root/p1core/build/src/server/apps/worldserver'
$script:Phase1UnitTestsPath = '/root/p1core/build/src/test/unit_tests'
$script:Phase1BuildTargets = @('unit_tests', 'worldserver')
$script:Phase1NativeBuildArguments = @('-j1')
$script:Phase1ExpectedCache = [ordered]@{
    CMAKE_BUILD_TYPE = 'RelWithDebInfo'
    CMAKE_CXX_FLAGS_RELWITHDEBINFO = '-O0 -g -DNDEBUG'
    BUILD_TESTING = 'ON'
    MODULES = 'static'
    SCRIPTS = 'minimal-static'
    SCRIPTS_OUTLAND = 'static'
    SCRIPTS_NORTHREND = 'static'
    SCRIPTS_KALIMDOR = 'static'
}
$script:Phase1ExactScriptNames = @(
    'boss_onyxia',
    'instance_onyxias_lair',
    'npc_onyxian_lair_guard'
)

function Resolve-Phase1WindowsPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$BasePath
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Write-Phase1Utf8File {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text
    )

    $directory = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function Get-Phase1WslProofPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro)

    $configure = @(
        'cmake', '-S', $script:Phase1SourceRoot, '-B', $script:Phase1BuildRoot
    )
    foreach ($entry in $script:Phase1ExpectedCache.GetEnumerator()) {
        $configure += "-D$($entry.Key)=$($entry.Value)"
    }

    $build = @(
        'cmake', '--build', $script:Phase1BuildRoot, '--target'
    ) + $script:Phase1BuildTargets + @('--') + $script:Phase1NativeBuildArguments

    return [pscustomobject][ordered]@{
        schema = $script:Phase1ProofSchema
        action = 'Plan'
        apply = $false
        distro = $Distro
        isolation = [pscustomobject][ordered]@{
            source_root = $script:Phase1SourceRoot
            build_root = $script:Phase1BuildRoot
            windows_build_used = $false
            install_or_deploy = $false
            server_lifecycle_actions = $false
        }
        configure_arguments = @($configure)
        build_arguments = @($build)
        cmake_cache = [pscustomobject]$script:Phase1ExpectedCache
        targets = @($script:Phase1BuildTargets)
        native_build_arguments = @($script:Phase1NativeBuildArguments)
        artifacts = [pscustomobject][ordered]@{
            unit_tests = $script:Phase1UnitTestsPath
            worldserver = $script:Phase1WorldserverPath
        }
    }
}

function Invoke-Phase1WslCommand {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Command,
        [switch]$AllowFailure
    )

    $wslArguments = @('-d', $Distro, '-u', 'root', '--') + $Command
    $output = @(& wsl.exe @wslArguments 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
    $result = [pscustomobject][ordered]@{
        exit_code = $exitCode
        output = @($output)
        command = @($Command)
    }
    if (-not $AllowFailure -and $exitCode -ne 0) {
        $detail = ($output -join [Environment]::NewLine).Trim()
        if ([string]::IsNullOrWhiteSpace($detail)) { $detail = '(no command output)' }
        throw "WSL command failed with exit code ${exitCode}: $($Command -join ' ')`n$detail"
    }
    return $result
}

function Assert-Phase1WslPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Expected
    )

    $result = Invoke-Phase1WslCommand -Distro $Distro -Command @('readlink', '-f', '--', $Path)
    $resolved = (($result.output | Select-Object -Last 1) -as [string]).Trim()
    if ($resolved -cne $Expected) {
        throw "WSL isolation guard failed: $Path resolved to '$resolved', expected '$Expected'."
    }
}

function Assert-Phase1WslExecutableArtifacts {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro)

    foreach ($path in @($script:Phase1UnitTestsPath, $script:Phase1WorldserverPath)) {
        foreach ($mode in @('-h', '-r')) {
            $readelf = Invoke-Phase1WslCommand -Distro $Distro -Command @('readelf', $mode, $path) -AllowFailure
            $diagnostic = ($readelf.output -join "`n")
            if ($readelf.exit_code -ne 0 -or $diagnostic -match '(?i)invalid symbol index|warning:.*relocation') {
                throw "ELF structural validation failed for $path (readelf $mode)."
            }
        }

        $dependencies = Invoke-Phase1WslCommand -Distro $Distro -Command @('ldd', $path) -AllowFailure
        if ($dependencies.exit_code -ne 0 -or ($dependencies.output -join "`n") -match '(?i)not found') {
            throw "ELF dependency validation failed for $path."
        }
    }

    # A valid-looking ELF can still contain a corrupt relocation table. Exercise the dynamic
    # loader and one tiny registered test before publishing hashes to the build manifest.
    $probe = Invoke-Phase1WslCommand -Distro $Distro -Command @(
        $script:Phase1UnitTestsPath,
        '--gtest_filter=FixtureFactoryContract.DefaultClosedAndAllowlistIsExact'
    ) -AllowFailure
    if ($probe.exit_code -ne 0 -or ($probe.output -join "`n") -notmatch '\[\s*PASSED\s*\]\s+1 test') {
        throw 'Unit-test executable launch validation failed; refusing to publish the build manifest.'
    }
}

function ConvertFrom-Phase1CMakeCache {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)

    $cache = @{}
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match '^(?<key>[^#/:=][^:=]*):[^=]+=(?<value>.*)$') {
            $cache[$Matches.key] = $Matches.value
        }
    }
    return $cache
}

function Assert-Phase1CMakeCache {
    [CmdletBinding()]
    param([Parameter(Mandatory)][hashtable]$Cache)

    foreach ($entry in $script:Phase1ExpectedCache.GetEnumerator()) {
        if (-not $Cache.ContainsKey([string]$entry.Key)) {
            throw "CMake cache is missing required key $($entry.Key)."
        }
        if ([string]$Cache[$entry.Key] -cne [string]$entry.Value) {
            throw "CMake cache mismatch for $($entry.Key): expected '$($entry.Value)', got '$($Cache[$entry.Key])'."
        }
    }
    if (-not $Cache.ContainsKey('CMAKE_HOME_DIRECTORY') -or
        [string]$Cache.CMAKE_HOME_DIRECTORY -cne $script:Phase1SourceRoot) {
        throw "CMake cache does not belong to $($script:Phase1SourceRoot)."
    }
}

function Get-Phase1CMakeCache {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Distro,
        [switch]$AssertProfile
    )

    $cachePath = "$($script:Phase1BuildRoot)/CMakeCache.txt"
    $result = Invoke-Phase1WslCommand -Distro $Distro -Command @('cat', '--', $cachePath)
    $cache = ConvertFrom-Phase1CMakeCache -Text ($result.output -join "`n")
    if ($AssertProfile) { Assert-Phase1CMakeCache -Cache $cache }
    return $cache
}

function Get-Phase1WslSha256 {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$Path
    )

    $result = Invoke-Phase1WslCommand -Distro $Distro -Command @('sha256sum', '--', $Path)
    $line = (($result.output | Select-Object -Last 1) -as [string]).Trim()
    if ($line -notmatch '^(?<hash>[0-9a-fA-F]{64})\s+') {
        throw "Could not parse SHA-256 for $Path."
    }
    return $Matches.hash.ToLowerInvariant()
}

function Get-Phase1TargetScriptErrors {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)

    $findings = [System.Collections.Generic.List[object]]::new()
    $lineNumber = 0
    foreach ($line in ($Text -split "`r?`n")) {
        $lineNumber++
        if ($line -notmatch "(?i)Script named\s+'(?<name>[^']+)'\s+is assigned in the database,\s+but has no code!") {
            continue
        }
        $name = [string]$Matches.name
        if ($script:Phase1ExactScriptNames -contains $name -or $name -match '(?i)ingvar') {
            [void]$findings.Add([pscustomobject][ordered]@{
                    script_name = $name
                    line = $lineNumber
                    message = $line.Trim()
                })
        }
    }
    return @($findings)
}

function Test-Phase1StartupLogEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$StartupPath,
        [Parameter(Mandatory)][string]$ErrorPath,
        [Parameter(Mandatory)][datetime]$NotBeforeUtc
    )

    $issues = [System.Collections.Generic.List[string]]::new()
    $allText = [System.Text.StringBuilder]::new()
    $records = [System.Collections.Generic.List[object]]::new()

    foreach ($item in @(
            [pscustomobject]@{ kind = 'startup'; path = $StartupPath; allow_empty = $false },
            [pscustomobject]@{ kind = 'stderr'; path = $ErrorPath; allow_empty = $true }
        )) {
        if (-not (Test-Path -LiteralPath $item.path -PathType Leaf)) {
            $issues.Add("$($item.kind)_log_missing:$($item.path)")
            continue
        }
        $file = Get-Item -LiteralPath $item.path
        if (-not $item.allow_empty -and $file.Length -eq 0) {
            $issues.Add("$($item.kind)_log_empty:$($item.path)")
        }
        if ($file.LastWriteTimeUtc -lt $NotBeforeUtc.ToUniversalTime()) {
            $issues.Add("$($item.kind)_log_predates_build:$($item.path)")
        }
        $text = [System.IO.File]::ReadAllText($file.FullName)
        [void]$allText.AppendLine($text)
        $records.Add([pscustomobject][ordered]@{
                kind = $item.kind
                path = $file.FullName
                length = $file.Length
                last_write_utc = $file.LastWriteTimeUtc.ToString('o')
                sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            })
    }

    $startupRecord = @($records | Where-Object kind -eq 'startup' | Select-Object -First 1)
    $ready = $false
    if ($startupRecord.Count -eq 1) {
        $startupText = [System.IO.File]::ReadAllText($startupRecord[0].path)
        $ready = $startupText -match '(?im)\(worldserver-daemon\)\s+ready\.\.\.'
        if (-not $ready) { $issues.Add('worldserver_ready_marker_missing') }
    }

    $targetErrors = @(Get-Phase1TargetScriptErrors -Text $allText.ToString())
    foreach ($finding in $targetErrors) {
        $issues.Add("target_script_has_no_code:$($finding.script_name)")
    }

    return [pscustomobject][ordered]@{
        status = if ($issues.Count -eq 0) { 'PASS' } else { 'FAIL_CLOSED' }
        passed = $issues.Count -eq 0
        not_before_utc = $NotBeforeUtc.ToUniversalTime().ToString('o')
        ready_marker_found = $ready
        logs = @($records)
        target_script_errors = @($targetErrors)
        issues = @($issues)
    }
}

function Assert-Phase1BuildManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Manifest,
        [Parameter(Mandatory)][string]$Distro
    )

    if ($Manifest.schema -cne $script:Phase1ProofSchema -or $Manifest.status -cne 'BUILT') {
        throw 'Build manifest schema/status is not an accepted completed proof build.'
    }
    if ($Manifest.distro -cne $Distro) {
        throw "Build manifest distro '$($Manifest.distro)' does not match requested distro '$Distro'."
    }
    if ($Manifest.source_root -cne $script:Phase1SourceRoot -or
        $Manifest.build_root -cne $script:Phase1BuildRoot -or
        $Manifest.cmake_home_directory -cne $script:Phase1SourceRoot -or
        $Manifest.artifacts.worldserver.path -cne $script:Phase1WorldserverPath -or
        $Manifest.artifacts.unit_tests.path -cne $script:Phase1UnitTestsPath) {
        throw 'Build manifest does not identify the fixed isolated /root/p1core artifacts.'
    }
    if ([bool]$Manifest.isolation.windows_build_used -or
        [bool]$Manifest.isolation.install_or_deploy -or
        [bool]$Manifest.isolation.server_lifecycle_actions) {
        throw 'Build manifest isolation claims are unsafe.'
    }
    if ((@($Manifest.targets) -join "`0") -cne ($script:Phase1BuildTargets -join "`0")) {
        throw 'Build manifest target list does not match unit_tests and worldserver.'
    }
    if ((@($Manifest.native_build_arguments) -join "`0") -cne ($script:Phase1NativeBuildArguments -join "`0")) {
        throw 'Build manifest native arguments do not contain the required single-job profile.'
    }
    foreach ($entry in $script:Phase1ExpectedCache.GetEnumerator()) {
        $property = $Manifest.actual_cmake_cache.PSObject.Properties[[string]$entry.Key]
        if ($null -eq $property -or [string]$property.Value -cne [string]$entry.Value) {
            $actual = if ($null -eq $property) { '<missing>' } else { [string]$property.Value }
            throw "Build manifest CMake flag mismatch for $($entry.Key): expected '$($entry.Value)', got '$actual'."
        }
    }
    foreach ($artifact in @($Manifest.artifacts.unit_tests, $Manifest.artifacts.worldserver)) {
        if ([string]$artifact.sha256 -notmatch '^[0-9a-f]{64}$') {
            throw "Build manifest SHA-256 is malformed for $($artifact.path)."
        }
    }
}

function Invoke-Phase1WslProofBuild {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$ManifestPath
    )

    Assert-Phase1WslPath -Distro $Distro -Path $script:Phase1SourceRoot -Expected $script:Phase1SourceRoot
    Assert-Phase1WslPath -Distro $Distro -Path $script:Phase1BuildRoot -Expected $script:Phase1BuildRoot
    foreach ($path in @(
            "$($script:Phase1SourceRoot)/CMakeLists.txt",
            "$($script:Phase1BuildRoot)/CMakeCache.txt"
        )) {
        [void](Invoke-Phase1WslCommand -Distro $Distro -Command @('test', '-f', $path))
    }

    $running = Invoke-Phase1WslCommand -Distro $Distro -Command @('pgrep', '-x', 'worldserver') -AllowFailure
    if ($running.exit_code -eq 0 -and $running.output.Count -gt 0) {
        throw "A WSL worldserver is running (pid(s) $($running.output -join ', ')); refusing to build or alter it."
    }
    if ($running.exit_code -notin @(0, 1)) {
        throw "Could not verify WSL worldserver process state (pgrep exit $($running.exit_code))."
    }

    # Reject a cache redirected to another source tree before allowing cmake to touch it.
    $preCache = Get-Phase1CMakeCache -Distro $Distro
    if (-not $preCache.ContainsKey('CMAKE_HOME_DIRECTORY') -or
        [string]$preCache.CMAKE_HOME_DIRECTORY -cne $script:Phase1SourceRoot) {
        throw "Existing build cache does not belong to $($script:Phase1SourceRoot)."
    }

    $plan = Get-Phase1WslProofPlan -Distro $Distro
    $manifestDirectory = Split-Path -Parent $ManifestPath
    New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null
    $configureLogPath = Join-Path $manifestDirectory 'phase1-wsl-proof-configure.log'
    $buildLogPath = Join-Path $manifestDirectory 'phase1-wsl-proof-build.log'

    $configure = Invoke-Phase1WslCommand -Distro $Distro -Command $plan.configure_arguments -AllowFailure
    Write-Phase1Utf8File -Path $configureLogPath -Text (($configure.output -join "`n") + "`n")
    if ($configure.exit_code -ne 0) {
        throw "Isolated WSL configure failed with exit code $($configure.exit_code); see $configureLogPath"
    }

    $actualCache = Get-Phase1CMakeCache -Distro $Distro -AssertProfile

    $build = Invoke-Phase1WslCommand -Distro $Distro -Command $plan.build_arguments -AllowFailure
    Write-Phase1Utf8File -Path $buildLogPath -Text (($build.output -join "`n") + "`n")
    if ($build.exit_code -ne 0) {
        throw "Isolated WSL target build failed with exit code $($build.exit_code); see $buildLogPath"
    }

    foreach ($path in @($script:Phase1UnitTestsPath, $script:Phase1WorldserverPath)) {
        [void](Invoke-Phase1WslCommand -Distro $Distro -Command @('test', '-x', $path))
    }
    Assert-Phase1WslExecutableArtifacts -Distro $Distro

    $completedUtc = (Get-Date).ToUniversalTime()
    $actualProfile = [ordered]@{}
    foreach ($entry in $script:Phase1ExpectedCache.GetEnumerator()) {
        $actualProfile[$entry.Key] = [string]$actualCache[$entry.Key]
    }
    $manifest = [pscustomobject][ordered]@{
        schema = $script:Phase1ProofSchema
        status = 'BUILT'
        completed_utc = $completedUtc.ToString('o')
        distro = $Distro
        isolation = $plan.isolation
        source_root = $script:Phase1SourceRoot
        build_root = $script:Phase1BuildRoot
        cmake_home_directory = [string]$actualCache.CMAKE_HOME_DIRECTORY
        requested_cmake_cache = $plan.cmake_cache
        actual_cmake_cache = [pscustomobject]$actualProfile
        targets = @($script:Phase1BuildTargets)
        native_build_arguments = @($script:Phase1NativeBuildArguments)
        commands = [pscustomobject][ordered]@{
            configure = @($plan.configure_arguments)
            build = @($plan.build_arguments)
        }
        artifacts = [pscustomobject][ordered]@{
            unit_tests = [pscustomobject][ordered]@{
                path = $script:Phase1UnitTestsPath
                sha256 = Get-Phase1WslSha256 -Distro $Distro -Path $script:Phase1UnitTestsPath
            }
            worldserver = [pscustomobject][ordered]@{
                path = $script:Phase1WorldserverPath
                sha256 = Get-Phase1WslSha256 -Distro $Distro -Path $script:Phase1WorldserverPath
            }
        }
        logs = [pscustomobject][ordered]@{
            configure = $configureLogPath
            build = $buildLogPath
        }
    }
    Write-Phase1Utf8File -Path $ManifestPath -Text ($manifest | ConvertTo-Json -Depth 10)
    return $manifest
}

function Invoke-Phase1StartupValidation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$ManifestPath,
        [Parameter(Mandatory)][string]$StartupPath,
        [Parameter(Mandatory)][string]$ErrorPath,
        [Parameter(Mandatory)][string]$ReceiptPath
    )

    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        throw "Build manifest not found: $ManifestPath"
    }
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -Depth 20
    Assert-Phase1BuildManifest -Manifest $manifest -Distro $Distro
    $expectedHash = ([string]$manifest.artifacts.worldserver.sha256).ToLowerInvariant()

    [void](Invoke-Phase1WslCommand -Distro $Distro -Command @('test', '-x', $script:Phase1WorldserverPath))
    $currentHash = Get-Phase1WslSha256 -Distro $Distro -Path $script:Phase1WorldserverPath
    $hashMatches = $currentHash -ceq $expectedHash
    $completedUtc = [datetime]::Parse(
        [string]$manifest.completed_utc,
        [System.Globalization.CultureInfo]::InvariantCulture,
        [System.Globalization.DateTimeStyles]::RoundtripKind)
    $logs = Test-Phase1StartupLogEvidence -StartupPath $StartupPath -ErrorPath $ErrorPath -NotBeforeUtc $completedUtc

    $issues = [System.Collections.Generic.List[string]]::new()
    foreach ($issue in $logs.issues) { $issues.Add([string]$issue) }
    if (-not $hashMatches) { $issues.Add('current_worldserver_hash_mismatch') }

    $receipt = [pscustomobject][ordered]@{
        schema = $script:Phase1ValidationSchema
        status = if ($issues.Count -eq 0) { 'PASS' } else { 'FAIL_CLOSED' }
        validated_utc = (Get-Date).ToUniversalTime().ToString('o')
        build_manifest = $ManifestPath
        distro = $Distro
        worldserver = [pscustomobject][ordered]@{
            path = $script:Phase1WorldserverPath
            expected_sha256 = $expectedHash
            current_sha256 = $currentHash
            hash_matches = $hashMatches
        }
        startup = $logs
        issues = @($issues)
    }
    Write-Phase1Utf8File -Path $ReceiptPath -Text ($receipt | ConvertTo-Json -Depth 10)
    if ($issues.Count -ne 0) {
        throw "Startup validation failed closed; see $ReceiptPath ($($issues -join ', '))."
    }
    return $receipt
}

function Invoke-Phase1WslProofMain {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$SelectedAction,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Distro,
        [string]$ManifestPath,
        [string]$StartupPath,
        [string]$ErrorPath,
        [string]$ReceiptPath,
        [switch]$AllowBuild
    )

    $resolvedRoot = [System.IO.Path]::GetFullPath($Root)
    if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
        $ManifestPath = Join-Path $resolvedRoot 'work\phase1-wsl-proof-build\build-manifest.json'
    }
    $ManifestPath = Resolve-Phase1WindowsPath -Path $ManifestPath -BasePath $resolvedRoot

    switch ($SelectedAction) {
        'Plan' {
            return Get-Phase1WslProofPlan -Distro $Distro
        }
        'Build' {
            if (-not $AllowBuild) { throw 'Build is fail-closed by default; pass -Action Build -Apply explicitly.' }
            return Invoke-Phase1WslProofBuild -Distro $Distro -ManifestPath $ManifestPath
        }
        'ValidateStartupLogs' {
            if ($AllowBuild) { throw '-Apply is not accepted for read-only startup-log validation.' }
            if ([string]::IsNullOrWhiteSpace($StartupPath) -or [string]::IsNullOrWhiteSpace($ErrorPath)) {
                throw 'ValidateStartupLogs requires explicit -StartupLogPath and -ErrorLogPath.'
            }
            if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
                $ReceiptPath = Join-Path $resolvedRoot 'work\phase1-wsl-proof-build\startup-validation.json'
            }
            return Invoke-Phase1StartupValidation -Distro $Distro -ManifestPath $ManifestPath `
                -StartupPath (Resolve-Phase1WindowsPath -Path $StartupPath -BasePath $resolvedRoot) `
                -ErrorPath (Resolve-Phase1WindowsPath -Path $ErrorPath -BasePath $resolvedRoot) `
                -ReceiptPath (Resolve-Phase1WindowsPath -Path $ReceiptPath -BasePath $resolvedRoot)
        }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Phase1WslProofMain -SelectedAction $Action -Root $ServerRoot -Distro $WslDistro `
        -ManifestPath $BuildManifestPath -StartupPath $StartupLogPath -ErrorPath $ErrorLogPath `
        -ReceiptPath $ValidationReceiptPath -AllowBuild:$Apply |
        ConvertTo-Json -Depth 12
}
