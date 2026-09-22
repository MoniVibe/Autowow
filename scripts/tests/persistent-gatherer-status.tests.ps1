BeforeAll {
    $script:StatusPath = Join-Path $PSScriptRoot '..\persistent-gatherer-status.ps1'
    $script:ManifestPath = Join-Path $PSScriptRoot '..\gather-lane-manifest.json'
    $script:Source = Get-Content -LiteralPath $script:StatusPath -Raw
    . $script:StatusPath -LibraryOnly
}

Describe 'persistent gatherer status safety and evidence contract' {
    It 'parses cleanly and discovers the fixed roster including Kurl and Pikli' {
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($script:StatusPath, [ref]$null, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0

        $members = Read-GathererManifest -Path $script:ManifestPath
        @($members).Count | Should -Be 3
        (@($members | Where-Object { $_.guid -eq 6 }).name) | Should -BeExactly 'Kurl'
        (@($members | Where-Object { $_.guid -eq 49 }).name) | Should -BeExactly 'Pikli'
    }

    It 'classifies offline to online as a reconnect without bridge mutation' {
        (Get-BridgeTransition -PreviousStatus 'offline' -CurrentStatus 'online') | Should -BeExactly 'bridge_reconnected'
        (Get-BridgeTransition -PreviousStatus 'online' -CurrentStatus 'offline') | Should -BeExactly 'bridge_disconnected'
        $script:Source | Should -Match "ValidateSet\('list', 'snapshot'\)"
        $script:Source | Should -Not -Match '\$LASTEXITCODE'
        $script:Source | Should -Match '\$controlArgs\s*=\s*@\{'
        $script:Source | Should -Match '& \$ControlPath @controlArgs'
        $script:Source | Should -Not -Match '(?i)Invoke-ReadOnlyBridgeJson\s+-Action\s+(activate|deactivate|deploy|travel|recover|pause|resume)'
    }

    It 'flags worker rows outside the explicit gather manifest' {
        $members = Read-GathererManifest -Path $script:ManifestPath
        $telemetry = [pscustomobject]@{ members = @(
            [pscustomobject]@{ guid = 6; name = 'Kurl'; role = 'worker'; affiliation = 'wayfarer'; declared_professions = @('Mining') },
            [pscustomobject]@{ guid = 17; name = 'Kornzoggoch'; role = 'worker'; affiliation = 'wayfarer'; declared_professions = @() }
        ) }
        $extra = Get-UnregisteredWorkers -Telemetry $telemetry -Members $members
        @($extra).Count | Should -Be 1
        @($extra)[0].guid | Should -Be 17
        @($extra)[0].name | Should -BeExactly 'Kornzoggoch'
    }

    It 'writes an offline JSONL sample without requiring a bridge or secrets' {
        $testRoot = Join-Path $TestDrive 'autowow'
        $receipt = Join-Path $testRoot 'logs\status.jsonl'
        $state = Join-Path $testRoot 'work\state.json'
        $result = (& $script:StatusPath -ServerRoot $testRoot -ManifestPath $script:ManifestPath -BridgePort 1 -BridgeTimeoutMs 100 -ReceiptPath $receipt -StatePath $state | Out-String) | ConvertFrom-Json
        $result.bridge_status | Should -BeExactly 'offline'
        $result.safety.bridge_mutations | Should -Be 0
        $result.safety.secrets_recorded | Should -BeFalse
        Test-Path -LiteralPath $receipt | Should -BeTrue
        @((Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })).Count | Should -Be 1
    }
}
