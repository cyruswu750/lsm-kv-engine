#!/usr/bin/env bash
# Runs the differential workloads for more seeds or operations than CI.
#
# Usage: scripts/run_differential.sh [BLOCKS] [OPS] [BUILD_DIR]
#   BLOCKS     number of 20-seed blocks (default 5: seeds 1..100)
#   OPS        operations per seed and workload (default 5000)
#   BUILD_DIR  build directory containing tests/harness/kv_tests (default build)
#
# Set KV_DIFF_REOPEN_EVERY to change the periodic-reopen interval (default 500).
# A failure prints the seed and operation index and writes a .trace file to
# the current directory.
set -euo pipefail

blocks="${1:-5}"
ops="${2:-5000}"
build="${3:-build}"
bin="${build}/tests/harness/kv_tests"

if [[ ! -x "${bin}" ]]; then
    echo "missing test binary: ${bin}" >&2
    exit 1
fi

for ((block = 0; block < blocks; block++)); do
    first=$((1 + block * 20))
    echo "== seeds ${first}..$((first + 19)), ${ops} ops"
    KV_DIFF_FIRST_SEED="${first}" KV_DIFF_OPS="${ops}" \
        "${bin}" --gtest_filter='Seeds/DifferentialTest.*' --gtest_brief=1
done
