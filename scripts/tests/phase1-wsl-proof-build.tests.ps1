<#
    Focused offline tests for phase1-wsl-proof-build.ps1.
    These tests do not call WSL, cmake, a server process, or the Windows build.

    Run:
      Invoke-Pester -Path .\scripts\tests\phase1-wsl-proof-build.tests.ps1 -Output Detailed
#>

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:BuildScriptPath = Join-Path $script:ScriptsRoot 'phase1-wsl-proof-build.ps1'
    . $script:BuildScriptPath

    function New-StartupFixture {
        param(
            [string]$StandardError = '',
            [string]$StandardOutput = 'AzerothCore (worldserver-daemon) ready...',
            [datetime]$WriteUtc = [datetime]'2026-07-14T12:05:00Z'
        )

        $startup = Join-Path $TestDrive 'worldserver-wsl-stdout.log'
        $stderr = Join-Path $TestDrive 'worldserver-wsl-stderr.log'
        [System.IO.File]::WriteAllText($startup, $StandardOutput, [System.Text.UTF8Encoding]::new($false))
        [System.IO.File]::WriteAllText($stderr, $StandardError, [System.Text.UTF8Encoding]::new($false))
        [System.IO.File]::SetLastWriteTimeUtc($startup, $WriteUtc)
        [System.IO.File]::SetLastWriteTimeUtc($stderr, $WriteUtc)
        return [pscustomobject]@{ startup = $startup; stderr = $stderr }
    }
}

