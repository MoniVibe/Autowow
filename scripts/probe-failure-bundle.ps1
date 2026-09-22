<#!
.SYNOPSIS
    Build a bounded, redacted, read-only failure bundle from Probe Lab evidence.

.DESCRIPTION
    Accepts a Probe Lab summary JSON or receipt JSONL. It creates a new directory below
    logs/failure-bundles, copies only the related summary/receipt as redacted evidence, and
    writes bounded tails and failure-focused excerpts from relevant Playerbots/runtime logs.
    It never connects to the bridge or database and never overwrites an existing path.
#>
[CmdletBinding()]
param(
    [string]$InputPath = '',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(10, 2000)][int]$TailLines = 200,
    [ValidateRange(10, 1000)][int]$ExcerptLines = 120
)

$ErrorActionPreference = 'Stop'

function Get-FailureBundleProperty {
    param([AllowNull()][object]$Object, [Parameter(Mandatory)][string]$Name, [AllowNull()][object]$Default = $null)
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) { return $Object[$Name] }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $Default
}

function ConvertTo-FailureBundleRedactedText {
    [CmdletBinding()]
    param([AllowNull()][string]$Text)
    if ($null -eq $Text) { return '' }
    $redacted = $Text
    $redacted = [regex]::Replace($redacted, '(?i)(\bBearer\s+)[A-Za-z0-9._~+/=-]+', '$1<redacted>')
    $redacted = [regex]::Replace($redacted, '(?i)(\b(?:password|passwd|pwd|token|access[_-]?token|refresh[_-]?token|api[_-]?key|secret|credential|client_secret|authorization)\b\s*[:=]\s*)("[^"]*"|''[^'']*''|[^\s,;]+)', '$1<redacted>')
    $redacted = [regex]::Replace($redacted, '(?i)(--(?:password|passwd|token|secret)=)[^\s]+', '$1<redacted>')
    $redacted = [regex]::Replace($redacted, '(?i)(://[^/\s:]+:)[^@\s]+(@)', '$1<redacted>$2')
    return $redacted
}

function Resolve-FailureBundleSourcePath {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$ServerRoot)
    $root = [IO.Path]::GetFullPath((Join-Path $ServerRoot 'logs')).TrimEnd('\')
    $relativePath = $Path -replace '^[.][\\/]', ''
    $candidate = if ([IO.Path]::IsPathRooted($Path)) {
        $Path
    } elseif ($relativePath -match '^(?i)logs[\\/]') {
        Join-Path $ServerRoot $relativePath
    } else {
        Join-Path $root $relativePath
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $null }
    $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $candidate).Path)
    if (-not ($resolved.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or $resolved.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase))) {
        throw "Failure-bundle source must remain beneath logs: $Path"
    }
    return $resolved
}

function Read-FailureBundleJsonLines {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $records.Add(($line | ConvertFrom-Json)) } catch { }
    }
    return @($records)
}

function Read-FailureBundleSummary {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)
    try { return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json) }
    catch { throw "Summary JSON is not valid: $Path" }
}

function Get-FailureBundleReferencedPaths {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$InputPath,
        [Parameter(Mandatory)][string]$ServerRoot
    )
    $extension = [IO.Path]::GetExtension($InputPath).ToLowerInvariant()
    $summaryPath = $null
    $receiptPath = $null
    $summary = $null
    $records = @()
    if ($extension -eq '.jsonl') {
        $records = @(Read-FailureBundleJsonLines -Path $InputPath)
        foreach ($record in $records) {
            $candidate = [string](Get-FailureBundleProperty $record 'summary_json_path' '')
            if ([string]::IsNullOrWhiteSpace($candidate)) { $candidate = [string](Get-FailureBundleProperty $record 'summary_path' '') }
            if (-not [string]::IsNullOrWhiteSpace($candidate)) {
                $resolved = Resolve-FailureBundleSourcePath -Path $candidate -ServerRoot $ServerRoot
                if ($null -ne $resolved) { $summaryPath = $resolved }
            }
        }
        $receiptPath = $InputPath
        if ($null -ne $summaryPath) { $summary = Read-FailureBundleSummary -Path $summaryPath }
    } else {
        $summaryPath = $InputPath
        $summary = Read-FailureBundleSummary -Path $summaryPath
        $candidateReceipt = [string](Get-FailureBundleProperty $summary 'receipt_path' '')
        if (-not [string]::IsNullOrWhiteSpace($candidateReceipt)) { $receiptPath = Resolve-FailureBundleSourcePath -Path $candidateReceipt -ServerRoot $ServerRoot }
        if ($null -ne $receiptPath) { $records = @(Read-FailureBundleJsonLines -Path $receiptPath) }
    }
    if ($records.Count -eq 0 -and $null -ne $receiptPath -and (Test-Path -LiteralPath $receiptPath)) { $records = @(Read-FailureBundleJsonLines -Path $receiptPath) }
    if ($null -eq $summary -and $null -ne $summaryPath) { $summary = Read-FailureBundleSummary -Path $summaryPath }
    if ($null -eq $summary -and $records.Count -eq 0) { throw 'Input did not contain a readable Probe Lab summary or receipt.' }
    [pscustomobject][ordered]@{ summary_path = $summaryPath; receipt_path = $receiptPath; summary = $summary; records = @($records) }
}

