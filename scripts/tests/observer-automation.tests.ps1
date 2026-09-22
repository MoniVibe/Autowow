BeforeAll {
    $script:Scripts = Split-Path -Parent $PSScriptRoot
    $script:Root = Split-Path -Parent $script:Scripts
    $script:Lib = Join-Path $script:Scripts 'observer-automation-lib.ps1'
    . $script:Lib
}

Describe 'observer password and SRP6 contract (offline)' {
    It 'generates a <=16 character WoW-safe password and persists it only as current-user DPAPI CLIXML' {
        $temp = Join-Path $TestDrive 'observer-password.clixml'
        $secure = New-ObserverPassword
        Save-ObserverPassword -Password $secure -Path $temp
        $imported = Import-ObserverPassword -Path $temp
        $plain = Use-ObserverPlainTextPassword -Password $imported -Action { param($value) $value }
        try {
            $plain.Length | Should -Be 16
            $plain | Should -Match '^[A-Za-z0-9]{12,16}$'
            $plain | Should -Match '[A-Z]'
            $plain | Should -Match '[a-z]'
            $plain | Should -Match '[0-9]'
            (Get-Content -LiteralPath $temp -Raw) | Should -Not -Match ([regex]::Escape($plain))
        }
        finally { $plain = $null }
    }

    It 'matches the AzerothCore little-endian SRP6 registration vector' {
        $salt = ConvertFrom-ObserverHex '000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F'
        $password = ConvertTo-SecureString 'testpass123' -AsPlainText -Force
        $registration = New-ObserverSrp6RegistrationData -Username autowatch -Password $password -Salt $salt
        (ConvertTo-ObserverHex $registration.salt) | Should -BeExactly '000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F'
        (ConvertTo-ObserverHex $registration.verifier) | Should -BeExactly 'FE8C48E9307B9BBEE48D1B5F6514C5CD952C9421804520FD79B1CCF718C9DD0F'
    }

    It 'redacts a secret without changing unrelated text' {
        Protect-ObserverText -Text 'before-Sensitive123-after' -Secret 'Sensitive123' | Should -BeExactly 'before-<redacted>-after'
    }

    It 'compares byte arrays without the unsupported ReadOnlySpan binding' {
        Test-ObserverByteArrayFixedTimeEqual -Left ([byte[]](1,2,3)) -Right ([byte[]](1,2,3)) | Should -BeTrue
        Test-ObserverByteArrayFixedTimeEqual -Left ([byte[]](1,2,3)) -Right ([byte[]](1,2,4)) | Should -BeFalse
        Test-ObserverByteArrayFixedTimeEqual -Left ([byte[]](1,2,3)) -Right ([byte[]](1,2,3,0)) | Should -BeFalse
    }
}

Describe 'worldserver config credential parsing (offline)' {
    It 'parses exactly one loopback five-field connection without reporting the password' {
        $config = Join-Path $TestDrive 'worldserver.conf'
        [IO.File]::WriteAllText($config, 'LoginDatabaseInfo = "127.0.0.1;3306;acore;service-secret;acore_auth"')
        $connection = Get-ObserverDatabaseConnectionFromConfig -ConfigPath $config -Key LoginDatabaseInfo
        $connection.host | Should -BeExactly '127.0.0.1'
        $connection.port | Should -Be 3306
        $connection.username | Should -BeExactly 'acore'
        $connection.database | Should -BeExactly 'acore_auth'
    }

    It 'refuses duplicate, malformed, and non-loopback entries' {
        $duplicate = Join-Path $TestDrive 'duplicate.conf'
        [IO.File]::WriteAllText($duplicate, "LoginDatabaseInfo = `"127.0.0.1;3306;acore;x;acore_auth`"`nLoginDatabaseInfo = `"127.0.0.1;3306;acore;x;acore_auth`"")
        { Get-ObserverDatabaseConnectionFromConfig -ConfigPath $duplicate -Key LoginDatabaseInfo } | Should -Throw '*exactly one*'
        $remote = Join-Path $TestDrive 'remote.conf'
        [IO.File]::WriteAllText($remote, 'LoginDatabaseInfo = "10.0.0.4;3306;acore;x;acore_auth"')
        { Get-ObserverDatabaseConnectionFromConfig -ConfigPath $remote -Key LoginDatabaseInfo } | Should -Throw '*loopback*'
    }
}

