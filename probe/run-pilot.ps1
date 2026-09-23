# Pilot sequence (2026-09-23): one quest at a time on one fixture bot, 5-minute budget each.
param([int[]]$Quests = @(354, 356, 358, 427, 428, 477, 447, 422, 437, 369), [uint32]$Bot = 36, [string]$RunLabel = '')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($q in $Quests) {
    & (Join-Path $here 'probe-quest.ps1') -QuestId $q -BotGuid $Bot -Mode Probe -BudgetMinutes 5 -PollSeconds 30 -RunLabel $RunLabel
}
