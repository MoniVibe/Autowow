# AutoWow Oracle Race Campaign

Generated: 2026-07-19T03:21:33.1985698Z

| Race | Character | Level | Online | Planned class | Planned professions |
|---|---|---:|:---:|---|---|
| Night Elf | Zathis (101) | 2 | True | balance | Herbalism, Alchemy |
| Human | Naomini (112) | 1 | True | protection | Mining, Blacksmithing |
| Tauren | Musliwho (121) | 1 | True | feral | Herbalism, Skinning |
| Blood Elf | Aenstus (123) | 1 | True | retribution | Mining, Jewelcrafting |
| Gnome | Bitlas (139) | 1 | True | frost | Mining, Engineering |
| Troll | Tzigdan (144) | 1 | True | beast_mastery | Herbalism, Alchemy |
| Dwarf | Khirgegs (154) | 1 | True | marksmanship | Mining, Engineering |
| Draenei | Valrula (166) | 1 | True | shadow | Herbalism, Inscription |
| Undead | Nolliano (236) | 1 | True | shadow | Herbalism, Tailoring |
| Orc | Ugrilok (244) | 1 | True | beast_mastery | Skinning, Leatherworking |

Online seeds: 10/10
Failure rows: 2

## Honest capability notes

- Native New RPG, quest objective, loot, trainer learning, equipment refresh, and talent-picking are measured from the live bot and database state.
- Gathering is exercised only during an explicit bounded worker rotation; it is never inferred from the ordinary quest loop.
- Crafting remains an explicit gap until the deployed runtime exposes a bounded craft verb and a per-bot receipt; missing craft evidence is recorded as `craft:evidence_unavailable`, not counted as a successful craft.

## Open failures

| GUID | Domain | Code | Status | Occurrences | Last level | Last seen |
|---:|---|---|---|---:|---:|---|
| 166 | progression | progression:no_progress | open | 8 | 1 | 2026-07-19 03:21:08 |
| 236 | progression | progression:no_progress | open | 8 | 1 | 2026-07-19 03:21:08 |
