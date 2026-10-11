#!/usr/bin/env bash
# Runs every benchmark and prints the results:
#   1. the risk check on its own
#   2. each stage of handling one order inside the gateway
#   3. end-to-end FIX throughput and latency: oems_load_client against a live
#      mini_oems on this machine, first with the in-memory benchmark config, then
#      with the production-like config (messages stored and logged to disk)
#
# Usage: scripts/bench.sh            (after scripts/build.sh)
# The client and the gateway share this machine's CPUs, so close other work first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
BIN="$MINI_OEMS_BUILD_ROOT/release"
RUN_DIR="$(mktemp -d)" # the gateway's store and log files go here, not into the repo
GATEWAY_PID=""

stop_gateway() {
  if [ -n "$GATEWAY_PID" ]; then
    kill "$GATEWAY_PID" 2>/dev/null || true
    wait "$GATEWAY_PID" 2>/dev/null || true
    GATEWAY_PID=""
  fi
}
trap stop_gateway EXIT

start_gateway() {
  (cd "$RUN_DIR" && exec "$BIN/mini_oems" "$ROOT/config/$1" "$ROOT/config/instruments.csv" \
    "$ROOT/config/accounts.csv" > "gateway-$1.log" 2>&1) &
  GATEWAY_PID=$!
  sleep 2
}

load() { "$BIN/bench/oems_load_client" "$ROOT/config/load_client.cfg" "$ROOT/config/instruments.csv" ACC1 "$@"; }

echo "== Machine"
uname -srm # not -a: no hostname in shared results
[ -r /proc/cpuinfo ] && grep -m1 "model name" /proc/cpuinfo
command -v nproc > /dev/null && echo "logical CPUs: $(nproc)"

echo
echo "== 1. Risk check"
"$BIN/bench/risk_benchmark" --benchmark_repetitions=10 --benchmark_report_aggregates_only=true \
  --benchmark_min_time=0.5s | grep -v "^Running "

echo
echo "== 2. Pipeline stages"
"$BIN/bench/pipeline_benchmark" --benchmark_repetitions=5 --benchmark_report_aggregates_only=true | grep -v "^Running "

for config in bench_acceptor.cfg acceptor.cfg; do
  echo
  echo "== 3. End to end, gateway config $config"
  start_gateway "$config"
  for run in 1 2 3; do
    echo "-- throughput run $run: 100,000 orders, up to 1,000 in flight"
    load 100000 1000
  done
  echo "-- latency: 20,000 orders, one at a time"
  load 20000 1
  stop_gateway
done
