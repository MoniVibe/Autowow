BeforeAll {
    $script:BundlePath = Join-Path $PSScriptRoot '..\probe-failure-bundle.ps1'
    . (Resolve-Path $script:BundlePath).Path
}

Describe 'probe-failure-bundle pure seams' {
    It 'redacts obvious credential patterns' {
        $redacted = ConvertTo-FailureBundleRedactedText 'password=secret token: abc123 Authorization: Bearer xyz api_key="value"'
        $redacted | Should -Not -Match 'secret|abc123|xyz|value'
        $redacted | Should -Match '<redacted>'
    }

    It 'returns bounded failure excerpts with context' {
        $lines = @('old', 'DungeonNavigator blocked=unreachable', 'context after', 'password=secret', 'tail')
        $excerpt = Get-FailureBundleLogExcerpt -Lines $lines -Hints @('unreachable') -MaxLines 10
        @($excerpt -split "`r?`n").Count | Should -BeLessOrEqual 10
        $excerpt | Should -Match 'blocked=unreachable'
        $excerpt | Should -Not -Match 'password=secret'
    }

    It 'handles an empty runtime log without failing the bundle' {
        $emptyCollection = [string[]]@()
        [string](Get-FailureBundleLogExcerpt -Lines $emptyCollection -Hints @('no_progress') -MaxLines 10) | Should -Be ''
        [string](Get-FailureBundleLogExcerpt -Lines '' -Hints @('no_progress') -MaxLines 10) | Should -Be ''
    }

    It 'builds a new redacted bundle from summary input and linked receipt/logs' {
        $root = Join-Path $TestDrive 'server'
        $logs = Join-Path $root 'logs'
        $runtime = Join-Path $logs 'phase1-runtime'
        New-Item -ItemType Directory -Path $runtime -Force | Out-Null
        $playerbots = Join-Path $runtime 'Playerbots.log'
        $worldserver = Join-Path $runtime 'worldserver.log'
        [IO.File]::WriteAllLines($playerbots, @('normal', 'ERROR no_progress password=secret', 'DungeonNavigator blocked=unreachable', 'tail'))
        [IO.File]::WriteAllLines($worldserver, @('world normal', 'runtime failure token=abc'))
        $receipt = Join-Path $logs 'probe.jsonl'
        $summary = Join-Path $logs 'probe-summary.json'
        $receiptRow = [ordered]@{ schema = 'autowow.probe-lab.receipt.v1'; event = 'monitor_complete'; summary_json_path = $summary; playerbots_log_path = $playerbots }
        [IO.File]::WriteAllText($receipt, (($receiptRow | ConvertTo-Json -Compress) + [Environment]::NewLine))
        $summaryObject = [ordered]@{
            schema = 'autowow.probe-lab.summary.v1'; status = 'FAIL'; reason = 'first_failure:no_progress'; receipt_path = $receipt; playerbots_log_path = $playerbots
            probes = @([ordered]@{ probe_id = 'test'; first_failure = [ordered]@{ code = 'no_progress'; detail = 'password=secret' } })
        }
        [IO.File]::WriteAllText($summary, ($summaryObject | ConvertTo-Json -Depth 10))

        $result = New-FailureBundle -InputPath $summary -ServerRoot $root -TailLines 10 -ExcerptLines 10 -CreatedAtUtc ([datetime]'2026-07-16T00:00:00Z')
        Test-Path -LiteralPath $result.manifest_path -PathType Leaf | Should -BeTrue
        Test-Path -LiteralPath $result.report_path -PathType Leaf | Should -BeTrue
        $result.manifest.safety.bridge_calls | Should -Be 0
        @($result.manifest.artifacts).Count | Should -Be 2
        @($result.manifest.logs).Count | Should -Be 2
        $allBundleText = Get-ChildItem -LiteralPath $result.bundle_path -File | Get-Content -Raw | Out-String
        $allBundleText | Should -Not -Match 'password=secret|token=abc'
        (Get-Content -LiteralPath (Join-Path $result.bundle_path 'Playerbots.log.tail.txt')).Count | Should -BeLessOrEqual 10
    }

    It 'accepts receipt JSONL and resolves linked summary without writing outside logs' {
        $root = Join-Path $TestDrive 'server'
        $logs = Join-Path $root 'logs'
        New-Item -ItemType Directory -Path $logs -Force | Out-Null
        $outside = Join-Path $TestDrive 'outside.json'
        [IO.File]::WriteAllText($outside, '{}')
        $summary = Join-Path $logs 'linked-summary.json'
        $receipt = Join-Path $logs 'linked-receipt.jsonl'
        [IO.File]::WriteAllText($summary, (@{ schema = 'autowow.probe-lab.summary.v1'; status = 'PASS'; reason = 'ok'; probes = @() } | ConvertTo-Json -Depth 5))
        [IO.File]::WriteAllText($receipt, ((@{ event = 'monitor_complete'; summary_json_path = 'logs/linked-summary.json' } | ConvertTo-Json -Compress) + [Environment]::NewLine))
        $result = New-FailureBundle -InputPath $receipt -ServerRoot $root -CreatedAtUtc ([datetime]'2026-07-16T00:01:00Z')
        $result.manifest.source_kind | Should -Be 'jsonl'
        Test-Path -LiteralPath (Join-Path $result.bundle_path 'probe-summary.json') -PathType Leaf | Should -BeTrue
        { Resolve-FailureBundleSourcePath -Path $outside -ServerRoot $root } | Should -Throw '*beneath logs*'
    }

    It 'suffixes colliding artifact names instead of overwriting either log capture' {
        $root = Join-Path $TestDrive 'collision-server'
        $logs = Join-Path $root 'logs'
        $left = Join-Path $logs 'left'
        $right = Join-Path $logs 'right'
        New-Item -ItemType Directory -Path $left,$right -Force | Out-Null
        $leftLog = Join-Path $left 'Playerbots.log'
        $rightLog = Join-Path $right 'Playerbots.log'
        [IO.File]::WriteAllLines($leftLog, @('left ERROR one'))
        [IO.File]::WriteAllLines($rightLog, @('right ERROR two'))
        $summary = Join-Path $logs 'collision-summary.json'
        [IO.File]::WriteAllText($summary, (@{
            schema = 'autowow.probe-lab.summary.v1'; status = 'FAIL'; reason = 'collision'; probes = @()
            playerbots_log_path = $leftLog; runtime_log_path = $rightLog
        } | ConvertTo-Json -Depth 5))

        $result = New-FailureBundle -InputPath $summary -ServerRoot $root -CreatedAtUtc ([datetime]'2026-07-16T00:02:00Z')
        @($result.manifest.logs).Count | Should -Be 2
        @($result.manifest.logs.tail_path | Select-Object -Unique).Count | Should -Be 2
        Test-Path -LiteralPath (Join-Path $result.bundle_path 'Playerbots.log.tail.txt') -PathType Leaf | Should -BeTrue
        Test-Path -LiteralPath (Join-Path $result.bundle_path 'Playerbots.log-1.tail.txt') -PathType Leaf | Should -BeTrue
    }

    It 'parse-checks the script and contains no bridge/database/process mutation surface' {
        $tokens = $null; $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $script:BundlePath).Path, [ref]$tokens, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0
        $source = Get-Content -LiteralPath $script:BundlePath -Raw
        $source | Should -Not -Match '(?i)TcpClient|mysql|Invoke-WebRequest|Start-Process|Stop-Process|Restart-Service|autowow-control'
        $source | Should -Match 'failure-bundles'
    }
}
