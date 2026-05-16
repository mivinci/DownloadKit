#!/usr/bin/env bash
# run_micro_bench.sh - Run moo micro-benchmarks
#
# Usage:
#   ./scripts/run_micro_bench.sh [filter]
#
# Examples:
#   ./scripts/run_micro_bench.sh              # Run all micro-benchmarks
#   ./scripts/run_micro_bench.sh heap          # Run only heap benchmarks
#   ./scripts/run_micro_bench.sh "BM_Heap.*"   # Run with gbenchmark filter
#
# Prerequisites:
#   cmake -B build -DMOO_BUILD_BENCHMARKS=ON && cmake --build build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/../build}"
RESULTS_DIR="${RESULTS_DIR:-${SCRIPT_DIR}/../bench_results}"
FILTER="${1:-}"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

mkdir -p "$RESULTS_DIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# All known micro-benchmark targets
BENCH_NAMES=(
  heap_bench
  mpsc_bench
  memory_bench
  event_bench
  timer_bench
  buf_bench
  ring_bench
  io_bench
)

# Search directories for benchmark binaries
SEARCH_DIRS=(
  "$BUILD_DIR/libx/x/base"
  "$BUILD_DIR/libx/x/buf"
  "$BUILD_DIR"
)

found=0

for bench in "${BENCH_NAMES[@]}"; do
  # If a filter is given and doesn't match the bench name, skip
  if [ -n "$FILTER" ] && [[ "$bench" != *"$FILTER"* ]]; then
    continue
  fi

  bin=""
  for dir in "${SEARCH_DIRS[@]}"; do
    if [ -x "$dir/$bench" ]; then
      bin="$dir/$bench"
      break
    fi
  done

  if [ -z "$bin" ]; then
    warn "$bench not found, skipping"
    continue
  fi

  out="$RESULTS_DIR/${bench}_${TIMESTAMP}.json"
  info "Running $bench → $out"

  # Pass through any gbenchmark filter if it looks like a regex
  if [ -n "$FILTER" ] && [[ "$FILTER" == BM_* ]]; then
    "$bin" --benchmark_format=console --benchmark_out="$out" \
           --benchmark_out_format=json --benchmark_filter="$FILTER"
  else
    "$bin" --benchmark_format=console --benchmark_out="$out" \
           --benchmark_out_format=json
  fi

  found=$((found + 1))
  info "$bench completed"
  echo ""
done

if [ "$found" -eq 0 ]; then
  warn "No benchmarks found. Build with: cmake -B build -DMOO_BUILD_BENCHMARKS=ON && cmake --build build"
  exit 1
fi

info "All micro-benchmarks completed. Results in: $RESULTS_DIR"
