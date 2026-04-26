#!/usr/bin/env bash
# Detect changed modules and output a GitHub Actions matrix JSON array.
#
# Usage:
#   scripts/ci-detect-changes.sh <event_name> <base_sha> <head_sha>
#
# Outputs:
#   matrix=<JSON array>  — written to $GITHUB_OUTPUT
set -euo pipefail

EVENT_NAME="${1:-push}"
BASE_SHA="${2:-}"
HEAD_SHA="${3:-}"

# ── Module dependency map ────────────────────────────────────────────────
# Requires bash >= 4 for associative arrays
if [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then
  echo "ERROR: This script requires bash >= 4 (found ${BASH_VERSION:-unknown})." >&2
  exit 1
fi

declare -A DEPENDENTS
DEPENDENTS[xbase]="xbase xlog xbuf xnet xcrypto xhttp xp2p xfer xai"
DEPENDENTS[xlog]="xlog"
DEPENDENTS[xbuf]="xbuf xnet xcrypto xhttp xp2p xfer xai"
DEPENDENTS[xnet]="xnet xhttp xp2p xfer xai"
DEPENDENTS[xcrypto]="xcrypto xp2p xfer"
DEPENDENTS[xhttp]="xhttp xai"
DEPENDENTS[xp2p]="xp2p xfer"
DEPENDENTS[xfer]="xfer"
DEPENDENTS[xai]="xai"

ALL_MODULES="xbase xlog xbuf xnet xcrypto xhttp xp2p xfer xai"

# ── Platform combinations ────────────────────────────────────────────────
PLATFORMS='
{"os":"macos-latest","compiler":"clang","cc":"clang","cxx":"clang++","tls_backend":"openssl"}
{"os":"macos-latest","compiler":"clang","cc":"clang","cxx":"clang++","tls_backend":"mbedtls"}
{"os":"ubuntu-latest","compiler":"gcc","cc":"gcc","cxx":"g++","tls_backend":"openssl"}
{"os":"ubuntu-latest","compiler":"gcc","cc":"gcc","cxx":"g++","tls_backend":"mbedtls"}
'

# ── Determine changed files ──────────────────────────────────────────────
if [ "$BASE_SHA" = "0000000000000000000000000000000000000000" ]; then
  CHANGED_FILES=$(git diff --name-only HEAD~1 HEAD 2>/dev/null || git ls-files)
else
  CHANGED_FILES=$(git diff --name-only "$BASE_SHA" "$HEAD_SHA" 2>/dev/null || git diff --name-only HEAD~1 HEAD)
fi

echo "Changed files:"
echo "$CHANGED_FILES"

# ── Check if CI / root build files changed ───────────────────────────────
ALWAYS_RUN=false
if echo "$CHANGED_FILES" | grep -qE '^(\.github/workflows/ci\.y|CMakeLists\.txt|cmake/)'; then
  ALWAYS_RUN=true
fi

# ── Compute affected modules ─────────────────────────────────────────────
AFFECTED_MODULES=""
if [ "$ALWAYS_RUN" = "true" ]; then
  AFFECTED_MODULES="$ALL_MODULES"
else
  for module in $ALL_MODULES; do
    if echo "$CHANGED_FILES" | grep -q "^modules/${module}/"; then
      for dep in ${DEPENDENTS[$module]}; do
        AFFECTED_MODULES="$AFFECTED_MODULES $dep"
      done
    fi
  done
fi

# Deduplicate
AFFECTED_MODULES=$(echo "$AFFECTED_MODULES" | tr ' ' '\n' | sort -u | grep -v '^$')

echo "Affected modules: $AFFECTED_MODULES"

# ── Build matrix include JSON array ──────────────────────────────────────
MATRIX_INCLUDE="["
first=true
for module in $AFFECTED_MODULES; do
  while IFS= read -r plat; do
    [ -z "$plat" ] && continue
    if [ "$first" = true ]; then
      first=false
    else
      MATRIX_INCLUDE="$MATRIX_INCLUDE,"
    fi
    ENTRY=$(echo "$plat" | jq --arg m "$module" '. + {module: $m}')
    MATRIX_INCLUDE="$MATRIX_INCLUDE$ENTRY"
  done <<< "$PLATFORMS"
done
MATRIX_INCLUDE="$MATRIX_INCLUDE]"

# ── Write output ─────────────────────────────────────────────────────────────────
if [ -n "${GITHUB_OUTPUT:-}" ]; then
  COMPACT_MATRIX=$(echo "$MATRIX_INCLUDE" | jq -c .)
  echo "matrix=$COMPACT_MATRIX" >> "$GITHUB_OUTPUT"
fi
echo "Generated matrix:"
echo "$MATRIX_INCLUDE" | jq .
