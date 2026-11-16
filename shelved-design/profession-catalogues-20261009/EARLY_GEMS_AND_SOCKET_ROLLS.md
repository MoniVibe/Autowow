# Early jewelcrafting and random gear sockets

**Design extension, not installed.** The requested direction is sockets that make jewelcrafting useful
during levels 1–60. The rates, stats, recipe costs and city-work times below are proposed starting values.
This adds 30 early cuts alongside the existing 90 profession concepts; it does not replace those designs.

## A socket is a useful lucky roll

Each newly created eligible crafted piece or earned loot item gets one server-owned roll. A success adds
**one empty prismatic socket**, allowing any ordinary gem color. Existing template sockets stay intact.
There is no second random socket and no overwriting an existing added socket.
Refuse a roll when no native socket position is available, even if current sockets are empty. More deliberately designed
sockets can belong to a separately admitted blueprint, within the actual client/core limits.

| Newly created gear quality | Proposed chance of one extra socket |
|---|---:|
| Common / white | 15% |
| Uncommon / green | 20% |
| Rare / blue | 25% |
| Epic / purple | 30% |
| Poor / grey | No random socket |
| Legendary | Blueprint-defined sockets; no extra random roll |

Apply to admitted weapons/shields and chest, legs, head, shoulders, bracers, feet, waist and gloves.
Exclude jewelry, trinkets, bags, ammo, quest-use props, conjured goods, probes and generated fixtures.
Background factory/free-maintenance/admin gear is not an earned creation source.
This starts at level 1; later gear can use separately admitted Outland/Northrend gem tiers.
An empty socket has nso stat bonus and is not justification to replace a materially better weapon.
Rates are per produced equipment unit: ten finished armors receive ten independent retained outcomes.
For example, ten eligible white pieces have an expected 1.5 successes, not a guaranteed one or two.

No rolls on auction purchase, mail, bank, trade, equip, login, save/load, repair or gem replacement.
Keep the no-socket outcome too. The committed source-event/output-GUID/policy-version record captures
the draw before exposing the item; a crash or retry resumes that outcome instead of rerolling it.
The first creation anchor is unique across policy versions. A later policy reload cannot create
a new roll for an old item. The same item retains its socket and gem through transfers.
No blanket retrofit of current gear.

## Five affordable gem tiers

Every gem gives one flat stat; no percentages, combat procs, haste/rating complexity or meta conditions.
Non-jewelcrafters can use them. Makers must have the real trained rank, character-level eligibility and
learned custom recipe. A level 1 recipient is not a level 1-profession-training bypass.
Work duration is proposed observed eligible city work, not a timer that mints goods or grants skill.

| Recipient reference band | Proposed minimum JC skill | Strength / Agility / Intellect / Spirit | Stamina | Spell power | Proposed work per cut |
|---|---:|---:|---:|---:|---:|
| 1–10 | 1 | +1 | +2 | +1 | 20 seconds |
| 11–20 | 50 | +2 | +3 | +2 | 25 seconds |
| 21–30 | 100 | +3 | +4 | +3 | 30 seconds |
| 31–45 | 150 | +4 | +6 | +5 | 40 seconds |
| 46–60 | 225 | +6 | +9 | +8 | 50 seconds |

The band is a supply/recipe target, not an expiry: a cheap lower-tier gem stays valid after leveling.
A higher-tier cut needs both wearer level and effective host required-level at least its band minimum.
For otherwise eligible no-level-requirement starter gear, effective host level is max(1, native RequiredLevel);
this allows novice gems but cannot disguise a starter item as a level 46 host for an artisan gem.
A skill 225 artisan can sell a starter cut without producing an endgame-strength gem.
Custom gem-level enforcement must cover human socket requests, bot requests, equip and load;
setting only an item-template RequiredLevel field is not admitted as sufficient enforcement.

## Exact proposed shopping lists

Each row produces **one cut gem**. Jeweler’s Kit and Simple Grinder are held reusable tools, not
consumed thirty times. The raw gem plus stone is the complete proposed direct bill; no bars,
relic core, Frostweave or Northrend reagents are required. These are custom recipes, not existing
native cut-gem recipes. Material names/entries exist in the current world catalogue.

