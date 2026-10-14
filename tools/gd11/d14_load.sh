#!/usr/bin/env bash
# d14_load.sh -- Phase 11.6 D-14: the API clients run next to the real drives
# while they move (started by run_w_10_9.sh through DURING, stopped with TERM).
#
#   d14_load.sh API_DIR SECONDS OUT_DIR
#
# Waits up to 30 s for API_DIR/stats.json, then on CPUs 1-2:
#   sampler   stats.json at 2 Hz -> OUT_DIR/samples.jsonl (t, fsm, axes pos/state)
#   reader    a second stats.json reader at 2 Hz (a web UI tab)
#   spam      sdo_read 0x1018:1 of slave 1 at 20 Hz -> OUT_DIR/spam.txt
#   sdo6064   sdo_read 0x6064:0 (position actual) of both slaves at 1 Hz
#             -> OUT_DIR/sdo6064.txt (the SDO path sees the motion)
#   churn     connect, hold 5 s, disconnect, in a loop
# Without stats.json after 30 s (API off: the negative control) it writes
# OUT_DIR/no_api and exits.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
D=$1; SEC=$2; OUT=$3
mkdir -p "$OUT"
KIDS=()
stop () { for p in "${KIDS[@]}"; do kill -TERM "$p" 2>/dev/null; done; wait 2>/dev/null; exit 0; }
trap stop TERM INT
echo "no $D/stats.json (yet)" > "$OUT/no_api"     # stays if the run ends first
for _ in $(seq 300); do [ -s "$D/stats.json" ] && break; sleep 0.1; done
[ -s "$D/stats.json" ] || { echo "no $D/stats.json after 30 s" > "$OUT/no_api"; exit 0; }
rm -f "$OUT/no_api"
CTL=(taskset -c 1-2 python3 "$HERE/ecm_ctl.py" --sock "$D/ctl.sock")

taskset -c 1-2 python3 - "$D/stats.json" "$OUT/samples.jsonl" <<'EOF' &
import json, sys, time
src, dst = sys.argv[1], sys.argv[2]
t0 = time.time()
with open(dst, "w", buffering=1) as f:
    while True:
        try:
            d = json.load(open(src))
            f.write(json.dumps({"t": round(time.time() - t0, 2), "fsm": d.get("bus", {}).get("fsm"),
                                "axes": [{"state": a.get("state"), "pos": a.get("pos")} for a in d.get("axes", [])]}) + "\n")
        except Exception as e:
            f.write(json.dumps({"t": round(time.time() - t0, 2), "error": str(e)}) + "\n")
        time.sleep(0.5)
EOF
KIDS+=($!)
taskset -c 1-2 python3 -c '
import json, sys, time
while True:
    try: json.load(open(sys.argv[1]))
    except Exception: pass
    time.sleep(0.5)' "$D/stats.json" &
KIDS+=($!)
SP=$((SEC - 12)); [ $SP -lt 5 ] && SP=5   # ends before ecm_run so its summary line is written
"${CTL[@]}" spam "$SP" 20 1 0x1018 1 u32 > "$OUT/spam.txt" 2>&1 &
KIDS+=($!)
( while :; do for s in 1 2; do echo "$(date +%s.%N) $s $("${CTL[@]}" read $s 0x6064 0 i32 2>&1 | tr -d '\n')"; done; sleep 1; done ) > "$OUT/sdo6064.txt" &
KIDS+=($!)
( while :; do "${CTL[@]}" hold 5 > /dev/null 2>&1; sleep 0.2; done ) &
KIDS+=($!)
wait
