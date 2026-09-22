Set-StrictMode -Version Latest

$script:ObserverAccountName = 'AUTOWATCH'
$script:ObserverCharacterName = 'Autowatch'
$script:ObserverForbiddenAccountId = 3
$script:ObserverForbiddenCharacterGuid = 21
$script:ObserverRace = 1
$script:ObserverClass = 1
$script:ObserverGender = 0

function Get-ObserverIdentityContract {
    [CmdletBinding()]
    param()

    [pscustomobject][ordered]@{
        account_name = $script:ObserverAccountName
        character_name = $script:ObserverCharacterName
        forbidden_account_id = $script:ObserverForbiddenAccountId
        forbidden_character_guid = $script:ObserverForbiddenCharacterGuid
        race = $script:ObserverRace
        class = $script:ObserverClass
        gender = $script:ObserverGender
        faction = 'Alliance'
        gmlevel = 3
        realm_id = -1
        maximum_characters_on_account = 1
    }
}

function Get-ObserverRandomIndex {
    [CmdletBinding()]
    param([Parameter(Mandatory)][ValidateRange(1, 4096)][int]$UpperExclusive)

    $limit = 256 - (256 % $UpperExclusive)
    $buffer = [byte[]]::new(1)
    do {
        [System.Security.Cryptography.RandomNumberGenerator]::Fill($buffer)
    } while ($buffer[0] -ge $limit)
    return ([int]$buffer[0] % $UpperExclusive)
}

function New-ObserverPassword {
    [CmdletBinding()]
    param([ValidateRange(12, 16)][int]$Length = 16)

    # Alphanumeric only: accepted by the 3.3.5 client and safe for WScript.SendKeys.
    $upper = 'ABCDEFGHJKLMNPQRSTUVWXYZ'
    $lower = 'abcdefghijkmnopqrstuvwxyz'
    $digits = '23456789'
    $all = $upper + $lower + $digits
    $chars = [char[]]::new($Length)
    $chars[0] = $upper[(Get-ObserverRandomIndex -UpperExclusive $upper.Length)]
    $chars[1] = $lower[(Get-ObserverRandomIndex -UpperExclusive $lower.Length)]
    $chars[2] = $digits[(Get-ObserverRandomIndex -UpperExclusive $digits.Length)]
    for ($i = 3; $i -lt $Length; $i++) {
        $chars[$i] = $all[(Get-ObserverRandomIndex -UpperExclusive $all.Length)]
    }
    for ($i = $Length - 1; $i -gt 0; $i--) {
        $j = Get-ObserverRandomIndex -UpperExclusive ($i + 1)
        $tmp = $chars[$i]
        $chars[$i] = $chars[$j]
        $chars[$j] = $tmp
    }

    $secure = [System.Security.SecureString]::new()
    foreach ($char in $chars) { $secure.AppendChar($char) }
    $secure.MakeReadOnly()
    [Array]::Clear($chars, 0, $chars.Length)
    return $secure
}

function Save-ObserverPassword {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][Security.SecureString]$Password,
        [Parameter(Mandatory)][string]$Path
    )

    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $Password | Export-Clixml -LiteralPath $Path -Force
}

function Import-ObserverPassword {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Observer DPAPI password file is missing: $Path"
    }
    $secure = Import-Clixml -LiteralPath $Path
    if ($secure -isnot [Security.SecureString]) { throw "Observer password file is not a SecureString CLIXML document: $Path" }
    return $secure
}

function Use-ObserverPlainTextPassword {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][Security.SecureString]$Password,
        [Parameter(Mandatory)][scriptblock]$Action
    )

    $bstr = [IntPtr]::Zero
    try {
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
        $plainText = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
        & $Action $plainText
    }
    finally {
        $plainText = $null
        if ($bstr -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    }
}

function Protect-ObserverText {
    [CmdletBinding()]
    param(
        [AllowNull()][string]$Text,
        [AllowNull()][string]$Secret
    )

    if ($null -eq $Text) { return $null }
    if ([string]::IsNullOrEmpty($Secret)) { return $Text }
    return $Text.Replace($Secret, '<redacted>')
}

