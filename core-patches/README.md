# Core patches
AzerothCore core source lives in WSL /root/autowow-advisor-t1-core (git, base = pinned mod-playerbots/azerothcore-wotlk 52f58186).
`series/` = `git format-patch 52f58186a..main` (authoritative). `core-delta-vs-52f58186.patch` + `untracked/` = pre-git snapshot (superseded).
Apply: `git am core-patches/series/*.patch` on a 52f58186 checkout.
