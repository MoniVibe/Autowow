# Observer Camera v0 — plan & implementation slice

Date: 2026-07-13 (Asia/Jerusalem)
Scope guardrails (hard): **does not** modify Playerbots quest logic, recovery logic, movement
logic, or database character data. Adds only an isolated observer-control surface.

## 1. Goal

One real local 3.3.5 client, logged in as a **dedicated, protected observer character**, that
can safely follow / periodically relocate to the **Northstar** and **Ember** party leaders and
capture real rendered screenshots/video — without ever interfering with the bots' economy,
combat, XP, or quest progress.

**In scope (v0):** the safest server-side control surface to (a) select a watched leader,
(b) relocate the observer, (c) enforce invisibility/GM protection, (d) prevent economic/combat
interference; plus an isolated implementation slice (bridge hook + scripts + tests).

**Out of scope (deferred):** the external overlay that draws watched-party stats. **Not built
here.** Also out of scope: launching/automating the client, creating accounts, storing passwords.

## 2. What already exists (reused, not rebuilt)

- **Client working copy:** `work\wow-client\Wow.exe`, realmlist → `127.0.0.1` (see
  `CLIENT_COPY_REPORT.md`). Launching/logging in is a human GUI step.
- **AutoWow bridge** (`src/AutoWow/AutoWowBridge.cpp`, loopback `127.0.0.1:18787`) already
  teleports real players on the world thread (`RallyParty`/`RouteParty` call `Player::TeleportTo`).
  That is the exact, already-blessed primitive the observer relocation reuses.
- **Leaders:** Ember = **Saewash (guid 7)**, Northstar = **Brandreas (guid 10)**
  (per `QUEST_CAPABILITY_MATRIX.md`, `QUEST_RECOVERY_V0.md`, and the live telemetry
  `logs\quest-telemetry-live-20260713-1137.jsonl`). Leaders are Playerbots bots, so the
  read-only bridge `snapshot <leader-guid>` returns their live position for the relocation loop.
- **Read-only telemetry** (`scripts\quest-telemetry.ps1`) — the future overlay's data source.

## 3. Determined safest control surface

**Decision: a new, isolated, allow-list-gated bridge order `observe`**, with GM protection
enforced as a *precondition* (not silently toggled), and the observer **never grouped** with bots.

| Requirement | Chosen mechanism | Why it is the safest option |
|---|---|---|
| Select watched leader | `observe watch <observer> <leader>` (server-side transient map) | No client automation, no DB write; server holds the binding |
| Observer relocation | `observe relocate <observer>` → `Player::TeleportTo` to the leader's live pose | Reuses the bridge's existing world-thread teleport path; only the observer moves; leader is read-only |
| Invisibility / GM protection | Enforced precondition `IsGameMaster() && !isGMVisible()`; optional `observe protect` sets it on the observer's own runtime state | GM mode = untargetable, no aggro, invisible to players; relocation **refuses** unless protected |
| No economic/combat interference | Server guards: observer must be **not in a group**, **not a bot**, **not in combat**; single observer that switches *watched leader*, never joins a party | Group membership would split bot XP/loot — explicitly refused |

**Rejected alternatives (and why):**
- *GM commands over SOAP* — `SOAP.Enabled=0` and disabled; more importantly SOAP has no in-world
  body, so player-relative commands (`.appear`, `.summon`) can't relocate a *named other* to a
  bot's live coordinates. Would also require storing GM credentials. Rejected as primary.
- *One client per bot / joining the bot party* — violates "single observer" and causes XP/loot
  interference. Rejected.
- *Client-side input automation (send keystrokes to Wow.exe)* — fragile, needs a human UI session
  for the control loop, and can desync. Rejected for automation; kept only as the manual fallback
  (§7) the human can use before the rebuild.

Isolation: all logic is in new files `src/AutoWow/ObserverControl.{h,cpp}`; the bridge change is
four surgical lines (include + enum + one dispatch branch + one parse branch). No Playerbots
quest/recovery/movement/targeting code is touched.

## 4. Architecture

- **Single observer, switches *watched leader*, not groups.** To move from watching Northstar to
  Ember, re-point the relocation loop (`-LeaderGuid`) / re-issue `observe watch`. The observer is
  never added to a bot group.
- **Server-authoritative relocation.** The PowerShell loop only reads the leader's pose (read-only
  `snapshot`) and asks the server to relocate; every guard re-runs server-side on each relocate.
