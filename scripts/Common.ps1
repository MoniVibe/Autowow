Set-StrictMode -Version Latest

$script:AutoWoWRoot = Split-Path -Parent $PSScriptRoot
$script:LogsRoot = Join-Path $script:AutoWoWRoot 'logs'

function Initialize-AutoWoWLayout {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Root
    )

    foreach ($path in @(
        $Root,
        (Join-Path $Root 'logs'),
        (Join-Path $Root 'scripts'),
        (Join-Path $Root 'server'),
        (Join-Path $Root 'server\logs'),
        (Join-Path $Root 'server\configs'),
        (Join-Path $Root 'server\configs\modules'),
        (Join-Path $Root 'server\data'),
        (Join-Path $Root 'server\temp')
    )) {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
}

function New-AutoWoWLogPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Name
    )

    New-Item -ItemType Directory -Path $script:LogsRoot -Force | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    return (Join-Path $script:LogsRoot ("{0}-{1}.log" -f $Name, $stamp))
}

function Invoke-NativeLogged {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    if (-not (Test-Path -LiteralPath $FilePath)) {
        throw "Executable not found: $FilePath"
    }

    $oldLocation = Get-Location
    $oldErrorActionPreference = $ErrorActionPreference
    try {
        Set-Location -LiteralPath $WorkingDirectory
        Write-Output ("Running: {0} {1}" -f $FilePath, ($ArgumentList -join ' '))
        # CMake writes normal progress messages to stderr.  Do not let a caller's Stop preference
        # turn those messages into a PowerShell exception; the process exit code remains authoritative.
        $ErrorActionPreference = 'Continue'
        & $FilePath @ArgumentList 2>&1 | Tee-Object -FilePath $LogPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldErrorActionPreference
        Set-Location -LiteralPath $oldLocation
    }

    if ($exitCode -ne 0) {
        throw "Native command failed with exit code $exitCode. See $LogPath"
    }

    return $exitCode
}

function Get-FirstExistingPath {
    [CmdletBinding()]
    param(
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return $null
}

function Set-ConfigValue {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$Value
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Configuration file not found: $Path"
    }

    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $lines.Add($line)
    }

    $escapedKey = [regex]::Escape($Key)
    $pattern = "^(\s*$escapedKey\s*=\s*).*$"
    $replacement = '${1}' + $Value
    $changed = $false

    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match $pattern) {
            $lines[$index] = [regex]::Replace($lines[$index], $pattern, $replacement)
            $changed = $true
        }
    }

    if (-not $changed) {
        $lines.Add("$Key = $Value")
    }

    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllLines($Path, $lines, $utf8NoBom)
}

function Get-SecretPresence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Name
    )

    $value = [Environment]::GetEnvironmentVariable($Name)
    return (-not [string]::IsNullOrWhiteSpace($value))
}

function Assert-LocalOnlyAddress {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Address
    )

    if ($Address -notin @('127.0.0.1', '::1', 'localhost')) {
        throw "Refusing non-local bind address '$Address'. This bootstrap is local-only."
    }
}

function Assert-WowClientDataReady {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ClientDir
    )

    $required = @(
        'Data\common.MPQ',
        'Data\common-2.MPQ',
        'Data\expansion.MPQ',
        'Data\lichking.MPQ',
        'Data\patch.MPQ',
        'Data\patch-2.MPQ',
        'Data\patch-3.MPQ',
        'Data\enUS\locale-enUS.MPQ',
        'Data\enUS\patch-enUS.MPQ',
        'Data\enUS\patch-enUS-2.MPQ',
        'Data\enUS\patch-enUS-3.MPQ'
    )
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($relative in $required) {
        $path = Join-Path $ClientDir $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $problems.Add("missing $relative")
            continue
        }
        $stream = $null
        try {
            $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            $header = New-Object byte[] 4
            $read = $stream.Read($header, 0, 4)
            if ($read -ne 4 -or $header[0] -ne 0x4D -or $header[1] -ne 0x50 -or $header[2] -ne 0x51 -or $header[3] -ne 0x1A) {
                $problems.Add("invalid or incomplete MPQ header in $relative")
            }
        }
        catch {
            $problems.Add("locked or unreadable $relative ($($_.Exception.Message))")
        }
        finally {
            if ($stream) { $stream.Dispose() }
        }
    }
    if ($problems.Count) {
        throw ("WoW client data is not complete/readable; the downloader may still be writing it:`n - " + ($problems -join "`n - "))
    }
}
