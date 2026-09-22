Set-StrictMode -Version Latest

BeforeAll {
    $script:LibraryPath = Join-Path $PSScriptRoot '..\overnight-instance-observer-lib.ps1'
    $script:ObserverPath = Join-Path $PSScriptRoot '..\overnight-instance-observer.ps1'
    $script:ObserverSource = Get-Content -LiteralPath $script:ObserverPath -Raw
    . $script:LibraryPath
}

Describe 'overnight instance observer pure contracts' {
    It 'computes added mask bits and terminal state' {
        $delta = Get-OvernightInstanceObserverMaskDelta -OldMask 1 -NewMask 5 -ExpectedMask 5
        $delta.added_bits | Should -Be 4
        $delta.terminal | Should -BeTrue
    }

    It 'extracts the latest sample for each probe across JSONL files' {
        $a = Join-Path $TestDrive 'a.jsonl'
        $b = Join-Path $TestDrive 'b.jsonl'
        $records = @(
            [ordered]@{ event = 'monitor_sample'; probe_id = 'dtk-party'; observed_at_utc = '2026-07-16T01:00:00Z'; members = @() },
            [ordered]@{ event = 'monitor_sample'; probe_id = 'dtk-party'; observed_at_utc = '2026-07-16T02:00:00Z'; members = @([ordered]@{ alive = $true }) },
            [ordered]@{ event = 'monitor_sample'; probe_id = 'nexus-party'; observed_at_utc = '2026-07-16T01:30:00Z'; members = @() }
        )
        $lineA = $records[0] | ConvertTo-Json -Compress
        $lineB = $records[1] | ConvertTo-Json -Compress
        $lineC = $records[2] | ConvertTo-Json -Compress
        [IO.File]::WriteAllLines($a, @($lineA, 'not-json'))
        [IO.File]::WriteAllLines($b, @($lineB, $lineC))
        $latest = @(Get-OvernightInstanceObserverLatestProbeSamples -Paths @($a, $b))
        $latest.Count | Should -Be 2
        ([datetime](@($latest | Where-Object probe_id -eq 'dtk-party'))[0].observed_at_utc).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss') | Should -Be '2026-07-16T02:00:00'
    }

    It 'parses only RaidLoot award and equip lines' {
        $events = @(Get-OvernightInstanceObserverRaidLootEvents -Lines @(
            '[RaidLoot] event=vote bot=Bot item=1',
            '[RaidLoot] event=award winner=Bot winner_guid=101 item=500 entry=500',
            '[RaidLoot] event=equip bot=Bot bot_guid=101 item=500 slot=13 result=equipped'
        ))
        $events.Count | Should -Be 2
        $events[0].source_event | Should -Be 'award'
        $events[0].fields.winner_guid | Should -Be '101'
        $events[1].source_event | Should -Be 'equip'
        $events[1].fields.result | Should -Be 'equipped'
    }

    It 'does not duplicate complete lines and handles partial append' {
        $path = Join-Path $TestDrive 'Playerbots.log'
        [IO.File]::WriteAllText($path, "old`n", [Text.UTF8Encoding]::new($false))
        $cursor = New-OvernightInstanceObserverLogCursor -Path $path
        [IO.File]::AppendAllText($path, "[RaidLoot] event=award item=500`n", [Text.UTF8Encoding]::new($false))
        $first = Read-OvernightInstanceObserverLogDelta -Path $path -Cursor $cursor
        @($first.lines).Count | Should -Be 1
        $second = Read-OvernightInstanceObserverLogDelta -Path $path -Cursor $first.cursor
        @($second.lines).Count | Should -Be 0
    }

    It 'resets safely on truncation and rotation' {
        $path = Join-Path $TestDrive 'Playerbots.log'
        [IO.File]::WriteAllText($path, "first line`n", [Text.UTF8Encoding]::new($false))
        $cursor = [pscustomobject][ordered]@{ path = $path; offset = 999L; identity = [pscustomobject]@{ creation_utc = ''; head_sha256 = '' } }
        $truncated = Read-OvernightInstanceObserverLogDelta -Path $path -Cursor $cursor
        $truncated.reset_reason | Should -Be 'truncated'
        @($truncated.lines).Count | Should -Be 1
        $oldCursor = New-OvernightInstanceObserverLogCursor -Path $path
        [IO.File]::WriteAllText($path, "rotated line`n", [Text.UTF8Encoding]::new($false))
        $rotated = Read-OvernightInstanceObserverLogDelta -Path $path -Cursor $oldCursor
        @($rotated.lines).Count | Should -Be 1
        $rotated.lines[0] | Should -Be 'rotated line'
    }

    It 'has a read-only source contract with no control-script invocation' {
        $script:ObserverSource | Should -Not -Match '(?i)(autowow-control|bridge|Start-Process|Stop-Process|Restart-Computer|Invoke-WebRequest)'
        $script:ObserverSource | Should -Not -Match '(?im)^[ \t]*(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE|LOAD|RENAME|LOCK|UNLOCK)[ \t]+'
        $script:ObserverSource | Should -Match 'Assert-OvernightInstanceObserverSelectOnlySql'
        $script:ObserverSource | Should -Match 'Invoke-RaidReadinessReadOnlySql'
        { Assert-OvernightInstanceObserverSelectOnlySql -Sql 'UPDATE acore_characters.instance SET completedEncounters=0' } | Should -Throw
        { Assert-OvernightInstanceObserverSelectOnlySql -Sql 'SELECT 1; SELECT 2' } | Should -Throw
    }

    It 'parse-checks the observer and library' {
        foreach ($path in @($script:ObserverPath, $script:LibraryPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
