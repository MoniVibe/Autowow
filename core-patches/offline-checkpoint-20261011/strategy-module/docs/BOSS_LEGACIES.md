# Boss legacies: gradual power with encounter identity

The latest user direction adds a permanent primary-attribute bonus alongside each distinct flavored trait.
Raid bosses grant +5% attributes, with +10% for the configured final boss instead of +5%. Dungeon bosses grant
+1%, with +2% for the configured final boss instead of +1%. All earned traits remain active. There
is no loadout limit, daily progression limit or shared legacy power cap. Heroes become mythical through many
earned victories, rather than a single boss doubling their health. Native immunity and arithmetic validity still
apply; no new rule promises invincibility or disables ordinary game constraints.

## Attribute layer

Attributes mean Strength, Agility, Stamina, Intellect and Spirit. Each canonical encounter-family first victory
awards its configured amount once, across sizes, difficulties and resets. The final-boss amount replaces the
ordinary amount; a raid finale awards 10%, not 5% plus 10%. Councils and paired encounters settle once.

Add percentage points in the legacy layer against the current pre-legacy attribute baseline; never compound
each boss reward into the next. Ten Molten Core encounters yield 9 x 5% + 10% = +55% attributes. A three-boss
dungeon yields 1% + 1% + 2% = +4%. Gear and ordinary attribute buffs continue to update that baseline.
Stamina-derived health and other derived stats follow native calculations; there is no second blanket health,
damage or healing award. Rounding applies once at final native evaluation, not once per stored encounter.

The attribute layer stacks with the boss's original flavored trait. All earned contributions remain active,
without an overall power cap. Complete-raid cards include both the attribute sum and individual trait effects
exactly once. Completing the card does not grant another copy of the summed attribute bonus.

Final bosses are explicit encounter-family configuration, never the last JSON entry, the last killed mob or an
optional encounter's position. Current proposal selects Prince Malchezaar for Karazhan and Yogg-Saron for Ulduar;
Nightbane, Tenris and Algalon retain ordinary raid amounts. The independent Vault encounters use Toravon as the
configured tier finale. These are design assignments awaiting native encounter admission. Dungeon terminal
families need an explicit per-dungeon mapping before implementation; no dungeon reward is active.

## Flavor and units

A reward should express the boss's school, weapon discipline, signature pressure or tactical lesson. Ragnaros
is fire resistance, with +15 native resistance points as the new draft reference. Onyxia/Nefarian can teach
precision through small, distinct critical-strike traits. Sapphiron teaches frost endurance. Razuvious teaches
martial discipline. These themes are proposals, not assumed native effect IDs.

Ordinary drafts use a few flat resistance or combat-rating points, or a small conditional percentage increment.
Always state the unit: resistance points, combat-rating points, percentage points of critical chance, percentage
increment or seconds. Do not confuse a rating with a skill or chance. Critical rating changes effectiveness with
level; a percentage-point bonus has a different progression curve. A percentage multiplier on zero resistance
is not a useful resistance reward. Rare triggered effects need simple observable events and bounded per-trigger
behavior; cooldowns regulate an individual mechanic, not how many permanent legacies a player may earn.

The selected attribute package now accompanies each trait; no separate generic output package does. No cheat
death, automatic revival, immunity, free stock,
guaranteed victory or difficult-to-prove avoided-hazard trigger is selected. Two differently named copies of the
same generic proc are not two distinctive designs. Class-specific rewards can be specialized without rewriting
classes or granting missing skills. Keep direct native-compatible passives the first integration target.

## Persistent earning and additive application

Use one durable (character identity, canonical encounter family) key across size, difficulty, resets, guild and
faction changes. Councils/paired bosses award once after complete victory. Distinct rotating encounter variants
may have explicit distinct families; phases, council members and adds are not new awards.

Eligible tanking, healing, damage and support participation counts even if the character dies before victory.
Spectators, last-hit taggers, fixed probes and administrative fixtures do not qualify. Encounter evidence binds
the admitted native raid/instance/save and participant to a durable unique settlement. Historical gear alone is
not proof of an earlier kill. Concurrent callbacks and restart retries must not duplicate awards.

Sum compatible earned effects using a pre-legacy baseline. Recompute after relog, gear change or revival without
adding the same aura twice. Different conditions remain independent: a dragons-only critical bonus must not
become unconditional critical chance. A proc cannot recursively trigger another legacy proc or farm friendly,
self-inflicted or environmental events. No earned-progression cap is introduced by that event attribution rule.

The four-field BossLegacyPolicy.h is a prior eligibility/arithmetic scaffold, not the new catalogue adapter.
Its test magnitudes are fixtures, not selected rewards. Rating types, schools, conditions, native proc evidence
and any new save/load representation require a separate reviewed implementation; nothing is active today.

## Raid completion as consolidation

After all configured encounter families are personally earned, present a named complete-raid legacy containing
their exact sum and individual conditional traits. This is a presentation and evaluation projection of existing
awards, not another spendable or stat-granting ledger. It never adds the boss totals a second time. Before and
after consolidation, identical equipment/context must produce identical effective power.

Keep all individual encounter receipts, even if the character page shows one tidy raid card. Later corrections
or a missing optional trait must be traceable to its original source. Define the mandatory completion set apart
from rotating/event-only/optional extras; optional rewards remain active when earned without blocking a mandatory
raid card forever. Consolidation itself does not supply evidence for an unearned boss.

The faction's industrial first-clear knowledge remains a separate reward; artisans can use that knowledge while
staying in cities. Any genuinely additional raid-completion perk must be identified separately and awarded once;
none is implied by summing existing bonuses. The revised era proposals may suggest such perks for later selection.

## Qualification sequence

Select one admitted conservative native-compatible trait, verify encounter participants, then settle the personal
award durably. Prove the effect through restart, equipment and death while respecting class and native school
rules. Expand the catalogue and add raid consolidation; test that it leaves power unchanged. Persist all earned
traits without loadout or progression caps. Competitive PvP treatment remains an explicit later decision.

This documentation changes source direction only. No native hook, stat mutation, schema, build, configuration,
control, restart or release is performed. The selected supported update and running server remain separate.
