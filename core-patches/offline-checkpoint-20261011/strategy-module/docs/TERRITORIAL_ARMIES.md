# Territorial militia: NPC armies, playerbot heroes

The first C++ campaign model is implemented in TerritorialArmyPolicy.h. It is staged outside the active server
build graph and has no native database, creature spawn, navigation or combat adapter. Its executable pilot is an
offline domain test, not evidence of armies fighting in the running world. Playerbots remain heroes, craftsmen,
gatherers and questers; none is commandeered, copied, reset or granted equipment by this source slice.

## First militia profile

One persistent squad per faction protects two configured neighboring objectives. Territory and route IDs in the
test are deliberately synthetic 100/200 and9; no live Redridge ownership, native routes or NPC template is inferred.
An overlord can propose hold, patrol, defend, attack or escort orders. Route/actor adapters must verify actual
movement, escorts and arrival separately; setting an order does not move or teleport any unit.

| Input | Prototype effect |
|---|---|
| Barracks level1..10 | Capacity10 troops per level |
| Armory level0..5 | +1 attack and +1 armor per level |
| Issued equipment grade0..3 | +20 health, +3 attack and +1 armor per grade |
| Base militia | 100 health,10 attack,2 armor |

These are campaign points and bounded prototype building grades, not actual WoW creature stats, player level caps,
hero progression caps or ratified economic rates. Existing buildings and upgrade resource costs must be verified;
a caller cannot claim an upgraded armory for free. Equipment grade must bind an admitted profile and exact original
issued weapons/armor. Skilled city makers can produce those useful supplies through the existing workshop/contract
path. Quality colors or nominal item level alone do not prove the grade. Profiles remain immutable for this slice;
automatic retrofit, later squad specialization and native stat conversion remain to implement.

## Recruitment and physical stock

Muster admits a previously absent squad with unique durable troop IDs. Each recruit issues one configured weapon
and one armor kit from the owner's available stock, plus captured initial food. All three item entries must be
distinct; item accounts, owner, capacity, available counts and profile are checked. Other reserved goods stay intact.
The returned army/three-stock proposal is one atomic operation, not independently committed steps.

Weapons/armor leave available guild stock and are indexed as issued kit belonging to those troop identities; they
must not remain withdrawable in another bank. Rations leave available food stock into squad supply custody. This
kernel counts supplies; the native adapter must retain original GUID/stack metadata and provenance in the issuance
ledger, including casualty kit disposition. Neither issuance nor casualty creates another item or new loot. Native
loss, recovery and reissue require explicit reconciliation. No craft, skill, profession, character XP or free item
grant is represented here. NPC recruitment itself needs a protected lifecycle and deliberate authoritative NPC IDs.

Resupply transfers only available matching food to the squad. Orders require current action/target/route authority,
snapshot and profile checks. No order or resupply is accepted for an engaged squad; no retreat, other goal install,
profile upgrade or invisible dematerialization can evade the battle lock. Squad event and battle event sequences
are independent; squad revisions still change at battle entry/exit and invalidate earlier squad proposals.

## One engagement, one outcome

Battle start binds the exact battle ID, objective/version, two origins, owner, participating squads and resolution
mode. Both squads must have their own authorized attack/defend orders for that objective, different factions and
disjoint troop identities. One ration per living troop on each side is consumed at entry. Starting again while
either squad is engaged refuses. Global troop/battle uniqueness and cross-squad locking are mandatory durable
adapter checks; local revision checks do not replace them. Both squad locks, supply debit and battle insert commit
together, or none commits. Fresh authority is required again at settlement.

Strategic mode requires verified absence of physical actors for these exact units at entry AND resolution. Presence
unknown means hold. A battle started strategically cannot become physical when a hero arrives; the hero's presence
is separate and cannot reset or duplicate its outcome. A physical engagement is never also simulated strategically.
Before a later encounter starts, a qualified adapter can materialize survivors using one-to-one stable troop/NPC
mapping; it cannot manufacture a second army. Converting an already active engagement is deliberately unsupported.

The prototype strategic resolver uses simultaneous rounds. Each living side supplies attack; the opposing living
troops supply armor. Net damage has a minimum of1 and is applied in stable ID order, retaining wounded health and
zero-health casualty tombstones. The resolver stops at destruction or128 rounds. Only a surviving attacker with
no surviving defenders proposes capture. A draw, stalemate or mutual destruction preserves prior ownership.
This deliberately simple attrition model has no terrain/tactics/healer mechanics yet and needs balance testing.

Physical mode requires an original same-battle, once-claimable, authoritative full-roster terminal receipt and
objective-control proof. Every troop ID is preserved; missing, foreign or duplicated identities refuse. The first
militia adapter contract admits only damage, with no healing/resurrection between the captured baseline and outcome.
Healing, medics and casualty recovery need their own explicit provenance before broadening this rule. No timer,
proximity, encounter disappearance, death count or client assertion proves the terminal roster.

Surviving NPC troop records gain one campaign-experience point, saturating at65535; casualties retain their record.
Morale loses10 per casualty, floors at0, and surviving winners regain5 up to100. Experience and morale persist in
the proposal but do not alter combat power yet. This is NPC campaign bookkeeping, not native player XP, glory,
honor, reputation or boss legacy. There is no casualty replacement, healing, training timer or passive resupply.

The outcome proposes both final squads, a terminal battle and territory owner/version together. A changed owner,
stale version, changed squad, mode swap, max-counter overflow or replay refuses. Capture must first reconcile the
old owner's completed income periods through the existing treasury mechanism before actual ownership/income
commits. This policy creates no income, moves no bank stock on capture and awards heroes no synthetic encounter loot.

## Integration and living acceptance

Verification booleans are trusted action-specific INTERNAL adapter results, never user-supplied fields. They must
bind all current arguments, original resources, account versions, roster IDs, physical actor presence, exact routes,
ownership version and human/master/pause permissions. Native backend admission remains off until durable transactions,
once-only claims and failure recovery qualify. An interrupted operation stays held with its original locks/stock.

Next connect checked native issuance and roster persistence, configure two genuine objectives and one supply route,
map campaign points to admitted creature templates, and implement protected one-to-one NPC materialization. Add
native navigation/combat observations and terminal corpse/kit handling; then qualify the real capture/income handoff.
No default global monster AI takeover, arbitrary world spawns or playerbot master seizure is authorized by this model.

Acceptance: authorized recruitment → conserved original supplies → durable unique squad → actual route/objective
→ mode-locked battle → verified casualties → atomic squad/territory settlement → reconciled future owner income.
Keep strategic battle reports separate from actual physical travel, playerbot kills/deaths and earned encounter traits.
Later specialist troops, sieges, hero commands, mercenary guilds and regional crises build on that accountable loop.
