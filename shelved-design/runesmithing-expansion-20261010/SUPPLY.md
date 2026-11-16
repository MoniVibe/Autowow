# Runesmithing supply design

**Status:** `DESIGN_ONLY_NOT_NATIVE` · **Native active:** `false` · **Lane:** `supply`
All six component outputs, costs, durations and services are proposals. No custom item, recipe, service, station or native route is installed. All native item, recipe and skill-line IDs remain null.

## Six component bills

Each supplier recipe proposes one physical component from real, unreserved input stock. One successful craft is one output and one receipt; multi-unit orders repeat the craft per piece. Durations are active workshop minutes for one output, not online timers or native XP awards. The matching machine-readable bills, gates and service parameters are in [SUPPLY.json](SUPPLY.json).

| Proposed output | Supplier profession | Direct inputs per output | Active minutes |
|---|---|---|---:|
| Copper Rune Blank | Blacksmithing | 2 Copper Bars; 1 Coarse Thread | 2 |
| Iron Rune Blank | Blacksmithing | 2 Iron Bars; 1 Coarse Thread | 2 |
| Mithril Rune Blank | Blacksmithing | 2 Mithril Bars; 1 Solid Stone | 3 |
| Adamantite Rune Blank | Blacksmithing | 2 Adamantite Bars; 1 Golden Draenite | 4 |
| Saronite Rune Blank | Blacksmithing | 2 Saronite Bars; 1 Dark Jade | 4 |
| Runic Binder | Tailoring | 2 Linen Cloth; 1 Coarse Thread | 2 |

These are one-item component yields, not new equipment or bonus materials. The proposed Runic Binder deliberately uses ordinary cloth and thread, not high-level dust, shards or a rare reagent, so it can supply low-tier work. Copper Bar, Iron Bar, Mithril Bar, Adamantite Bar, Solid Stone, Golden Draenite, Linen Cloth and Coarse Thread are additional same-era or ordinary-name references; their exact native names, entries and availability remain pending verification. Saronite Bar and Dark Jade appear in the historical [material reference](../enchantment-expansion-20261009/MATERIAL_REFERENCE.json), which is evidence of past name references only. None of these references proves current stock, current recipe availability, or an admitted route. Verify each native name and entry before a future admission; all entries remain null here.

Each supplier must already meet the real native profession, learned-recipe, character, skill and reagent checks for its eventual admitted route. The exact native skill minimum and recipe entry are intentionally unresolved. Runesmithing is a proposed city-workshop career, not a new native skill line; career standing or fame cannot supply a recipe, raise native skill, bypass character/class/proficiency rules, or award native XP. Death Knight runeforging remains intact, as do ordinary Enchanting and gems.

## Three bounded services

1. **Inspect or restore one imprint.** Inspection is an owner or authorized-representative service on one exact item GUID; it consumes no material, changes nothing and reports the recorded recipe, state and charges. A separate restore mode may consume one Runic Binder, but can repair only a verified fault in that same recipe record. It cannot change the item, recipe, charges, capacity or rights.
2. **Re-inscribe one existing host.** For one compatible item, reserve exactly one admitted target recipe bill: its named components or their expanded raw bill, never both. The item keeps its identity. On commit the old pattern and any unused charges end; there is no refund or free host. Check the target recipe's wearer and artisan gates and entitlement. A legendary target needs its own valid blueprint entitlement and one physical relic for that new commission; transferable knowledge is not the relic.
3. **Restore optional exceptional charges.** This service applies only to a recipe that defines finite optional charges, a refill bill, charges restored per bill, and a per-service restore limit. Restore `min(recipe_max_restore_per_service, capacity - current_charges)` and reserve `ceil(charges_restored / recipe_charges_per_refill_bill)` copies of that exact recipe-version bill. Add no universal binder or reagent and never consume a relic on refill. A five-charge batch is only an example if a recipe itself sets it. At zero charges, only the optional charged effect stops; a recipe's declared passive remains.

Every service is limited to one item per order, uses a separately quoted real-currency fee with no price invented here, and requires the exact item and inputs to remain in held custody at an approved city station until a once-only receipt commits. Capture the service and recipe versions, item GUID, input identities, owner/authorized custodian and before/after state. Recheck authority and recipe rights after transfer. If item or input consumption is uncertain, hold the case for reconciliation rather than retrying on timeout. Charges and item-family cooldown identity follow the same item through trade, bank movement, owner change and relog; none resets by transfer. Durable field runes have no charge meter or compulsory maintenance tax.

## Orders, stock and useful work

Guild orders name one admitted item or component profile, quantity, quote, supplier, delivery terms and deadline. Any member may deposit real funds into the existing guild account. Only a representative with current order, withdrawal and acceptance authority may reserve available funds or settle a contract through the existing guild-bank path. A quote records the actual unit cost and total hold; it invents no universal price and creates no second spendable bank.

Gatherers, adventurers and traders bring actual resources to city production. Reserve the exact physical inputs before work, record their original identities and sources, bind the artisan, recipe version and city station, then retain native craft and output receipts through delivery. No station, service, time credit or ledger creates stock. The component designs use inputs already named in the historical material set where possible; pending names require exact native verification and actual custody before admission.

Quote a bulk order from the full multiplied bill. Produce each piece sequentially with its own successful native craft event, output identity and settlement receipt. A combined shipment may carry completed pieces but is not another craft. Components count as consumed stock; a gear order pays either for the named component or its exact expanded raw materials, never both. Release only unused reservations on cancellation; unknown handoff or craft outcomes remain held until reconciled. Ordinary equipment remains eligible for separate small orders while a long project occupies workshop time.

Blueprint knowledge and a scarce physical relic are separate. Where a legendary design requires boss acquisition, its stated transferable entitlement authorizes knowledge only; the actual relic is supplied and consumed once per new commission. Refill, inspection, restore and station access do not grant encounter credit or recreate a relic. Only recipe-defined rare patterns may use optional charges; ordinary durable field patterns never acquire upkeep.

A future runeforge station upgrade can use a separately quoted bill of verified actual resources. It cannot create free output, native skill, recipe knowledge or station access. A proposed artisan-fame receipt may reward a useful, distinct completed craft once, after its real craft and accepted demand settle. Do not reward failed work, timer slices, the same item moving through mail/bank/trade, duplicate receipts, self-orders, wash trades or repeated transfers. This city-career receipt is separate from native profession gains.

## Admission boundary

These tables are design inputs only. Before any live route, verify exact material and custom output identities, supplier recipe and native gates, city station and input custody. Then qualify durable reservations, authoritative craft correlation, once-only output and payment settlement, service versioning and transfer reconciliation. Active time must be continuously observed for the qualified artisan at the station; discard excess time at stage boundaries and credit nothing during pause, departure, combat, uncertain custody, outage or login catch-up. The workshop and payment/custody requirements follow [WORKSHOPS.md](../../work/server-strategy-20261008/mod-autowow-strategy/docs/WORKSHOPS.md) and [SUPPLY_CONTRACTS.md](../../work/server-strategy-20261008/mod-autowow-strategy/docs/SUPPLY_CONTRACTS.md). No native item, recipe, scheduler, service, guild mutation or control path is proposed as active by these files.
