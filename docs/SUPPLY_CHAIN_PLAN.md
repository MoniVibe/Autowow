# Supply Chain V1: guilds, treasury, craft contracts

Owner direction (2026-09-25): tailors should equip the faction with bags. Contracts pay them.
Guild representatives take the bags and send them to members who need them. Crafters sell the surplus when that makes sense.
Class spells stay auto-learned for now (trainer visits come later).

## Facts (S39, cohort guids 62955-63004)

- No guilds: 0 of 50 cohort bots are in a guild.
- Crafting is dormant. 5 tailors (Olbrek 62964, Wizzlecog 62972, Kragzul 62984, Ashkanu 62994, Rakazun 62997) are all at skill 1, and every other crafting skill is at 1 too. Gathering does move: herbalism averages 11.
- Cloth in bags: linen 423, wool 50, silk 58, mageweave 4. SellTradeGoods sells cloth that a bot's own professions don't use.
- Bags: 21 bots have 0 equipped bags, 16 have 1, 8 have 2, 5 have 4.
- Money: the whole cohort holds 14.1g; median 4.6s. None of the 37 bots at L20+ can afford riding.

## Rules

- **No free gold or items.** Every copper and item moves between real players: tax, contract pay, mail. Gold only enters the economy the stock way (loot coin, quest money, vendor sales).
- **Everything goes behind flags that default to off.** Everything writes to the ledger with append-only event ids. Pure policy lives in headers with unit tests.
- **Retrofit-proofing.** Versioned state, deterministic ordering, numeric ids (no strings in decisions), bounded maps, locks on map-thread state. The guild-bank balance is the persistent treasury, so there is no side-table snapshot to keep in sync.

## Owner refinements (2026-09-25)

- **Profession houses.** Each faction gets several guilds, one per profession house: Weavers (tailoring, enchanting), Smiths (mining, blacksmithing, engineering), Tanners (skinning, leatherworking), Herbalists (herbalism, alchemy). A bot joins the house that matches its profession.
- **The guild rep is the hub.** Materials are mailed to the rep. The rep hands out craft contracts and materials, collects the goods, and delivers them to members who need them.
  - The rep needs plenty of storage. For V1 that is the rep's own bags plus the character bank.
  - Guild bank item tabs cost 100g and up in 3.3.5. Buying the first tab becomes a faction milestone unlock.
- **Inter-rep trade (lane C).** Reps buy and sell between houses, for example Weavers buying leather from Tanners. Gold and items move rep to rep by mail, priced from vendor value and AH price.
- **Who plays the rep (owner ruling 2026-09-25).** A dedicated character per house per faction, 8 in total.
  - **Never quests.** Starts at level 1. Gains XP only from successful trades and deals, with the XP award named and logged.
  - **Home:** a faction capital, next to the bank, mailbox and auction house. Level 1 is safe there.
  - **The guild's mule.** It runs operations for members: gear, bags, materials and mail.
  - **Keeps ticking.** It has its own operations loop (the RepRuntime), not the questing AI.
  - **Provisioning:** `.autowow cohort create <account> <race> <class> <gender> <name>` in the one-shot console, with the world stopped. It goes on spare slots of the existing cohort accounts, so no new accounts are needed.
  - **Guild role:** the rep is the guild leader, so it controls the guild bank.

## Lane A: guilds and treasury (superseded in part by the refinements above)

1. **Bootstrap** (`AutoWow.Guilds.Enable`). At startup, make sure one guild exists per faction for the cohort: "AutoWoW Alliance Cohort" and "AutoWoW Horde Cohort".
   - Configurable guild master. Default: the lowest cohort guid of that team.
   - Every cohort bot joins its faction guild. The step is idempotent.
   - The cohort is identified by an explicit guid range or list in config (default 62955-63004), not by guessing.
2. **Tax.** When a member sells to a vendor (the errand sell stop), `AutoWow.Guilds.TaxPct` (default 10) of the copper earned is deposited into the guild bank. This is a real transfer, logged as ledger `trade` action `tax`.
3. **Treasury API.** `Balance(team)`, `Pay(team, player, copper, reason)` (bank to player), `Deposit`.
   - Every movement goes to the ledger.
   - Payments are refused if the balance is short.
4. **Mail helper.** `SendItems(fromGuid, toGuid, items[])` and `SendMoney` use the core mail system. The guild master is the sender, and postage is paid from the treasury.
5. **Ledger.** New event id 19 `guild` (bootstrap, join, tax, pay, deposit).

## Lane B: craft contracts, cloth routing, delivery (after lane A)

1. **Need detection.** A member's bag need = empty equipped bag slots (19-22), or bags smaller than 10 slots. The board keeps a need list per faction.
2. **Cloth routing** (`AutoWow.Supply.RouteCloth`). At the errand sell stop, non-tailors mail cloth (linen, wool, silk) to the faction's highest-skill tailor instead of vendoring it, up to that tailor's need. The ledger records each donation.
3. **Craft contract.** The board issues "craft N bags" to the tailor: Linen Bag (needs skill 45) or recipes the tailor already knows.
   - Early tailoring skill goes through cheap stock recipes (Bolt of Linen Cloth, Linen Cloak, Linen Bag).
   - The tailor buys Coarse Thread from a trade-supplies vendor on errand runs, then crafts when idle.
   - Crafting uses the stock spell cast. Skill-ups follow the stock rules.
   - The bot needs to learn tailoring ranks and recipes. Auto-learn covers class spells, so check whether it also covers professions; if not, trainer stops handle it.
4. **Pay.** For each bag delivered, the treasury pays the tailor the bag's vendor value plus a margin (`AutoWow.Supply.BagPayPct`).
5. **Delivery (virtual rep, V1).** Finished bags are mailed tailor to guild master, then guild master to the neediest member (lowest bag slots first; ties to the lower guid). The member's AI equips the bag when it arrives.
6. **Surplus.** With no open need, the tailor sells extra crafts at the AH (existing AutoWowTrade) or a vendor, whichever pays more.
7. **Ledger.** New event id 20 `supply` (donate, craft, deliver, pay, surplus).

## Later

- **V2 rep:** the rep gets a body and walks cargo between towns (the owner's representatives vision).
- **More crafts:** blacksmith weapons, leatherworking armor, alchemy potions. Same board, same treasury.
- Contract gold rewards from the treasury, and the overlord buying faction buffs.