Describe 'Phase 1 isolated WSL proof profile' {
    It 'plans the exact source, cache flags, targets, and command-line O0 override' {
        $plan = Get-Phase1WslProofPlan -Distro 'Ubuntu-24.04'

        $plan.isolation.source_root | Should -BeExactly '/root/p1core'
        $plan.isolation.build_root | Should -BeExactly '/root/p1core/build'
        $plan.isolation.windows_build_used | Should -BeFalse
        $plan.isolation.install_or_deploy | Should -BeFalse
        $plan.isolation.server_lifecycle_actions | Should -BeFalse
        @($plan.configure_arguments) | Should -Be @(
            'cmake', '-S', '/root/p1core', '-B', '/root/p1core/build',
            '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
            '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O0 -g -DNDEBUG',
            '-DBUILD_TESTING=ON',
            '-DMODULES=static',
            '-DSCRIPTS=minimal-static',
            '-DSCRIPTS_OUTLAND=static',
            '-DSCRIPTS_NORTHREND=static',
            '-DSCRIPTS_KALIMDOR=static'
        )
        @($plan.build_arguments) | Should -Be @(
            'cmake', '--build', '/root/p1core/build', '--target',
            'unit_tests', 'worldserver', '--', '-j1'
        )
    }

    It 'runs Plan without WSL and emits machine-readable JSON' {
        $plan = (& $script:BuildScriptPath -Action Plan | Out-String) | ConvertFrom-Json
        $plan.action | Should -BeExactly 'Plan'
        $plan.apply | Should -BeFalse
        $plan.cmake_cache.SCRIPTS_NORTHREND | Should -BeExactly 'static'
        $plan.cmake_cache.SCRIPTS_KALIMDOR | Should -BeExactly 'static'
    }

    It 'refuses Build unless Apply is explicit before any WSL call' {
        {
            Invoke-Phase1WslProofMain -SelectedAction Build -Root $TestDrive `
                -Distro 'not-a-real-distro' -ManifestPath ''
        } | Should -Throw '*pass -Action Build -Apply explicitly*'
    }

    It 'contains no Windows-build, install, deploy, or server lifecycle invocation' {
        $source = Get-Content -LiteralPath $script:BuildScriptPath -Raw
        $source | Should -Not -Match '(?im)&\s+.*scripts[\\/]build\.ps1'
        $source | Should -Not -Match '(?im)\b(Start-Process|Stop-Process|pkill|taskkill|cmake\s+--install)\b'
    }

    It 'structurally validates both ELF artifacts and launches one focused test before hashing' {
        $source = Get-Content -LiteralPath $script:BuildScriptPath -Raw
        $source | Should -Match 'foreach \(\$mode in @\(''-h'', ''-r''\)\)'
        $source | Should -Match '@\(''ldd'', \$path\)'
        $source | Should -Match 'invalid symbol index'
        $source | Should -Match 'FixtureFactoryContract\.DefaultClosedAndAllowlistIsExact'
        $source.IndexOf('Assert-Phase1WslExecutableArtifacts -Distro $Distro') |
            Should -BeLessThan $source.IndexOf('sha256 = Get-Phase1WslSha256')
    }
}

Describe 'Phase 1 CMake cache guard' {
    BeforeEach {
        $script:GoodCacheText = @'
CMAKE_BUILD_TYPE:STRING=RelWithDebInfo
CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O0 -g -DNDEBUG
BUILD_TESTING:BOOL=ON
MODULES:STRING=static
SCRIPTS:STRING=minimal-static
SCRIPTS_OUTLAND:STRING=static
SCRIPTS_NORTHREND:STRING=static
SCRIPTS_KALIMDOR:STRING=static
CMAKE_HOME_DIRECTORY:INTERNAL=/root/p1core
'@
    }

    It 'accepts only the complete expected profile' {
        $cache = ConvertFrom-Phase1CMakeCache -Text $script:GoodCacheText
        { Assert-Phase1CMakeCache -Cache $cache } | Should -Not -Throw
    }

    It 'fails closed when Kalimdor remains default' {
        $cache = ConvertFrom-Phase1CMakeCache -Text ($script:GoodCacheText -replace 'SCRIPTS_KALIMDOR:STRING=static', 'SCRIPTS_KALIMDOR:STRING=default')
        { Assert-Phase1CMakeCache -Cache $cache } | Should -Throw '*SCRIPTS_KALIMDOR*expected*static*got*default*'
    }

    It 'fails closed for a cache owned by another source tree' {
        $cache = ConvertFrom-Phase1CMakeCache -Text ($script:GoodCacheText -replace '/root/p1core', '/mnt/d/Games/wowstuff/AutoWoW')
        { Assert-Phase1CMakeCache -Cache $cache } | Should -Throw '*does not belong to /root/p1core*'
    }
}

Describe 'Phase 1 build manifest guard' {
    BeforeEach {
        $actualCache = [ordered]@{}
        foreach ($entry in $script:Phase1ExpectedCache.GetEnumerator()) {
            $actualCache[$entry.Key] = $entry.Value
        }
        $script:GoodManifest = [pscustomobject][ordered]@{
            schema = $script:Phase1ProofSchema
            status = 'BUILT'
            distro = 'Ubuntu-24.04'
            source_root = '/root/p1core'
            build_root = '/root/p1core/build'
            cmake_home_directory = '/root/p1core'
            actual_cmake_cache = [pscustomobject]$actualCache
            targets = @('unit_tests', 'worldserver')
            native_build_arguments = @('-j1')
            isolation = [pscustomobject]@{
                windows_build_used = $false
                install_or_deploy = $false
                server_lifecycle_actions = $false
            }
            artifacts = [pscustomobject]@{
                unit_tests = [pscustomobject]@{
                    path = '/root/p1core/build/src/test/unit_tests'
                    sha256 = ('a' * 64)
                }
                worldserver = [pscustomobject]@{
                    path = '/root/p1core/build/src/server/apps/worldserver'
                    sha256 = ('b' * 64)
                }
            }
        }
    }

    It 'accepts a complete isolated build manifest' {
        { Assert-Phase1BuildManifest -Manifest $script:GoodManifest -Distro 'Ubuntu-24.04' } | Should -Not -Throw
    }

    It 'rejects config drift even when schema, paths, and hashes look valid' {
        $script:GoodManifest.actual_cmake_cache.SCRIPTS_KALIMDOR = 'default'
        { Assert-Phase1BuildManifest -Manifest $script:GoodManifest -Distro 'Ubuntu-24.04' } |
            Should -Throw '*CMake flag mismatch for SCRIPTS_KALIMDOR*'
    }
}

Describe 'Phase 1 startup log proof' {
    It 'passes fresh ready startup output and an empty fresh stderr log' {
        $fixture = New-StartupFixture
        $result = Test-Phase1StartupLogEvidence -StartupPath $fixture.startup -ErrorPath $fixture.stderr `
            -NotBeforeUtc ([datetime]'2026-07-14T12:00:00Z')

        $result.status | Should -BeExactly 'PASS'
        $result.ready_marker_found | Should -BeTrue
        @($result.target_script_errors).Count | Should -Be 0
        @($result.logs).Count | Should -Be 2
    }

    It 'fails the exact required script name <Name>' -TestCases @(
        @{ Name = 'boss_onyxia' }
        @{ Name = 'instance_onyxias_lair' }
        @{ Name = 'npc_onyxian_lair_guard' }
        @{ Name = 'boss_ingvar_the_plunderer' }
        @{ Name = 'npc_ingvar_resurrection_dummy' }
    ) {
        param($Name)
        $errorLine = "Script named '$Name' is assigned in the database, but has no code!"
        $fixture = New-StartupFixture -StandardError $errorLine
        $result = Test-Phase1StartupLogEvidence -StartupPath $fixture.startup -ErrorPath $fixture.stderr `
            -NotBeforeUtc ([datetime]'2026-07-14T12:00:00Z')

        $result.status | Should -BeExactly 'FAIL_CLOSED'
        @($result.target_script_errors).script_name | Should -Contain $Name
        @($result.issues) | Should -Contain "target_script_has_no_code:$Name"
    }

    It 'does not widen the scoped gate to an unrelated has-no-code script' {
        $fixture = New-StartupFixture -StandardError "Script named 'boss_unrelated' is assigned in the database, but has no code!"
        $result = Test-Phase1StartupLogEvidence -StartupPath $fixture.startup -ErrorPath $fixture.stderr `
            -NotBeforeUtc ([datetime]'2026-07-14T12:00:00Z')
        $result.status | Should -BeExactly 'PASS'
    }

    It 'fails closed when logs predate the manifest or the ready marker is absent' {
        $fixture = New-StartupFixture -StandardOutput 'startup did not complete' -WriteUtc ([datetime]'2026-07-14T11:00:00Z')
        $result = Test-Phase1StartupLogEvidence -StartupPath $fixture.startup -ErrorPath $fixture.stderr `
            -NotBeforeUtc ([datetime]'2026-07-14T12:00:00Z')

        $result.status | Should -BeExactly 'FAIL_CLOSED'
        @($result.issues) | Should -Contain 'worldserver_ready_marker_missing'
        (@($result.issues) -join ',') | Should -Match 'log_predates_build'
    }

    It 'fails closed when either required log is missing' {
        $missingStartup = Join-Path $TestDrive 'missing-stdout.log'
        $missingError = Join-Path $TestDrive 'missing-stderr.log'
        $result = Test-Phase1StartupLogEvidence -StartupPath $missingStartup -ErrorPath $missingError `
            -NotBeforeUtc ([datetime]'2026-07-14T12:00:00Z')
        $result.status | Should -BeExactly 'FAIL_CLOSED'
        @($result.issues).Count | Should -BeGreaterOrEqual 2
    }
}

Describe 'Phase 1 WSL proof PowerShell syntax' {
    It 'parse-checks the orchestrator and focused test' {
        foreach ($path in @($script:BuildScriptPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
