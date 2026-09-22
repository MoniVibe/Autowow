Set-StrictMode -Version Latest

function Get-OvernightInstanceObserverProperty {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) { return $Object[$Name] }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $Default
}

function ConvertTo-OvernightInstanceObserverInt {
    param([AllowNull()][object]$Value, [int]$Default = 0)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return $Default }
    $number = 0
    if ([int]::TryParse([string]$Value, [ref]$number)) { return $number }
    if ([string]$Value -match '^0[xX]([0-9a-fA-F]+)$') {
        return [Convert]::ToInt32($Matches[1], 16)
    }
    return $Default
}

function Get-OvernightInstanceObserverMaskDelta {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$OldMask,
        [Parameter(Mandatory)][object]$NewMask,
        [Parameter(Mandatory)][int]$ExpectedMask
    )

    $hasOld = $null -ne $OldMask -and -not [string]::IsNullOrWhiteSpace([string]$OldMask)
    $old = if ($hasOld) { ConvertTo-OvernightInstanceObserverInt $OldMask } else { 0 }
    $new = ConvertTo-OvernightInstanceObserverInt $NewMask
    [pscustomobject][ordered]@{
        old_mask = if ($hasOld) { $old } else { $null }
        new_mask = $new
        added_bits = ($new -band (-bnot $old))
        terminal = (($new -band $ExpectedMask) -eq $ExpectedMask)
    }
}

function Get-OvernightInstanceObserverRosterState {
    [CmdletBinding()]
    param([AllowNull()][object[]]$Members)

    $memberList = @($Members)
    $alive = @($memberList | Where-Object {
        [bool](Get-OvernightInstanceObserverProperty $_ 'alive' $false) -or
        [string](Get-OvernightInstanceObserverProperty $_ 'death_state' '') -eq 'alive'
    }).Count
    $combat = @($memberList | Where-Object { [bool](Get-OvernightInstanceObserverProperty $_ 'in_combat' $false) }).Count
    $released = @($memberList | Where-Object { [bool](Get-OvernightInstanceObserverProperty $_ 'released_corpse' $false) }).Count
    [pscustomobject][ordered]@{
        total = $memberList.Count
        alive = $alive
        dead = $memberList.Count - $alive
        combat = $combat
        released = $released
    }
}

function Get-OvernightInstanceObserverLatestProbeSamples {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string[]]$Paths)

    $latest = @{}
    foreach ($path in @($Paths | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })) {
        foreach ($line in @(Get-Content -LiteralPath $path -ErrorAction Stop)) {
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            try { $record = $line | ConvertFrom-Json -ErrorAction Stop } catch { continue }
            if ([string](Get-OvernightInstanceObserverProperty $record 'event' '') -ne 'monitor_sample') { continue }
            $probeId = [string](Get-OvernightInstanceObserverProperty $record 'probe_id' '')
            if ([string]::IsNullOrWhiteSpace($probeId)) { continue }
            $observed = [string](Get-OvernightInstanceObserverProperty $record 'observed_at_utc' '')
            if (-not $latest.ContainsKey($probeId) -or $observed -gt [string]$latest[$probeId].observed_at_utc) {
                $latest[$probeId] = $record
            }
        }
    }
    return @($latest.Values | Sort-Object probe_id)
}

function ConvertFrom-OvernightInstanceObserverKeyValueTail {
    param([AllowNull()][string]$Text)

    $fields = [ordered]@{}
    foreach ($match in [regex]::Matches([string]$Text, '(?<key>[A-Za-z0-9_]+)=(?<value>"[^"]*"|\S+)')) {
        $value = $match.Groups['value'].Value
        if ($value.Length -ge 2 -and $value.StartsWith('"') -and $value.EndsWith('"')) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $fields[$match.Groups['key'].Value] = $value
    }
    return [pscustomobject]$fields
}

