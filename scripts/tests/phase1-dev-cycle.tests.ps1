<#
    Offline Pester coverage for phase1-dev-cycle.ps1.
    These tests use pure plan/emit/parser seams only. They never invoke WSL,
    rsync, cmake, a runtime process, or any lifecycle script.
#>

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:CyclePath = Join-Path $script:ScriptsRoot 'phase1-dev-cycle.ps1'
    . $script:CyclePath
}

Describe 'Phase 1 developer lifecycle plan' {
    It 'plans without WSL mutation and keeps the exact topology' {
        $plan = Get-Phase1DevCyclePlan -Root 'D:\Games\wowstuff\AutoWoW' -Distro 'Ubuntu-24.04' -GTestFilter '*QuestObjective*' -MinimumDiskGiB 5 -MinimumMemoryMiB 1024
        $plan.dry_run | Should -BeTrue
        $plan.canonical_module_windows_path | Should -BeExactly 'D:\Games\wowstuff\AutoWoW\_phase1_worktree\mod-playerbots'
        $plan.canonical_module_wsl_path | Should -BeExactly '/mnt/d/Games/wowstuff/AutoWoW/_phase1_worktree/mod-playerbots'
        $plan.staging_wsl_path | Should -BeExactly '/root/p1core/modules/mod-playerbots'
        $plan.build_wsl_path | Should -BeExactly '/root/p1core/build'
        $plan.runtime_config_wsl_path | Should -BeExactly '/root/p1runtime/worldserver.conf'
        @($plan.rsync_arguments) | Should -Contain '--archive'
        @($plan.rsync_arguments) | Should -Contain '--checksum'
        @($plan.rsync_arguments) | Should -Contain '--no-times'
        @($plan.rsync_arguments) | Should -Contain '--delete'
        @($plan.rsync_arguments) | Should -Contain '--exclude=.git/'
        @($plan.rsync_arguments) | Should -Contain '--exclude=build/'
        @($plan.rsync_arguments) | Should -Contain '--exclude=generated/'
        @($plan.rsync_arguments) | Should -Contain '--exclude=*.log'
    }

    It 'orders unit_tests, focused gtest, and worldserver build steps' {
        $plan = Get-Phase1DevCyclePlan -Root 'D:\Games\wowstuff\AutoWoW' -Distro 'Ubuntu-24.04' -GTestFilter 'FixtureFactoryContract.*' -MinimumDiskGiB 5 -MinimumMemoryMiB 1024
        $plan.build_jobs | Should -Be 4
        $plan.build_sequence[0] | Should -Be @('cmake', '--build', '/root/p1core/build', '--target', 'unit_tests', '--', '-j4')
        $plan.build_sequence[1] | Should -Be @('/root/p1core/build/src/test/unit_tests', '--gtest_filter=FixtureFactoryContract.*')
        $plan.build_sequence[2] | Should -Be @('cmake', '--build', '/root/p1core/build', '--target', 'worldserver', '--', '-j4')
        $plan.lifecycle.restart_stale_server_after_build_failure | Should -BeFalse
        $plan.lifecycle.safe_rollback | Should -BeFalse
    }

    It 'supports a bounded lower build parallelism when requested' {
        $plan = Get-Phase1DevCyclePlan -Root 'D:\Games\wowstuff\AutoWoW' -Distro 'Ubuntu-24.04' -GTestFilter '*' -BuildJobs 2 -MinimumDiskGiB 5 -MinimumMemoryMiB 1024
        $plan.build_jobs | Should -Be 2
        $plan.build_sequence[0][-1] | Should -BeExactly '-j2'
        $plan.build_sequence[2][-1] | Should -BeExactly '-j2'
    }

    It 'detects content changes without preserving source mtimes or requesting a clean build' {
        $plan = Get-Phase1DevCyclePlan -Root 'D:\Games\wowstuff\AutoWoW' -Distro 'Ubuntu-24.04' -GTestFilter '*' -MinimumDiskGiB 5 -MinimumMemoryMiB 1024
        @($plan.rsync_arguments) | Should -Contain '--checksum'
        @($plan.rsync_arguments) | Should -Contain '--no-times'
        @($plan.build_sequence | Where-Object { $_ -contains '--clean-first' }).Count | Should -Be 0
        @($plan.build_sequence | Where-Object { $_ -contains '--clean' }).Count | Should -Be 0
    }

    It 'uses the shared quester and raider filter by default' {
        $output = & $script:CyclePath -Action Plan | Out-String
        $plan = $output | ConvertFrom-Json
        $plan.focused_gtest_filter | Should -BeExactly '*QuestObjective*:*Dungeon*:*Raid*:*WorkerGather*:*ExactPartyRepair*:*Resurrect*'
    }

    It 'refuses deploy before any WSL call unless Apply is explicit' {
        { Invoke-Phase1DevCycleMain -SelectedAction Deploy -Root 'D:\Games\wowstuff\AutoWoW' -Distro 'not-a-real-distro' -GTestFilter '*QuestObjective*' -MinimumDiskGiB 5 -MinimumMemoryMiB 1024 } | Should -Throw '*Deploy is dry-run by default*'
    }

    It 'rejects Apply for read-only actions' {
        { Invoke-Phase1DevCycleMain -SelectedAction Plan -Root 'D:\Games\wowstuff\AutoWoW' -Distro 'Ubuntu-24.04' -GTestFilter '*QuestObjective*' -MinimumDiskGiB 5 -MinimumMemoryMiB 1024 -AllowApply } | Should -Throw '*accepted only with -Action Deploy*'
    }
}