function ConvertFrom-ObserverHex {
    [CmdletBinding()]
    param([Parameter(Mandatory)][ValidatePattern('^(?:[0-9A-Fa-f]{2})+$')][string]$Hex)

    $bytes = [byte[]]::new($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16) }
    return $bytes
}

function Test-ObserverByteArrayFixedTimeEqual {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][byte[]]$Left,
        [Parameter(Mandatory)][byte[]]$Right
    )

    # CryptographicOperations.FixedTimeEquals takes ReadOnlySpan<byte>, which PowerShell cannot
    # bind directly. Keep the comparison length-independent after recording the public length
    # mismatch, and fold every byte difference into one accumulator.
    $difference = $Left.Length -bxor $Right.Length
    $maximumLength = [Math]::Max($Left.Length, $Right.Length)
    for ($index = 0; $index -lt $maximumLength; ++$index) {
        $leftByte = if ($index -lt $Left.Length) { [int]$Left[$index] } else { 0 }
        $rightByte = if ($index -lt $Right.Length) { [int]$Right[$index] } else { 0 }
        $difference = $difference -bor ($leftByte -bxor $rightByte)
    }
    return $difference -eq 0
}

function ConvertTo-ObserverHex {
    [CmdletBinding()]
    param([Parameter(Mandatory)][byte[]]$Bytes)
    return [Convert]::ToHexString($Bytes)
}

function New-ObserverSrp6RegistrationData {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_]{3,16}$')][string]$Username,
        [Parameter(Mandatory)][Security.SecureString]$Password,
        [ValidateCount(32, 32)][byte[]]$Salt
    )

    if ($null -eq $Salt) {
        $Salt = [byte[]]::new(32)
        [Security.Cryptography.RandomNumberGenerator]::Fill($Salt)
    }
    else {
        $Salt = [byte[]]$Salt.Clone()
    }

    $passwordBytes = $null
    $identityBytes = $null
    $inner = $null
    $outer = $null
    $sha1 = [Security.Cryptography.SHA1]::Create()
    try {
        $passwordBytes = [byte[]](Use-ObserverPlainTextPassword -Password $Password -Action {
            param($plainText)
            $upperPassword = $plainText.ToUpperInvariant()
            if ($upperPassword.Length -gt 16 -or $upperPassword -notmatch '^[A-Z0-9]+$') {
                throw 'Observer password is outside the uppercase AzerothCore/WoW-compatible contract.'
            }
            [Text.Encoding]::UTF8.GetBytes($upperPassword)
        })
        $usernameBytes = [Text.Encoding]::UTF8.GetBytes($Username.ToUpperInvariant())
        $identityBytes = [byte[]]::new($usernameBytes.Length + 1 + $passwordBytes.Length)
        [Array]::Copy($usernameBytes, 0, $identityBytes, 0, $usernameBytes.Length)
        $identityBytes[$usernameBytes.Length] = [byte][char]':'
        [Array]::Copy($passwordBytes, 0, $identityBytes, $usernameBytes.Length + 1, $passwordBytes.Length)
        $inner = $sha1.ComputeHash($identityBytes)
        $outerInput = [byte[]]::new(32 + $inner.Length)
        [Array]::Copy($Salt, 0, $outerInput, 0, 32)
        [Array]::Copy($inner, 0, $outerInput, 32, $inner.Length)
        $outer = $sha1.ComputeHash($outerInput)

        # AzerothCore BigNumber consumes the SHA1 digest as little-endian, and ToByteArray<32>()
        # serializes the modular result as exactly 32 little-endian bytes.
        $xBytes = [byte[]]::new($outer.Length + 1)
        [Array]::Copy($outer, $xBytes, $outer.Length)
        $x = [Numerics.BigInteger]::new($xBytes)
        $nBigEndian = ConvertFrom-ObserverHex -Hex '894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7'
        [Array]::Reverse($nBigEndian)
        $nBytes = [byte[]]::new(33)
        [Array]::Copy($nBigEndian, $nBytes, 32)
        $n = [Numerics.BigInteger]::new($nBytes)
        $verifierNumber = [Numerics.BigInteger]::ModPow([Numerics.BigInteger]::new(7), $x, $n)
        $serialized = $verifierNumber.ToByteArray()
        $verifier = [byte[]]::new(32)
        [Array]::Copy($serialized, 0, $verifier, 0, [Math]::Min(32, $serialized.Length))
        return [pscustomobject]@{ salt = $Salt; verifier = $verifier }
    }
    finally {
        $sha1.Dispose()
        foreach ($buffer in @($passwordBytes, $identityBytes, $inner, $outer)) {
            if ($null -ne $buffer) { [Array]::Clear($buffer, 0, $buffer.Length) }
        }
    }
}

