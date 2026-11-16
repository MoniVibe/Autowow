# AutoWoW mercenaries and notable craftsmen — October 9

The user selected dedicated bodyguard/mercenary guilds or fighters supplied by adventuring/raiding guilds, plus
skill-dependent one/two-affix masterworks that make individual city craftsmen valuable. Root developed the native
masterwork eligibility source and recorded both gameplay designs. No live guard assignment or item bonus is active.

## Service guilds

One funded duty contract supports dedicated mercenary guilds and available detachments from ordinary military
guilds. Provider membership, earned equipment and professions stay intact. Client representatives reserve wages;
provider representatives reserve actual autonomous fighter identities, roles, availability and deployment terms.
Leader glory can unlock a bounded retinue; guild funds pay services. Contract/provider reputation gives a guild an
economic reason to equip reliable fighters. No new characters, free gear, guild transfer or human-master takeover.

The plan binds commander/task/fee/time and settlement receipts, excludes double assignments and respects current
group, motion, pause and control custody. Native recruitment/travel precede service. Expiry/unavailable leader/death
must reconcile native recovery, unused budgets and safe reassignment. Persistence, controller and wages are future
integration, not behavior of this new source file.

## Individual maker specialties

City artisans keep native recipe/rank/reagent/skill progression. Proposed armor specialties include stamina/defense;
physical weapons include damage/attack-power/haste; applicable caster crafts include spell-power/intellect. One
signature affix is the first target; a compatible two-effect bundle is a later mastery tier. Exact thresholds,
effects and magnitudes are not selected. Skill is captured at the successful craft; output/recipe tier constrains
bonus power so a master does not turn starter gear into unlimited endgame output. Maker identity and qualified
commission history make buyers choose a craftsman, while the bonus belongs to the exact traded item.

Added `strategy-module/src/MasterworkPolicy.h`: proposed one-event/crafter/item eligibility using
confirmed self-craft, recipe/specialty, city workshop, unstackable single output, empty property slots, no existing
random property, no prior masterwork, native catalogue admission, actual skill threshold and bounded power cost.
The native property ID must fit the selected core's signed16-bit save/load width. It returns a proposal only;
no item/setter/skill/database operation is called. Test property IDs and thresholds are synthetic, not real admission.

## Native seams and remaining proof

Read-only inspection verified selected core Item save/load fields for creator/enchantments/random property and
`Item::SetItemRandomProperties` resolving native property/suffix entries into property enchantment slots. This
provides a candidate per-instance route; shared ItemTemplate stats must not be changed for a single maker's output.
One native bundle can contain several effects; invoking the setter twice overwrites rather than appends independent
properties. Existing enchants/sockets/recipe properties must remain intact. No native effect catalogue was loaded,
validated or applied. Client tooltip/stat scaling and property compatibility remain gates.

`Spell::DoCreateItem` calls `OnPlayerCreateItem` after storage/creator handling and before normal craft skill gain.
The hook lacks recipe ID and proven self-craft context, so it alone cannot safely decorate every newly created item.
Exact recipe/cast/output correlation or qualified context-bearing integration is required. Signature display depends
on native template support; commission provenance can retain a maker where the standard tooltip cannot display one.
Custom names/new DBC effects may need client work. Weapon quickness through haste is a candidate; direct base swing
speed changes need separate qualification. Broker/AI gear valuation must eventually include actual instance bonuses.

## Changed paths and verification

Added native `src/MasterworkPolicy.h`, `tests/MasterworkPolicyTests.cpp`, `docs/MERCENARIES_MASTERWORKS.md`.
Updated native `README.md`, `docs/CAMPAIGNS.md` and the strategic implementation plan. Host mirrors remain under
`work/server-strategy-20261008`; the actual component remains outside the active build graph.

Root reviewed the exact six-path final diff, SHA
`0d4afabaed7eab11d12247590c0189c7209b0b928a32adec5346392563daec8d`.
`native-source-receipt-r6.json` pins seventeen files; changed R5 preimages are retained. Official native-src C++
codestyle passed nine checks and static hash/wiring/LF/120-column/unwired-mutation checks passed. Twenty-seven new
native masterwork assertions are prepared, separate from the prior bank/guild/strategy suites. None was compiled
or executed. Static source checks are not item persistence/equip, contract or gameplay proof.

Fresh pre/post exact world/auth executable/argv/start/parent/hash, three configurations, core/module HEAD/index
and all 266 supported-update source identities matched; no guest builder. Live S125 and the pending supported
update were preserved. No compiler/linker, heavy resource baseline, SQL/migration, configuration, control, population,
guild/item mutation, restart, commit/push or automation operation. Galaxy resource priority remains.

Next masterwork proof: one eligible actual city craft -> admitted modest native property -> once-only persistent
maker/item event -> save/load/mail/bank/equip -> exact stat effect and preserved ordinary enchants/sockets. Then add
the second-affix mastery tier. Mercenary contracts separately need durable funded duty and safe group/control proof.
