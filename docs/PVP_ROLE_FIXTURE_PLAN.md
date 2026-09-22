# Role-balanced 10v10 WSG fixture lane

## Scope

This lane is a new, disjoint fixture namespace for a deterministic Warsong Gulch roster. Its fixed fixture ID is `awrole1`; it does not reuse the generic `awpvp1` account or character identities.

The implementation is limited to:

- `scripts/pvp-role-fixture-lib.ps1`
- `scripts/pvp-role-fixture-world.ps1`
- `scripts/tests/pvp-role-fixture.tests.ps1`
- `docs/PVP_ROLE_FIXTURE_PLAN.md`

It dot-sources `pvp-fixture-lib.ps1` for the existing SQL literal and identifier guards. It does not edit or invoke mutation paths in existing PvP, WSG, dungeon, module C++, configuration, or gathering files.

## Exact mirrored roster

Every side has one durable carrier/tank, two healers, and seven DPS. Class, spec, role category, and capability tags are identical across each mirrored slot; only the faction-valid race and scoped identity differ.

| Slot | Role | Class and exact fixture-init spec | Alliance race | Horde race | Coverage |
|---:|---|---|---|---|---|
| 1 | Flag carrier / tank | Druid, `bear pve` index 1 | Night Elf | Tauren | durable, carrier, melee, tank |
| 2 | Healer | Priest, `disc pvp` index 3 | Human | Undead | caster, dispel, ranged healing |
| 3 | Healer | Shaman, `resto pvp` index 5 | Draenei | Tauren | caster, ranged healing, interrupt |
| 4 | DPS | Rogue, `subtlety pvp` index 5 | Human | Undead | melee, stealth, interrupt |
| 5 | DPS | Warrior, `arms pvp` index 3 | Human | Orc | melee, Mortal Strike, interrupt |
| 6 | DPS | Hunter, `mm pvp` index 4 | Dwarf | Troll | physical ranged, trap control |
| 7 | DPS | Mage, `frost pvp` index 6 | Gnome | Blood Elf | caster, ranged control, interrupt |
| 8 | DPS | Warlock, `affli pvp` index 3 | Gnome | Orc | caster, ranged pressure |
| 9 | DPS | Shaman, `ele pvp` index 3 | Draenei | Troll | caster, ranged, interrupt |
| 10 | DPS | Paladin, `ret pvp` index 5 | Human | Blood Elf | melee, support, stun control |

All members target level 80 and rare quality (`3`) gear through the current `fixture-init` control surface after login.

## Deterministic identities

Accounts are disposable and fixture-scoped:

- Alliance: `AWROLE1A01` through `AWROLE1A10`
- Horde: `AWROLE1H01` through `AWROLE1H10`

The client-valid, letters-only character order is:

| Side | Ordered names |
|---|---|
| Alliance | `Roleaflag`, `Roleapriest`, `Rolearesto`, `Rolearogue`, `Roleawarrior`, `Roleahunter`, `Roleamage`, `Roleawarlock`, `Roleaele`, `Roleapaladin` |
| Horde | `Rolehflag`, `Rolehpriest`, `Rolehresto`, `Rolehrogue`, `Rolehwarrior`, `Rolehhunter`, `Rolehmage`, `Rolehwarlock`, `Rolehele`, `Rolehpaladin` |

The resolved WSG GUID manifest is always Alliance slots 1-10 followed by Horde slots 1-10. Database return order cannot change it.

## Safety and idempotence

Dry-run is the default and performs no database, bridge, process, or report write:

```powershell
pwsh -NoProfile -File .\scripts\pvp-role-fixture-world.ps1
```

Live local work requires `-Apply`. The WoW account password is accepted only from `WOW_ACCOUNT_PASSWORD`, must contain 3-16 non-whitespace characters, is sent through redirected worldserver standard input, and is redacted before console logs are written. It is never accepted as a parameter or included in reports.

```powershell
$env:WOW_ACCOUNT_PASSWORD = '<3-16-character-local-secret>'
pwsh -NoProfile -File .\scripts\pvp-role-fixture-world.ps1 -Apply
```

Each apply resolves accounts and characters before writing:

1. Query only the 20 exact account names, 20 exact character names, and characters owned by resolved exact accounts.
2. Reuse only one exact account/character match with the expected owner, race, and class.
3. Fail closed if a fixture name belongs to another account, an exact account owns an unrelated or extra character, or race/class differs.
4. Create missing accounts only when no normal AutoWoW worldserver is running. The script uses the existing binary as a transient foreground console, then lets that process exit; it does not stop or restart the normal server.
5. Insert a missing character only while the normal worldserver is stopped, under a fixture-specific MySQL named lock. The statement is insert-if-absent and requires the exact account to be empty. It has no update, delete, truncate, alter, or drop path.
6. Re-query and rebuild the deterministic manifest. Re-running against an exact fixture performs no account or character bootstrap.
7. Before login, require every resolved GUID to already be present in the existing `AutoWow.FixtureGuids` allowlist. This lane reports missing GUIDs but does not edit configuration.
8. When the normal worldserver is already online and the allowlist is complete, activate the exact GUIDs, wait for all 20 logins, run per-member `fixture-init`, then verify exact class, level, spec index/name, combat role, equipped slot count, and rare-only gear evidence through `fixture-status`.

The lane never calls build, start, stop, or restart scripts. It never writes configuration, changes `battleground_template`, changes league/gathering tables, forms parties, or dispatches a WSG queue.

## Apply outcomes

- `READY`: exact manifest is online and all 20 per-role fixture-init checks passed.
- `PROVISIONED_PENDING_FIXTURE_ALLOWLIST`: identities are complete, but one or more resolved GUIDs are absent from the existing fixture allowlist. Satisfying that prerequisite requires a separately authorized configuration/lifecycle lane.
- `PROVISIONED_PENDING_SERVER_LOGIN_INIT`: identities and allowlist are complete, but the normal worldserver is offline. This lane does not start it.
- `FAILED`: a collision, unsafe state, local dependency failure, activation timeout, or exact loadout mismatch caused a fail-closed result.

Apply reports are unique JSON files under `AutoWoW\logs`. The `wsg_handoff` section contains the ordered 20 GUIDs without queueing them. A consumer must preserve the per-member spec indexes; a single-spec normalizer would destroy the role balance.

## Tests

The focused Pester suite covers the exact mirrored roster, Wrath-valid faction race/class pairs, deterministic identities, role and capability counts, idempotent manifest order, collision rejection, scoped SQL, exact fixture-init evidence, dry-run behavior, password handling, forbidden lifecycle/config/WSG operations, and PowerShell parsing.

```powershell
Invoke-Pester .\scripts\tests\pvp-role-fixture.tests.ps1
```