function Get-ObserverDatabaseConnectionFromConfig {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigPath,
        [Parameter(Mandatory)][ValidateSet('LoginDatabaseInfo','CharacterDatabaseInfo')][string]$Key
    )

    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { throw "Worldserver config is missing: $ConfigPath" }
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*=\s*"([^"]+)"\s*(?:#.*)?$'
    $matches = @([IO.File]::ReadAllLines($ConfigPath) | ForEach-Object { if ($_ -match $pattern) { $Matches[1] } })
    if ($matches.Count -ne 1) { throw "Expected exactly one active $Key entry in $ConfigPath; found $($matches.Count)." }
    $parts = $matches[0].Split(';')
    if ($parts.Count -ne 5 -or @($parts | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) {
        throw "$Key must contain exactly five non-empty semicolon-delimited fields."
    }
    if ($parts[0] -notin @('127.0.0.1','localhost','::1')) { throw "$Key is not loopback-local." }
    [int]$port = 0
    if (-not [int]::TryParse($parts[1], [ref]$port) -or $port -lt 1 -or $port -gt 65535) { throw "$Key has an invalid port." }
    if ($parts[2] -notmatch '^[A-Za-z0-9_]+$' -or $parts[4] -notmatch '^[A-Za-z0-9_]+$') { throw "$Key has an unsafe username or database identifier." }
    [pscustomobject]@{ host = $parts[0]; port = $port; username = $parts[2]; password = $parts[3]; database = $parts[4] }
}

function New-ObserverProvisioningSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateCount(32, 32)][byte[]]$Salt,
        [Parameter(Mandatory)][ValidateCount(32, 32)][byte[]]$Verifier,
        [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$RealmId
    )

    $saltHex = ConvertTo-ObserverHex -Bytes $Salt
    $verifierHex = ConvertTo-ObserverHex -Bytes $Verifier
    @"
SELECT GET_LOCK('autowow.observer.provision', 30) INTO @observer_lock;
START TRANSACTION;
SET @observer_next_account_id = (SELECT COALESCE(AUTO_INCREMENT, 0) FROM information_schema.TABLES WHERE TABLE_SCHEMA='acore_auth' AND TABLE_NAME='account');
INSERT INTO acore_auth.account (username, salt, verifier, expansion, reg_mail, email, joindate, last_ip, last_attempt_ip)
SELECT 'AUTOWATCH', UNHEX('$saltHex'), UNHEX('$verifierHex'), 2, '', '', NOW(), '127.0.0.1', '127.0.0.1'
WHERE @observer_lock = 1
  AND @observer_next_account_id <> 3
  AND NOT EXISTS (SELECT 1 FROM acore_auth.account WHERE UPPER(username) IN ('AUTOWATCH','SHONHAY') AND UPPER(username)='AUTOWATCH');
SET @observer_account_id = (SELECT id FROM acore_auth.account
    WHERE UPPER(username)='AUTOWATCH' AND id<>3 AND salt=UNHEX('$saltHex') AND verifier=UNHEX('$verifierHex') LIMIT 1);
SET @observer_next_guid = (SELECT CASE WHEN COALESCE(MAX(guid),0)+1=21 THEN 22 ELSE COALESCE(MAX(guid),0)+1 END FROM acore_characters.characters);
INSERT INTO acore_characters.characters
    (guid, account, name, race, class, gender, level, map, position_x, position_y, position_z, orientation, taximask, cinematic, health, innTriggerId, at_login)
