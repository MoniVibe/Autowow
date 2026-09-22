#!/bin/bash
# Soak config swap for AutoWoW bulk zone soak (docs/BULK_SOAK_PLAN.md section 2A/5).
# Usage: soak-config.sh apply <profile-file>   -- back up live confs once, rewrite named keys only, print key diff
#        soak-config.sh restore                -- restore the pre-soak backups byte-for-byte
# Profile file: lines "pb|Key|Value" (playerbots.conf) or "ws|Key|Value" (worldserver.conf).
# Never prints full config files (BotGuids and DB lines stay private).
set -euo pipefail
PB=/usr/local/etc/modules/playerbots.conf
WS=/root/p1runtime/worldserver.conf
BK=/root/autowow-soak/config-backup
mode="${1:-}"

hash_all() { sha256sum "$PB" "$WS"; }

if [ "$mode" = "restore" ]; then
  test -f "$BK/playerbots.conf.pre-soak" && test -f "$BK/worldserver.conf.pre-soak"
  if pgrep -x worldserver >/dev/null; then echo "refuse: worldserver running" >&2; exit 2; fi
  cp -p "$BK/playerbots.conf.pre-soak" "$PB"
  cp -p "$BK/worldserver.conf.pre-soak" "$WS"
  hash_all; cat "$BK/pre-soak.sha256"
  exit 0
fi

[ "$mode" = "apply" ] || { echo "usage: $0 apply <profile>|restore" >&2; exit 1; }
PROFILE="$2"
if pgrep -x worldserver >/dev/null; then echo "refuse: worldserver running" >&2; exit 2; fi
mkdir -p "$BK"; chmod 700 "$BK"
if [ ! -f "$BK/playerbots.conf.pre-soak" ]; then
  cp -p "$PB" "$BK/playerbots.conf.pre-soak"
  cp -p "$WS" "$BK/worldserver.conf.pre-soak"
  hash_all > "$BK/pre-soak.sha256"
fi
tmpPB=$(mktemp); tmpWS=$(mktemp)
cp -p "$PB" "$tmpPB"; cp -p "$WS" "$tmpWS"
while IFS='|' read -r file key value; do
  [ -z "${file// }" ] && continue
  case "$file" in \#*) continue;; esac
  case "$file" in pb) t="$tmpPB";; ws) t="$tmpWS";; *) echo "bad file tag $file" >&2; exit 3;; esac
  n=$(grep -cE "^[[:space:]]*${key//./\\.}[[:space:]]*=" "$t" || true)
  if [ "$n" = "1" ]; then
    kre="${key//./\\.}"
    python3 - "$t" "$kre" "$key" "$value" <<'PY'
import re, sys
path, kre, key, value = sys.argv[1:5]
with open(path, 'r', newline='') as f: lines = f.readlines()
pat = re.compile(r'^\s*' + kre + r'\s*=')
for i, l in enumerate(lines):
    if pat.match(l):
        eol = '\r\n' if l.endswith('\r\n') else '\n'
        lines[i] = key + ' = ' + value + eol
with open(path, 'w', newline='') as f: f.writelines(lines)
PY
  elif [ "$n" = "0" ]; then
    printf '%s = %s\n' "$key" "$value" >> "$t"
  else
    echo "key $key appears $n times in $file; refusing" >&2; exit 4
  fi
done < "$PROFILE"
echo "== playerbots.conf key diff"; diff "$PB" "$tmpPB" | grep -E '^[<>]' | sed -E 's/(BotGuids *= *).*/\1<list>/' || true
echo "== worldserver.conf key diff"; diff "$WS" "$tmpWS" | grep -E '^[<>]' | grep -viE 'DatabaseInfo|Password' || true
cp "$tmpPB" "$PB"; cp "$tmpWS" "$WS"; rm -f "$tmpPB" "$tmpWS"
hash_all