Describe 'observer provisioning idempotency and collision guards (offline)' {
    BeforeAll {
        $script:ExactAccount = [pscustomobject]@{ id=8; username='AUTOWATCH' }
        $script:ExactCharacter = [pscustomobject]@{ guid=31; account=8; name='Autowatch'; race=1; class=1; online=0 }
        $script:ExactAccess = [pscustomobject]@{ id=8; gmlevel=3; RealmID=-1 }
        $script:ExactRealm = [pscustomobject]@{ realmid=1; acctid=8; numchars=1 }
    }

    It 'plans exact create actions from an empty state' {
        $decision = Resolve-ObserverProvisioningState -NextAccountId 8 -RealmIds 1 -ObserverRealmId 1
        $decision.safe | Should -BeTrue
        $decision.actions | Should -Contain 'create_dedicated_account'
        $decision.actions | Should -Contain 'create_human_warrior_Autowatch'
        $decision.actions | Should -Contain 'set_exact_all_realms_GM3'
    }

    It 'is complete when rerun against the exact dedicated state' {
        $decision = Resolve-ObserverProvisioningState -Accounts $script:ExactAccount -Characters $script:ExactCharacter `
            -AccessRows $script:ExactAccess -RealmRows $script:ExactRealm -RealmIds 1 -ObserverRealmId 1 -NextAccountId 9
        $decision.safe | Should -BeTrue
        $decision.complete | Should -BeTrue
        @($decision.actions).Count | Should -Be 0
    }

    It 'refuses account 3, GUID 21, wrong ownership, extra characters, and group membership' {
        (Resolve-ObserverProvisioningState -Accounts ([pscustomobject]@{id=3;username='AUTOWATCH'}) -NextAccountId 9).safe | Should -BeFalse
        (Resolve-ObserverProvisioningState -Accounts $script:ExactAccount -Characters ([pscustomobject]@{guid=21;account=8;name='Autowatch';race=1;class=1;online=0}) -NextAccountId 9).safe | Should -BeFalse
        (Resolve-ObserverProvisioningState -Accounts $script:ExactAccount -Characters ([pscustomobject]@{guid=31;account=9;name='Autowatch';race=1;class=1;online=0}) -NextAccountId 9).safe | Should -BeFalse
        $extra = @($script:ExactCharacter,[pscustomobject]@{guid=32;account=8;name='Otherchar';race=1;class=1;online=0})
        (Resolve-ObserverProvisioningState -Accounts $script:ExactAccount -Characters $extra -NextAccountId 9).safe | Should -BeFalse
        (Resolve-ObserverProvisioningState -Accounts $script:ExactAccount -Characters $script:ExactCharacter -GroupRows ([pscustomobject]@{memberGuid=31}) -NextAccountId 9).safe | Should -BeFalse
    }

    It 'constructs one local transaction for SRP6 account, realmcharacters, GM3, and exact human character' {
        $sql = New-ObserverProvisioningSql -Salt ([byte[]](0..31)) -Verifier ([byte[]](32..63)) -RealmId 1
        $sql | Should -Match "GET_LOCK\('autowow\.observer\.provision'"
        $sql | Should -Match 'START TRANSACTION'
        $sql | Should -Match 'acore_auth\.account \(username, salt, verifier'
        $sql | Should -Match 'acore_auth\.realmcharacters'
        $sql | Should -Match 'acore_auth\.account_access'
        $sql | Should -Match "'Autowatch', 1, 1, 0, 1"
        $sql | Should -Match '@observer_account_id <> 3'
        $sql | Should -Match '@observer_next_guid <> 21'
        $sql | Should -Match 'guid=@observer_next_guid'
        $sql | Should -Not -Match 'guid IN \(@observer_next_guid,21\)'
        $sql | Should -Match "salt=UNHEX\('[0-9A-F]{64}'\) AND verifier=UNHEX\('[0-9A-F]{64}'\)"
        $sql | Should -Match '@observer_lock=1'
        $sql | Should -Not -Match 'TESTPASS123|WOW_ACCOUNT_PASSWORD|MYSQL_ROOT_PASSWORD'
    }
}

Describe 'exact observer process and window guards (offline)' {
    BeforeAll {
        $script:ExpectedProcess = [pscustomobject]@{process_id=44;start_time_utc='2026-07-17T10:00:00.0000000Z';executable_path='D:\observer\Wow.exe';required_command_line_tokens=@('token-abc','observer-relocate-host.ps1')}
        $script:ActualProcess = [pscustomobject]@{process_id=44;start_time_utc='2026-07-17T10:00:00.0000000Z';executable_path='D:\observer\Wow.exe';command_line='pwsh observer-relocate-host.ps1 token-abc'}
    }

    It 'accepts only an exact process identity' {
        Test-ObserverProcessIdentity -Expected $script:ExpectedProcess -Actual $script:ActualProcess | Should -BeTrue
        Test-ObserverProcessIdentity -Expected $script:ExpectedProcess -Actual ($script:ActualProcess.PSObject.Copy() | ForEach-Object {$_.process_id=45;$_}) | Should -BeFalse
        Test-ObserverProcessIdentity -Expected $script:ExpectedProcess -Actual ($script:ActualProcess.PSObject.Copy() | ForEach-Object {$_.start_time_utc='later';$_}) | Should -BeFalse
        Test-ObserverProcessIdentity -Expected $script:ExpectedProcess -Actual ($script:ActualProcess.PSObject.Copy() | ForEach-Object {$_.executable_path='D:\user\Wow.exe';$_}) | Should -BeFalse
        Test-ObserverProcessIdentity -Expected $script:ExpectedProcess -Actual ($script:ActualProcess.PSObject.Copy() | ForEach-Object {$_.command_line='unrelated';$_}) | Should -BeFalse
    }

    It 'requires exact foreground identity before SendKeys and exact non-foreground identity for PrintWindow' {
        $args=@{ExpectedProcessId=44;ExpectedWindowHandle=900;ExpectedExecutablePath='D:\observer\Wow.exe';AllowedTitles=@('World of Warcraft');ActualProcessId=44;ActualWindowHandle=900;ForegroundWindowHandle=900;ActualExecutablePath='D:\observer\Wow.exe';ActualTitle='World of Warcraft'}
        Test-ObserverWindowIdentity @args | Should -BeTrue
        $args.ForegroundWindowHandle=901
        Test-ObserverWindowIdentity @args | Should -BeFalse
        $capture=@{ExpectedProcessId=44;ExpectedWindowHandle=900;ExpectedExecutablePath='D:\observer\Wow.exe';AllowedTitles=@('World of Warcraft');ActualProcessId=44;ActualWindowHandle=900;ActualExecutablePath='D:\observer\Wow.exe';ActualTitle='World of Warcraft'}
        Test-ObserverCaptureWindowIdentity @capture | Should -BeTrue
        $capture.ActualTitle='Other Window'
        Test-ObserverCaptureWindowIdentity @capture | Should -BeFalse
    }
}

Describe 'observer automation static safety contracts' {
    It 'has no root credential, create-account helper, or server lifecycle dependency in provisioning' {
        $source = Get-Content -LiteralPath (Join-Path $script:Scripts 'provision-observer.ps1') -Raw
        $source | Should -Not -Match 'MYSQL_ROOT_PASSWORD|create-account\.ps1|stop-server|start-server|worldserver\.exe'
        $source | Should -Match 'Get-ObserverDatabaseConnectionFromConfig'
        $source | Should -Match 'MYSQL_PWD'
    }

    It 'starts relocation only after online identity and protection/no-group checks' {
        $source = Get-Content -LiteralPath (Join-Path $script:Scripts 'start-observer-automation.ps1') -Raw
        $protectIndex = $source.IndexOf('Invoke-ObserverControlJson -Action protect')
        $guardIndex = $source.IndexOf("[bool]`$status.in_group")
        $startIndex = $source.IndexOf("`$relocate = Start-Process")
        $protectIndex | Should -BeGreaterThan -1
        $guardIndex | Should -BeGreaterThan $protectIndex
        $startIndex | Should -BeGreaterThan $guardIndex
    }

    It 'guards screenshot fallback and never requests a whole-desktop capture' {
        $source = Get-Content -LiteralPath (Join-Path $script:Scripts 'capture-observer-screenshot.ps1') -Raw
        $source | Should -Match 'PrintWindow'
        $source | Should -Match 'Test-ObserverWindowIdentity'
        $source.IndexOf('Test-ObserverWindowIdentity') | Should -BeLessThan $source.IndexOf('$graphics.CopyFromScreen')
        $source | Should -Not -Match 'PrimaryScreen|VirtualScreen|CopyFromScreen\(0\s*,\s*0'
        $source | Should -Match 'IsIconic'
    }

    It 'does not write passwords to Config.wtf or user-visible output' {
        $prepare = Get-Content -LiteralPath (Join-Path $script:Scripts 'prepare-observer-client.ps1') -Raw
        $prepare | Should -Not -Match '(?im)^SET password'
        $all = @('observer-automation-lib.ps1','provision-observer.ps1','start-observer-automation.ps1') | ForEach-Object { Get-Content -LiteralPath (Join-Path $script:Scripts $_) -Raw }
        ($all -join "`n") | Should -Not -Match '(?i)Write-(Output|Host|Verbose|Information)[^\r\n]*\$.*password'
    }
}
