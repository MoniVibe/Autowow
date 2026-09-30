BeforeAll {
    $script:Scripts = Split-Path -Parent $PSScriptRoot
    $script:CohortStartSource = Join-Path $script:Scripts 'cohort\cohort-start.ps1'
    $script:SoakStartSource = Join-Path $script:Scripts 'start-soak-full.ps1'
    $script:SubjectDir = Join-Path $TestDrive 'cohort'
    New-Item -ItemType Directory -Path $script:SubjectDir | Out-Null
    Copy-Item -LiteralPath $script:CohortStartSource -Destination (Join-Path $script:SubjectDir 'cohort-start.ps1')
    $script:Subject = Join-Path $script:SubjectDir 'cohort-start.ps1'
    Set-Content -LiteralPath (Join-Path $script:SubjectDir 'cohort-lib.ps1') -Encoding Ascii -Value @'
Set-StrictMode -Version Latest
function Get-CohortManifest { param($Path) return $global:CohortTestManifest }
function Select-CohortEntries { param($Manifest, $Id, $Faction) return @($Manifest.entries) }
function Get-CohortDb { return [pscustomobject]@{} }
function Get-CohortInventory { param($DbInfo, $Entries) return [pscustomobject]@{ rows = @($global:CohortTestRows) } }
function Get-CohortOracleAllowlist { param($WslDistro) return [pscustomobject]@{ enabled = $false; guids = @() } }
function Get-CohortOnlineGuids { return @($global:CohortTestOnline) }
function Invoke-CohortBridge {
    param($Action, [uint32]$Guid)
    $global:CohortTestCalls.Add("$Action/$Guid")
    if ($Action -eq 'activate') {
        if ($Guid -notin $global:CohortTestOnline) { $global:CohortTestOnline = @($global:CohortTestOnline) + $Guid }
        return $global:CohortTestActivateResult
    }
    if ($Action -eq 'independent') {
        $result = @($global:CohortTestIndependentResults)[$global:CohortTestIndependentIndex]
        $global:CohortTestIndependentIndex++
        return $result
    }
    throw "Unexpected bridge action $Action"
}
'@

    $tokens = $null
    $errors = $null
    $tree = [Management.Automation.Language.Parser]::ParseFile($script:SoakStartSource, [ref]$tokens, [ref]$errors)
    $script:SoakStartTree = $tree
    $retryFunction = $tree.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-CohortStartWithRetry'
    }, $true)
    . ([scriptblock]::Create($retryFunction.Extent.Text))
    $launchPlanFunction = $tree.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-FullSoakServerLaunchPlan'
    }, $true)
    . ([scriptblock]::Create($launchPlanFunction.Extent.Text))
    foreach ($functionName in @(
        'ConvertFrom-FullSoakControlResult',
        'Invoke-FullSoakWslScoutStart',
        'Invoke-FullSoakScoutDispatch')) {
        $functionAst = $tree.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
        }, $true)
        . ([scriptblock]::Create($functionAst.Extent.Text))
    }
}

