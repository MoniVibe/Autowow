[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot)
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot

if ([string]::IsNullOrWhiteSpace($env:WOW_ACCOUNT_NAME) -or [string]::IsNullOrWhiteSpace($env:WOW_ACCOUNT_PASSWORD)) {
    throw 'Set WOW_ACCOUNT_NAME and WOW_ACCOUNT_PASSWORD in the process environment. They are never accepted as script arguments or written to reports.'
}
$account = $env:WOW_ACCOUNT_NAME.ToUpperInvariant()
$password = $env:WOW_ACCOUNT_PASSWORD
if ($account -notmatch '^[A-Z0-9_]{3,16}$') { throw 'WOW_ACCOUNT_NAME must be 3-16 ASCII letters, digits, or underscores.' }
if ($password.Length -lt 3 -or $password.Length -gt 16 -or $password -match '\s') { throw 'WOW_ACCOUNT_PASSWORD must be 3-16 characters with no whitespace.' }
if ([string]::IsNullOrWhiteSpace($env:MYSQL_ROOT_PASSWORD)) {
    throw 'Set MYSQL_ROOT_PASSWORD so the created account and GM access can be verified without putting that secret in a command argument.'
}

$installRoot = Join-Path $ServerRoot 'server'
$world = Join-Path $installRoot 'worldserver.exe'
$worldConfig = Join-Path $installRoot 'configs\worldserver.conf'
$pidFile = Join-Path $ServerRoot 'worldserver.pid'
foreach ($required in @($world, $worldConfig)) { if (-not (Test-Path -LiteralPath $required)) { throw "Missing account-creation prerequisite: $required" } }
if (Test-Path -LiteralPath $pidFile) {
    $existingPid = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    if (Get-Process -Id $existingPid -ErrorAction SilentlyContinue) { throw 'Stop the normal worldserver before running create-account.ps1.' }
    Remove-Item -LiteralPath $pidFile -Force
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stdoutPath = Join-Path $installRoot ("logs\create-account-$stamp-stdout.log")
$stderrPath = Join-Path $installRoot ("logs\create-account-$stamp-stderr.log")
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $world
$psi.WorkingDirectory = $installRoot
$psi.Arguments = '-c "' + $worldConfig + '"'
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $psi
$null = $process.Start()
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
try {
    $process.StandardInput.WriteLine(".account create $account $password")
    $process.StandardInput.WriteLine(".account set gmlevel $account 3 -1")
    $process.StandardInput.Flush()
    # Playerbots initialization can take longer than the core's basic startup.
    # Wait for that initialization before sending console commands so they are
    # processed by a ready worldserver rather than lost during startup.
    Start-Sleep -Seconds 45
}
finally {
    $process.StandardInput.Close()
}
if (-not $process.WaitForExit(180000)) {
    $process.Kill()
    throw "Temporary worldserver did not exit after account commands. Check $stdoutPath and $stderrPath"
}
$stdout = $stdoutTask.Result
$stderr = $stderrTask.Result
$safeStdout = $stdout.Replace($password, '<redacted>')
$safeStderr = $stderr.Replace($password, '<redacted>')
[System.IO.File]::WriteAllText($stdoutPath, $safeStdout, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText($stderrPath, $safeStderr, [System.Text.UTF8Encoding]::new($false))
if ($process.ExitCode -ne 0) { throw "Temporary worldserver exited with code $($process.ExitCode). Check $stdoutPath and $stderrPath" }

$mysqlRoot = Get-FirstExistingPath -Candidates @(
    $env:MYSQL_ROOT_DIR,
    (Join-Path $ServerRoot 'third_party\mysql'),
    'C:\Program Files\MySQL\MySQL Server 8.4',
    'C:\Program Files\MySQL\MySQL Server 8.0'
)
$mysqlExe = if ($mysqlRoot) { Get-FirstExistingPath -Candidates @((Join-Path $mysqlRoot 'bin\mysql.exe')) } else { $null }
if (-not $mysqlExe) { throw 'MySQL CLI was not found for account verification.' }
$oldPwd = $env:MYSQL_PWD
$env:MYSQL_PWD = $env:MYSQL_ROOT_PASSWORD
try {
    # Console account creation persists reliably, but console GM commands may be
    # interrupted when the one-shot worldserver exits. Apply the all-realms GM
    # row directly, then verify both records through the same secret-scoped CLI.
    $sql = "INSERT INTO acore_auth.account_access (id, gmlevel, RealmID) SELECT id, 3, -1 FROM acore_auth.account WHERE username='$account' ON DUPLICATE KEY UPDATE gmlevel=VALUES(gmlevel); SELECT COUNT(*) FROM acore_auth.account WHERE username='$account'; SELECT COUNT(*) FROM acore_auth.account_access aa INNER JOIN acore_auth.account a ON a.id=aa.id WHERE a.username='$account' AND aa.gmlevel >= 3 AND aa.RealmID=-1;"
    $result = & $mysqlExe --protocol=tcp --host=127.0.0.1 --port=3306 --user=root --batch --skip-column-names --execute=$sql 2>&1
    $exitCode = $LASTEXITCODE
}
finally {
    if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
}
$textResult = @($result | ForEach-Object { $_.ToString() })
$filteredResult = @($textResult | Where-Object { $_ -notmatch '^mysql: (Unknown OS character set|Switching to the default character set)' })
if ($exitCode -ne 0) { throw "Account verification query failed: $($filteredResult -join ' ')" }
if ($filteredResult.Count -lt 2 -or $filteredResult[0].Trim() -ne '1' -or $filteredResult[1].Trim() -ne '1') {
    throw "Account or GM-level verification failed for $account. Check $stdoutPath and $stderrPath"
}

$report = @(
    '# AutoWoW account report',
    '',
    ('Generated: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')),
    '',
    ('- Account: {0}' -f $account),
    '- Password: supplied through `WOW_ACCOUNT_PASSWORD` (value intentionally omitted)',
    '- GM level: 3, all realms',
    ('- Temporary worldserver stdout: {0}' -f $stdoutPath),
    ('- Temporary worldserver stderr: {0}' -f $stderrPath),
    '- Account and GM access verification: PASS'
) -join [Environment]::NewLine
[System.IO.File]::WriteAllText((Join-Path $ServerRoot 'ACCOUNT_REPORT.md'), $report, [System.Text.UTF8Encoding]::new($false))
Write-Output "Account $account created/updated with GM level 3. Report: $(Join-Path $ServerRoot 'ACCOUNT_REPORT.md')"