SELECT @observer_next_guid, @observer_account_id, 'Autowatch', 1, 1, 0, 1, 0, -8949.95, -132.493, 83.5312, 0, '', 1, 1, 0, 0
WHERE @observer_lock = 1
  AND @observer_account_id IS NOT NULL AND @observer_account_id <> 3 AND @observer_next_guid <> 21
  AND NOT EXISTS (SELECT 1 FROM acore_characters.characters WHERE guid=@observer_next_guid)
  AND NOT EXISTS (SELECT 1 FROM acore_characters.characters WHERE LOWER(name)=LOWER('Autowatch'))
  AND NOT EXISTS (SELECT 1 FROM acore_characters.characters WHERE account=@observer_account_id);
SET @observer_character_inserted = ROW_COUNT();
SET @observer_character_guid = (SELECT guid FROM acore_characters.characters WHERE LOWER(name)=LOWER('Autowatch') AND account=@observer_account_id LIMIT 1);
INSERT INTO acore_characters.character_homebind (guid,mapId,zoneId,posX,posY,posZ)
SELECT @observer_character_guid,0,12,-8949.95,-132.493,83.5312
WHERE @observer_character_guid IS NOT NULL
  AND @observer_lock = 1
  AND NOT EXISTS (SELECT 1 FROM acore_characters.character_homebind WHERE guid=@observer_character_guid);
DELETE FROM acore_auth.account_access WHERE id=@observer_account_id AND @observer_account_id<>3 AND @observer_lock=1;
INSERT INTO acore_auth.account_access (id,gmlevel,RealmID,comment)
SELECT @observer_account_id,3,-1,'dedicated local AutoWoW observer'
WHERE @observer_account_id IS NOT NULL AND @observer_account_id<>3 AND @observer_lock=1;
INSERT INTO acore_auth.realmcharacters (realmid,acctid,numchars)
SELECT id,@observer_account_id,CASE WHEN id=$RealmId THEN 1 ELSE 0 END FROM acore_auth.realmlist
WHERE @observer_account_id IS NOT NULL AND @observer_account_id<>3 AND @observer_lock=1
ON DUPLICATE KEY UPDATE numchars=VALUES(numchars);
SELECT CONCAT('PROVISION_RESULT',CHAR(9),@observer_lock,CHAR(9),COALESCE(@observer_account_id,0),CHAR(9),COALESCE(@observer_character_guid,0),CHAR(9),@observer_character_inserted);
COMMIT;
DO RELEASE_LOCK('autowow.observer.provision');
"@
}

