# soak-s2-80: 80 stock random bots (+6 scouts), PvP realm, perf fixes B1+B3 (main 3cbccb75).
# 32 Oracle-managed (12 prior allowlist + 20 random), rest stock = control. Legacy L80 bots kept as Northrend stratum.
pb|AiPlayerbot.MinRandomBots|80
pb|AiPlayerbot.MaxRandomBots|80
pb|AiPlayerbot.RandomBotMinLevel|5
pb|AiPlayerbot.RandomBotMaxLevel|60
pb|AiPlayerbot.RandomBotMaxLevelChance|0.02
pb|AiPlayerbot.RandomBotMaps|0,1,530,571
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
pb|AutoWow.OracleRuntime.BotGuids|"7,10,101,112,121,123,139,144,154,166,236,244,3,4,5,6,9,11,12,14,15,16,17,18,19,25,35,22,23,24,27,28"
pb|AutoWow.OracleRuntime.MaxBots|32
pb|AutoWow.OracleRuntime.SliceBots|8
pb|AutoWow.Ledger.Enable|1
pb|AutoWow.Ledger.RunId|"soak-s2-80-r1"
ws|GameType|1
ws|LogsDir|"/root/autowow-soak/logs"
ws|RecordUpdateTimeDiffInterval|60000
ws|MinRecordUpdateTimeDiff|0
ws|Logger.playerbots|4,Console Playerbots
ws|Appender.Ledger|2,4,1,ledger.log,a
ws|Logger.autowow.ledger|4,Ledger