- **Protection is a gate.** `observe relocate` returns `observer_not_protected` unless the observer
  is GM + GM-invisible, so an unprotected observer can never be teleported into a bot's world.

## 5. Prerequisites (exact)

1. **Observer account + character (human-provisioned; this plan creates neither).**
   - Create an account **out of band** and a level-appropriate observer character (e.g. same
     faction as the watched parties so it can share a map without PvP flags). The repo's
     `scripts\provision-local-player.ps1` can do this from `WOW_ACCOUNT_NAME` /
     `WOW_ACCOUNT_PASSWORD` env vars (secrets never stored) — **run it yourself; do not commit
     credentials.**
   - Grant it GM authority so `.gm on` works (auth DB, not character data), e.g. set its
     `acore_auth.account_access.gmlevel = 3` for its realm. This is an operator step.
2. **Find the observer character GUID** (read-only):
   ```sql
   SELECT guid, name FROM acore_characters.characters WHERE name = '<ObserverName>';
   ```
3. **Enroll the GUID** in `server\configs\modules\playerbots.conf` (option now documented in
   `conf\playerbots.conf.dist`):
   ```
   AutoWow.ObserverGuids = 5           # the observer's character GUID; empty = no observers
   ```
4. **Build the ObserverControl bridge hook** (adds the `observe` order). Standard module rebuild:
   ```powershell
   .\scripts\build.ps1                 # compiles worldserver incl. the new src/AutoWow files
   .\scripts\stop-server.ps1 ; .\scripts\start-server.ps1
   ```
   The ObserverControl hook is now built and installed in the 2026-07-13 RelWithDebInfo server.
   Before that build, the `observe` order returned `unknown command` and the scripts stayed in
   safe dry-run (they detect and report `observe_endpoint_not_built`).
5. **Launch the client and log in the observer** (human GUI step): run `work\wow-client\Wow.exe`,
   log in the observer account, enter world.

## 6. Commands & scripts (the isolated slice)

New files added by this slice:

| File | Role |
|---|---|
| `src/AutoWow/ObserverControl.h` / `.cpp` | Isolated server-side observer logic (allow-list, protection gate, no-group/not-a-bot guards, relocate/status/watch/release/protect). |
| `AutoWowBridge.cpp` (4 lines) | Dispatches the `observe` order to `AutoWowObserver::Dispatch`. |
| `conf/playerbots.conf.dist` (+1 option) | Declares `AutoWow.ObserverGuids` (default empty). |
| `scripts/observer-control.ps1` | Thin loopback client for `observe <sub> <observer> [<leader>]`. |
| `scripts/observer-relocate.ps1` | Periodic relocation loop, **dry-run by default**, `-Execute` to act. |
| `scripts/tests/observer-control.tests.ps1` | Pester tests: offline request-construction + live allow-list enforcement (auto-skips offline). |

Wire protocol (loopback, one line per connection):
```
observe protect  <observer-guid>
observe watch    <observer-guid> <leader-guid>
observe relocate <observer-guid>
observe status   <observer-guid>
observe release  <observer-guid>
```

Typical use once prerequisites are met:
```powershell
# 1. Protect the observer (or do .gm on + .gm visible off in the client)
.\scripts\observer-control.ps1 -Action protect -ObserverGuid 5

# 2. Follow Northstar (Brandreas, guid 10), server-authoritative relocation every 15s
.\scripts\observer-relocate.ps1 -ObserverGuid 5 -LeaderGuid 10 -Execute

# switch to Ember (Saewash, guid 7): stop, then re-run with -LeaderGuid 7
```

Dry-run (safe before the rebuild — observes and logs intended moves, issues no relocation):
```powershell
.\scripts\observer-relocate.ps1 -ObserverGuid 5 -LeaderGuid 10 -DurationMinutes 30
```

## 7. Risk controls

- **Invisibility / non-interference:** relocation is refused unless the observer is
  `IsGameMaster() && !isGMVisible()`. GM mode makes it untargetable, aggro-free, and invisible to
  players. `observe protect` also disables GM chat and whisper acceptance.
- **No economic/combat interference:** server refuses to relocate an observer that is **in a
  group** (would split bot XP/loot) or **in combat**. The observer is never added to a bot party;
  "switching groups" = switching the watched leader only.
- **Never drives a bot:** every observe call refuses if the target GUID resolves to a
  Playerbots-controlled character (`GetPlayerbotAI != null`).
