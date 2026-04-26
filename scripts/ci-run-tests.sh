#!/usr/bin/env bash
# Run ctest with retry logic for flaky tests.
#
# Usage:
#   scripts/ci-run-tests.sh <test_pattern> [max_attempts]
set -euo pipefail

PATTERN="${1:?Usage: ci-run-tests.sh <test_pattern> [max_attempts]}"
MAX_ATTEMPTS="${2:-3}"

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
  echo "── Attempt $attempt / $MAX_ATTEMPTS ──"
  if ctest --test-dir build -R "$PATTERN" --output-on-failure --parallel 4; then
    echo "✅ Tests passed on attempt $attempt"
    exit 0
  fi
  if [ "$attempt" -lt "$MAX_ATTEMPTS" ]; then
    echo "⚠️  Tests failed, retrying..."
    sleep 2
  fi
done

echo "❌ Tests failed after $MAX_ATTEMPTS attempts"
exit 1