function Resolve-ObserverProvisioningState {
    [CmdletBinding()]
    param(
        [object[]]$Accounts = @(),
        [object[]]$Characters = @(),
        [object[]]$AccessRows = @(),
        [object[]]$GroupRows = @(),
        [object[]]$RealmRows = @(),
        [uint32[]]$RealmIds = @(),
        [uint32]$ObserverRealmId = 1,
        [uint64]$NextAccountId = 0
    )

    $errors = [Collections.Generic.List[string]]::new()
    $actions = [Collections.Generic.List[string]]::new()
    $desiredAccounts = @($Accounts | Where-Object { [string]$_.username -ieq $script:ObserverAccountName })
    $desiredCharacters = @($Characters | Where-Object { [string]$_.name -ieq $script:ObserverCharacterName })

    if ($desiredAccounts.Count -gt 1) { $errors.Add('multiple_AUTOWATCH_accounts') }
    if ($desiredCharacters.Count -gt 1) { $errors.Add('multiple_Autowatch_characters') }

    $account = $desiredAccounts | Select-Object -First 1
    $character = $desiredCharacters | Select-Object -First 1

    if (-not $account) {
        if ($character) { $errors.Add('Autowatch_character_exists_without_AUTOWATCH_account') }
        if ($NextAccountId -eq $script:ObserverForbiddenAccountId) { $errors.Add('next_account_id_is_forbidden_3') }
        $actions.Add('create_dedicated_account')
    }
    else {
        if ([uint64]$account.id -eq $script:ObserverForbiddenAccountId) { $errors.Add('AUTOWATCH_resolves_to_forbidden_account_3') }
        $accountCharacters = @($Characters | Where-Object { [uint64]$_.account -eq [uint64]$account.id })
        $unexpected = @($accountCharacters | Where-Object { [string]$_.name -ine $script:ObserverCharacterName })
        if ($unexpected.Count -gt 0) { $errors.Add('AUTOWATCH_account_is_not_dedicated') }
    }

    if (-not $character) {
        $actions.Add('create_human_warrior_Autowatch')
    }
    elseif ($account) {
        if ([uint64]$character.guid -eq $script:ObserverForbiddenCharacterGuid) { $errors.Add('Autowatch_resolves_to_forbidden_guid_21') }
        if ([uint64]$character.account -ne [uint64]$account.id) { $errors.Add('Autowatch_owned_by_wrong_account') }
        if ([int]$character.race -ne $script:ObserverRace) { $errors.Add('Autowatch_is_not_human') }
        if ([int]$character.class -ne $script:ObserverClass) { $errors.Add('Autowatch_is_not_expected_warrior') }
        if ($character.PSObject.Properties['online'] -and [int]$character.online -ne 0) { $errors.Add('Autowatch_is_online_during_provisioning') }
        if (@($GroupRows | Where-Object { [uint64]$_.memberGuid -eq [uint64]$character.guid }).Count -gt 0) { $errors.Add('Autowatch_is_in_a_group') }
    }

    if ($account) {
        $access = @($AccessRows | Where-Object { [uint64]$_.id -eq [uint64]$account.id })
        $exactAccess = @($access | Where-Object { [int]$_.RealmID -eq -1 -and [int]$_.gmlevel -eq 3 })
        if ($exactAccess.Count -ne 1 -or $access.Count -ne 1) { $actions.Add('set_exact_all_realms_GM3') }
        if ($RealmIds.Count -gt 0) {
            $accountRealmRows = @($RealmRows | Where-Object { [uint64]$_.acctid -eq [uint64]$account.id })
            foreach ($realmId in $RealmIds) {
                $expectedCount = if ($realmId -eq $ObserverRealmId) { 1 } else { 0 }
                $matching = @($accountRealmRows | Where-Object { [uint32]$_.realmid -eq $realmId -and [int]$_.numchars -eq $expectedCount })
                if ($matching.Count -ne 1) { $actions.Add('synchronize_realmcharacters'); break }
            }
        }
    }
    else {
        $actions.Add('set_exact_all_realms_GM3')
    }

    [pscustomobject][ordered]@{
        safe = ($errors.Count -eq 0)
        complete = ($errors.Count -eq 0 -and $actions.Count -eq 0)
        errors = @($errors)
        actions = @($actions)
        account_id = if ($account) { [uint64]$account.id } else { $null }
        character_guid = if ($character) { [uint64]$character.guid } else { $null }
    }
}

function Test-ObserverWindowIdentity {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][uint32]$ExpectedProcessId,
        [Parameter(Mandatory)][uint64]$ExpectedWindowHandle,
        [Parameter(Mandatory)][string]$ExpectedExecutablePath,
        [Parameter(Mandatory)][string[]]$AllowedTitles,
        [Parameter(Mandatory)][uint32]$ActualProcessId,
        [Parameter(Mandatory)][uint64]$ActualWindowHandle,
        [Parameter(Mandatory)][uint64]$ForegroundWindowHandle,
        [Parameter(Mandatory)][string]$ActualExecutablePath,
        [Parameter(Mandatory)][string]$ActualTitle
    )

    if ($ExpectedProcessId -ne $ActualProcessId) { return $false }
    if ($ExpectedWindowHandle -eq 0 -or $ExpectedWindowHandle -ne $ActualWindowHandle) { return $false }
    if ($ForegroundWindowHandle -ne $ExpectedWindowHandle) { return $false }
    if (-not [string]::Equals([IO.Path]::GetFullPath($ExpectedExecutablePath), [IO.Path]::GetFullPath($ActualExecutablePath), [StringComparison]::OrdinalIgnoreCase)) { return $false }
    if (-not ($AllowedTitles -ccontains $ActualTitle)) { return $false }
    return $true
}