Describe 'Phase 1 JSONL emit seam' {
    It 'emits one compact, secret-free JSONL event without WSL' {
        $receipt = Join-Path $TestDrive 'phase1-dev-cycle.jsonl'
        $event = Write-Phase1DevCycleEvent -ReceiptPath $receipt -Event 'doctor_passed' -Status PASS -Details @{ path = '/root/p1core/modules/mod-playerbots'; password = '<redacted-by-test-fixture>' }
        $event.schema | Should -BeExactly 'autowow.phase1.dev-cycle.v1'
        $lines = @(Get-Content -LiteralPath $receipt)
        $lines.Count | Should -Be 1
        $record = $lines[0] | ConvertFrom-Json
        $record.event | Should -BeExactly 'doctor_passed'
        $record.status | Should -BeExactly 'PASS'
        $record.details.path | Should -BeExactly '/root/p1core/modules/mod-playerbots'
        $lines[0] | Should -Match '"schema":"autowow\.phase1\.dev-cycle\.v1"'
    }

    It 'preserves event ordering and does not write a summary as a side effect' {
        $receipt = Join-Path $TestDrive 'ordered.jsonl'
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receipt -Event 'first')
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receipt -Event 'second' -Status WARN)
        $records = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })
        @($records.event) | Should -Be @('first', 'second')
        (Test-Path -LiteralPath (Join-Path $TestDrive 'ordered.json')) | Should -BeFalse
    }
}

Describe 'Phase 1 command output capture seam' {
    It 'persists command output in a per-run log without invoking WSL' {
        $logPath = Join-Path $TestDrive 'phase1-dev-cycle-run.build.log'
        $result = [pscustomobject]@{ exit_code = 17; command = @('cmake', '--build', '/root/p1core/build'); output = @('compile error: QuestLogView.cpp'); invocation_error = $null }

        Write-Phase1DevCycleCommandLog -LogPath $logPath -Label 'unit_tests_build' -Result $result | Should -Be $logPath
        $text = Get-Content -LiteralPath $logPath -Raw
        $text | Should -Match 'unit_tests_build exit_code=17'
        $text | Should -Match 'compile error: QuestLogView\.cpp'
    }
}

Describe 'Phase 1 Doctor capacity parsing' {
    It 'accepts real-output-shaped scalar df and free rows under strict mode' {
        Mock Invoke-Phase1DevCycleWsl {
            param($Distro, $Command)
            if ($Command[0] -eq 'df') {
                return [pscustomobject]@{ exit_code = 0; command = $Command; output = @(
                    'Filesystem 1024-blocks Used Available Capacity Mounted on'
                    '/dev/sdb 104857600 52428800 52428800 50% /'
                ); invocation_error = $null }
            }
            if ($Command[0] -eq 'free') {
                return [pscustomobject]@{ exit_code = 0; command = $Command; output = @(
                    '              total        used        free      shared  buff/cache   available'
                    'Mem:           16000        4000        2000         100       10000       12000'
                ); invocation_error = $null }
            }
            throw "Unexpected capacity command: $($Command -join ' ')"
        }

        $checks = @(Get-Phase1DevCycleCapacityChecks -Distro 'Ubuntu-24.04' -MinimumDiskGiB 5 -MinimumMemoryMiB 1024)
        @($checks).Count | Should -Be 2
        $checks[0].status | Should -BeExactly 'PASS'
        $checks[0].details.available_gib | Should -Be 50
        $checks[1].status | Should -BeExactly 'PASS'
        $checks[1].details.available_mib | Should -Be 12000
    }
}

