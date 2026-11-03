#!/bin/bash
# Runs the four M4 experiment groups (docs/m4-design.md section 5) once
# each, three repetitions per configuration, strictly one at a time.
#
# Usage: scripts/run_benchmarks.sh <kv_bench binary> <data dir> <results dir> [smoke]
#   data dir     scratch space for databases (must be on the storage to measure)
#   results dir  raw JSON lines, environment, and logs are written here
#   smoke        tiny workloads, to check the pipeline only
#
# Every run's JSON line is appended to <results>/raw/<group>.jsonl with its
# repetition number. A run that exits nonzero or prints ok=false is recorded
# in <results>/raw/failures.jsonl and excluded by the summary script.
set -u
BENCH=$1
DATA=$2
OUT=$3
SMOKE=${4:-}

REPS=3
SEED=42
KEY=16
VALUE=100
# Insert groups: uniform random keys, sync on unless stated.
INSERT_OPS=20000
INSERT_MEMTABLE=65536
# Threshold group: overwrites and 10% deletes over a smaller keyspace.
TH_OPS=20000
TH_KEYSPACE=10000
TH_DELETE=10
# Read groups: the common setup (prepare) and the lookup counts.
PREP_KEYS=200000
PREP_MEMTABLE=262144
READS=50000
WARMUP=20000
if [ "$SMOKE" = smoke ]; then
  REPS=1; INSERT_OPS=500; TH_OPS=500; TH_KEYSPACE=250; PREP_KEYS=5000
  READS=1000; WARMUP=200
fi

mkdir -p "$OUT/raw" "$DATA"
COMMON="--seed=$SEED --key-size=$KEY --value-size=$VALUE"

run() {  # run <group> <rep> <label> <args...>
  local group=$1 rep=$2 label=$3; shift 3
  local line
  line=$("$BENCH" "$@" $COMMON 2>>"$OUT/stderr.log")
  local rc=$?
  local entry="{\"group\":\"$group\",\"label\":\"$label\",\"rep\":$rep,\"exit\":$rc,\"started\":\"$START\",\"result\":${line:-null}}"
  if [ $rc -ne 0 ] || ! printf '%s' "$line" | grep -q '"ok":true'; then
    echo "$entry" >> "$OUT/raw/failures.jsonl"
  else
    echo "$entry" >> "$OUT/raw/$group.jsonl"
  fi
  echo "$(date -Is) $group $label rep=$rep exit=$rc" >> "$OUT/progress.log"
}

# Group 1 and 2: synchronous inserts versus the benchmark-only sync bypass,
# each from its own fresh database with the same trace.
for rep in $(seq 1 $REPS); do
  for sync in on off; do
    START=$(date -Is)
    d="$DATA/insert-$sync-$rep"; rm -rf "$d"
    skip=0; [ $sync = off ] && skip=1
    run insert_sync "$rep" "sync_$sync" --mode=insert --dir="$d" \
      --ops=$INSERT_OPS --memtable-bytes=$INSERT_MEMTABLE --trigger=4 \
      --skip-sync=$skip
    rm -rf "$d"
  done
done

# Common read setup: one filter-bearing database, prepared once with the
# sync bypass (setup only; nothing measured depends on its durability).
START=$(date -Is)
P="$DATA/read-prepared"; rm -rf "$P"
"$BENCH" --mode=prepare --dir="$P" --ops=$PREP_KEYS \
  --memtable-bytes=$PREP_MEMTABLE --trigger=4 --skip-sync=1 $COMMON \
  > "$OUT/raw/prepare.json" 2>>"$OUT/stderr.log"

# Group 1 (reads) and group 3: the same files and the same lookup traces,
# with filter queries used or bypassed, alternating.
for rep in $(seq 1 $REPS); do
  for bypass in 0 1; do
    START=$(date -Is)
    run read_bloom "$rep" "bypass_$bypass" --mode=read --dir="$P" \
      --ops=$PREP_KEYS --reads=$READS --warmup=$WARMUP \
      --memtable-bytes=$PREP_MEMTABLE --trigger=4 --bypass-bloom=$bypass
  done
done

# Group 4: run thresholds with the same trace and final boundary.
for rep in $(seq 1 $REPS); do
  for trigger in 2 4 8 0; do
    START=$(date -Is)
    d="$DATA/threshold-$trigger-$rep"; rm -rf "$d"
    run threshold "$rep" "trigger_$trigger" --mode=insert --dir="$d" \
      --ops=$TH_OPS --keyspace=$TH_KEYSPACE --delete-percent=$TH_DELETE \
      --memtable-bytes=$INSERT_MEMTABLE --trigger=$trigger
    rm -rf "$d"
  done
done
echo "$(date -Is) done" >> "$OUT/progress.log"
