# AutoWow profession/economy telemetry wiring note

The implementation in `AutoWowProfessionEconomyTelemetry.{h,cpp}` is intentionally isolated from
the current `AutoWowBridge.cpp`. That bridge file is a collision boundary in the R13/R6/R5
integration worktree and must not be edited by this sidecar.

The intended read-only order is:

```text
professioneconomy <enrolled-bot-guid>
```

When Sol integrates the order in a coordinated bridge edit:

1. Add a new `AutoWowRequestType` value, for example `ProfessionEconomyTelemetry`.
2. Parse the exact two-token form `professioneconomy <guid>` and reject trailing tokens.
3. In the existing world-thread operation dispatch, apply the existing `IsLeagueMember(guid)`
   read-only enrollment gate before resolving the player.
4. Resolve the online `Player*` and Playerbot AI on the world thread, then return
   `AutoWowProfessionEconomyTelemetry::Build(bot, true)`.
5. Return the helper's JSON unchanged. It is schema
   `autowow.profession.economy.v1`, contains `read_only:true`, and has no action/command field.

The helper does not query SQL, so the bridge remains the owner of the existing league-membership
policy. Calling `Build(bot, false)` fails closed with `bot_not_enrolled_in_league`; passing a null
bot fails with `bot_not_online`. The world-thread precondition must be retained because the helper
reads live `Player`, spell, inventory, and DBC state.

The currently available Playerbots surfaces expose skills, known spellbook recipes (including
superseded inactive ranks), reagent and output-capacity facts, bag material totals, and money. Only
the usable current recipe rank can be `immediately_craftable`. No existing per-bot craft
attempt/success/failure counter was found, so `craft_evidence.available` is deliberately false and
must not be interpreted as zero successful crafts.

Suggested Sol verification after the coordinated bridge edit:

```text
professioneconomy 10
```

Verify that the response contains `schema=autowow.profession.economy.v1`, the enrolled bot GUID,
professions, a bounded recipe list, reagent `bag_count`/`crafts_available`, `bag_materials`,
`money_copper`, and `craft_evidence.available=false` unless a separate existing receipt source is
explicitly integrated.
