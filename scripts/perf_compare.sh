#!/usr/bin/env bash
set -e

ITEMS=${1:-200000}
RUNS=${2:-1}
BIN=${BIN:-./build/benchmark_runner}

mkdir -p results

EVENTS=task-clock,context-switches,cpu-migrations,cycles,instructions,cache-references,cache-misses

echo "Running scheduler-controlled benchmark..."
perf stat -e "$EVENTS" -o results/perf_scheduler.txt -- \
    "$BIN" --items "$ITEMS" --runs "$RUNS" \
    --output results/throughput_scheduler.csv \
    --latency-output results/latency_scheduler.csv

echo
echo "Running pinned benchmark..."
perf stat -e "$EVENTS" -o results/perf_pinned.txt -- \
    "$BIN" --items "$ITEMS" --runs "$RUNS" --pin \
    --output results/throughput_pinned.csv \
    --latency-output results/latency_pinned.csv

echo
echo "Wrote:"
echo "  results/perf_scheduler.txt"
echo "  results/perf_pinned.txt"
echo "  results/throughput_scheduler.csv"
echo "  results/throughput_pinned.csv"
echo "  results/latency_scheduler.csv"
echo "  results/latency_pinned.csv"