function Get-OvernightInstanceObserverRaidLootEvents {
    [CmdletBinding()]
    param([AllowNull()][string[]]$Lines)

    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($line in @($Lines)) {
        $match = [regex]::Match([string]$line, '\[RaidLoot\]\s+event=(?<event>award|equip)\b(?<tail>.*)$')
        if (-not $match.Success) { continue }
        $fields = ConvertFrom-OvernightInstanceObserverKeyValueTail $match.Groups['tail'].Value
        [void]$events.Add([pscustomobject][ordered]@{
            source_event = $match.Groups['event'].Value
            fields = $fields
            raw_sha256 = ([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes([string]$line)) | ForEach-Object { $_.ToString('x2') }) -join ''
        })
    }
    return @($events)
}

function Get-OvernightInstanceObserverLogIdentity {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $buffer = New-Object byte[] 256
        $read = 0
        $firstLineEnd = -1
        while ($read -lt $buffer.Length -and $stream.Position -lt $stream.Length) {
            $n = $stream.Read($buffer, $read, 1)
            if ($n -le 0) { break }
            if ($buffer[$read] -eq 10) { $firstLineEnd = $read + 1; break }
            $read++
        }
        $hashLength = if ($firstLineEnd -ge 0) { $firstLineEnd } else { $read }
        $firstLine = New-Object byte[] $hashLength
        if ($hashLength -gt 0) { [Array]::Copy($buffer, 0, $firstLine, 0, $hashLength) }
        $hash = ([Security.Cryptography.SHA256]::Create().ComputeHash($firstLine) | ForEach-Object { $_.ToString('x2') }) -join ''
        return [pscustomobject][ordered]@{
            path = [IO.Path]::GetFullPath($Path)
            creation_utc = $item.CreationTimeUtc.ToString('o')
            head_sha256 = $hash
        }
    } finally { $stream.Dispose() }
}

function New-OvernightInstanceObserverLogCursor {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject][ordered]@{ path = [IO.Path]::GetFullPath($Path); offset = 0L; identity = $null }
    }
    $identity = Get-OvernightInstanceObserverLogIdentity -Path $Path
    return [pscustomobject][ordered]@{ path = $identity.path; offset = [long](Get-Item -LiteralPath $Path).Length; identity = $identity }
}

