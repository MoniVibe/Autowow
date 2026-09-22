# AutoWow persistent questing proof

Date: 2026-07-13 (Asia/Jerusalem)

## Guardrails

- The AutoWow bridge was bound to `127.0.0.1:18787` throughout the proof.
- All quest acceptance, turn-in, POI movement, combat, loot, XP, and quest persistence came from AzerothCore/Playerbots actions.
- No test command inserted or updated `character_queststatus`, `character_queststatus_rewarded`, XP, money, items, or objective counters.
- Database queries were read-only. The configured `acore` password was loaded only into the current process environment and was never emitted.

## Ember proof: Saewash (guid 7)

1. Created a two-bot party and staged it by Foreman Thazz'ril in the Valley of Trials.
2. Issued the `quest 7` bridge order. The bridge selected Playerbots RPG POI handling for completed quest `788 – Cutting Teeth`.
3. Saewash moved along the real route and reached Gornek's questgiver position near `-600,-4186`.
4. The next cycle changed from quest 788 turn-in to `789 – Sting of the Scorpid` objective work. XP changed from 336 to 556.
5. Safe logout persisted the following evidence:

```text
rewarded: 788
active:   789, 792, 1516, 3084, 5441
```

## Northstar proof: Brandreas (guid 10)

1. Created the full five-bot Northstar party and staged it in Teldrassil by the valid questgiver for its existing Night Elf chain.
2. Issued `quest 10`. The leader rewarded `3120 – Verdant Sigil`, moving from 317 to 357 XP, then selected `457 – The Balance of Nature`.
3. Repeated bounded quest cycles. The leader moved to the objective POI and entered combat against a Young Nightsaber and then a Thistle Boar; XP reached 373.
4. Safe logout persisted:

```text
rewarded: 3120
active:   457 (incomplete, mobcount1 = 1), 916
```

## Result

PASS for the narrow gate: two persistent campaign leaders can use native Playerbots quest flow to turn in real quests, accept follow-on work, move to quest objectives, enter combat, update a quest objective counter, persist through logout, and resume without random-grind fallback.

The next gate is a multi-hour observation of these same parties with stuck/POI-missing recovery and a progress-rate baseline; it should not claim raid or 1–80 readiness yet.
