BeforeAll {
    $script:Revive = Join-Path (Split-Path -Parent $PSScriptRoot) 'revive-autowow.ps1'
    $tokens = $null
    $errors = $null
    $tree = [Management.Automation.Language.Parser]::ParseFile($script:Revive, [ref]$tokens, [ref]$errors)
    $waitFunction = $tree.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Wait-ForRosterOnline' }, $true)
    . ([scriptblock]::Create($waitFunction.Extent.Text))
    $actionFunction = $tree.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-BridgeAction' }, $true)
    . ([scriptblock]::Create($actionFunction.Extent.Text))
    $script:bridgeScript = 'Invoke-TestBridge'
    function Invoke-TestBridge { param($Action, $BotGuid) throw 'Unexpected live bridge substitute call.' }
}

Describe 'bounded revival entrypoint (offline)' {
    It 'parses without PowerShell errors' {
        $tokens = $null
        $errors = $null
        [void][Management.Automation.Language.Parser]::ParseFile($script:Revive, [ref]$tokens, [ref]$errors)
        @($errors).Count | Should -Be 0
    }

    It 'requires explicit Apply for Start and Park before inspecting or changing runtime' {
        { & $script:Revive -Action Start } | Should -Throw '*requires -Apply*'
        { & $script:Revive -Action Park } | Should -Throw '*requires -Apply*'
    }

    It 'rejects an unapproved roster before any runtime operation' {
        { & $script:Revive -Action Status -RosterGuids @(101,40454) } | Should -Throw '*restricted to the existing GUIDs*'
    }

    It 'uses only existing-data MySQL startup and no legacy director or seed launcher' {
        $source = Get-Content -LiteralPath $script:Revive -Raw
        $source | Should -Match 'server\\mysql-data\\mysql'
        $source | Should -Match 'start-phase1-wsl-worldserver\.ps1'
        $source | Should -Not -Match 'MYSQL_ROOT_PASSWORD'
        $source | Should -Not -Match '&\s*\$?[^\r\n]*(start-mysql|league-simulation-director|oracle-race-campaign)\.ps1'
    }

    It 'waits for queued logins before issuing independent to newly activated GUIDs' {
        $source = Get-Content -LiteralPath $script:Revive -Raw
        $activation = $source.IndexOf('Invoke-BridgeAction -Verb activate -Guid $guid')
        $wait = $source.IndexOf('Wait-ForRosterOnline -Guids @($newlyActivated)')
        $independent = $source.IndexOf('Invoke-BridgeAction -Verb independent -Guid $guid')
        $activation | Should -BeGreaterThan -1
        $wait | Should -BeGreaterThan $activation
        $independent | Should -BeGreaterThan $wait
        $source | Should -Match 'Timed out waiting for newly activated roster'
    }

    It 'polls a queued login until the selected bot appears' {
        $script:polls = 0
        function Get-BridgeList {
            $script:polls++
            if ($script:polls -eq 1) { return [pscustomobject]@{ bots = @() } }
            return [pscustomobject]@{ bots = @([pscustomobject]@{ guid = 101 }) }
        }
        Mock Start-Sleep {}
        { Wait-ForRosterOnline -Guids @(101) -TimeoutSeconds 5 } | Should -Not -Throw
        $script:polls | Should -Be 2
    }

    It 'retries a combat deferral until the newly activated scout can be armed' {
        $script:armCalls = 0
        Mock Invoke-TestBridge {
            $script:armCalls++
            if ($script:armCalls -eq 1) { return '{"ok":false,"error":"independent_deferred_combat"}' }
            return '{"ok":true}'
        }
        Mock Start-Sleep {}
        (Invoke-BridgeAction -Verb independent -Guid 236).ok | Should -BeTrue
        $script:armCalls | Should -Be 2
        Should -Invoke Start-Sleep -Times 1 -Exactly -ParameterFilter { $Seconds -eq 2 }
        Should -Invoke Invoke-TestBridge -Times 2 -Exactly -ParameterFilter { $Action -eq 'independent' -and $BotGuid -eq 236 }
    }

    It 'reports sustained combat after a bounded number of retries' {
        Mock Invoke-TestBridge { '{"ok":false,"error":"independent_deferred_combat"}' }
        Mock Start-Sleep {}
        { Invoke-BridgeAction -Verb independent -Guid 236 } | Should -Throw '*independent_deferred_combat*'
        Should -Invoke Invoke-TestBridge -Times 11 -Exactly
        Should -Invoke Start-Sleep -Times 10 -Exactly
    }

    It 'does not retry other bridge rejections' {
        Mock Invoke-TestBridge { '{"ok":false,"error":"unknown_bot"}' }
        Mock Start-Sleep {}
        { Invoke-BridgeAction -Verb independent -Guid 236 } | Should -Throw '*unknown_bot*'
        Should -Invoke Invoke-TestBridge -Times 1 -Exactly
        Should -Invoke Start-Sleep -Times 0 -Exactly
    }

    It 'does not repeat activation or deactivation on a combat-shaped rejection' {
        Mock Invoke-TestBridge { '{"ok":false,"error":"independent_deferred_combat"}' }
        Mock Start-Sleep {}
        { Invoke-BridgeAction -Verb activate -Guid 236 } | Should -Throw '*was rejected*'
        { Invoke-BridgeAction -Verb deactivate -Guid 236 } | Should -Throw '*was rejected*'
        Should -Invoke Invoke-TestBridge -Times 2 -Exactly
        Should -Invoke Start-Sleep -Times 0 -Exactly
    }
}
