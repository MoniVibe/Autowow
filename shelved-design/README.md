# AutoWoW: shelved design archive

This is the authored design and prototype archive. Server source lives on [main](https://github.com/MoniVibe/Autowow/tree/main). The base and reconstruction notes, core patch and Git bundle, and unintegrated strategic module are in the [October 11 source checkpoint](../core-patches/offline-checkpoint-20261011/README.md). The old world and player saves are not part of the archive; rebuild from source if the project resumes.

- [Roadmap](AUTOWOW_STRATEGIC_AUGMENTATION_ROADMAP_2026-10-09.md), [territory economy](AUTOWOW_TERRITORY_ECONOMY_DESIGN_2026-10-07.md), [guild and faction banks](AUTOWOW_FACTION_GUILD_BANKS_2026-10-09.md), and [territorial militia](AUTOWOW_TERRITORIAL_MILITIA_2026-10-09.md).
- [Original boss traits](AUTOWOW_RAID_BOSS_TRAITS_CONSERVATIVE_2026-10-09.md) and the later [attribute layer](AUTOWOW_BOSS_ATTRIBUTE_LEGACIES_2026-10-09.md).
- [Legendary workshops](AUTOWOW_LEGENDARY_WORKSHOP_DESIGNS_2026-10-09.md), [named makers](AUTOWOW_LEGENDARY_MAKERS_2026-10-09.md), and [workshop progression](AUTOWOW_WORKSHOP_CAREERS.md).
- [Craft commissions](AUTOWOW_CRAFT_COMMISSION_DESIGN.md), [profession material bills](profession-catalogues-20261009/MATERIAL_BILLS.md), [early gems and sockets](profession-catalogues-20261009/EARLY_GEMS_AND_SOCKET_ROLLS.md), [expanded enchants](enchantment-expansion-20261009/INDEX.md) and [runesmithing](runesmithing-expansion-20261010/INDEX.md).
- [Offline territory simulator](territory-prototype/territory-strategy.html). Download/open the HTML locally, or run `node test.js` and `node package-view.js` from its directory.

These files include proposals. The strategic module was never integrated into the supported server build. The prototype uses synthetic inputs and cannot grant live resources, progression or gear.

### Restore the exact core checkout

`core-restore.bundle` includes the six AutoWoW commits through `bb599d71c8b21122c700340afaed183f0cab83e4`, including the otherwise unavailable local base of the older bundle. Its only prerequisites are two public commits of `mod-playerbots/azerothcore-wotlk`. From the archive clone, with Git Bash or another shell:

```sh
git clone --no-checkout https://github.com/mod-playerbots/azerothcore-wotlk.git core
git -C core fetch origin 52f58186a53399e603c46c24977fe60fcaad7f9d 7f12e89ee5f467a50e62eba1d525eac7dc953d03
git -C core bundle verify ../shelved-design/core-restore.bundle
git -C core fetch ../shelved-design/core-restore.bundle refs/heads/checkpoint/s128-offline-20261011:refs/heads/autowow-s128
git -C core switch autowow-s128
```

Use the supported Playerbots module at `e9d801199667a70f59a3f517b176febb67798c40` from this repository's `main` branch. Keep the strategic module separate until its native integration is qualified. Initialize a new database; do not replay old release scripts.