function Get-FailureBundleHints {
    [CmdletBinding()]
    param([AllowNull()][psobject]$Summary, [object[]]$Records = @())
    $hints = [System.Collections.Generic.List[string]]::new()
    $add = {
        param([AllowNull()][object]$Value)
        if ($null -ne $Value -and -not [string]::IsNullOrWhiteSpace([string]$Value)) { $hints.Add([string]$Value) }
    }
    & $add (Get-FailureBundleProperty $Summary 'status' '')
    & $add (Get-FailureBundleProperty $Summary 'reason' '')
    foreach ($probe in @(Get-FailureBundleProperty $Summary 'probes' @())) {
        & $add (Get-FailureBundleProperty $probe 'probe_id' '')
        $failure = Get-FailureBundleProperty $probe 'first_failure' $null
        & $add (Get-FailureBundleProperty $failure 'code' '')
        & $add (Get-FailureBundleProperty $failure 'detail' '')
        & $add (Get-FailureBundleProperty $failure 'navigator_reason' '')
    }
    foreach ($record in @($Records)) {
        & $add (Get-FailureBundleProperty $record 'event' '')
        $failure = Get-FailureBundleProperty $record 'failure' $null
        & $add (Get-FailureBundleProperty $failure 'code' '')
        & $add (Get-FailureBundleProperty $failure 'detail' '')
    }
    return @($hints | Select-Object -Unique)
}

function Get-FailureBundleLogCandidates {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ServerRoot,
        [AllowNull()][psobject]$Summary,
        [object[]]$Records = @()
    )
    $result = [System.Collections.Generic.List[string]]::new()
    $add = {
        param([AllowNull()][string]$Path)
        if ([string]::IsNullOrWhiteSpace($Path)) { return }
        try { $resolved = Resolve-FailureBundleSourcePath -Path $Path -ServerRoot $ServerRoot } catch { return }
        if ($null -ne $resolved -and $result -notcontains $resolved) { $result.Add($resolved) }
    }
    & $add ([string](Get-FailureBundleProperty $Summary 'playerbots_log_path' ''))
    & $add ([string](Get-FailureBundleProperty $Summary 'runtime_log_path' ''))
    foreach ($record in @($Records)) {
        & $add ([string](Get-FailureBundleProperty $record 'playerbots_log_path' ''))
        & $add ([string](Get-FailureBundleProperty $record 'runtime_log_path' ''))
        foreach ($path in @(Get-FailureBundleProperty $record 'runtime_logs' @())) { & $add ([string]$path) }
    }
    $directories = @($result | ForEach-Object { Split-Path -Parent $_ })
    $directories += Join-Path $ServerRoot 'logs\phase1-runtime'
    foreach ($directory in @($directories | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) { continue }
        foreach ($file in @(Get-ChildItem -LiteralPath $directory -File -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -match '(?i)(playerbots|runtime|worldserver|server).*\.log$'
        })) { & $add $file.FullName }
    }
    return @($result | Select-Object -First 12)
}

function Get-FailureBundleLogTail {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [ValidateRange(1, 2000)][int]$MaxLines = 200)
    $lines = @(Get-Content -LiteralPath $Path -Tail $MaxLines -ErrorAction Stop)
    if ($lines.Count -eq 0) { return '(empty log)' }
    return ConvertTo-FailureBundleRedactedText -Text ($lines -join [Environment]::NewLine)
}

