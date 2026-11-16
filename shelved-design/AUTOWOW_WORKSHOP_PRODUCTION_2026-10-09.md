# AutoWoW workshop production

Approved direction: city artisans receive bulk work orders; ordinary equipment flows steadily, masterworks take
longer, and legendary commissions occupy several workshop hours across stages. The exact recipe timings remain
to balance. The first implementation slice is an unlinked pure scheduling policy plus regression sources and a
durable-integration contract, not an activated city scheduler.

| Order | Experience | Completion evidence |
|---|---|---|
| Ten ordinary breastplates | One queued order, ten sequential pieces | Ten actual crafts and distinct item identities |
| Masterwork | Several timed stages, specialty finish | Exact successful craft and captured maker skill |
| Legendary | Preparation, forging/weaving, tempering, finishing, inspection | Rare inputs, native craft, exact item and once-earned maker receipt |

Only observed eligible city work credits time. Material shortages, unsafe/busy artisans and interrupted observation
pause progress. Committed progress survives restart; downtime earns no catch-up output. A timer makes a piece ready
to craft, then actual native reagent consumption/output decides success. Skill gains remain native craft results.

One artisan works one active order at a time. Materials and budgets reserve once. Failed or uncertain native casts
hold for reconciliation; retries cannot make duplicate goods. Finished pieces may ship as they finish or together.
Cancelling releases unused inputs and preserves already-produced goods. Queue and settlement adapters remain pending.

Teaching can later allocate workshop time to apprentices and reserve demanding finishes for masters. It is not
implemented by this production slice. No apprentice skill, character XP, recipe unlock or legendary effect is granted.

Source: `WorkshopPolicy.h`; regression source: `WorkshopPolicyTests.cpp`; detailed contract: `docs/WORKSHOPS.md` in
the unlinked strategy component. These are prepared for memory-deferred compilation and later native integration.
The existing database transaction admission blocker also applies to future persistent workshop state.
