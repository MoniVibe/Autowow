<#
.SYNOPSIS
    Plans or explicitly provisions the dedicated local AutoWoW rendering observer.

.DESCRIPTION
    Plan is the default. The script parses the loopback AzerothCore service credentials from the
    existing worldserver.conf, performs read-only collision checks, and stores the random observer
    password only as current-user DPAPI SecureString CLIXML. Apply performs one local transaction
    using AzerothCore-compatible SRP6 registration data. It does not need a root password and does
    not stop or start any server.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('Plan', 'Apply')][string]$Mode = 'Plan',
    [string]$StateDirectory = '',
    [string]$MysqlRoot = $(if ($env:MYSQL_ROOT_DIR) { $env:MYSQL_ROOT_DIR } else { '' })
)

. (Join-Path $PSScriptRoot 'observer-automation-lib.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
if ([string]::IsNullOrWhiteSpace($StateDirectory)) { $StateDirectory = Join-Path $ServerRoot 'work\observer-state' }
$StateDirectory = [IO.Path]::GetFullPath($StateDirectory)
New-Item -ItemType Directory -Path $StateDirectory -Force | Out-Null

$secretPath = Join-Path $StateDirectory 'observer-password.clixml'
$statePath = Join-Path $StateDirectory 'observer-state.json'
$planPath = Join-Path $StateDirectory 'observer-provisioning-plan.json'
$worldConfig = Join-Path $ServerRoot 'server\configs\worldserver.conf'
$contract = Get-ObserverIdentityContract

if (-not (Test-Path -LiteralPath $secretPath)) { Save-ObserverPassword -Password (New-ObserverPassword) -Path $secretPath }
$securePassword = Import-ObserverPassword -Path $secretPath

$loginDb = Get-ObserverDatabaseConnectionFromConfig -ConfigPath $worldConfig -Key LoginDatabaseInfo
$characterDb = Get-ObserverDatabaseConnectionFromConfig -ConfigPath $worldConfig -Key CharacterDatabaseInfo
if ($loginDb.database -cne 'acore_auth' -or $characterDb.database -cne 'acore_characters') { throw 'Observer provisioning requires the expected acore_auth/acore_characters schemas.' }
foreach ($property in @('host','port','username','password')) {
    if (-not [string]::Equals([string]$loginDb.$property, [string]$characterDb.$property, [StringComparison]::Ordinal)) {
        throw "LoginDatabaseInfo and CharacterDatabaseInfo must use one identical local service credential ($property differs)."
    }
}
$realmLine = @([IO.File]::ReadAllLines($worldConfig) | ForEach-Object { if ($_ -match '^\s*RealmID\s*=\s*([0-9]+)\s*(?:#.*)?$') { $Matches[1] } })
if ($realmLine.Count -ne 1) { throw 'Expected exactly one active numeric RealmID in worldserver.conf.' }
$realmId = [int]$realmLine[0]
if ($realmId -lt 1) { throw 'Observer RealmID must be positive.' }

if ([string]::IsNullOrWhiteSpace($MysqlRoot)) {
    $MysqlRoot = @((Join-Path $ServerRoot 'third_party\mysql'),'C:\Program Files\MySQL\MySQL Server 8.4','C:\Program Files\MySQL\MySQL Server 8.0') |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
$mysqlExe = if ($MysqlRoot) { Join-Path $MysqlRoot 'bin\mysql.exe' } else { '' }
if (-not (Test-Path -LiteralPath $mysqlExe -PathType Leaf)) { throw 'MySQL CLI not found. Set MYSQL_ROOT_DIR.' }

function Invoke-ObserverMySql {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Sql, [switch]$Mutation)

    if ($Mutation -and $Mode -ne 'Apply') { throw 'Database mutations require explicit -Mode Apply.' }
    $old = $env:MYSQL_PWD
    $env:MYSQL_PWD = [string]$loginDb.password
    try {
        $result = $Sql | & $mysqlExe --protocol=tcp --host=$($loginDb.host) --port=$($loginDb.port) `
            --user=$($loginDb.username) --batch --raw --skip-column-names 2>&1
        $exit = $LASTEXITCODE
    }
    finally {
        if ($null -eq $old) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $old }
    }
    $lines = @($result | ForEach-Object { $_.ToString() } | Where-Object { $_ -notmatch '^mysql: (Unknown OS character set|Switching to the default character set)' })
    if ($exit -ne 0) { throw 'Local AzerothCore service-credential MySQL request failed; output omitted to avoid derived credential disclosure.' }
    return $lines
}

function Get-ObserverDatabaseSnapshot {
    $sql = @"
SELECT CONCAT('ACCOUNT',CHAR(9),id,CHAR(9),username,CHAR(9),HEX(salt),CHAR(9),HEX(verifier)) FROM acore_auth.account WHERE UPPER(username) IN ('AUTOWATCH','SHONHAY') OR id=3;
SELECT CONCAT('CHARACTER',CHAR(9),guid,CHAR(9),account,CHAR(9),name,CHAR(9),race,CHAR(9),class,CHAR(9),online) FROM acore_characters.characters WHERE LOWER(name)=LOWER('Autowatch') OR guid=21 OR account IN (SELECT id FROM acore_auth.account WHERE UPPER(username)='AUTOWATCH');
SELECT CONCAT('ACCESS',CHAR(9),id,CHAR(9),gmlevel,CHAR(9),RealmID) FROM acore_auth.account_access WHERE id IN (SELECT id FROM acore_auth.account WHERE UPPER(username)='AUTOWATCH');
SELECT CONCAT('GROUP',CHAR(9),memberGuid) FROM acore_characters.group_member WHERE memberGuid IN (SELECT guid FROM acore_characters.characters WHERE LOWER(name)=LOWER('Autowatch'));
SELECT CONCAT('REALM',CHAR(9),realmid,CHAR(9),acctid,CHAR(9),numchars) FROM acore_auth.realmcharacters WHERE acctid IN (SELECT id FROM acore_auth.account WHERE UPPER(username)='AUTOWATCH');
SELECT CONCAT('REALMID',CHAR(9),id) FROM acore_auth.realmlist;
SELECT CONCAT('NEXT',CHAR(9),COALESCE(AUTO_INCREMENT,0)) FROM information_schema.TABLES WHERE TABLE_SCHEMA='acore_auth' AND TABLE_NAME='account';
"@
    $accounts=@(); $characters=@(); $access=@(); $groups=@(); $realmRows=@(); $realmIds=@(); [uint64]$next=0
    foreach ($line in @(Invoke-ObserverMySql -Sql $sql)) {
        $f = $line -split "`t"
        switch ($f[0]) {
            'ACCOUNT' { $accounts += [pscustomobject]@{ id=[uint64]$f[1]; username=$f[2]; salt_hex=$f[3]; verifier_hex=$f[4] } }
            'CHARACTER' { $characters += [pscustomobject]@{ guid=[uint64]$f[1]; account=[uint64]$f[2]; name=$f[3]; race=[int]$f[4]; class=[int]$f[5]; online=[int]$f[6] } }
            'ACCESS' { $access += [pscustomobject]@{ id=[uint64]$f[1]; gmlevel=[int]$f[2]; RealmID=[int]$f[3] } }
            'GROUP' { $groups += [pscustomobject]@{ memberGuid=[uint64]$f[1] } }
            'REALM' { $realmRows += [pscustomobject]@{ realmid=[uint32]$f[1]; acctid=[uint64]$f[2]; numchars=[int]$f[3] } }
            'REALMID' { $realmIds += [uint32]$f[1] }
            'NEXT' { $next=[uint64]$f[1] }
        }
    }
    [pscustomobject]@{ accounts=$accounts; characters=$characters; access=$access; groups=$groups; realm_rows=$realmRows; realm_ids=$realmIds; next_account_id=$next }
}

function Get-ObserverDecision {
    $snapshot = Get-ObserverDatabaseSnapshot
    $decision = Resolve-ObserverProvisioningState -Accounts $snapshot.accounts -Characters $snapshot.characters -AccessRows $snapshot.access `
        -GroupRows $snapshot.groups -RealmRows $snapshot.realm_rows -RealmIds $snapshot.realm_ids -ObserverRealmId $realmId -NextAccountId $snapshot.next_account_id
    $account = @($snapshot.accounts | Where-Object username -ieq 'AUTOWATCH' | Select-Object -First 1)
    $credentialMatches = $null
    if ($account.Count -eq 1) {
        $computed = $null
        $salt = ConvertFrom-ObserverHex -Hex ([string]$account[0].salt_hex)
        $storedVerifier = ConvertFrom-ObserverHex -Hex ([string]$account[0].verifier_hex)
        try {
            $computed = New-ObserverSrp6RegistrationData -Username AUTOWATCH -Password $securePassword -Salt $salt
            $credentialMatches = Test-ObserverByteArrayFixedTimeEqual -Left $computed.verifier -Right $storedVerifier
        }
        finally {
            [Array]::Clear($salt,0,$salt.Length)
            [Array]::Clear($storedVerifier,0,$storedVerifier.Length)
            if ($computed) { [Array]::Clear($computed.verifier,0,$computed.verifier.Length); [Array]::Clear($computed.salt,0,$computed.salt.Length) }
        }
        if (-not $credentialMatches) {
            $decision.errors += 'AUTOWATCH_password_does_not_match_DPAPI_secret'
            $decision.safe = $false
            $decision.complete = $false
        }
    }
    [pscustomobject]@{ snapshot=$snapshot; decision=$decision; credential_matches=$credentialMatches }
}

$preflight = Get-ObserverDecision
$plan = [pscustomobject][ordered]@{
    schema='autowow.observer.provisioning-plan.v2'; generated_utc=(Get-Date).ToUniversalTime().ToString('o'); mode=$Mode.ToLowerInvariant(); local_only=$true
    identity=$contract; database_credential_source='worldserver.conf local service credential'; database_host=$loginDb.host; database_port=$loginDb.port; database_user=$loginDb.username
    database_password_reported=$false; root_credential_required=$false; server_restart_required=$false
    collision_guard_result=if($preflight.decision.safe){'pass'}else{'refused'}; collision_errors=@($preflight.decision.errors); actions=@($preflight.decision.actions)
    account_registration='AzerothCore SRP6 SHA1, 32-byte random salt, 32-byte little-endian verifier'; credential_match=$preflight.credential_matches
    password_storage='current-Windows-user DPAPI SecureString CLIXML'; password_path=$secretPath; password_plaintext_in_plan=$false
    existing_account_id=$preflight.decision.account_id; existing_character_guid=$preflight.decision.character_guid
    forbidden_reuse=@{account_id=3;character_guid=21;account_name='SHONHAY'}
}
[IO.File]::WriteAllText($planPath,($plan|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
if (-not $preflight.decision.safe) { throw "Observer provisioning refused by exact collision guards: $($preflight.decision.errors -join ', '). Redacted plan: $planPath" }

if ($Mode -eq 'Apply' -and -not $preflight.decision.complete) {
    $existingAccount = @($preflight.snapshot.accounts | Where-Object username -ieq 'AUTOWATCH' | Select-Object -First 1)
    if ($existingAccount.Count -eq 1) {
        $existingSalt = ConvertFrom-ObserverHex -Hex ([string]$existingAccount[0].salt_hex)
        try { $registration = New-ObserverSrp6RegistrationData -Username AUTOWATCH -Password $securePassword -Salt $existingSalt }
        finally { [Array]::Clear($existingSalt,0,$existingSalt.Length) }
    }
    else {
        $registration = New-ObserverSrp6RegistrationData -Username AUTOWATCH -Password $securePassword
    }
    try {
        $mutationResult = @(Invoke-ObserverMySql -Sql (New-ObserverProvisioningSql -Salt $registration.salt -Verifier $registration.verifier -RealmId $realmId) -Mutation)
        $resultLine = @($mutationResult | Where-Object { $_ -like "PROVISION_RESULT`t*" })
        if ($resultLine.Count -ne 1) { throw 'Observer provisioning transaction returned no unique redacted result.' }
        $resultFields = $resultLine[0] -split "`t"
        if ($resultFields.Count -ne 5 -or $resultFields[1] -ne '1' -or [uint64]$resultFields[2] -eq 0) {
            throw 'Observer provisioning transaction did not acquire its lock or resolve an account.'
        }
    }
    finally { [Array]::Clear($registration.salt,0,$registration.salt.Length); [Array]::Clear($registration.verifier,0,$registration.verifier.Length) }
}

$final = Get-ObserverDecision
if ($Mode -eq 'Apply') {
    if (-not $final.decision.complete) { throw "Observer provisioning did not converge: actions=$($final.decision.actions -join ',') errors=$($final.decision.errors -join ',')" }
    if ([uint64]$final.decision.account_id -eq 3 -or [uint64]$final.decision.character_guid -eq 21) { throw 'Final forbidden-identity guard failed.' }
    $state=[pscustomobject][ordered]@{schema='autowow.observer.state.v1';account_name='AUTOWATCH';account_id=[uint64]$final.decision.account_id;character_name='Autowatch';character_guid=[uint64]$final.decision.character_guid;race=1;class=1;faction='Alliance';gmlevel=3;group_member=$false;secret_path=$secretPath;provisioned_utc=(Get-Date).ToUniversalTime().ToString('o')}
    [IO.File]::WriteAllText($statePath,($state|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    Write-Output "Dedicated observer provisioned and verified without stopping servers. State: $statePath"
} else { Write-Output "Observer provisioning plan completed without DB writes. Plan: $planPath" }