Describe 'cohort startup retry correctness' {
    BeforeEach {
        $global:CohortTestManifest = [pscustomobject]@{ entries = @([pscustomobject]@{ id = 'A-HU-01' }) }
        $global:CohortTestRows = @([pscustomobject]@{
            id = 'A-HU-01'; guid = [uint32]31; state = 'ok'; enrolled = $true; control_arm = 'stock'
        })
        $global:CohortTestOnline = @()
        $global:CohortTestCalls = [System.Collections.Generic.List[string]]::new()
        $global:CohortTestIndependentIndex = 0
        $global:CohortTestActivateResult = [pscustomobject]@{ ok = $true }
        $global:CohortTestIndependentResults = @()
        Mock Start-Sleep {}
    }

    AfterAll {
        Remove-Variable -Scope Global -Name CohortTestManifest,CohortTestRows,CohortTestOnline,CohortTestCalls,CohortTestIndependentIndex,CohortTestActivateResult,CohortTestIndependentResults -ErrorAction SilentlyContinue
    }

    It 'retries a newly-online bot after the first bounded combat deferral' {
        $deferred = [pscustomobject]@{ ok = $false; error = 'independent_deferred_combat' }
        $global:CohortTestIndependentResults = @(1..11 | ForEach-Object { $deferred }) + @([pscustomobject]@{ ok = $true })

        $first = ((& $script:Subject -ControlMode Stock -Apply -AllowIncomplete | Out-String) | ConvertFrom-Json)
        $first.complete | Should -BeFalse
        @($first.pending_guids) | Should -Be @(31)

        $second = ((& $script:Subject -ControlMode Stock -Apply -PendingGuid $first.pending_guids -AllowIncomplete | Out-String) | ConvertFrom-Json)
        $second.complete | Should -BeTrue
        @($second.pending_guids).Count | Should -Be 0
        @($global:CohortTestCalls | Where-Object { $_ -eq 'activate/31' }).Count | Should -Be 1
        @($global:CohortTestCalls | Where-Object { $_ -eq 'independent/31' }).Count | Should -Be 12
    }

    It 'keeps persistent combat deferral pending instead of reporting success' {
        $global:CohortTestIndependentResults = @(1..22 | ForEach-Object {
            [pscustomobject]@{ ok = $false; error = 'independent_deferred_combat' }
        })

        $first = ((& $script:Subject -ControlMode Stock -Apply -AllowIncomplete | Out-String) | ConvertFrom-Json)
        $second = ((& $script:Subject -ControlMode Stock -Apply -PendingGuid $first.pending_guids -AllowIncomplete | Out-String) | ConvertFrom-Json)
        $second.complete | Should -BeFalse
        @($second.pending_guids) | Should -Be @(31)
    }

    It 'clears an activation rejection when the bot appears asynchronously and arms successfully' {
        $global:CohortTestActivateResult = [pscustomobject]@{ ok = $false; error = 'login_already_queued' }
        $global:CohortTestIndependentResults = @([pscustomobject]@{ ok = $true })

        $status = ((& $script:Subject -ControlMode Stock -Apply -AllowIncomplete | Out-String) | ConvertFrom-Json)
        $status.complete | Should -BeTrue
        @($status.pending_guids).Count | Should -Be 0
        @($status.armed) | Should -Contain 'A-HU-01=31'
        $global:CohortTestCalls | Should -Contain 'independent/31'
    }

    It 'does not issue independent or activate for an originally-online healthy bot' {
        $global:CohortTestOnline = @(31)
        $status = ((& $script:Subject -ControlMode Stock -Apply -AllowIncomplete | Out-String) | ConvertFrom-Json)
        $status.complete | Should -BeTrue
        $global:CohortTestCalls.Count | Should -Be 0
        @($status.already_online) | Should -Contain 'A-HU-01=31'
    }

    It 'preserves an originally-online native group by making no independent call' {
        $global:CohortTestOnline = @(31)
        $global:CohortTestIndependentResults = @([pscustomobject]@{ ok = $false; error = 'independent_requires_solo' })
        $null = & $script:Subject -ControlMode Stock -Apply -AllowIncomplete
        @($global:CohortTestCalls | Where-Object { $_ -like 'independent/*' }).Count | Should -Be 0
    }

    It 'keeps dry-run free of bridge mutations and exposes the pending preflight GUID' {
        $status = ((& $script:Subject -ControlMode Stock | Out-String) | ConvertFrom-Json)
        $status.complete | Should -BeFalse
        @($status.pending_guids) | Should -Be @(31)
        $global:CohortTestCalls.Count | Should -Be 0
    }
}