- **Allow-list default-closed:** empty `AutoWow.ObserverGuids` means no character can be an
  observer. Only the enrolled GUID is accepted.
- **No Playerbots logic touched:** quest, recovery, movement, and targeting code are untouched;
  the observer path only reads a leader's position and teleports the observer.
- **No DB character writes:** the surface issues no SQL. `TeleportTo` mutates only the observer's
  own runtime position (its position persists on its own character on the next normal save — this
  is the observer's character, never a bot, and never quest/XP/money/item/objective data).
- **No random movement, no client automation:** relocation is explicit and server-driven; the
  scripts never send input to Wow.exe.
- **Dry-run by default:** `observer-relocate.ps1` performs no relocation unless `-Execute`, and it
  reverts to dry-run the moment any guard trips (protection lost, grouped, endpoint missing).
- **Capture safety:** screenshots/video are captured from the human's client window only; no game
  state is altered by capture.

### Manual fallback (zero rebuild, available today)

Even without the automated ObserverControl order, a human logged in on a GM observer can protect and
follow with built-in GM commands — no code, no scripts:
```
/console gm on            (or .gm on)
.gm visible off
.appear Brandreas         (relocate to Northstar leader; use Saewash for Ember)
```
This is manual and client-side; the `observe` order exists to make it automated, server-authoritative,
and allow-list-safe.

## 8. Verification steps

**Headless (no client, safe to run now):**
1. **Parse + build the hook:** completed; `src/AutoWow/ObserverControl.cpp` and the bridge change
   are present in the installed worldserver build with no compiler errors.
2. **Unit tests:** completed with Pester 9/9 passing while the rebuilt server was online. The tests
   cover request construction, argument validation, and live allow-list refusal.
3. **Live allow-list negative:** completed; `observe status 4293918596` returned
   `{"ok":false,"error":"not_an_observer"}` with the default-empty allow-list.
4. **Dry-run loop:** `observer-relocate.ps1 -ObserverGuid <g> -LeaderGuid 10` writes
   `leader_observed` + `relocate_dry_run` receipts and issues no relocation.

**In-world (human GUI step):**
5. Log the observer in; `observe protect <g>` → `observe status <g>` shows
   `protected:true, is_gm:true, gm_visible:false, in_group:false`.
6. `observe watch <g> 10` then `observe relocate <g>` → observer teleports to Brandreas; a second
   relocate at the same spot returns `already_positioned`. Confirm the bots' target/combat/XP are
   unaffected (compare `snapshot 10` before/after; the observer should not appear as anyone's target).
7. Switch `-LeaderGuid 7` and confirm relocation to Saewash (Ember), including cross-map if the
   parties are on different maps.
8. Capture a screenshot/short clip from the client to confirm the camera frames the leader.

## 9. Screenshot / video capture (human client)

Capture is intentionally decoupled from the control surface:
- **Screenshots:** in-client `PrintScreen` (writes to `work\wow-client\Screenshots\`), or an OS
  tool; no automation required.
- **Video:** OBS / Xbox Game Bar recording the Wow.exe window.
The overlay that annotates captures with watched-party stats is **deferred to a later phase** and
is not implemented here; when built it will read `quest-telemetry.ps1` output, not the client.

## 10. File manifest (this slice)

```
OBSERVER_CAMERA_V0_PLAN.md                                   (this document)
azerothcore-wotlk/modules/mod-playerbots/
  src/AutoWow/ObserverControl.h                              (new, isolated)
  src/AutoWow/ObserverControl.cpp                            (new, isolated)
  src/AutoWow/AutoWowBridge.cpp                              (+4 lines: dispatch only)
  conf/playerbots.conf.dist                                  (+1 documented option)
scripts/observer-control.ps1                                 (new)
scripts/observer-relocate.ps1                                (new, dry-run default)
scripts/tests/observer-control.tests.ps1                     (new)
```

## 11. Status & honest limits

- The C++ hook and all scripts are built, statically checked, and live allow-list tested. End-to-end
  relocation + capture still require the human GUI steps in §5/§8: a provisioned GM observer
  character must be logged in and enrolled.
- The observer is a separate character; its own position persists normally on save. This is not a
  bot and not quest/economy data, so it is within the "no character-data modification" guardrail
  for bots — but note it for completeness.
- Unrelated to the observer: the New-RPG engine's own 90s teleport fallback (see
  `QUEST_RECOVERY_V0.md`) affects **bots**, not the observer, and is untouched here.
```
