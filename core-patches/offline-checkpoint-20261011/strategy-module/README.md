# mod-autowow-strategy

Native AzerothCore module source for AutoWoW's strategic world layer. GPLv2-or-later original implementation.

The initial slice is **observation only**, default off. It provides native `WorldScript` and `PlayerScript` hooks,
not a browser simulation. It has no dependency on mod-playerbots' private headers. Existing core code is preserved;
the knowledge schema is staged as an unapplied pending characters migration.
It targets the selected custom Playerbot core, whose headless-session API supports bots sharing an account.

`StrategyPolicy.h` contains bounded income-period settlement planning and the initial glory eligibility/decay
policy. `StrategyScripts.cpp` registers `Addmod_autowow_strategyScripts`, observes due periods and eligible native
PvP kill events. Settings are cached at startup/reload; disabled cost is cached flag reads. Map-thread kill
callbacks do not query databases or retain Player pointers. Preview caches are bounded, mutex-protected and
cleared by the world thread on a new UTC day.

`KnowledgePolicy.h` now prepares immutable discovery intents from verified completion/participation and captured
character/guild/faction entitlement. Unrun regression sources cover invalid identities, probes, captured membership
and durable settlement classification. KnowledgeRepository prepares a private asynchronous backend; its default
transaction admission is false pending isolated native failure/lifecycle tests of its private checked adapter.
The adapter checks BEGIN/body/COMMIT, forbids statement reconnect/replay and quarantines uncertain sessions.
Six sequencing-test families passed. Eight task/native-queue scenarios now cover execution, exception,
cancellation, shutdown and queue reset; no connection, pool or DB ran. Final native-header syntax passed.
Private connections now own separate descriptions to prevent native socket normalization leaking into siblings;
linked copy/lifetime and no-query native worker execution/join checks passed, with no Open/DB call.
No discovery hooks,
pool instance, recipe learning or persistence are active; see `docs/DISCOVERIES.md`.

`TreasuryPolicy.h` implements conserved per-resource production/overflow, guild reservation, partial delivery,
workshop consumption and upgrade investment proposals, refusing invalid balances, overspending and integer overflow.
It returns proposed states only; persistent event identity and atomic commits remain the repository adapter's job.

`BankPolicy.h` adds explicit faction/guild owners, per-item or copper balances, reserved stock, authorized transfers
and two-sided item-for-money or barter deals. Both outgoing legs reserve together; settlement or cancellation is
proposed atomically, with expiry, capacity, authority and state checks. Native guild banks remain authoritative for
physical goods and money. See `docs/BANKS.md` for the durable and physical handoff boundary; no adapter is active yet.

`GuildPolicy.h` adds member-deposit/representative-withdrawal eligibility under native permissions, plus useful
donation quantities against confirmed remaining demand. It awards no standing and enforces no live rank change.
`docs/CAMPAIGNS.md` connects guild contribution to future depot raids, capture income and supply protection.

`MasterworkPolicy.h` prepares one-time native-property eligibility for an exact successful city craft, captured
skill/specialty and bounded output power. It admits no real effect catalogue and changes no item. The mercenary
guild contract and notable-maker direction is described in `docs/MERCENARIES_MASTERWORKS.md`.

`WorkshopPolicy.h` prepares timed city work, sequential bulk orders and five-stage legendary commissions.
Only observed eligible work advances a proposed cursor; timers create no goods or skill. Each piece needs its own
authoritative native craft settlement. `docs/WORKSHOPS.md` defines durable queue/reservation/recovery contracts.
Five isolated workshop-policy test families passed, including bulk sequencing and five-stage pacing.
No scheduler, repository, timing catalogue or native craft adapter is active; native production is untested.

`SupplyContractPolicy.h` prepares funded guild commissions with owned copper holds, registered native shipment
intents, verified partial delivery/payment, expiry and cargo resolution. `CaravanPolicy.h` prepares immutable physical
manifests and sequential native route custody. Isolated contract/caravan/BankPolicy tests passed; full-module and
native bank/actor/route integration remain inactive. See `docs/SUPPLY_CONTRACTS.md` for the exact proof boundaries.

`TerritorialArmyPolicy.h` implements the first militia campaign proposals: stock-backed muster/resupply, persistent
troop health/casualties, overlord orders, immutable building/equipment profiles and exclusive two-sided battle modes.
The executable two-objective pilot exercises strategic attrition/capture and physical outcome refusals. It spawns no
NPCs and does not control playerbots; durable native issuance, actors/routes and combat remain inactive. See
`docs/TERRITORIAL_ARMIES.md` for prototype points, authority, conservation and the native acceptance chain.

`BossLegacyPolicy.h` prepares once-per-character/boss-family reward eligibility and additive permanent power
proposals. `docs/BOSS_LEGACIES.md` selects flavored traits plus +5% attributes per raid boss/+10% at its finale,
and +1% per dungeon boss/+2% at its finale. All earned rewards stay active and add without compounding.
Raid completion consolidates the sum without re-awarding it. Separate huge health/output packages are superseded.
The kernel needs a typed trait adapter; no encounter catalogue, durable first-kill ledger or stat effect is active.

No current code grants actual glory, honor, reputation, guild standing, materials, items, money, XP or skills.
No actor is ordered into combat. The session-local preview cache is not durable reward state; restarts reset it.
The unordered opponent pair shares its10/5/2 preview allowance across reciprocal kills, with a10-minute interval
and a50-point per-player/day preview cap. These are observation defaults, not a ratified seasonal score.
This does not identify all collusion, bot masters or human-controlled bots and must not authorize real rewards.

Territory hooks currently emit **timing receipts only**, with a cursor initialized at startup. Ownership, regional
catalogues, actual inventories, atomic durable treasury/allocation and native deliveries are the next implementation
slice. The pure income policy requires a caller-supplied durable cursor for real settlement; no such adapter exists
yet. Resource production is never represented as native gathered loot.

For future native integration, place the directory at `modules/mod-autowow-strategy` in the selected core tree.
The native source collector uses `src`, the loader function matches that directory, and `conf/*.conf.dist` follows
the normal configuration mechanism. **Do not independently configure/build or copy over the live executable.**
Integrate this module into a newly qualified source/build tuple after the supported update's pending release gates.
Current source is staged at `/root/autowow-strategy-module`, outside that build graph, preserving update custody.

Static codestyle/API checks are recorded in task receipts. Policy tests are provided outside runtime `src`;
militia, contract, caravan and inherited bank suites have isolated compile/run evidence. Other suites retain their
individual recorded qualification. No server build, migration, configuration activation, live script observation
or deployment is claimed.

See `.agents/plans/strategic-world/strategic-world.PLAN.md` for the economy, territory and glory development plan.
