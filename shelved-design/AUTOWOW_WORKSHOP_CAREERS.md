# S90 shared workshop careers — read-only design

## Finding

The smallest safe model is **many persistent worker profiles, short-lived workshop jobs, and native-gate careers**. A dedicated house worker and an outside paid contractor use the same recipe/material/craft receipt path; they differ only in job source, priority, and settlement. Configuration no longer makes a character a permanently parked artisan.

Current code cannot express this. `gArtisan[team]` and `gLineArtisan[line][team]` hold one GUID, `RoleInfo` holds one line/gear assignment, and `Working()` accepts only that configured active role. `ActiveRoleOf()` turns every configured artisan into a permanent role at global `ArtisanMinLevel` 10. `NewRpgSupply::SupplyStep()` then forces Idle/home and explicitly prevents quests, grind, errands, and contracts forever. This blocks the real character levels needed for later profession ranks and specializations.

## Shared worker and job model

`WorkerProfile` is versioned and keyed by the character GUID: faction, home guild, allowed production lines, dedicated/shared status, planned primary professions, and optional intended specialization spell. It never moves the character between guilds. `WorkerFacts` is observed live: level, paid profession ranks/skill, known recipes, specialization spell plus provenance, tools, location, and current AI ownership.

`WorkshopJob` is the common executable unit: never-reused job ID, source (`home_order` or `paid_contract`), requester guild, recipe/output/count, required specialization, material custody, fee, station route, deadline, and receipt state. The already accepted S89 contract design owns cross-guild money/COD/refund rules; this model supplies its worker selection and career state rather than creating a second contract system.

Selection is deterministic:

1. A qualified, available dedicated worker of the requesting guild gets first refusal.
2. Another qualified same-guild worker may take the job.
3. If neither accepts before a short offer deadline, publish the paid job to qualified outside-guild workers of the same faction.
4. Outside workers may accept only when they have no earlier home job. One active lease per worker; ties use fewest settled jobs in the fairness window, oldest last win, then GUID.

`ActiveRoleOf()` should mean **rep or currently leased workshop worker**, not “configured artisan above level 10.” An unleased worker remains ordinary NPC AI and may quest, party, grind, or run errands. A lease starts only after explicit eligibility/acceptance and ends on settlement, finite failure, or pre-custody preemption. This lets several own specialists and outside contractors coexist without parking the background population.

## Career transitions from native gates

Remove the global graduation threshold. At startup, derive `NextRankGate` from loaded trainer spells that actually grant the worker's planned skill line: required character level, current profession skill, price, trainer, and resulting rank cap. The fresh DB read confirms the ordinary paid ladder: Apprentice level 5; Journeyman skill 50/level 10; Expert 125/20; Artisan 200/35; Master 275/50; Grand Master 350/60 for Blacksmithing in this DB and 350/65 for Leatherworking, Alchemy, Tailoring, and Engineering. Costs rise from 10 copper to 35 gold. Do not hardcode the current `ReachSkill()` approximation, which treats every Grand Master gate as level 65.

The career state is derived, not freely awarded:

- **Adventure:** current skill/rank is capped by a higher character-level gate, or no real workshop job can advance the profession. Release the role lease and let normal quest/combat XP raise the character.
- **Train:** level and skill meet the next loaded trainer gate and the worker/house can pay. Use the existing trainer trip and observe the paid rank afterward.
- **Practice/work:** a real demand job or bounded material-funded skill-up uses a known native recipe. Profession skill rises only through the native cast.
- **Qualify:** an intended specialization has reached its real level/skill gate. Invoke the native quest/gossip path documented in S89 and record observed provenance; never factory-cast it.
- **Available:** rank/spec/recipe is sufficient. The worker stays in ordinary AI until a home or paid job lease arrives.

Current supply delivery calls `GiveXP` for crafted items, with zero configuration meaning a computed default rather than off. Career progression must not rely on that synthetic character XP. Dedicated workers should earn required character levels through ordinary gameplay; gate or remove work XP for the career path while retaining real gold pay and native profession skill-ups.

## Outside supply evidence

The fresh read-only census found 24 characters with persisted specialization spells, 16 currently online, and **0 in the ten craft-house guilds**. This shows an outside candidate pool, not legitimate coverage. It proves only stored known spells: native acquisition and willingness were not observed, and one level-16 character carries Shadoweave despite the native level-60 gate. Mark legacy rows `known_unproven`; they may not satisfy a specialization-required job or count toward faction coverage until native provenance is observed. Ordinary recipes can still qualify independently through current recipe, skill, and item checks.

## Phases and likely write set

1. **Career policy:** add `WorkshopCareerPolicy.h` plus focused tests for dynamic rank gates, modes, lease eligibility, selection, fairness, and provenance. Preserve the current single artisan as a one-entry compatibility profile.
2. **Multiple own workers:** in `SupplyPolicy.h` / `AutoWowSupply.cpp`, replace scalar artisan GUIDs with per-line/team worker vectors and job leases; aggregate stock/demand once, then assign one job once. Change `NewRpgSupply.cpp` from permanent-role control to lease-bound station work and native-gate travel/training. Update `PlayerbotAIConfig.*`, `AutoWowTrainPolicy.h`, and `conf/playerbots.conf.dist` to parse multiple worker GUIDs and numeric specialization intent while retaining paid profession plans.
3. **Shared contracts:** connect the accepted `AutoWowCraftContracts` design to the same worker registry/job executor and exact-craft receipt; do not duplicate recipe selection, custody, or craft authority.
4. **Legitimate specialists:** add native qualification receipts from the S89 specialization path, then expand worker profiles by observed demand per faction. Known-only legacy specialists remain contractor-ineligible for gated recipes.

Likely collateral checks are `PartyRuntime.cpp`, `NewRpgMarket.cpp`, `NewRpgSelfCraft.cpp`, and dungeon/probe gates because they currently interpret `ActiveRoleOf()` as permanent exclusion. Append worker/job/career fields to the existing ledger; keep GUIDs and spell/line IDs as string-free canon and bump state versions.

**Risks:** two workers consuming one order, home work starvation by outside jobs, material double reservation, lease surviving logout/restart, a gate inferred from a recipe instead of a rank spell, legacy free specialization contamination, and synthetic work XP silently satisfying level gates. Each needs a deterministic test and a live receipt before broad coverage is claimed.

**Status:** design only at source HEAD `fce1f33bc08c9f65a2db212698f3785ea493a44e`. No source, configuration, build, runtime, database, bot, or Git action was performed. Implementation must remain serialized behind the current S90/S91 runtime work and the Smith line cutover.