function Read-OvernightInstanceObserverLogDelta {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path,
        [AllowNull()][object]$Cursor
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject][ordered]@{ lines = @(); cursor = [pscustomobject][ordered]@{ path = $fullPath; offset = 0L; identity = $null }; reset_reason = 'missing' }
    }
    $identity = Get-OvernightInstanceObserverLogIdentity -Path $Path
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    $oldOffset = if ($null -eq $Cursor) { 0L } else { [long](Get-OvernightInstanceObserverProperty $Cursor 'offset' 0) }
    $oldIdentity = if ($null -eq $Cursor) { $null } else { Get-OvernightInstanceObserverProperty $Cursor 'identity' $null }
    $resetReason = $null
    if ($oldOffset -gt $item.Length) {
        $oldOffset = 0L
        $resetReason = 'truncated'
    } elseif ($null -ne $oldIdentity -and ([string](Get-OvernightInstanceObserverProperty $oldIdentity 'creation_utc' '') -ne $identity.creation_utc -or [string](Get-OvernightInstanceObserverProperty $oldIdentity 'head_sha256' '') -ne $identity.head_sha256)) {
        $oldOffset = 0L
        $resetReason = 'rotated'
    }

    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        [void]$stream.Seek($oldOffset, [IO.SeekOrigin]::Begin)
        $remaining = [int]($stream.Length - $oldOffset)
        if ($remaining -le 0) {
            return [pscustomobject][ordered]@{ lines = @(); cursor = [pscustomobject][ordered]@{ path = $fullPath; offset = $oldOffset; identity = $identity }; reset_reason = $resetReason }
        }
        $bytes = New-Object byte[] $remaining
        $read = 0
        while ($read -lt $remaining) {
            $n = $stream.Read($bytes, $read, $remaining - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        $lastLf = -1
        for ($index = $read - 1; $index -ge 0; $index--) { if ($bytes[$index] -eq 10) { $lastLf = $index; break } }
        if ($lastLf -lt 0) {
            return [pscustomobject][ordered]@{ lines = @(); cursor = [pscustomobject][ordered]@{ path = $fullPath; offset = $oldOffset; identity = $identity }; reset_reason = $resetReason }
        }
        $completeCount = $lastLf + 1
        $complete = New-Object byte[] $completeCount
        [Array]::Copy($bytes, 0, $complete, 0, $completeCount)
        $text = [Text.Encoding]::UTF8.GetString($complete)
        $lines = @($text -split '\r?\n' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        return [pscustomobject][ordered]@{
            lines = $lines
            cursor = [pscustomobject][ordered]@{ path = $fullPath; offset = ($oldOffset + $completeCount); identity = $identity }
            reset_reason = $resetReason
        }
    } finally { $stream.Dispose() }
}

function Get-OvernightInstanceObserverProcessIdentity {
    [CmdletBinding()]
    param([int]$ProcessId = $PID)

    $process = Get-CimInstance -ClassName Win32_Process -Filter ("ProcessId = {0}" -f $ProcessId) -ErrorAction SilentlyContinue | Select-Object -First 1
    $commandLine = if ($null -ne $process -and $null -ne $process.CommandLine) { [string]$process.CommandLine } else { [string][Environment]::CommandLine }
    [pscustomobject][ordered]@{ pid = $ProcessId; command_line = $commandLine }
}

function Enter-OvernightInstanceObserverLock {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$LockPath)

    $directory = Split-Path -Parent $LockPath
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $identity = Get-OvernightInstanceObserverProcessIdentity
    if (Test-Path -LiteralPath $LockPath -PathType Leaf) {
        $existing = $null
        try { $existing = Get-Content -LiteralPath $LockPath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop } catch { }
        if ($null -ne $existing -and (ConvertTo-OvernightInstanceObserverInt (Get-OvernightInstanceObserverProperty $existing 'pid' 0)) -gt 0) {
            $existingPid = ConvertTo-OvernightInstanceObserverInt (Get-OvernightInstanceObserverProperty $existing 'pid' 0)
            $owner = $null
            try {
                $owner = Get-CimInstance -ClassName Win32_Process -Filter ("ProcessId = {0}" -f $existingPid) -ErrorAction Stop | Select-Object -First 1
            } catch {
                throw 'Could not validate existing observer lock owner; refusing to remove it.'
            }
            $storedCommand = [string](Get-OvernightInstanceObserverProperty $existing 'command_line' '')
            if ($null -ne $owner -and [string]$owner.CommandLine -eq $storedCommand -and -not [string]::IsNullOrWhiteSpace($storedCommand)) {
                throw "An overnight instance observer is already running (pid $existingPid)."
            }
        }
        Remove-Item -LiteralPath $LockPath -Force -ErrorAction SilentlyContinue
    }

    $stream = [IO.FileStream]::new($LockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
    $payload = [ordered]@{ pid = $identity.pid; command_line = $identity.command_line; acquired_utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush($true)
    return [pscustomobject][ordered]@{ path = $LockPath; stream = $stream; identity = $identity }
}

function Exit-OvernightInstanceObserverLock {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object]$Lock)

    try {
        if ($null -ne $Lock.stream) { $Lock.stream.Dispose() }
    } finally {
        if (Test-Path -LiteralPath $Lock.path -PathType Leaf) {
            $current = $null
            try { $current = Get-Content -LiteralPath $Lock.path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop } catch { }
            if ($null -ne $current -and (ConvertTo-OvernightInstanceObserverInt (Get-OvernightInstanceObserverProperty $current 'pid' 0)) -eq [int]$Lock.identity.pid -and [string](Get-OvernightInstanceObserverProperty $current 'command_line' '') -eq [string]$Lock.identity.command_line) {
                Remove-Item -LiteralPath $Lock.path -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

function Assert-OvernightInstanceObserverSelectOnlySql {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Sql)

    $trimmed = $Sql.Trim()
    if ($trimmed -notmatch '^(?i:SELECT)\b' -or $trimmed.Contains(';') -or $trimmed -match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE|LOAD|RENAME|LOCK|UNLOCK)\b') {
        throw 'Observer accepts SELECT-only SQL.'
    }
}
