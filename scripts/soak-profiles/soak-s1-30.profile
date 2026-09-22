# soak-s1-30: 36 stock random bots (levels 5-60, old world) + 6 managed scouts.
# Level/gear/first placement = SETUP. Later randomize/teleport/revive = contamination.
pb|AiPlayerbot.MinRandomBots|36
pb|AiPlayerbot.MaxRandomBots|36
pb|AiPlayerbot.RandomBotMinLevel|5
pb|AiPlayerbot.RandomBotMaxLevel|60
pb|AiPlayerbot.RandomBotMaxLevelChance|0.02
pb|AiPlayerbot.RandomBotMaps|0,1,530
pb|AiPlayerbot.MinRandomBotRandomizeTime|2592000
pb|AiPlayerbot.MaxRandomBotRandomizeTime|2592000
pb|AiPlayerbot.MinRandomBotTeleportInterval|2592000
pb|AiPlayerbot.MaxRandomBotTeleportInterval|2592000
pb|AiPlayerbot.MinRandomBotReviveTime|1800
pb|AiPlayerbot.MaxRandomBotReviveTime|2400
pb|AiPlayerbot.MinRandomBotInWorldTime|2592000
pb|AiPlayerbot.MaxRandomBotInWorldTime|2592000
pb|AiPlayerbot.BotActiveAlone|100
pb|AiPlayerbot.botActiveAloneSmartScale|0
ws|LogsDir|"/root/autowow-soak/logs"
ws|RecordUpdateTimeDiffInterval|60000
ws|MinRecordUpdateTimeDiff|0
ws|Logger.playerbots|4,Console Playerbots
pb|AutoWow.Ledger.Enable|1
pb|AutoWow.Ledger.RunId|"soak-s1-30-r1"
ws|Appender.Ledger|2,4,1,ledger.log,a
ws|Logger.autowow.ledger|4,Ledger