| Cut / design ID | Recipient band | Color | Flat bonus | Direct inputs for one gem |
|---|---|---|---|---|
| **Novice Bold Cut**<br>`jewel.early.novice.bold` | 1–10 | red | +1 Strength | 1 × Tigerseye; 2 × Rough Stone |
| **Novice Delicate Cut**<br>`jewel.early.novice.delicate` | 1–10 | red | +1 Agility | 1 × Tigerseye; 2 × Rough Stone |
| **Novice Brilliant Cut**<br>`jewel.early.novice.brilliant` | 1–10 | yellow | +1 Intellect | 1 × Tigerseye; 2 × Rough Stone |
| **Novice Solid Cut**<br>`jewel.early.novice.solid` | 1–10 | blue | +2 Stamina | 1 × Malachite; 2 × Rough Stone |
| **Novice Runed Cut**<br>`jewel.early.novice.runed` | 1–10 | red | +1 Spell power | 1 × Tigerseye; 2 × Rough Stone |
| **Novice Lustrous Cut**<br>`jewel.early.novice.lustrous` | 1–10 | blue | +1 Spirit | 1 × Malachite; 2 × Rough Stone |
| **Apprentice Bold Cut**<br>`jewel.early.apprentice.bold` | 11–20 | red | +2 Strength | 1 × Shadowgem; 2 × Coarse Stone |
| **Apprentice Delicate Cut**<br>`jewel.early.apprentice.delicate` | 11–20 | red | +2 Agility | 1 × Shadowgem; 2 × Coarse Stone |
| **Apprentice Brilliant Cut**<br>`jewel.early.apprentice.brilliant` | 11–20 | yellow | +2 Intellect | 1 × Lesser Moonstone; 2 × Coarse Stone |
| **Apprentice Solid Cut**<br>`jewel.early.apprentice.solid` | 11–20 | blue | +3 Stamina | 1 × Moss Agate; 2 × Coarse Stone |
| **Apprentice Runed Cut**<br>`jewel.early.apprentice.runed` | 11–20 | red | +2 Spell power | 1 × Shadowgem; 2 × Coarse Stone |
| **Apprentice Lustrous Cut**<br>`jewel.early.apprentice.lustrous` | 11–20 | blue | +2 Spirit | 1 × Lesser Moonstone; 2 × Coarse Stone |
| **Journeyman Bold Cut**<br>`jewel.early.journeyman.bold` | 21–30 | red | +3 Strength | 1 × Citrine; 2 × Heavy Stone |
| **Journeyman Delicate Cut**<br>`jewel.early.journeyman.delicate` | 21–30 | red | +3 Agility | 1 × Jade; 2 × Heavy Stone |
| **Journeyman Brilliant Cut**<br>`jewel.early.journeyman.brilliant` | 21–30 | yellow | +3 Intellect | 1 × Citrine; 2 × Heavy Stone |
| **Journeyman Solid Cut**<br>`jewel.early.journeyman.solid` | 21–30 | blue | +4 Stamina | 1 × Jade; 2 × Heavy Stone |
| **Journeyman Runed Cut**<br>`jewel.early.journeyman.runed` | 21–30 | red | +3 Spell power | 1 × Citrine; 2 × Heavy Stone |
| **Journeyman Lustrous Cut**<br>`jewel.early.journeyman.lustrous` | 21–30 | blue | +3 Spirit | 1 × Jade; 2 × Heavy Stone |
| **Adept Bold Cut**<br>`jewel.early.adept.bold` | 31–45 | red | +4 Strength | 1 × Star Ruby; 2 × Solid Stone |
| **Adept Delicate Cut**<br>`jewel.early.adept.delicate` | 31–45 | red | +4 Agility | 1 × Star Ruby; 2 × Solid Stone |
| **Adept Brilliant Cut**<br>`jewel.early.adept.brilliant` | 31–45 | yellow | +4 Intellect | 1 × Citrine; 2 × Solid Stone |
| **Adept Solid Cut**<br>`jewel.early.adept.solid` | 31–45 | blue | +6 Stamina | 1 × Aquamarine; 2 × Solid Stone |
| **Adept Runed Cut**<br>`jewel.early.adept.runed` | 31–45 | red | +5 Spell power | 1 × Star Ruby; 2 × Solid Stone |
| **Adept Lustrous Cut**<br>`jewel.early.adept.lustrous` | 31–45 | blue | +4 Spirit | 1 × Aquamarine; 2 × Solid Stone |
| **Artisan Bold Cut**<br>`jewel.early.artisan.bold` | 46–60 | red | +6 Strength | 1 × Large Opal; 2 × Dense Stone |
| **Artisan Delicate Cut**<br>`jewel.early.artisan.delicate` | 46–60 | red | +6 Agility | 1 × Huge Emerald; 2 × Dense Stone |
| **Artisan Brilliant Cut**<br>`jewel.early.artisan.brilliant` | 46–60 | yellow | +6 Intellect | 1 × Azerothian Diamond; 2 × Dense Stone |
| **Artisan Solid Cut**<br>`jewel.early.artisan.solid` | 46–60 | blue | +9 Stamina | 1 × Blue Sapphire; 2 × Dense Stone |
| **Artisan Runed Cut**<br>`jewel.early.artisan.runed` | 46–60 | red | +8 Spell power | 1 × Large Opal; 2 × Dense Stone |
| **Artisan Lustrous Cut**<br>`jewel.early.artisan.lustrous` | 46–60 | blue | +6 Spirit | 1 × Huge Emerald; 2 × Dense Stone |