function Test-ObserverCaptureWindowIdentity {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][uint32]$ExpectedProcessId,
        [Parameter(Mandatory)][uint64]$ExpectedWindowHandle,
        [Parameter(Mandatory)][string]$ExpectedExecutablePath,
        [Parameter(Mandatory)][string[]]$AllowedTitles,
        [Parameter(Mandatory)][uint32]$ActualProcessId,
        [Parameter(Mandatory)][uint64]$ActualWindowHandle,
        [Parameter(Mandatory)][string]$ActualExecutablePath,
        [Parameter(Mandatory)][string]$ActualTitle
    )

    if ($ExpectedProcessId -ne $ActualProcessId) { return $false }
    if ($ExpectedWindowHandle -eq 0 -or $ExpectedWindowHandle -ne $ActualWindowHandle) { return $false }
    if (-not [string]::Equals([IO.Path]::GetFullPath($ExpectedExecutablePath), [IO.Path]::GetFullPath($ActualExecutablePath), [StringComparison]::OrdinalIgnoreCase)) { return $false }
    if (-not ($AllowedTitles -ccontains $ActualTitle)) { return $false }
    return $true
}

function Test-ObserverProcessIdentity {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Expected,
        [Parameter(Mandatory)][psobject]$Actual
    )

    if ([uint32]$Expected.process_id -ne [uint32]$Actual.process_id) { return $false }
    # ConvertFrom-Json materializes ISO timestamps as DateTime objects, whose string cast is
    # culture-dependent. Compare the same instant instead of their rendered strings.
    try {
        $expectedStart = ([DateTimeOffset]$Expected.start_time_utc).ToUniversalTime()
        $actualStart = ([DateTimeOffset]$Actual.start_time_utc).ToUniversalTime()
    }
    catch { return $false }
    if ($expectedStart.UtcTicks -ne $actualStart.UtcTicks) { return $false }
    if (-not [string]::Equals([IO.Path]::GetFullPath([string]$Expected.executable_path), [IO.Path]::GetFullPath([string]$Actual.executable_path), [StringComparison]::OrdinalIgnoreCase)) { return $false }
    foreach ($token in @($Expected.required_command_line_tokens | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })) {
        if ([string]$Actual.command_line -notlike ('*' + [string]$token + '*')) { return $false }
    }
    return $true
}

function Get-ObserverActualProcessIdentity {
    [CmdletBinding()]
    param([Parameter(Mandatory)][uint32]$ProcessId)

    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $process) { return $null }
    $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction Stop
    [pscustomobject][ordered]@{
        process_id = [uint32]$process.Id
        start_time_utc = $process.StartTime.ToUniversalTime().ToString('o')
        executable_path = [string]$cim.ExecutablePath
        command_line = [string]$cim.CommandLine
    }
}

function Get-ObserverApprovedExecutableRoots {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$ServerRoot)

    $resolvedServerRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
    $roots = [Collections.Generic.List[object]]::new()

    # The operator-supplied current client is an explicit approved root.  The exact
    # executable is Wow.exe; a different executable anywhere below this directory is
    # never accepted.
    $currentClientRoot = [IO.Path]::GetFullPath((Join-Path $resolvedServerRoot 'work\wow-client'))
    $currentClientExe = [IO.Path]::GetFullPath((Join-Path $currentClientRoot 'Wow.exe'))
    if (Test-Path -LiteralPath $currentClientExe -PathType Leaf) {
        $roots.Add([pscustomobject][ordered]@{
            label = 'current-wow-client'
            root = $currentClientRoot
            executable_path = $currentClientExe
            justification = 'operator-supplied current 3.3.5 client'
            manifest_path = $null
        })
    }

    # The private observer copy is approved only when its own preparation manifest
    # proves the exact directory/executable relationship.  An absent or malformed
    # manifest intentionally contributes no approved root.
    $observerClientRoot = [IO.Path]::GetFullPath((Join-Path $resolvedServerRoot 'work\observer-client'))
    $observerClientExe = [IO.Path]::GetFullPath((Join-Path $observerClientRoot 'Wow.exe'))
    $observerManifestPath = Join-Path $observerClientRoot 'observer-client-manifest.json'
    if (Test-Path -LiteralPath $observerManifestPath -PathType Leaf) {
        try {
            $manifest = Get-Content -LiteralPath $observerManifestPath -Raw -ErrorAction Stop | ConvertFrom-Json
            $manifestClient = [IO.Path]::GetFullPath([string]$manifest.observer_client)
            $manifestExe = [IO.Path]::GetFullPath([string]$manifest.executable_path)
            $sourceUnmodified = $manifest.PSObject.Properties['source_client_modified'] -and
                -not [bool]$manifest.source_client_modified
            if ([string]$manifest.schema -ceq 'autowow.observer.client.v1' -and
                [string]::Equals($manifestClient, $observerClientRoot, [StringComparison]::OrdinalIgnoreCase) -and
                [string]::Equals($manifestExe, $observerClientExe, [StringComparison]::OrdinalIgnoreCase) -and
                $sourceUnmodified -and (Test-Path -LiteralPath $observerClientExe -PathType Leaf)) {
                $roots.Add([pscustomobject][ordered]@{
                    label = 'dedicated-observer-client'
                    root = $observerClientRoot
                    executable_path = $observerClientExe
                    justification = 'validated observer-client-manifest.v1 with unmodified source client'
                    manifest_path = [IO.Path]::GetFullPath($observerManifestPath)
                })
            }
        }
        catch {
            # Invalid preparation metadata is not an error for discovery; it simply
            # cannot justify an executable root.
        }
    }

    return @($roots)
}

