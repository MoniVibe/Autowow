# Dedicated rendering observer runbook

The automation owns one local identity only: account `AUTOWATCH`, human Alliance warrior
`Autowatch`, GM level 3, with no group. Account ID `3`, character GUID `21`, and `SHONHAY` are
never reused. Every collision or ownership ambiguity fails closed.

## 1. Plan, then provision

`Plan` creates a random 16-character alphanumeric password and writes it only as a current-Windows-
user DPAPI `SecureString` CLIXML file. It parses the loopback service credential from the active
`LoginDatabaseInfo` and `CharacterDatabaseInfo` entries in `server\configs\worldserver.conf` and
performs read-only collision checks. It requires neither `MYSQL_ROOT_PASSWORD` nor a server restart.

```powershell
pwsh .\scripts\provision-observer.ps1 -Mode Plan
```

Review `work\observer-state\observer-provisioning-plan.json`. It contains no password, salt,
verifier, or database service password. To perform the explicit live write later:

```powershell
pwsh .\scripts\provision-observer.ps1 -Mode Apply
```

`Apply` uses one named-lock transaction for AzerothCore-compatible SRP6 account registration
(`x = SHA1(salt || SHA1(UPPER(user):UPPER(password)))` as little-endian, then
`verifier = 7^x mod N` serialized as 32 little-endian bytes),
`realmcharacters`, exact all-realms GM3, the human character, and its home bind. Re-running is
idempotent. Existing `AUTOWATCH` must match the DPAPI password and own exactly `Autowatch`; the
script never changes an unknown existing account password.

This implementation was validated offline only; the implementation task did not execute `Apply`.

## 2. Enroll the resulting GUID

Read `work\observer-state\observer-state.json`, then place only its `character_guid` in the
operator-managed `AutoWow.ObserverGuids` setting. Do not use `21`. A normal operator-controlled
worldserver restart is required for that configuration change; none of these observer scripts
stop or start servers.

## 3. Prepare the isolated client

Point the command at a known 3.3.5 client. The destination defaults to `work\observer-client`.
Root binaries and small data are copied; same-volume MPQs are hard-linked (copied cross-volume).
`WTF`, `Cache`, `Logs`, `Screenshots`, `Interface`, and `realmlist.wtf` remain private. The source
client is never opened for write.

```powershell
pwsh .\scripts\prepare-observer-client.ps1 -SourceClientDirectory D:\path\to\clean-3.3.5-client
```

The private `Config.wtf` saves `AUTOWATCH` but no password and uses an 800x600 window with low
render settings.

## 4. Start, capture, stop

The start wrapper launches only the isolated `Wow.exe`, verifies PID, executable path, HWND, exact
title, and foreground ownership before every `SendKeys`, then decrypts the DPAPI password only in
memory. Because provisioning permits exactly one character, Enter selects `Autowatch`
deterministically. Relocation starts only after bridge `status` identifies `Autowatch`, `protect`
succeeds, and a second status proves GM-invisible, ungrouped, and out of combat.

```powershell
# Northstar; use -LeaderGuid 7 for Ember
pwsh .\scripts\start-observer-automation.ps1 -LeaderGuid 10

# Exact observer window only; output is timestamped under the private Screenshots directory
pwsh .\scripts\capture-observer-screenshot.ps1

# Stops only PIDs whose start time, path, command-line token, and (for WoW) HWND match the session
pwsh .\scripts\stop-observer-automation.ps1
```

## True limitations

- Login is timed DirectX UI automation. Slow realm/login screens, pop-ups, or a changed initial
  focus can time out; no relocation starts unless the bridge subsequently proves the exact observer
  online and protected.
- Windows `PrintWindow` can return a black frame for DirectX clients. The command detects a near-
  uniform black result and permits `CopyFromScreen` only for the exact observer window rectangle
  while that same HWND is foreground. It never captures the whole desktop by default.
- A minimized window is refused. A locked/disconnected desktop commonly makes rendering black or
  removes the foreground identity; capture then fails without saving a PNG.
- Hard-linked MPQs are shared immutable game data. Do not patch or update through the observer
  client; rebuild its working directory from a clean source when game data changes.

Offline verification:

```powershell
Invoke-Pester .\scripts\tests\observer-automation.tests.ps1
```
