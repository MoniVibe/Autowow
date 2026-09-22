#!/bin/bash
# Soak metrics sampler (WSL). One JSON line per interval to /root/autowow-soak/metrics-<run>.jsonl.
# Usage: soak-metrics.sh <run_id> <interval_s> <duration_s>
RUN="$1"; INT="${2:-60}"; DUR="${3:-7200}"
OUT=/root/autowow-soak/metrics-$RUN.jsonl; L=/root/autowow-soak/logs
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
  P=$(pgrep -x worldserver | head -1)
  if [ -z "$P" ]; then echo "{\"utc\":\"$(date -u +%FT%TZ)\",\"world\":null}" >> $OUT; sleep "$INT"; continue; fi
  read RSS CPU < <(ps -o rss=,pcpu= -p "$P")
  AV=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
  SW=$(awk '/SwapTotal/{t=$2}/SwapFree/{f=$2}END{print int((t-f)/1024)}' /proc/meminfo)
  DIFF=$(grep -a "Update time diff" $L/Server.log | tail -1 | grep -oE '[0-9]+ms' | head -1 | tr -d ms)
  LED=$(wc -l < $L/ledger.log 2>/dev/null || echo 0)
  ERR=$(wc -l < $L/Errors.log 2>/dev/null || echo 0)
  echo "{\"utc\":\"$(date -u +%FT%TZ)\",\"pid\":$P,\"rss_mb\":$((RSS/1024)),\"cpu\":$CPU,\"memavail_mb\":$AV,\"swap_used_mb\":$SW,\"last_diff_ms\":${DIFF:-null},\"ledger_lines\":$LED,\"error_lines\":$ERR}" >> $OUT
  sleep "$INT"
done
