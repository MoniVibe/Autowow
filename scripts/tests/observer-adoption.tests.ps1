<#
    Offline Pester coverage for explicit adoption of an already-running observer.

    These tests never open the bridge, inspect a live process, capture a screenshot,
    launch/close WoW, or write the real observer-state directory.
#>

BeforeAll {
    $script:Scripts = Split-Path -Parent $PSScriptRoot
    $script:Root = Split-Path -Parent $script:Scripts
    $script:Lib = Join-Path $script:Scripts 'observer-automation-lib.ps1'
    $script:Adopt = Join-Path $script:Scripts 'adopt-observer-session.ps1'
    $script:Capture = Join-Path $script:Scripts 'capture-observer-screenshot.ps1'
    . $script:Lib

    $script:ValidStatus = [pscustomobject]@{
        ok = $true
        observer = [uint32]21
        protected = $true
        is_gm = $true
        gm_visible = $false
        in_group = $false
        in_combat = $false
        name = 'Flacid'
    }
}

Describe 'observer bridge adoption status guard (offline)' {
    It 'requires the exact observer and every protected/no-interference boolean' {
        $result = Test-ObserverBridgeStatusForAdoption -Response $script:ValidStatus -ObserverGuid 21
        $result.valid | Should -BeTrue
        @($result.reasons).Count | Should -Be 0
    }

    It 'rejects a wrong observer GUID' {
        $wrong = $script:ValidStatus.PSObject.Copy()
        $wrong.observer = 22
        (Test-ObserverBridgeStatusForAdoption -Response $wrong -ObserverGuid 21).valid | Should -BeFalse
    }

    It 'rejects protection loss, visibility, grouping, combat, and missing fields' {
        foreach ($field in @('protected','is_gm','gm_visible','in_group','in_combat')) {
            $variant = $script:ValidStatus.PSObject.Copy()
            if ($field -in @('protected','is_gm')) { $variant.$field = $false } else { $variant.$field = $true }
            (Test-ObserverBridgeStatusForAdoption -Response $variant -ObserverGuid 21).valid | Should -BeFalse
        }
        $missing = [pscustomobject]@{ ok=$true; observer=[uint32]21; protected=$true; is_gm=$true; gm_visible=$false; in_group=$false }
        $check = Test-ObserverBridgeStatusForAdoption -Response $missing -ObserverGuid 21
        $check.valid | Should -BeFalse
        @($check.reasons) | Should -Contain 'bridge_field_missing_or_not_boolean:in_combat'
    }
}

Describe 'explicit approved observer executable roots (offline)' {
    BeforeEach {
        $script:FixtureRoot = Join-Path $TestDrive 'AutoWoW'
        $currentRoot = Join-Path $script:FixtureRoot 'work\wow-client'
        $dedicatedRoot = Join-Path $script:FixtureRoot 'work\observer-client'
        New-Item -ItemType Directory -Path $currentRoot,$dedicatedRoot -Force | Out-Null
        New-Item -ItemType File -Path (Join-Path $currentRoot 'Wow.exe'),(Join-Path $dedicatedRoot 'Wow.exe') -Force | Out-Null
        $manifest = [ordered]@{
            schema = 'autowow.observer.client.v1'
            observer_client = [IO.Path]::GetFullPath($dedicatedRoot)
            executable_path = [IO.Path]::GetFullPath((Join-Path $dedicatedRoot 'Wow.exe'))
            source_client_modified = $false
        }
        $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dedicatedRoot 'observer-client-manifest.json') -Encoding UTF8
    }

    It 'approves the current client and a manifest-justified dedicated copy only' {
        $roots = @(Get-ObserverApprovedExecutableRoots -ServerRoot $script:FixtureRoot)
        @($roots.label) | Should -Contain 'current-wow-client'
        @($roots.label) | Should -Contain 'dedicated-observer-client'
        (Test-ObserverApprovedExecutablePath -ServerRoot $script:FixtureRoot -ExecutablePath (Join-Path $script:FixtureRoot 'work\wow-client\Wow.exe')) | Should -BeTrue
        (Test-ObserverApprovedExecutablePath -ServerRoot $script:FixtureRoot -ExecutablePath (Join-Path $script:FixtureRoot 'work\observer-client\Wow.exe')) | Should -BeTrue
    }

    It 'rejects arbitrary executables and a child path below an approved root' {
        (Get-ObserverApprovedExecutable -ServerRoot $script:FixtureRoot -ExecutablePath 'D:\Games\other\Wow.exe') | Should -Be $null
        (Get-ObserverApprovedExecutable -ServerRoot $script:FixtureRoot -ExecutablePath (Join-Path $script:FixtureRoot 'work\wow-client\bin\Wow.exe')) | Should -Be $null
    }

    It 'does not approve the dedicated copy when its preparation manifest is invalid' {
        $manifestPath = Join-Path $script:FixtureRoot 'work\observer-client\observer-client-manifest.json'
        $invalid = [ordered]@{
            schema = 'autowow.observer.client.v0'
            observer_client = [IO.Path]::GetFullPath((Join-Path $script:FixtureRoot 'work\observer-client'))
            executable_path = [IO.Path]::GetFullPath((Join-Path $script:FixtureRoot 'work\observer-client\Wow.exe'))
            source_client_modified = $false
        }
        $invalid | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
        @((Get-ObserverApprovedExecutableRoots -ServerRoot $script:FixtureRoot).label) | Should -Not -Contain 'dedicated-observer-client'
    }
}

