# Phase 1 developer cycle

This runbook covers the canonical Playerbots module at:

    D:\Games\wowstuff\AutoWoW\_phase1_worktree\mod-playerbots

The WSL staging target is fixed and guarded:

    /root/p1core/modules/mod-playerbots

The lifecycle script is dry-run by default. It does not edit database or
runtime configuration files and does not print configuration contents or
credentials.

## Actions

Plan is local and non-mutating. It prints the exact rsync, build, test, and
lifecycle sequence without contacting WSL:

    pwsh -NoProfile -File .\scripts\phase1-dev-cycle.ps1 -Action Plan

Doctor is read-only. It checks the exact canonical, staging, build, CMake
cache, runtime, and log paths; Ubuntu-24.04; rsync and cmake; the CMake source
guard; worldserver and bridge state; and free disk/RAM thresholds. Artifact
executable checks are advisory because a first build may create them.

    pwsh -NoProfile -File .\scripts\phase1-dev-cycle.ps1 -Action Doctor

Deploy requires an explicit apply switch:

    pwsh -NoProfile -File .\scripts\phase1-dev-cycle.ps1 -Action Deploy -FocusedGTestFilter '*QuestObjective*:*Dungeon*:*RaidFixture*' -Apply

The default focused filter covers the shared quester and raider cycle. It can
be narrowed or widened explicitly, for example:

    pwsh -NoProfile -File .\scripts\phase1-dev-cycle.ps1 -Action Deploy -FocusedGTestFilter 'FixtureFactoryContract.*' -Apply

## Deploy sequence and safety gates

Deploy first requires the exact Windows root and canonical module path, then
runs the read-only Doctor checks. The staging directory is resolved again and
must equal /root/p1core/modules/mod-playerbots immediately before rsync.

The existing worldserver remains live while all source/build gates run. The
sequence is:

1. Capture the current worldserver/bridge state without stopping it.
2. Rsync the canonical module with rsync --archive --delete.
3. Build unit_tests.
4. Run the configurable focused gtest filter.
5. Build worldserver.
6. Hash /root/p1core/build/src/server/apps/worldserver.
7. Archive logs\phase1-runtime\Playerbots.log.
8. Invoke the existing stop script if worldserver or its runtime state is present.
9. Invoke the existing start script once.

Linux can keep the old worldserver inode mapped while the linker replaces the
artifact. Therefore rsync/build/test/hash failures leave the live runtime
untouched. After a successful hash, a stop or start failure is recorded as
FAILED_CLOSED; the script never restarts a stale server after a failed build or
test, and there is no automatic rollback implementation.

The rsync excludes .git, build directories, generated directories, log
directories, and log files. The script does not prepare or edit DB/config
files.

## Receipts

Each apply run writes both files under:

    logs\phase1-dev-cycle\

The JSONL file contains one sanitized event per lifecycle transition. The JSON
file is the final summary with status DEPLOYED or FAILED_CLOSED, artifact hash,
runtime state, archive path, and failure reason where relevant. No command
output or configuration contents are placed in these receipts.

## Offline verification

The focused Pester tests use only the pure plan and JSONL emit seams. They do
not call WSL, rsync, cmake, MySQL, or a server process:

    Invoke-Pester -Path .\scripts\tests\phase1-dev-cycle.tests.ps1 -Output Detailed

PowerShell parser check:

    $tokens = $null; $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path .\scripts\phase1-dev-cycle.ps1), [ref]$tokens, [ref]$errors) | Out-Null
    @($errors).Count