Describe 'full soak cohort retry gate' {
    BeforeEach {
        $global:CohortLauncherCalls = [System.Collections.Generic.List[string]]::new()
        $global:CohortLauncherAttempt = 0
        Mock Start-Sleep {}
    }

    AfterAll {
        Remove-Variable -Scope Global -Name CohortLauncherCalls,CohortLauncherAttempt -ErrorAction SilentlyContinue
    }

    It 'passes the preflight pending GUID into a later successful retry' {
        $fake = Join-Path $TestDrive 'cohort-launcher-success.ps1'
        Set-Content -LiteralPath $fake -Encoding Ascii -Value @'
param([string]$ControlMode, [switch]$Apply, [uint32[]]$PendingGuid = @(), [switch]$AllowIncomplete)
$global:CohortLauncherCalls.Add("apply=$Apply pending=$($PendingGuid -join ',')")
if (-not $Apply) { '{"complete":false,"pending_guids":[31]}' ; return }
$global:CohortLauncherAttempt++
if ($global:CohortLauncherAttempt -eq 1) { '{"complete":false,"pending_guids":[31]}' }
else { '{"complete":true,"pending_guids":[]}' }
'@
        $status = Invoke-CohortStartWithRetry -CohortStartPath $fake -Attempts 2 -DelaySeconds 0
        $status.complete | Should -BeTrue
        $global:CohortLauncherCalls | Should -Contain 'apply=True pending=31'
    }

    It 'throws after bounded attempts while a GUID remains pending' {
        $fake = Join-Path $TestDrive 'cohort-launcher-fail.ps1'
        Set-Content -LiteralPath $fake -Encoding Ascii -Value @'
param([string]$ControlMode, [switch]$Apply, [uint32[]]$PendingGuid = @(), [switch]$AllowIncomplete)
if (-not $Apply) { '{"complete":false,"pending_guids":[31]}' ; return }
'{"complete":false,"pending_guids":[31]}'
'@
        { Invoke-CohortStartWithRetry -CohortStartPath $fake -Attempts 2 -DelaySeconds 0 } |
            Should -Throw '*incomplete after 2 attempts*pending GUIDs: 31*'
    }

    It 'retains the preflight GUID when an apply attempt throws unexpectedly' {
        $fake = Join-Path $TestDrive 'cohort-launcher-throw.ps1'
        Set-Content -LiteralPath $fake -Encoding Ascii -Value @'
param([string]$ControlMode, [switch]$Apply, [uint32[]]$PendingGuid = @(), [switch]$AllowIncomplete)
if (-not $Apply) { '{"complete":false,"pending_guids":[31]}' ; return }
$global:CohortLauncherCalls.Add("pending=$($PendingGuid -join ',')")
$global:CohortLauncherAttempt++
if ($global:CohortLauncherAttempt -eq 1) { throw 'transport failed' }
'{"complete":true,"pending_guids":[]}'
'@
        $status = Invoke-CohortStartWithRetry -CohortStartPath $fake -Attempts 2 -DelaySeconds 0
        $status.complete | Should -BeTrue
        @($global:CohortLauncherCalls | Where-Object { $_ -eq 'pending=31' }).Count | Should -Be 2
    }

    It 'does not trust a complete flag while pending GUIDs remain' {
        $fake = Join-Path $TestDrive 'cohort-launcher-false-success.ps1'
        Set-Content -LiteralPath $fake -Encoding Ascii -Value @'
param([string]$ControlMode, [switch]$Apply, [uint32[]]$PendingGuid = @(), [switch]$AllowIncomplete)
if (-not $Apply) { '{"complete":false,"pending_guids":[31]}' ; return }
'{"complete":true,"pending_guids":[31]}'
'@
        { Invoke-CohortStartWithRetry -CohortStartPath $fake -Attempts 1 -DelaySeconds 0 } |
            Should -Throw '*incomplete after 1 attempts*'
    }
}