function Get-ObserverApprovedExecutable {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ServerRoot,
        [Parameter(Mandatory)][AllowEmptyString()][string]$ExecutablePath
    )

    if ([string]::IsNullOrWhiteSpace($ExecutablePath)) { return $null }
    try { $candidate = [IO.Path]::GetFullPath($ExecutablePath) }
    catch { return $null }

    foreach ($root in @(Get-ObserverApprovedExecutableRoots -ServerRoot $ServerRoot)) {
        if ([string]::Equals($candidate, [string]$root.executable_path, [StringComparison]::OrdinalIgnoreCase)) {
            return $root
        }
    }
    return $null
}

function Test-ObserverApprovedExecutablePath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ServerRoot,
        [Parameter(Mandatory)][AllowEmptyString()][string]$ExecutablePath
    )

    return $null -ne (Get-ObserverApprovedExecutable -ServerRoot $ServerRoot -ExecutablePath $ExecutablePath)
}

function Test-ObserverBridgeStatusForAdoption {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Response,
        [Parameter(Mandatory)][uint32]$ObserverGuid
    )

    $reasons = [Collections.Generic.List[string]]::new()
    if (-not $Response.PSObject.Properties['ok'] -or $Response.ok -isnot [bool] -or -not [bool]$Response.ok) {
        $reasons.Add('bridge_status_not_ok')
    }
    if (-not $Response.PSObject.Properties['observer'] -or [uint32]$Response.observer -ne $ObserverGuid) {
        $reasons.Add('bridge_observer_guid_mismatch')
    }

    foreach ($field in @('protected','is_gm','gm_visible','in_group','in_combat')) {
        if (-not $Response.PSObject.Properties[$field] -or $Response.$field -isnot [bool]) {
            $reasons.Add("bridge_field_missing_or_not_boolean:$field")
        }
    }
    if ($Response.PSObject.Properties['protected'] -and $Response.protected -is [bool] -and -not $Response.protected) { $reasons.Add('observer_not_protected') }
    if ($Response.PSObject.Properties['is_gm'] -and $Response.is_gm -is [bool] -and -not $Response.is_gm) { $reasons.Add('observer_not_gm') }
    if ($Response.PSObject.Properties['gm_visible'] -and $Response.gm_visible -is [bool] -and $Response.gm_visible) { $reasons.Add('observer_gm_visible') }
    if ($Response.PSObject.Properties['in_group'] -and $Response.in_group -is [bool] -and $Response.in_group) { $reasons.Add('observer_in_group') }
    if ($Response.PSObject.Properties['in_combat'] -and $Response.in_combat -is [bool] -and $Response.in_combat) { $reasons.Add('observer_in_combat') }

    [pscustomobject][ordered]@{
        valid = ($reasons.Count -eq 0)
        reasons = @($reasons)
    }
}