Describe 'adopted observer session manifest contract (offline)' {
    It 'records exact process, start time, executable root, HWND, title, and verified bridge state' {
        $process = [pscustomobject]@{
            process_id = [uint32]24148
            start_time_utc = '2026-07-18T20:00:00.0000000Z'
            executable_path = 'D:\Games\wowstuff\AutoWoW\work\wow-client\Wow.exe'
            command_line = 'Wow.exe'
        }
        $window = [pscustomobject]@{
            process_id = [uint32]24148
            start_time_utc = $process.start_time_utc
            executable_path = $process.executable_path
            command_line = $process.command_line
            window_handle = [uint64]8844
            title = 'World of Warcraft'
        }
        $root = [pscustomobject]@{ label='current-wow-client'; executable_path=$process.executable_path }
        $manifest = New-ObserverAdoptedSessionManifest -ObserverGuid 21 -ProcessIdentity $process `
            -WindowIdentity $window -ApprovedExecutable $root -BridgeStatus $script:ValidStatus `
            -BridgeHost '127.0.0.1' -BridgePort 18787 -ObservedUtc '2026-07-18T20:00:01.0000000Z'
        $manifest.schema | Should -BeExactly 'autowow.observer.session.v1'
        $manifest.session_mode | Should -BeExactly 'adopted'
        $manifest.managed_by_automation | Should -BeFalse
        $manifest.observer_guid | Should -Be 21
        $manifest.wow.process_id | Should -Be 24148
        $manifest.wow.start_time_utc | Should -BeExactly $process.start_time_utc
        $manifest.wow.executable_path | Should -BeExactly $process.executable_path
        $manifest.wow.approved_executable_root | Should -BeExactly 'current-wow-client'
        $manifest.wow.window_handle | Should -Be 8844
        @($manifest.wow.allowed_titles) | Should -BeExactly @('World of Warcraft')
        $manifest.bridge.protected | Should -BeTrue
        $manifest.bridge.is_gm | Should -BeTrue
        $manifest.bridge.gm_visible | Should -BeFalse
        $manifest.bridge.in_group | Should -BeFalse
        $manifest.bridge.in_combat | Should -BeFalse
    }

    It 'rejects adoption manifests for any observer other than GUID 21' {
        $process = [pscustomobject]@{ process_id=24148; start_time_utc='2026-07-18T20:00:00.0000000Z'; executable_path='D:\Wow.exe' }
        $window = [pscustomobject]@{ process_id=24148; window_handle=8844; executable_path='D:\Wow.exe'; title='World of Warcraft' }
        $root = [pscustomobject]@{ label='current-wow-client'; executable_path='D:\Wow.exe' }
        { New-ObserverAdoptedSessionManifest -ObserverGuid 22 -ProcessIdentity $process -WindowIdentity $window `
            -ApprovedExecutable $root -BridgeStatus $script:ValidStatus -BridgeHost '127.0.0.1' -BridgePort 18787 `
            -ObservedUtc '2026-07-18T20:00:01.0000000Z' } | Should -Throw '*GUID 21*'
    }
}

Describe 'exact title comparison (offline)' {
    It 'rejects a title with different casing or extra text' {
        $args = @{
            ExpectedProcessId = 24148
            ExpectedWindowHandle = [uint64]8844
            ExpectedExecutablePath = 'D:\Games\wowstuff\AutoWoW\work\wow-client\Wow.exe'
            AllowedTitles = @('World of Warcraft')
            ActualProcessId = 24148
            ActualWindowHandle = [uint64]8844
            ActualExecutablePath = 'D:\Games\wowstuff\AutoWoW\work\wow-client\Wow.exe'
            ActualTitle = 'world of warcraft'
        }
        Test-ObserverCaptureWindowIdentity @args | Should -BeFalse
        $args.ActualTitle = 'World of Warcraft - Other'
        Test-ObserverCaptureWindowIdentity @args | Should -BeFalse
    }
}

Describe 'adoption and capture static safety contracts' {
    It 'requires explicit adoption and has no login, launch, close, or relocation capability' {
        $source = Get-Content -LiteralPath $script:Adopt -Raw
        $source | Should -Match '\[switch\]\$Adopt'
        $source | Should -Match 'Get-ObserverActualProcessIdentity'
        $source | Should -Match 'Get-ObserverActualWindowIdentity'
        $source | Should -Match 'Test-ObserverBridgeStatusForAdoption'
        $source | Should -Match 'ShouldProcess'
        $source | Should -Not -Match '(?i)Start-Process|Stop-Process|SendKeys|CloseMainWindow|observe (?:protect|watch|relocate)'
    }

    It 'requires GUID 21 for adopted sessions and no longer rejects GUID 21 unconditionally' {
        $source = Get-Content -LiteralPath $script:Capture -Raw
        $source | Should -Match "sessionMode -eq 'adopted'.*-ne 21"
        $source | Should -Not -Match 'observer_guid -eq 21\) \{ throw .+invalid'
        $source | Should -Match 'Get-ObserverApprovedExecutable'
    }

    It 'binds the process start time before accepting the window identity' {
        $source = Get-Content -LiteralPath $script:Adopt -Raw
        $source | Should -Match 'Test-ObserverProcessIdentity'
        $source | Should -Match 'start_time_utc'
    }
}