Describe 'full soak server launch contract' {
    It 'defaults to the upstream S83 WSL worldserver and authserver pair' {
        $plan = Get-FullSoakServerLaunchPlan -ServerRoot 'D:\Games\wowstuff\AutoWoW'

        $plan.auth_mode | Should -BeExactly 'wsl'
        $plan.start_windows_auth | Should -BeFalse
        $plan.arguments | Should -BeExactly (
            '-ServerRoot "D:\Games\wowstuff\AutoWoW" ' +
            '-WorldserverBinary "/root/autowow-upstream-s83-build/src/server/apps/worldserver" ' +
            '-AuthserverBinary "/root/autowow-upstream-s83-build/src/server/apps/authserver" ' +
            '-AuthserverConfig "/root/p1runtime/authserver-s83.conf"')
    }

    It 'preserves Windows-auth mode when both WSL auth paths are explicitly empty' {
        $plan = Get-FullSoakServerLaunchPlan -ServerRoot 'D:\Games\wowstuff\AutoWoW' `
            -AuthserverBinary '' -AuthserverConfig ''

        $plan.auth_mode | Should -BeExactly 'windows'
        $plan.start_windows_auth | Should -BeTrue
        $plan.arguments | Should -Match '-AuthserverBinary "" -AuthserverConfig ""$'
    }

    It 'rejects a partial WSL auth selection' {
        { Get-FullSoakServerLaunchPlan -ServerRoot 'D:\Games\wowstuff\AutoWoW' `
            -AuthserverBinary '/root/build/authserver' -AuthserverConfig '' } |
            Should -Throw '*must be supplied together*'
    }

    It 'keeps all server planning and startup inside the SkipWorld gate' {
        $skipWorldGate = $script:SoakStartTree.Find({
            param($node)
            $node -is [Management.Automation.Language.IfStatementAst] -and
                $node.Clauses[0].Item1.Extent.Text -match '\$SkipWorld'
        }, $true)
        $gateText = $skipWorldGate.Extent.Text

        $gateText | Should -Match 'Get-FullSoakServerLaunchPlan'
        $gateText | Should -Match 'start-phase1-wsl-worldserver\.ps1'
        $gateText | Should -Match '\$launchPlan\.start_windows_auth'
    }
}