function Get-FailureBundleLogExcerpt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines,
        [string[]]$Hints = @(),
        [ValidateRange(10, 1000)][int]$MaxLines = 120
    )
    if ($null -eq $Lines -or $Lines.Count -eq 0 -or ($Lines.Count -eq 1 -and [string]::IsNullOrEmpty($Lines[0]))) {
        return [string]''
    }
    $tokens = @($Hints | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { [regex]::Escape($_) })
    $pattern = if ($tokens.Count -gt 0) { '(?i)(error|fail|exception|crash|DungeonNavigator|blocked|' + ($tokens -join '|') + ')' } else { '(?i)(error|fail|exception|crash|DungeonNavigator|blocked)' }
    $selected = [System.Collections.Generic.List[string]]::new()
    $previous = [System.Collections.Generic.Queue[string]]::new()
    $after = 0
    $lineNumber = 0
    foreach ($line in $Lines) {
        $lineNumber++
        $text = [string]$line
        $match = $text -match $pattern
        if ($match) {
            foreach ($prior in $previous) { $selected.Add($prior) }
            $selected.Add("[$lineNumber] $text")
            $after = 2
        } elseif ($after -gt 0) {
            $selected.Add("[$lineNumber] $text")
            $after--
        }
        $previous.Enqueue("[$lineNumber] $text")
        while ($previous.Count -gt 2) { [void]$previous.Dequeue() }
    }
    if ($selected.Count -eq 0) { return 'No matching failure/error lines found.' }
    return ConvertTo-FailureBundleRedactedText -Text ((@($selected | Select-Object -Unique | Select-Object -Last $MaxLines)) -join [Environment]::NewLine)
}

function New-FailureBundleId {
    [CmdletBinding()]
    param([datetime]$Now = [datetime]::UtcNow)
    return ('{0}-{1}' -f $Now.ToString('yyyyMMdd-HHmmss-fff'), (New-Guid).ToString('N').Substring(0, 8))
}

