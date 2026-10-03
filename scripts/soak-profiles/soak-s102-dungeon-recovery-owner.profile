# S83: paired supported core/module upstream plus four reviewed staging loot fixes; all gameplay parameters retained.
# S79 combat progress: S78 parameters; productive combat watchdog and bounded cohort arming retry.
# soak-s77-full: S76 + brewtiers (PotionTiers) + outland (ZoneProgression.Outland) + mounts (Errands.Mounts) + cohort dungeons Maraudon/Dire Maul.
# soak-s76-full: S75 + dgates8 (Maraudon lake, Dire Maul East) + dparty5 (GatherPortal 60s) + craftflow (RouteBagExtra, GearStockSell, GearSkillupRestock) + TaxPct 20.
# soak-s75-full: S74 + dparty4 (RecruitTankOverLevel 10).
# soak-s74-full: S73 + dparty3 (RecruitWidePool=1, RecruitMaxParties=2) + probe RunTimeoutMs 90 min.
# soak-s73-full: S72 + Party.LevelSpread 6 (recruit windows) + probe rotation starts at Maraudon/DireMaul/BRD/Strat.
# soak-s72-full: S71 + dparty2 (RequireTank, RecruitStragglerWalk) + dgates7 (RFD legs).
# soak-s71-full: S70 + dparty (Dungeon.Recruit=1) + dgates6 (Sunken Temple legs).
# soak-s70-full: S69 + cohort Dungeon.List adds probe-proven RFD 37-46, Uldaman 41-51, ZF 44-54.
# soak-s69-full: S68 + dgates4/5 (RFD spiral leg, Uldaman set-asides, ZF area trigger, probe set-aside scoring).
# soak-s68-full: S67 + gearflow (Gear.Flow=1) + probe rotation starts with unproven dungeons (RFD, Uldaman, ZF, ST, Maraudon, DM, BRD, Strat).
# soak-s67-full: S66 + ahgear (AuctionUpgrades=1) + cohort Dungeon.List adds Gnomeregan 29-38 and RFK 30-40 (probe-proven).
# soak-s66-full: S65 + outfit2 (OutfitGear=1) + restsafe (RestSafe=1) + Tactics.Enable=1 (ArmPct 50 A/B).
# soak-s65-full: S64 + squadzone2 (sticky bench) + dgates3 (raw swim, LOS approach, party escort defence).
# soak-s64-full: S63 + lane dgates2 (escort defend, gate straggler walk, BFD Sarevess leg).
# soak-s63-full: S62 + gatherscale (AnySkill/YieldScale/MultiGain, Craft.MultiGain, TrainRuns) + squadzone (LevelWindow 2).
# soak-s62-full: S61 + lane wcroute (Wailing Caverns curated legs, direct steps).
# soak-s61-full: S60 + lane tinkers2 (EngGuns=1).
# soak-s60-full: S59 + lanes dmroute (curated Deadmines cove legs) + bootstrap (GearBootstrap=1).
# soak-s59-full: S58 + lane dnav4 (water-surface slope-free retry, neutral bosses attackable).
# soak-s58-full: S57 + lane dnav3 (slope-free route-leg retry, cliff route penalty).
# soak-s57-full: S56 + lane s56fix (gate straggler walk, done-before-key, cast logs, own-line supply rows).
# soak-s56-full: S55 + lane s55fix (gate slope-free approach, artisans go home first).
# soak-s55-full: S54 + gather-only adventurers (owner ruling 2026-09-27: cohort questers MH/MS/HS, crafting in house artisans; AutoWow.Professions.DropOffPlan) + AutoWow.DungeonNav.Gates.
# soak-s54-full: S53 + lane dnav2 (probe strand reset, leader no re-anchor, direct hop, faction-aware rotation).
# soak-s53-full: S52 + Tanners artisans (72755/72756, leather_gear), cloth/leather gear lines DemandOnly (squad first), Unstick.V2, DungeonNav.ConvoyV2.
# S52: S51 economy + dungeon probe parties (Alliance 72385-72389, Horde 72390-72394; magic-geared test instruments) looping classic dungeons.
# 27 Oracle-managed (prior 12 + 15 sub-80 random), rest stock = control. Legacy L80 random bots rerolled once at login (setup).
# MapUpdateInterval 10 -> 50 (perf A/B vs S2 at 10; confounded by bot count, compare per-bot map-thread CPU).
pb|AiPlayerbot.MinRandomBots|100
pb|AiPlayerbot.MaxRandomBots|100
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
pb|AutoWow.Ledger.RunId|"soak-s102-dungeon-recovery-owner-r1"
pb|AutoWow.Ledger.BlockedDedupeMs|60000
pb|AutoWow.QuestFullBagRelief.Enable|1
pb|AutoWow.PvpRealmZoneRules.Enable|1
pb|AutoWow.Soak.RerollAboveMaxLevel|1
pb|AutoWow.WarmQuestSpawnMap|1
ws|GameType|1
ws|MapUpdateInterval|50
ws|LogsDir|"/root/autowow-soak/logs"
ws|DataDir|"/root/autowow-upstream-s83-data"
ws|Console.Enable|0
ws|Updates.EnableDatabases|0
ws|RecordUpdateTimeDiffInterval|60000
ws|MinRecordUpdateTimeDiff|0
ws|Logger.playerbots|4,Console Playerbots
ws|Appender.Ledger|2,4,1,ledger.log,a
ws|Logger.autowow.ledger|4,Ledger
pb|AutoWow.OracleRuntime.DeferNoCandidatePasses|5
pb|AutoWow.OracleRuntime.NoRandomTeleport|1
pb|AutoWow.Independent.AutoMaintenance|2
pb|AutoWow.Chat.BotSpeakerByAI|1
pb|AutoWow.Chat.MinReplyIntervalMs|5000
pb|AutoWow.QuestTravelProgressWatch.Enable|1
pb|AutoWow.QuestBlockedDefer.Enable|1
pb|AutoWow.QuestItemTargetConditions.Enable|1
pb|AutoWow.Ledger.ProgressSampleMs|60000
pb|AutoWow.CombatTelemetry.Enable|1
pb|AutoWow.CombatTelemetry.LogIntervalMs|60000
pb|AutoWow.Combat.DotLifetimeGate|1
pb|AutoWow.Professions.Enable|1
pb|AutoWow.Professions.TrainOnArrival|1
pb|AutoWow.Professions.CraftPriorityFix|1
pb|AutoWow.Ledger.SkillUp|1
pb|AutoWow.Professions.Assignments|"62955:186,182;62956:182,393;62957:186,393;62958:186,393;62959:182,393;62960:186,164;62961:186,182;62962:182,393;62963:186,393;62964:197,333;62965:182,393;62966:186,182;62967:186,182;62968:182,393;62969:186,393;62970:186,393;62971:182,393;62972:182,393;62973:186,182;62974:186,182;62975:186,393;62976:186,393;62977:186,182;62978:182,393;62979:186,182;62980:182,393;62981:186,393;62982:186,182;62983:186,164;62984:186,393;62985:186,182;62986:186,393;62987:186,182;62988:186,393;62989:182,393;62990:186,393;62991:182,393;62992:186,182;62993:186,182;62994:182,393;62995:186,182;62996:182,393;62997:197,333;62998:182,393;62999:186,182;63000:186,393;63001:182,393;63002:186,393;63003:182,393;63004:186,182;70573:186,182;70574:186,393;70575:182,393;70576:393,186;70763:182,186;70578:186,182;70579:182,393;70580:393,186;70581:186,182;70764:182,393;70577:171,182;70582:171,182;72755:165,393;72756:165,393;73298:202,186;73300:202,186"
pb|AutoWow.TravelIntent.Enable|1
pb|AutoWow.QuestScheduler.Enable|1
pb|AutoWow.QuestAvoidFile|"/mnt/d/Games/wowstuff/AutoWoW/probe/avoid-quests.cfg.txt"
pb|AutoWow.DeathLoop.Enable|1
pb|AutoWow.Tactics.Observe|1
pb|AutoWow.Tactics.Enable|1
pb|AutoWow.Tactics.ArmPct|50
pb|AutoWow.Tactics.PriestShadowLevelingSpec|1
pb|AutoWow.ZoneProgression.Enable|1
pb|AutoWow.Transports.Enable|1
pb|AutoWow.ZoneProgression.PortalAfterMs|720000
pb|AutoWow.Errands.Enable|1
pb|AiPlayerbot.BotCheats|"raid"
pb|AutoWow.DeathLoop.EscapeViaZoneProgression|1
pb|AutoWow.Survival.PullLevelCap|1
pb|AutoWow.Survival.RestGate|1
pb|AutoWow.Survival.KeepConsumables|1
pb|AutoWow.DeathLoop.V2|1
pb|AutoWow.QuestScheduler.PreferGearRewards|1
pb|AutoWow.Walking.V2|1
pb|AutoWow.Travel.Safe|1
pb|AutoWow.Survival.SafeRevive|1
pb|AutoWow.Survival.Unstick|1
pb|AiPlayerbot.RpgStatusProbWeight.TravelFlight|0
pb|AiPlayerbot.RpgStatusProbWeight.WanderNpc|2
pb|AiPlayerbot.RpgStatusProbWeight.GoCamp|0
pb|AiPlayerbot.RpgStatusProbWeight.Rest|0
pb|AiPlayerbot.RpgStatusProbWeight.OutdoorPvp|2
pb|AiPlayerbot.RpgStatusProbWeight.GoGrind|30
pb|AiPlayerbot.RpgStatusProbWeight.WanderRandom|30
pb|AutoWow.Travel.FlightTrapFix|1
pb|AutoWow.Travel.VerticalSnap|1
pb|AutoWow.Tactics.Classes|"priest,warlock,hunter,warrior,rogue,mage,paladin"
pb|AutoWow.ZoneProgression.HighRoutes|1
pb|AutoWow.Gathering.Detours|1
pb|AutoWow.Trade.Enable|1
pb|AutoWow.Ledger.Treasury|1
pb|AutoWow.Gear.Upgrades|1
pb|AutoWow.Survival.PackAvoid|1
pb|AutoWow.Survival.SafeRevive.V2|1
pb|AutoWow.Tactics.ObserveClasses|"priest,warlock,hunter,warrior,rogue,mage,paladin,druid,shaman"
pb|AutoWow.Survival.HardEscape|1
pb|AutoWow.Party.Enable|1
pb|AutoWow.Party.Roles|1
pb|AutoWow.Dungeon.Enable|1
pb|AutoWow.Errands.AuctionDetourMs|180000
pb|AutoWow.Survival.HardEscape.ClusterDeaths|8
pb|AutoWow.Dungeon.PortalFallback|1
pb|AutoWow.Dungeon.StageTimeoutMs|300000
pb|AutoWow.Dungeon.MinSize|4
pb|AutoWow.Party.LevelSpread|6
pb|AutoWow.Dungeon.List|"389:15:20:H,36:19:26:A,43:19:26:AH,33:20:26:AH,48:22:30:AH,34:24:32:A,90:29:38:AH,47:30:40:AH,129:37:46:AH,70:41:51:AH,209:44:54:AH,349:46:55:AH,429:55:60:AH,543:61:67:AH"
pb|AutoWow.Errands.SellTradeGoods|1
pb|AutoWow.Party.HubYards|3000
pb|AutoWow.Party.SeparatedMs|1200000
pb|AutoWow.Survival.CorpsePortal|1
pb|AutoWow.Contracts.Enable|1
pb|AutoWow.Contracts.LevelBelow|5
pb|AutoWow.Contracts.LevelAbove|0
pb|AutoWow.Contracts.LevelGapMin|0
pb|AutoWow.Contracts.StallMs|480000
pb|AutoWow.Guilds.Enable|1
pb|AutoWow.Guilds.Rep.Weavers.Alliance|69665
pb|AutoWow.Guilds.Rep.Smiths.Alliance|69666
pb|AutoWow.Guilds.Rep.Tanners.Alliance|69667
pb|AutoWow.Guilds.Rep.Brewers.Alliance|69668
pb|AutoWow.Guilds.Rep.Weavers.Horde|69669
pb|AutoWow.Guilds.Rep.Smiths.Horde|69670
pb|AutoWow.Guilds.Rep.Tanners.Horde|69671
pb|AutoWow.Guilds.Rep.Brewers.Horde|69672
pb|AutoWow.Supply.Enable|1
pb|AutoWow.Supply.RouteCloth|1
pb|AutoWow.Supply.Artisan.Weavers.Alliance|62964
pb|AutoWow.Supply.Artisan.Weavers.Horde|62997
ws|MailDeliveryDelay|60
pb|AutoWow.Guilds.Levy|1
pb|AutoWow.Supply.Tiers|1
pb|AutoWow.Supply.Market|1
pb|AutoWow.Supply.BuyBudget|2000
pb|AutoWow.Supply.Products|"bags,potions,cloth_gear,mail_gear,leather_gear,eng"
pb|AutoWow.Supply.RouteHerbs|1
pb|AutoWow.Supply.Artisan.Brewers.Alliance|70577
pb|AutoWow.Supply.Artisan.Brewers.Horde|70582
pb|AutoWow.SelfCraft.Enable|1
pb|AutoWow.Guilds.CohortGuids|"62955-63004,70573-70582,70763-70764,72755-72756,73298,73300"
pb|AutoWow.Squad.Enable|1
pb|AutoWow.Squad.Alliance|"70573,70574,70575,70576,70763"
pb|AutoWow.Squad.Horde|"70578,70579,70580,70581,70764"
pb|AutoWow.Supply.RouteRaw|1
pb|AutoWow.Supply.Outfit|1
pb|AutoWow.Supply.OutfitMaxCopper|2000
pb|AutoWow.ClassQuests.Enable|1
pb|AutoWow.Quests.NeutralNpcs|1
pb|AutoWow.Supply.DirectRoutes|1
pb|AutoWow.Supply.MailPickup|1
pb|AutoWow.Market.RandomSellers|1
pb|AutoWow.Market.MailOrders|1
pb|AutoWow.DungeonProbe.Enable|1
pb|AutoWow.DungeonProbe.Parties|"alliance:72385,72386,72387,72388,72389;horde:72390,72391,72392,72393,72394"
pb|AutoWow.DungeonProbe.Loops|0
pb|AutoWow.FixtureGuids|"2,8,21,24,25,26,36,41,45,60,68,72,83,16472,16473,16474,16475,16476,16477,16478,16479,16480,16481,16482,16483,16484,16485,16486,16487,16488,16489,16490,16491,17392,17393,17394,17395,17396,17397,17398,17399,17400,17401,17402,17403,17404,17405,17406,17407,17408,17409,17410,17411,19032,19033,72385,72386,72387,72388,72389,72390,72391,72392,72393,72394"
pb|AutoWow.Supply.Artisan.Tanners.Alliance|72755
pb|AutoWow.Supply.Artisan.Tanners.Horde|72756
pb|AutoWow.Supply.DemandOnly|1
pb|AutoWow.Supply.PriorityGuids|"70573,70574,70575,70576,70763,70578,70579,70580,70581,70764"
pb|AutoWow.Unstick.V2|1
pb|AutoWow.DungeonNav.ConvoyV2|1
pb|AutoWow.DungeonNav.Gates|1
pb|AutoWow.Professions.DropOffPlan|1
pb|AutoWow.Guilds.Houses|"Tinkers:202;Weavers:197,333;Smiths:186,164;Tanners:393,165;Brewers:182,171"
pb|AutoWow.Guilds.Rep.Tinkers.Alliance|73297
pb|AutoWow.Guilds.Rep.Tinkers.Horde|73299
pb|AutoWow.Supply.Artisan.Tinkers.Alliance|73298
pb|AutoWow.Supply.Artisan.Tinkers.Horde|73300
pb|AutoWow.Supply.House.Ore|"Tinkers"
pb|AutoWow.Supply.GearBootstrap|1
pb|AutoWow.Supply.EngGuns|1
pb|AutoWow.Gather.AnySkill|1
pb|AutoWow.Gather.YieldScale|1
pb|AutoWow.Gather.MultiGain|1
pb|AutoWow.Craft.MultiGain|1
pb|AutoWow.Professions.TrainRuns|1
pb|AutoWow.Squad.LevelWindow|2
pb|AutoWow.Supply.OutfitGear|1
pb|AutoWow.Survival.RestSafe|1
pb|AutoWow.Gear.AuctionUpgrades|1
pb|AutoWow.Gear.Flow|1
pb|AutoWow.DungeonProbe.Dungeons|"349,429,230,329,109,129,70,209,90,48,47,43,36,389"
pb|AutoWow.Dungeon.Recruit|1
pb|AutoWow.Dungeon.RequireTank|1
pb|AutoWow.Dungeon.RecruitStragglerWalk|1
pb|AutoWow.Dungeon.RecruitWidePool|1
pb|AutoWow.Dungeon.RecruitMaxParties|2
pb|AutoWow.DungeonProbe.RunTimeoutMs|5400000
pb|AutoWow.Dungeon.RecruitTankOverLevel|10
pb|AutoWow.Dungeon.RecruitGatherPortalMs|60000
pb|AutoWow.Supply.RouteBagExtra|1
pb|AutoWow.Supply.GearStockSell|1
pb|AutoWow.Supply.GearSkillupRestock|1
pb|AutoWow.Guilds.TaxPct|20
pb|AutoWow.Supply.PotionTiers|1
pb|AutoWow.ZoneProgression.Outland|1
pb|AutoWow.Errands.Mounts|1

pb|AutoWow.Supply.Artisan.Smiths.Alliance|62960
pb|AutoWow.Supply.Artisan.Smiths.Horde|62983

pb|AutoWow.SelfCraft.Smelting|1

pb|AutoWow.ZoneProgression.Northrend|1
