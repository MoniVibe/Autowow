# Boss attribute legacies - October 9

Selected design adds the following permanent attributes alongside the existing flavored traits. No native reward is active.

| Encounter | Ordinary boss | Configured final boss |
|---|---:|---:|
| Raid | +5% | +10% |
| Dungeon | +1% | +2% |

Applies to Strength, Agility, Stamina, Intellect and Spirit. Final amounts replace ordinary amounts: 10%, not 15%.
Each canonical encounter family grants once across resets and difficulties; councils settle once. All earned bonuses remain active without a power or loadout cap.
Percentages add against current pre-legacy attributes rather than multiplying each other. Native derived stats follow normally; no extra blanket health/damage/healing award.

Molten Core: nine ordinary encounters plus Ragnaros = **+55% attributes**, alongside the ten flavored traits.
A three-boss dungeon: 1% + 1% + 2% = **+4% attributes**. A single-boss raid grants 10%, not 5% + 10%.
Raid completion presents one named legacy with the attribute total and the flavored traits, without awarding either twice.

## Explicit raid finales

These are configured design assignments, not inferred from array order or the last kill. Karazhan uses Prince Malchezaar; Ulduar uses Yogg-Saron; their optional encounters retain 5%. Vault uses Toravon as its tier finale.
Dungeon final identities still require their own explicit catalogue before native implementation.
All-listed totals below include separately earned optional and rotating encounters; a mandatory raid card must not imply those extras were earned.

| Raid | Configured final encounter | Listed families | All-listed attributes |
|---|---|---:|---:|
| Molten Core | Ragnaros | 10 | +55% |
| Blackwing Lair | Nefarian | 8 | +45% |
| Zul'Gurub | Hakkar the Soulflayer | 13 | +70% |
| Ruins of Ahn'Qiraj | Ossirian the Unscarred | 6 | +35% |
| Temple of Ahn'Qiraj | C'Thun | 9 | +50% |
| Karazhan | Prince Malchezaar | 17 | +90% |
| Gruul's Lair | Gruul the Dragonkiller | 2 | +15% |
| Magtheridon's Lair | Magtheridon | 1 | +10% |
| Serpentshrine Cavern | Lady Vashj | 6 | +35% |
| The Eye (Tempest Keep) | Kael'thas Sunstrider | 4 | +25% |
| Battle for Mount Hyjal | Archimonde | 5 | +30% |
| Black Temple | Illidan Stormrage | 9 | +50% |
| Sunwell Plateau | Kil'jaeden | 6 | +35% |
| Zul'Aman | Zul'jin | 6 | +35% |
| Naxxramas | Kel'Thuzad | 15 | +80% |
| Obsidian Sanctum | Sartharion | 4 | +25% |
| Eye of Eternity | Malygos | 1 | +10% |
| Vault of Archavon | Toravon the Ice Watcher | 4 | +25% |
| Ulduar | Yogg-Saron | 14 | +75% |
| Trial of the Crusader | Anub'arak | 5 | +30% |
| Onyxia's Lair | Onyxia | 1 | +10% |
| Icecrown Citadel | The Lich King | 12 | +65% |
| Ruby Sanctum | Halion | 4 | +25% |

The accompanying JSON retains all 162 flavored proposals unchanged and adds the explicit attribute package to every encounter. This supplements the previous flavored catalogue and supersedes its no-generic-attribute rule.
Only design/documentation changed; no compilation, encounter reward, database, configuration, bot control or restart occurred.
