# Isolated Phase 1 WSL proof build

`scripts/phase1-wsl-proof-build.ps1` owns only the existing Ubuntu
`/root/p1core` source and `/root/p1core/build` tree. It does not call the
Windows `scripts/build.ps1`, install or deploy binaries, edit source, or start,
stop, or restart a server.

The default action is a non-mutating plan. `Build` is rejected unless `-Apply`
is explicit. Before configuring, an apply run resolves both Linux paths,
requires the existing CMake cache to belong to `/root/p1core`, and refuses to
continue if a WSL `worldserver` process is running. It does not stop that
process.

## Fixed profile

The repeatable profile is:

| Setting | Required value |
|---|---|
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` |
| `BUILD_TESTING` | `ON` |
| `MODULES` | `static` |
| `SCRIPTS` | `minimal-static` |
| `SCRIPTS_OUTLAND` | `static` |
| `SCRIPTS_NORTHREND` | `static` |
| `SCRIPTS_KALIMDOR` | `static` |

This preserves the known smaller static profile and adds the Northrend and
Kalimdor script groups needed by Utgarde Keep/Ingvar and Onyxia's Lair. It does
not use the prior full-static profile that triggered GCC internal compiler
errors.

The only build targets are `unit_tests` and `worldserver`. GNU Make receives
only `-j1`. The isolated CMake cache sets
`CMAKE_CXX_FLAGS_RELWITHDEBINFO=-O0 -g -DNDEBUG`, which retains CMake's generated
C++20 mode, definitions, include paths, and target flags while disabling the
optimizer that previously triggered GCC internal compiler errors.

## Commands for the maintenance window

Inspect the exact argument arrays without contacting WSL:

```powershell
pwsh -NoProfile -File .\scripts\phase1-wsl-proof-build.ps1 -Action Plan
```

Configure and build the next isolated proof binary only when rebuilding is
authorized:

```powershell
pwsh -NoProfile -File .\scripts\phase1-wsl-proof-build.ps1 -Action Build -Apply
```

That command is equivalent to these Linux commands, after the isolation and
process guards pass:

```bash
cmake -S /root/p1core -B /root/p1core/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DMODULES=static \
  -DSCRIPTS=minimal-static \
  -DSCRIPTS_OUTLAND=static \
  -DSCRIPTS_NORTHREND=static \
  -DSCRIPTS_KALIMDOR=static

cmake --build /root/p1core/build \
  --target unit_tests worldserver -- -j1
```

No install step follows. The proof binary remains:

```text
/root/p1core/build/src/server/apps/worldserver
```

## Build manifest

After both targets succeed, the script verifies both files are executable and
writes:

```text
work\phase1-wsl-proof-build\build-manifest.json
```

The manifest records the exact requested and observed CMake flags, build
arguments, target paths, SHA-256 hashes for `unit_tests` and `worldserver`, and
configure/build log paths. It is written only after a successful configure,
cache re-check, build, artifact check, and hash calculation.

## Startup log gate

Server lifecycle remains a separate, manually authorized operation. After the
new isolated binary has been started by the existing maintenance procedure,
validate its freshly redirected startup logs with:

```powershell
pwsh -NoProfile -File .\scripts\phase1-wsl-proof-build.ps1 `
  -Action ValidateStartupLogs `
  -StartupLogPath .\logs\phase1-runtime\worldserver-wsl-stdout.log `
  -ErrorLogPath .\logs\phase1-runtime\worldserver-wsl-stderr.log
```

Validation is read-only and rejects `-Apply`. It fails closed unless:

- the completed build manifest has the expected schema and fixed Linux paths;
- the current isolated `worldserver` hash matches the manifest;
- both redirected logs exist and are newer than the completed build;
- stdout contains the `(worldserver-daemon) ready...` marker; and
- neither log contains a `has no code` error for `boss_onyxia`,
  `instance_onyxias_lair`, `npc_onyxian_lair_guard`, or any script name
  containing `Ingvar` (case-insensitive).

The result is written to
`work\phase1-wsl-proof-build\startup-validation.json`. Any failed gate writes a
`FAIL_CLOSED` receipt and returns a terminating error.

## Offline verification

The focused tests never call WSL, CMake, MySQL, or a server process:

```powershell
Invoke-Pester -Path .\scripts\tests\phase1-wsl-proof-build.tests.ps1 -Output Detailed
```