function Get-ObserverActualWindowIdentity {
    [CmdletBinding()]
    param([Parameter(Mandatory)][uint32]$ProcessId)

    $processIdentity = Get-ObserverActualProcessIdentity -ProcessId $ProcessId
    if (-not $processIdentity) { return $null }
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $process) { return $null }
    $process.Refresh()
    $handle = [uint64]$process.MainWindowHandle.ToInt64()
    if ($handle -eq 0) { return $null }

    if (-not ('AutoWowObserverWindowIdentityNative' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class AutoWowObserverWindowIdentityNative {
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
}
'@
    }

    [uint32]$ownerPid = 0
    $hwnd = [IntPtr][int64]$handle
    $null = [AutoWowObserverWindowIdentityNative]::GetWindowThreadProcessId($hwnd, [ref]$ownerPid)
    $titleBuilder = [Text.StringBuilder]::new(512)
    $null = [AutoWowObserverWindowIdentityNative]::GetWindowText($hwnd, $titleBuilder, $titleBuilder.Capacity)
    [pscustomobject][ordered]@{
        process_id = [uint32]$ownerPid
        start_time_utc = [string]$processIdentity.start_time_utc
        executable_path = [string]$processIdentity.executable_path
        command_line = [string]$processIdentity.command_line
        window_handle = $handle
        title = $titleBuilder.ToString()
    }
}

function New-ObserverAdoptedSessionManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][uint32]$ObserverGuid,
        [Parameter(Mandatory)][psobject]$ProcessIdentity,
        [Parameter(Mandatory)][psobject]$WindowIdentity,
        [Parameter(Mandatory)][psobject]$ApprovedExecutable,
        [Parameter(Mandatory)][psobject]$BridgeStatus,
        [Parameter(Mandatory)][string]$BridgeHost,
        [Parameter(Mandatory)][int]$BridgePort,
        [Parameter(Mandatory)][string]$ObservedUtc
    )

    if ($ObserverGuid -ne 21) { throw 'User-designated observer adoption is restricted to observer GUID 21.' }
    $verification = Test-ObserverBridgeStatusForAdoption -Response $BridgeStatus -ObserverGuid $ObserverGuid
    if (-not $verification.valid) { throw "Observer bridge status failed adoption guard: $($verification.reasons -join ', ')" }
    if ([uint32]$WindowIdentity.process_id -ne [uint32]$ProcessIdentity.process_id) { throw 'Window owner PID does not match process identity.' }
    if ([uint64]$WindowIdentity.window_handle -eq 0) { throw 'Observer window has no HWND.' }
    if ([string]::IsNullOrWhiteSpace([string]$WindowIdentity.title)) { throw 'Observer window title is empty.' }
    if (-not [string]::Equals([string]$WindowIdentity.executable_path, [string]$ApprovedExecutable.executable_path, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Observed executable path does not match the approved executable root.'
    }

    $displayName = if ($BridgeStatus.PSObject.Properties['name']) { [string]$BridgeStatus.name } else { $null }
    [pscustomobject][ordered]@{
        schema = 'autowow.observer.session.v1'
        session_mode = 'adopted'
        managed_by_automation = $false
        adoption = [pscustomobject][ordered]@{
            adopted_utc = $ObservedUtc
            reason = 'explicit operator adoption of an already-running observer window'
            no_login_automation = $true
        }
        session_token = $null
        observer_guid = [uint32]$ObserverGuid
        observer_name = $displayName
        bridge = [pscustomobject][ordered]@{
            host = $BridgeHost
            port = [int]$BridgePort
            status_verified_utc = $ObservedUtc
            observer_guid = [uint32]$ObserverGuid
            protected = $true
            is_gm = $true
            gm_visible = $false
            in_group = $false
            in_combat = $false
        }
        wow = [pscustomobject][ordered]@{
            process_id = [uint32]$ProcessIdentity.process_id
            start_time_utc = [string]$ProcessIdentity.start_time_utc
            executable_path = [string]$ApprovedExecutable.executable_path
            approved_executable_root = [string]$ApprovedExecutable.label
            required_command_line_tokens = @()
            window_handle = [uint64]$WindowIdentity.window_handle
            allowed_titles = @([string]$WindowIdentity.title)
        }
        relocate = $null
    }
}