Describe 'full soak scout dispatch' {
    BeforeEach {
        $global:ScoutDispatchCalls = [System.Collections.Generic.List[string]]::new()
        $global:ScoutGuids = @([uint32]101, [uint32]112, [uint32]121, [uint32]123, [uint32]236, [uint32]244)
        Mock Start-Sleep {}
    }

    AfterAll {
        Remove-Variable -Scope Global -Name ScoutDispatchCalls,ScoutGuids -ErrorAction SilentlyContinue
    }

    It 'uses bridge control only for the exact six scouts in WSL-auth mode' {
        $control = {
            param($action, $guid)
            $global:ScoutDispatchCalls.Add("control:$action/$guid")
            if ($action -eq 'list') {
                return [pscustomobject]@{ ok = $true; bots = @($global:ScoutGuids | ForEach-Object {
                    [pscustomobject]@{ guid = $_; group = [pscustomobject]@{ members = 0; leader_guid = 0 } }
                }) }
            }
            [pscustomobject]@{ ok = $true }
        }
        $legacy = { $global:ScoutDispatchCalls.Add('unexpected:revive'); throw 'revive must not run' }

        $result = Invoke-FullSoakScoutDispatch -AuthMode wsl -ServerRoot 'D:\root' `
            -WorldserverBinary '/root/s83/worldserver' -ControlPath 'control.ps1' -RevivePath 'revive.ps1' `
            -ControlInvoker $control -LegacyInvoker $legacy -OnlineAttempts 1 -ArmAttempts 1 -ControlDelaySeconds 0

        $result.complete | Should -BeTrue
        $global:ScoutDispatchCalls | Should -Not -Contain 'unexpected:revive'
        @($global:ScoutDispatchCalls | Where-Object { $_ -like 'control:independent/*' }) |
            Should -Be @($global:ScoutGuids | ForEach-Object { "control:independent/$_" })
        @($global:ScoutDispatchCalls | Where-Object { $_ -like 'control:activate/*' }).Count | Should -Be 0
    }

    It 'passes the selected root and world binary to the legacy revival helper' {
        $legacy = {
            param($path, $root, $binary, $guids)
            $global:ScoutDispatchCalls.Add("legacy:$path|$root|$binary|$($guids -join ',')")
        }
        $control = { $global:ScoutDispatchCalls.Add('unexpected:control'); throw 'control must not run' }

        $result = Invoke-FullSoakScoutDispatch -AuthMode windows -ServerRoot 'D:\actual-root' `
            -WorldserverBinary '/root/actual/worldserver' -ControlPath 'control.ps1' -RevivePath 'revive.ps1' `
            -ControlInvoker $control -LegacyInvoker $legacy -LegacyAttempts 1 -LegacyDelaySeconds 0

        $result.complete | Should -BeTrue
        $global:ScoutDispatchCalls | Should -Contain `
            'legacy:revive.ps1|D:\actual-root|/root/actual/worldserver|101,112,121,123,236,244'
        $global:ScoutDispatchCalls | Should -Not -Contain 'unexpected:control'
    }

    It 'arms already-online scouts but reports every GUID still offline after bounded retries' {
        $online = @([uint32]101, [uint32]121, [uint32]123, [uint32]236, [uint32]244)
        $control = {
            param($action, $guid)
            $global:ScoutDispatchCalls.Add("$action/$guid")
            if ($action -eq 'list') {
                return [pscustomobject]@{ ok = $true; bots = @($online | ForEach-Object {
                    [pscustomobject]@{ guid = $_; group = [pscustomobject]@{ members = 0; leader_guid = 0 } }
                }) }
            }
            if ($action -eq 'activate') { return [pscustomobject]@{ ok = $false; error = 'login_already_queued' } }
            [pscustomobject]@{ ok = $true }
        }.GetNewClosure()

        { Invoke-FullSoakWslScoutStart -ControlPath 'control.ps1' -ControlInvoker $control `
            -OnlineAttempts 2 -ArmAttempts 1 -DelaySeconds 0 } |
            Should -Throw '*not_online=112*'
        $global:ScoutDispatchCalls | Should -Contain 'independent/101'
        $global:ScoutDispatchCalls | Should -Contain 'activate/112'
    }

    It 'accepts independent-requires-solo only when list evidence shows the scout is grouped' {
        $groupedControl = {
            param($action, $guid)
            if ($action -eq 'list') {
                return [pscustomobject]@{ ok = $true; bots = @($global:ScoutGuids | ForEach-Object {
                    [pscustomobject]@{ guid = $_; group = [pscustomobject]@{ members = 2; leader_guid = 101 } }
                }) }
            }
            [pscustomobject]@{ ok = $false; error = 'independent_requires_solo' }
        }
        $grouped = Invoke-FullSoakWslScoutStart -ControlPath 'control.ps1' -ControlInvoker $groupedControl `
            -OnlineAttempts 1 -ArmAttempts 1 -DelaySeconds 0
        @($grouped.grouped_guids) | Should -Be $global:ScoutGuids

        $soloControl = {
            param($action, $guid)
            if ($action -eq 'list') {
                return [pscustomobject]@{ ok = $true; bots = @($global:ScoutGuids | ForEach-Object {
                    [pscustomobject]@{ guid = $_; group = [pscustomobject]@{ members = 0; leader_guid = 0 } }
                }) }
            }
            [pscustomobject]@{ ok = $false; error = 'independent_requires_solo' }
        }
        { Invoke-FullSoakWslScoutStart -ControlPath 'control.ps1' -ControlInvoker $soloControl `
            -OnlineAttempts 1 -ArmAttempts 1 -DelaySeconds 0 } |
            Should -Throw '*arm_failed=*independent_requires_solo*'
    }
}
