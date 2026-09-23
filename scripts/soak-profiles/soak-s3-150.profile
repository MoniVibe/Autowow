# soak-s3-150: 150 stock random bots (+6 scouts), PvP realm, module main a8a61fb4 (B1/B3 + lane B fixes).
# 27 Oracle-managed (prior 12 + 15 sub-80 random), rest stock = control. Legacy L80 random bots rerolled once at login (setup).
# MapUpdateInterval 10 -> 50 (perf A/B vs S2 at 10; confounded by bot count, compare per-bot map-thread CPU).
pb|AiPlayerbot.MinRandomBots|150
pb|AiPlayerbot.MaxRandomBots|150
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
pb|AutoWow.OracleRuntime.BotGuids|"7,10,101,112,121,123,139,144,154,166,236,244,3,4,5,6,9,11,12,14,15,16,17,18,19,25,35"
pb|AutoWow.OracleRuntime.MaxBots|27
pb|AutoWow.OracleRuntime.SliceBots|8
pb|AutoWow.Ledger.Enable|1
pb|AutoWow.Ledger.RunId|"soak-s3-150-r1"
pb|AutoWow.Ledger.BlockedDedupeMs|60000
pb|AutoWow.QuestFullBagRelief.Enable|1
pb|AutoWow.PvpRealmZoneRules.Enable|1
pb|AutoWow.Soak.RerollAboveMaxLevel|1
pb|AutoWow.WarmQuestSpawnMap|1
ws|GameType|1
ws|MapUpdateInterval|50
ws|LogsDir|"/root/autowow-soak/logs"
ws|RecordUpdateTimeDiffInterval|60000
ws|MinRecordUpdateTimeDiff|0
ws|Logger.playerbots|4,Console Playerbots
ws|Appender.Ledger|2,4,1,ledger.log,a
ws|Logger.autowow.ledger|4,Ledger