function New-FailureBundle {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$InputPath,
        [Parameter(Mandatory)][string]$ServerRoot,
        [ValidateRange(10, 2000)][int]$TailLines = 200,
        [ValidateRange(10, 1000)][int]$ExcerptLines = 120,
        [datetime]$CreatedAtUtc = [datetime]::UtcNow
    )
    $root = (Resolve-Path -LiteralPath $ServerRoot).Path
    $resolvedInput = Resolve-FailureBundleSourcePath -Path $InputPath -ServerRoot $root
    if ($null -eq $resolvedInput) { throw "Failure-bundle input was not found beneath logs: $InputPath" }
    $references = Get-FailureBundleReferencedPaths -InputPath $resolvedInput -ServerRoot $root
    $bundleId = New-FailureBundleId -Now $CreatedAtUtc
    $bundleRoot = Join-Path $root 'logs\failure-bundles'
    New-Item -ItemType Directory -Path $bundleRoot -Force | Out-Null
    $bundlePath = Join-Path $bundleRoot $bundleId
    while (Test-Path -LiteralPath $bundlePath) {
        $bundleId = New-FailureBundleId
        $bundlePath = Join-Path $bundleRoot $bundleId
    }
    New-Item -ItemType Directory -Path $bundlePath | Out-Null

    $artifacts = [System.Collections.Generic.List[object]]::new()
    $copy = {
        param([AllowNull()][string]$Source, [string]$Name, [string]$Kind)
        if ([string]::IsNullOrWhiteSpace($Source) -or -not (Test-Path -LiteralPath $Source -PathType Leaf)) { return }
        $destination = Join-Path $bundlePath $Name
        if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite bundle artifact: $destination" }
        $text = Get-Content -LiteralPath $Source -Raw
        [IO.File]::WriteAllText($destination, (ConvertTo-FailureBundleRedactedText -Text $text), [Text.UTF8Encoding]::new($false))
        $artifacts.Add([pscustomobject][ordered]@{ kind = $Kind; source_path = $Source; bundle_path = $Name; redacted = $true })
    }
    & $copy $references.summary_path 'probe-summary.json' 'summary'
    & $copy $references.receipt_path 'probe-receipt.jsonl' 'receipt'

    $summary = $references.summary
    $records = @($references.records)
    $hints = @(Get-FailureBundleHints -Summary $summary -Records $records)
    $logs = [System.Collections.Generic.List[object]]::new()
    foreach ($logPath in @(Get-FailureBundleLogCandidates -ServerRoot $root -Summary $summary -Records $records)) {
        $base = [IO.Path]::GetFileName($logPath) -replace '[^A-Za-z0-9._-]', '_'
        $baseStem = $base
        $suffix = 1
        while ((Test-Path -LiteralPath (Join-Path $bundlePath "$base.tail.txt")) -or (Test-Path -LiteralPath (Join-Path $bundlePath "$base.excerpt.txt"))) {
            $base = "$baseStem-$suffix"
            $suffix++
        }
        $tailName = "$base.tail.txt"
        $excerptName = "$base.excerpt.txt"
        $tailLinesText = Get-FailureBundleLogTail -Path $logPath -MaxLines $TailLines
        [string[]]$allLines = @(Get-Content -LiteralPath $logPath -ErrorAction Stop | ForEach-Object { [string]$_ })
        $excerptText = Get-FailureBundleLogExcerpt -Lines $allLines -Hints $hints -MaxLines $ExcerptLines
        [IO.File]::WriteAllText((Join-Path $bundlePath $tailName), $tailLinesText + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $bundlePath $excerptName), $excerptText + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        $logs.Add([pscustomobject][ordered]@{ source_path = $logPath; tail_path = $tailName; excerpt_path = $excerptName; tail_line_bound = $TailLines; excerpt_line_bound = $ExcerptLines; redacted = $true })
    }

    $probeLines = foreach ($probe in @(Get-FailureBundleProperty $summary 'probes' @())) {
        $failure = Get-FailureBundleProperty $probe 'first_failure' $null
        if ($null -ne $failure) {
            "- $((Get-FailureBundleProperty $probe 'probe_id' '<unknown>')): $((Get-FailureBundleProperty $failure 'code' 'unknown')) - $((ConvertTo-FailureBundleRedactedText ([string](Get-FailureBundleProperty $failure 'detail' ''))))"
        }
    }
    if (@($probeLines).Count -eq 0) { $probeLines = @('- No structured first_failure entries were present.') }
    $status = [string](Get-FailureBundleProperty $summary 'status' 'UNKNOWN')
    $reason = [string](Get-FailureBundleProperty $summary 'reason' 'not supplied')
    $reportLines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in @(
        '# Probe failure bundle',
        '',
        "- Bundle: $bundleId",
        "- Created UTC: $($CreatedAtUtc.ToString('o'))",
        "- Input: $resolvedInput",
        "- Status: $status",
        "- Reason: $(ConvertTo-FailureBundleRedactedText $reason)",
        '',
        '## First failures',
        ''
    )) { $reportLines.Add([string]$line) }
    foreach ($line in @($probeLines)) { $reportLines.Add([string]$line) }
    foreach ($line in @(
        '',
        '## Evidence',
        '',
        "- Summary: $(if (Test-Path (Join-Path $bundlePath 'probe-summary.json')) { 'probe-summary.json' } else { 'not available' })",
        "- Receipt: $(if (Test-Path (Join-Path $bundlePath 'probe-receipt.jsonl')) { 'probe-receipt.jsonl' } else { 'not available' })",
        "- Log sources: $($logs.Count); each output is bounded and redacted.",
        '',
        'This bundle was created without bridge, database, server-process, or source-log mutation.'
    )) { $reportLines.Add([string]$line) }
    [IO.File]::WriteAllLines((Join-Path $bundlePath 'report.md'), $reportLines, [Text.UTF8Encoding]::new($false))
    $manifest = [pscustomobject][ordered]@{
        schema = 'autowow.probe-failure-bundle.v1'; bundle_id = $bundleId; created_at_utc = $CreatedAtUtc.ToString('o')
        input_path = $resolvedInput; source_kind = [IO.Path]::GetExtension($resolvedInput).TrimStart('.')
        redaction = [ordered]@{ applied = $true; patterns = @('password', 'token', 'secret', 'credential', 'authorization', 'bearer') }
        bounds = [ordered]@{ log_tail_lines = $TailLines; log_excerpt_lines = $ExcerptLines }
        safety = [ordered]@{ bridge_calls = 0; database_access = $false; source_log_mutation = $false; overwrite = $false }
        artifacts = @($artifacts); logs = @($logs); report_path = 'report.md'
    }
    [IO.File]::WriteAllText((Join-Path $bundlePath 'manifest.json'), ($manifest | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
    return [pscustomobject][ordered]@{ bundle_id = $bundleId; bundle_path = $bundlePath; manifest_path = (Join-Path $bundlePath 'manifest.json'); report_path = (Join-Path $bundlePath 'report.md'); manifest = $manifest }
}

if (-not [string]::IsNullOrWhiteSpace($InputPath)) {
    $resolvedRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
    New-FailureBundle -InputPath $InputPath -ServerRoot $resolvedRoot -TailLines $TailLines -ExcerptLines $ExcerptLines
}
