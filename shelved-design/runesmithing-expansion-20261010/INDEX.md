# Runesmithing: craft the way a hero fights

Four Luna agents at xhigh developed the grammar, progression, supply chain and boss alphabets. This is a proposed city workshop profession, with a concrete catalogue and a small first implementation target. It is not installed on the server.

A rune is a sentence: **Trigger → Transformation → Release**. A shield block gathers impact for a counterstrike; alternating spell schools focuses energy into a burst; healing an injured ally prepares another useful response. The catalogue fixes the conditions and numerical limits of each sentence. Learning glyphs does not enable every possible combination.

| Part | Proposed scope |
|---|---|
| Rune language | 20 glyphs and six illustrative patterns |
| Everyday equipment | 10 durable inscriptions, two in each wearer band from 1–20 through 71–80 |
| Advanced commissions | Two durable masterworks |
| Boss alphabets | 12 legendary inscriptions: eight raid sources and four world-boss sources |
| Supplier work | Five grades of metal blank and one cloth binder, each with its own bill |
| Artisan career | Custom earned mastery 1–450, productive commissions, specialties and teaching |

Runesmiths stay in cities. Blacksmiths supply blanks, tailors supply binders, and enchanting and jewelcrafting materials support the finishing work. Adventurers bring the scarce relics and transferable pattern knowledge. A maker does not have to accompany the raid to undertake its commission. Native supplier professions retain their existing training and character gates.

Ordinary inscriptions are durable. Most legendary patterns are also durable; only a small exceptional subset has finite charges and an ordinary-material refill service. The compatible host remains useful when those charges run out. No refill consumes a second boss relic.

Death knights keep their class runeforging. Enchants, gems and boss legacies keep their separate identities. A proposed rune channel carries one pattern on an exact item; compatibility with native equipment must still be implemented and tested. There is no global hero-trait cap.

## First working example

**Grounding Reprise** would inscribe a usable novice shield. A genuine hostile-NPC block primes the next melee hit against that opponent: proposed +6% hit damage, capped at three extra damage, within five seconds, with a 20-second persistent cooldown. It neither grants shield proficiency nor manufactures an extra attack.

The buyer supplies the shield. The inscription bill is **one Copper Rune Blank, one Runic Binder, five Strange Dust and one Lesser Magic Essence**, followed by 12 active city-work minutes. Supplier work for those components costs **two Copper Bars, two Linen Cloth and two Coarse Thread**, taking two active minutes for each component. Pay either for supplied components or their upstream production; do not consume both bills for the same goods. All recipe and native item identities still need qualification.

The first implementation should prove this whole path: reserved real inputs → observed work → exact host inscription → genuine block and counter → bounded effect → delivery/payment → useful maker progression. Timers, designs or a successful build cannot substitute for that observation.

## Read the design

- [Architecture and acceptance plan](DESIGN.md)
- [Recipe book with direct and expanded material bills](RECIPE_BOOK.md)
- [Career, specialties and ordinary patterns](PROGRESSION.md)
- [Legendary alphabets and five-stage commissions](LEGENDS.md)
- [Supplier components and services](SUPPLY.md)
- [Rune grammar and event provenance](GRAMMAR.md)
- [Structured catalogue](COMBINED.json) and [root consistency checks](ROOT_CHECKS.json)

All balance, costs and effects are design proposals. Custom career progress is not native character XP. No source, build, database, configuration, bot-control or runtime changes were made for this catalogue.
