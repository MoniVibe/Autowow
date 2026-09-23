<#
.SYNOPSIS
  Renders the mod-playerbots quest avoid list (AutoWow.QuestAvoidIds / AutoWow.QuestAvoidFile).

.DESCRIPTION
  Default: reads probe\avoid-quest-ids.txt (build_registry.py output) and prints the config line
    AutoWow.QuestAvoidIds = "19,25,..."
  The AzerothCore config reader has no line-length limit (std::getline), so the inline form is valid;
  the file form keeps playerbots.conf readable.

  -Categories unavailable,family,flag   take ids from quest-registry.json instead, keeping a quest when
                                        any of its avoidReasons starts with one of these prefixes
                                        (prefixes: unavailable, family, flag, failing). The three
                                        structural ones exclude evidence-only `failing:*` entries.
  -OutFile <path>                       write the ids (one comma list) to a file and print
                                        AutoWow.QuestAvoidFile = "<ServerPath or OutFile>"
  -ServerPath <path>                    the path the worldserver reads (e.g. a /root/... WSL path)

  Windows PowerShell 5.1, ASCII only.
#>
param(
    [string[]]$Categories = @(),
    [string]$OutFile = "",
    [string]$ServerPath = ""
)

$ErrorActionPreference = "Stop"
$probe = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($Categories.Count -gt 0) {
    $Categories = @($Categories | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $registry = Get-Content -Raw -Path (Join-Path $probe "quest-registry.json") | ConvertFrom-Json
    $ids = @($registry.quests | Where-Object {
        $q = $_
        $q.avoid -and @($q.avoidReasons | Where-Object {
            $reason = $_
            @($Categories | Where-Object { $reason -eq $_ -or $reason.StartsWith($_ + ":") }).Count -gt 0
        }).Count -gt 0
    } | ForEach-Object { [uint32]$_.id })
} else {
    $text = Get-Content -Raw -Path (Join-Path $probe "avoid-quest-ids.txt")
    $ids = @($text -split "[,\s]+" | Where-Object { $_ } | ForEach-Object { [uint32]$_ })
}

$ids = @($ids | Sort-Object -Unique)
if ($ids.Count -eq 0) { throw "no quest ids selected" }
$list = ($ids -join ",")
[Console]::Error.WriteLine("render-avoid: $($ids.Count) ids, $($list.Length) chars")

if ($OutFile) {
    $full = [IO.Path]::GetFullPath($OutFile)
    [IO.File]::WriteAllText($full, "# AutoWoW quest avoid list, rendered by probe/render-avoid.ps1`n" + $list + "`n",
        (New-Object Text.ASCIIEncoding))
    $target = if ($ServerPath) { $ServerPath } else { $full }
    Write-Output ('AutoWow.QuestAvoidFile = "' + $target + '"')
} else {
    Write-Output ('AutoWow.QuestAvoidIds = "' + $list + '"')
}
