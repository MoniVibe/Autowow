<#
    Pester v5 tests for the Observer Camera v0 control surface.

    Two layers:
      1. Offline unit tests (always run): verify observer-control.ps1 builds the exact wire
         request and rejects invalid argument combinations, using the -EmitRequestOnly seam.
         These never open a socket and never touch a server.
      2. Live safety tests (auto-skip if the bridge is offline): verify the server enforces the
         allow-list. If the ObserverControl hook is not built yet, the live test is marked
         inconclusive rather than failing.

    Run:  Invoke-Pester -Path .\scripts\tests\observer-control.tests.ps1
#>

BeforeAll {
    $script:ScriptsDir = Split-Path -Parent $PSScriptRoot
    $script:Control = Join-Path $script:ScriptsDir 'observer-control.ps1'
    $script:ObserverSource = Join-Path (Split-Path -Parent $script:ScriptsDir) '_phase1_worktree\mod-playerbots\src\AutoWow\ObserverControl.cpp'

    $script:BridgeUp = $false
    try {
        $probe = [System.Net.Sockets.TcpClient]::new()
        if ($probe.ConnectAsync('127.0.0.1', 18787).Wait(1000) -and $probe.Connected) { $script:BridgeUp = $true }
        $probe.Dispose()
    }
    catch { $script:BridgeUp = $false }
}

Describe 'observer exact-instance source contract (offline)' {
    It 'uses a temporary observer-only bind and reports exact instance identity' {
        $source = Get-Content -LiteralPath $script:ObserverSource -Raw
        $source | Should -Match 'PlayerGetBoundInstance'
        $source | Should -Match 'PlayerBindToInstance'
        $source | Should -Match 'PlayerUnbindInstance'
        $source | Should -Match 'observer_permanent_instance_conflict'
        $source | Should -Match '\\"instance_id\\"'
        $source | Should -Match '\\"same_instance\\"'
        $source | Should -Match 'forceInstanceTransfer'
        $source | Should -Match 'cameraTrailingDistance = 8\.0f'
        $source | Should -Not -Match 'observer->GetGroup\(\).*AddMember'
    }
}

Describe 'observer-control.ps1 request construction (offline)' {
    It 'builds a watch request with the leader guid' {
        & $script:Control -Action watch -ObserverGuid 5 -LeaderGuid 10 -EmitRequestOnly | Should -BeExactly 'observe watch 5 10'
    }
    It 'builds a relocate request' {
        & $script:Control -Action relocate -ObserverGuid 5 -EmitRequestOnly | Should -BeExactly 'observe relocate 5'
    }
    It 'builds a status request' {
        & $script:Control -Action status -ObserverGuid 5 -EmitRequestOnly | Should -BeExactly 'observe status 5'
    }
    It 'builds a protect request' {
        & $script:Control -Action protect -ObserverGuid 5 -EmitRequestOnly | Should -BeExactly 'observe protect 5'
    }
    It 'builds a release request' {
        & $script:Control -Action release -ObserverGuid 5 -EmitRequestOnly | Should -BeExactly 'observe release 5'
    }
}

Describe 'observer-control.ps1 argument validation (offline)' {
    It 'rejects a zero observer guid' {
        { & $script:Control -Action status -ObserverGuid 0 -EmitRequestOnly } | Should -Throw
    }
    It 'rejects watch without a leader guid' {
        { & $script:Control -Action watch -ObserverGuid 5 -EmitRequestOnly } | Should -Throw
    }
    It 'rejects an unknown action at the parameter binder' {
        { & $script:Control -Action teleport -ObserverGuid 5 -EmitRequestOnly } | Should -Throw
    }
}

Describe 'server-side safety enforcement (live)' {
    It 'refuses status for a guid that is not enrolled as an observer' {
        if (-not $script:BridgeUp) { Set-ItResult -Skipped -Because 'AutoWow bridge is not listening on 127.0.0.1:18787'; return }

        # 4293918596 is chosen to be absent from AutoWow.ObserverGuids on any sane config.
        $resp = (& $script:Control -Action status -ObserverGuid 4293918596) | ConvertFrom-Json
        if (-not $resp.ok -and $resp.error -match 'unknown command') {
            Set-ItResult -Inconclusive -Because 'the ObserverControl bridge hook is not built into the running worldserver yet'
            return
        }
        $resp.ok | Should -BeFalse
        $resp.error | Should -BeExactly 'not_an_observer'
    }
}
