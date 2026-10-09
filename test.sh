#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "$0")"
mkdir -p results
workload=${1:-workloada.spec}

run() {
  local name=$1
  shift
  perf stat -ddd -o "results/$name" -- "$@" \
    src/redis-server redis.conf --ycsb-run "$workload" \
    | tee "results/${name}.out"
  grep YUKON_YCSB "results/${name}.out" >> "results/$name"
}

run libc
run yukon_nohandle env LD_PRELOAD=./libyukon_stub_nohandle.so
run yukon_handle env LD_PRELOAD=./libyukon_stub_handle.so
