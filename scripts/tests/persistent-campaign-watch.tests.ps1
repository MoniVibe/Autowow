<##
    Offline, non-mutating Pester tests for persistent-campaign-watch.ps1.
    The live supervisor is only parsed; it is never launched by this test file.
#>

BeforeAll {
    $script:WatcherPath = Join-Path $PSScriptRoot '..\persistent-campaign-watch.ps1'
    $script:SupervisorPath = Join-Path $PSScriptRoot '..\persistent-campaign-supervisor.ps1'
    $script:WatcherSource = Get-Content -LiteralPath $script:WatcherPath -Raw
    . $script:WatcherPath -LibraryOnly
}

Describe 'persistent campaign watcher parse and safety contract' {
    It 'parses the watcher and existing supervisor without errors' {
        foreach ($path in @($script:WatcherPath, $script:SupervisorPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0
        }
    }

    It 'uses the fixed AutoWoW root and exact existing supervisor path' {
        $paths = Get-FixedWatchPaths
        $paths.server_root | Should -Be ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')))
        $paths.supervisor_path | Should -Be ([System.IO.Path]::GetFullPath($script:SupervisorPath))
        $paths.lock_path | Should -Match '\\work\\persistent-campaign-watch\\watch\.lock$'
        $paths.receipt_path | Should -Match '\\work\\persistent-campaign-watch\\persistent-campaign-watch\.jsonl$'
    }

    It 'allows only current questing and gathering phases' {
        (Get-Command $script:WatcherPath).Parameters['Phase'].Attributes.TypeId.FullName | Should -Not -BeNullOrEmpty
        { New-WatchSupervisorArguments -SupervisorPath $script:SupervisorPath -ServerRoot 'D:\Games\wowstuff\AutoWoW' -Phase raid -DurationMinutes 60 -Apply $false } | Should -Throw
    }

    It 'bounds duration and polling parameter metadata' {
        $command = Get-Command $script:WatcherPath
        ($command.Parameters['DurationHours'].Attributes | Where-Object { $_ -is [System.Management.Automation.ValidateRangeAttribute] }).MinRange | Should -Be 1
        ($command.Parameters['DurationHours'].Attributes | Where-Object { $_ -is [System.Management.Automation.ValidateRangeAttribute] }).MaxRange | Should -Be 24
        ($command.Parameters['PollSeconds'].Attributes | Where-Object { $_ -is [System.Management.Automation.ValidateRangeAttribute] }).MinRange | Should -Be 60
        ($command.Parameters['PollSeconds'].Attributes | Where-Object { $_ -is [System.Management.Automation.ValidateRangeAttribute] }).MaxRange | Should -Be 900
    }

    It 'passes explicit fixed arguments and gates -Apply' {
        $dry = New-WatchSupervisorArguments -SupervisorPath $script:SupervisorPath -ServerRoot 'D:\Games\wowstuff\AutoWoW' -Phase gathering -DurationMinutes 1440 -Apply $false
        $dry | Should -Contain '-ServerRoot'
        $dry | Should -Contain 'D:\Games\wowstuff\AutoWoW'
        $dry | Should -Contain '-Phase'
        $dry | Should -Contain 'gathering'
        $dry | Should -Contain '-DurationMinutes'
        $dry | Should -Not -Contain '-Apply'

        $apply = New-WatchSupervisorArguments -SupervisorPath $script:SupervisorPath -ServerRoot 'D:\Games\wowstuff\AutoWoW' -Phase questing -DurationMinutes 60 -Apply $true
        $apply | Should -Contain '-Apply'
    }

    It 'captures child exit code, stdout, and stderr using an offline stub' {
        $stub = Join-Path $TestDrive 'offline-supervisor-stub.ps1'
        @'
Write-Output 'offline-stdout'
[Console]::Error.WriteLine('offline-stderr')
exit 7
'@ | Set-Content -LiteralPath $stub -Encoding utf8

        $result = Invoke-WatchReconcile -SupervisorPath $stub -ServerRoot $TestDrive -Phase questing -DurationMinutes 60 -Apply $false
        $result.success | Should -BeFalse
        $result.exit_code | Should -Be 7
        $result.stdout | Should -Match 'offline-stdout'
        $result.stderr | Should -Match 'offline-stderr'
        $result.command_line | Should -Match 'offline-supervisor-stub\.ps1'
    }

    It 'fails closed for a missing or parse-invalid supervisor' {
        $missing = Test-WatchSupervisor -Path (Join-Path $TestDrive 'missing-supervisor.ps1')
        $missing.valid | Should -BeFalse
        $missing.reason | Should -Be 'supervisor_missing'

        $invalidPath = Join-Path $TestDrive 'invalid-supervisor.ps1'
        'if (' | Set-Content -LiteralPath $invalidPath -Encoding utf8
        $invalid = Test-WatchSupervisor -Path $invalidPath
        $invalid.valid | Should -BeFalse
        $invalid.reason | Should -Be 'supervisor_parse_invalid'
        @($invalid.parse_errors).Count | Should -BeGreaterThan 0
    }

    It 'uses capped exponential backoff after reconcile failures' {
        (Get-WatchBackoffSeconds -PollSeconds 60 -ConsecutiveFailures 1) | Should -Be 120
        (Get-WatchBackoffSeconds -PollSeconds 60 -ConsecutiveFailures 2) | Should -Be 240
        (Get-WatchBackoffSeconds -PollSeconds 60 -ConsecutiveFailures 3) | Should -Be 480
        (Get-WatchBackoffSeconds -PollSeconds 60 -ConsecutiveFailures 4) | Should -Be 900
        (Get-WatchBackoffSeconds -PollSeconds 900 -ConsecutiveFailures 30) | Should -Be 900
    }

    It 'writes the required JSONL lifecycle events without touching live receipts' {
        $receipt = Join-Path $TestDrive 'watch-receipt.jsonl'
        Write-WatchReceipt -Path $receipt -Event started -Fields @{ mode = 'dry_run' }
        Write-WatchReceipt -Path $receipt -Event reconcile_result -Fields @{ exit_code = 0; stdout = 'plan'; stderr = '' }
        Write-WatchReceipt -Path $receipt -Event failure -Fields @{ kind = 'reconcile'; error = 'bounded test failure' }
        Write-WatchReceipt -Path $receipt -Event completed -Fields @{ status = 'completed_with_errors' }

        $events = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })
        @($events).Count | Should -Be 4
        @($events.event) | Should -Be @('started', 'reconcile_result', 'failure', 'completed')
        $events[0].schema | Should -Be 'autowow.persistent-campaign-watch'
    }

    It 'contains no destructive process or live-operation primitive' {
        $script:WatcherSource | Should -Not -Match '(?m)\bStop-Process\b'
        $script:WatcherSource | Should -Not -Match '(?m)\bRemove-Item\b'
        $script:WatcherSource | Should -Not -Match '(?i)\b(bridge|mysql|database|teleport)\b'
    }
}