Ten cuts need ten raw gems and 20 stone units, with ten completed craft/output receipts.
Native skillups depend on admitted recipes and skill difficulty, not an unconditional point per cut.
Maker character work-XP remains a separate admitted actual-work mechanism; no adventure requirement
or free recipe/skill/XP is introduced.

## How supply and bot demand create early JC work

Mining crews supply real ore, rough stones and recovered gems. Prospecting can supply gems only where
a learned native prospecting route qualifies; its random yield is not a guaranteed ore-to-gem recipe.
Representatives can buy scarce stones or raw gems from real funded stock/auction sellers.
City jewelcrafters cut to role demand, receive payment for accepted goods and return finished gems.
Mining, prospecting, cutting, mail and socketing are separate stages of the same goods.

Read actual recipient level, host GUID/slot/required-level/socket metadata, existing gems and role.
Strength suits strength melee/tanks; agility suits agility melee/ranged; stamina serves actual
survival needs; intellect/spell power/spirit use class-specific weights. Spirit is not a universal
mana-stat upgrade. Red/yellow/blue identify native-compatible colors; early added sockets are
prismatic, so a tank can install a blue Solid Cut in a lucky weapon socket.
Built-in colored sockets keep their real color/meta and socket-bonus rules. Do not invent matching
bonuses or alter an existing unique-gem rule. Existing gems are not silently overwritten.

Order only a compatible in-stock/learned cut that improves the current gem or an empty socket.
Urgent weapons/body armor take funding priority over minor gem optimization. Gemless gear without
an admitted socket still cannot take a gem. A socket roll grants no free gem or stats.
Consume the exact gem through native socketing, retain the result and re-score actual equipped
stats. Replacing a gem follows native consumption/destruction; this design offers no free recovery.

## Native route and proof still needed

The selected core’s Item::HasSocket and HandleSocketOpcode explicitly support one additional
prismatic-enchantment channel alongside template sockets. This is source feasibility, not a
qualified enchantment ID, new item, live adapter or working client display.
AzerothCore documents template socket colors and GemProperties records:
[item_template](https://www.azerothcore.org/wiki/item_template).
New low-stat cut items need an admitted item/GemProperties/enchantment/client mapping; whether
existing low-stat records cover every combination is not yet established. No silent aura fallback.

Before implementation: qualify the immutable source-event creation hook and atomic outcome store;
select/verify a compatible added-socket enchantment; map each cut and learned recipe; enforce
level/tier rules for all socket users; and integrate role-aware bot demand/native socket actions.
Existing repository transaction admission remains false; durable random-roll/settlement cannot
be enabled on an unchecked commit/reconnect path.

Acceptance must cover: unrolled/zero/success/replayed creation; batch unit identity; full/existing
sockets; foreign/probe sources; wrong wearer/host level; paid materials and one native output;
native socket consumption; equip/unequip stats; save/load/mail/trade; client tooltip; no free reroll
or duplicated gem. These are required future tests, not checks that ran this turn.

This turn checked the 30 design rows, quantities, tier coverage/stat progression, input-name identities
and read-only native source/runtime custody. No C++/SQL/configuration/control/build/restart/Git or
item/gem/skill/XP mutation occurred. The existing 90 catalogue and its material bills stay intact.