Describe 'Phase 1 lifecycle child process boundary' {
    It 'returns after the lifecycle script exits while leaving its long-lived descendant running' {
        $fixturePath = Join-Path $TestDrive 'start-lifecycle-fixture.ps1'
        $descendantPidPath = Join-Path $TestDrive 'descendant.pid'
        [System.IO.File]::WriteAllText($fixturePath, @'
param(
    [string]$ServerRoot,
    [string]$WslDistro
)
$descendant = Start-Process -FilePath 'pwsh.exe' -ArgumentList @(
    '-NoProfile', '-Command', 'Start-Sleep -Seconds 6'
) -WindowStyle Hidden -PassThru
[System.IO.File]::WriteAllText(
    (Join-Path $PSScriptRoot 'descendant.pid'),
    [string]$descendant.Id,
    [System.Text.UTF8Encoding]::new($false))
Write-Output "fixture ready; descendant PID $($descendant.Id)"
'@, [System.Text.UTF8Encoding]::new($false))

        $descendantId = $null
        try {
            $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
            $result = Invoke-Phase1DevCycleWindowsScript -ScriptPath $fixturePath -Root $TestDrive -Distro 'fixture-distro'
            $stopwatch.Stop()

            $result.exit_code | Should -Be 0
            ($result.output -join [Environment]::NewLine) | Should -Match 'fixture ready; descendant PID \d+'
            $stopwatch.Elapsed.TotalSeconds | Should -BeLessThan 3
            $descendantId = [int]([System.IO.File]::ReadAllText($descendantPidPath))
            (Get-Process -Id $descendantId -ErrorAction SilentlyContinue) | Should -Not -BeNullOrEmpty
        } finally {
            if ($descendantId) {
                Stop-Process -Id $descendantId -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

Describe 'Phase 1 developer lifecycle syntax and safety' {
    It 'parse-checks the lifecycle script and this test file' {
        foreach ($path in @($script:CyclePath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }

    It 'contains the fail-closed and no-config-mutation contracts' {
        $source = Get-Content -LiteralPath $script:CyclePath -Raw
        $source | Should -Match 'Assert-Phase1DevCycleDeployRoot'
        $source | Should -Match 'readlink.*-f'
        $source | Should -Match 'restart_stale_server_after_build_failure = \$false'
        $source | Should -Match 'database_or_config_mutation = \$false'
        $source | Should -Match 'build_log_path = \$buildLogPath'
        $source | Should -Not -Match '(?i)Set-ConfigValue|mysql\.exe|mysqladmin|UPDATE\s+|ALTER\s+|DROP\s+'
        $source | Should -Not -Match '(?i)WriteAllText\([^\n]*worldserver\.conf|playerbots\.conf'
    }

    It 'keeps the live runtime through build gates and stops only after the hash gate' {
        $source = Get-Content -LiteralPath $script:CyclePath -Raw
        $liveMarker = $source.IndexOf("runtime_kept_live_for_build", [System.StringComparison]::Ordinal)
        $rsyncMarker = $source.IndexOf("module_rsynced", [System.StringComparison]::Ordinal)
        $hashMarker = $source.IndexOf('$worldHash = Get-Phase1DevCycleSha256', [System.StringComparison]::Ordinal)
        $stopMarker = $source.IndexOf('$stop = Invoke-Phase1DevCycleWindowsScript -ScriptPath $stopScript', [System.StringComparison]::Ordinal)
        $startMarker = $source.IndexOf('$start = Invoke-Phase1DevCycleWindowsScript -ScriptPath $startScript', [System.StringComparison]::Ordinal)
        $liveMarker | Should -BeGreaterThan -1
        $rsyncMarker | Should -BeGreaterThan $liveMarker
        $hashMarker | Should -BeGreaterThan $rsyncMarker
        $stopMarker | Should -BeGreaterThan $hashMarker
        $startMarker | Should -BeGreaterThan $stopMarker
        $source | Should -Match 'restart_stale_server_after_build_failure = \$false'
    }
}
