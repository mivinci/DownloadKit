#!/usr/bin/env zsh
#
# test-mac.sh - Run unit tests for changed modules on macOS
#
# Detects which modules changed (vs main branch or a given base),
# resolves transitive dependents, then builds & runs only those tests.
#
# Usage:
#   ./scripts/test-mac.sh                        # default: openssl, vs origin/main
#   ./scripts/test-mac.sh -t mbedtls             # use mbedTLS backend
#   ./scripts/test-mac.sh -b feature/foo         # compare against feature/foo
#   ./scripts/test-mac.sh -t openssl -j8         # 8 parallel jobs
#   ./scripts/test-mac.sh --all                  # force test all modules
#
# Prerequisites:
#   brew install googletest llhttp nghttp2 openssl mbedtls cjson libusrsctp

set -euo pipefail

# ── Colors (disabled in CI mode) ───────────────────────────────────────
if [[ "${CI_MODE:-0}" -eq 1 ]]; then
    GREEN='' YELLOW='' RED='' CYAN='' NC=''
else
    GREEN='\033[0;32m' YELLOW='\033[1;33m' RED='\033[0;31m' CYAN='\033[0;36m' NC='\033[0m'
fi

info()  { echo -e "${GREEN}[INFO]${NC} $*" >&2; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
error() { echo -e "${RED}[ERR]${NC} $*" >&2; }
step()  { echo -e "${CYAN}[STEP]${NC} $*" >&2; }

# ── Defaults ────────────────────────────────────────────────────────────
TLS_BACKEND="openssl"
BASE_REF="origin/main"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
BUILD_TYPE="Debug"
BUILD_DIR=""
FORCE_ALL=0
CI_MODE=0
DETECT_ONLY=0
ASAN=0
BASE_SHA=""

# ── Parse args ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--tls)     TLS_BACKEND="$2"; shift 2 ;;
        -b|--base)    BASE_REF="$2"; shift 2 ;;
        -j|--jobs)    JOBS="$2"; shift 2 ;;
        -B|--build)   BUILD_DIR="$2"; shift 2 ;;
        --all)        FORCE_ALL=1; shift ;;
        --ci)         CI_MODE=1; shift ;;
        --detect-only) DETECT_ONLY=1; shift ;;
        --asan)       ASAN=1; shift ;;
        --base-sha)   BASE_SHA="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -t, --tls BACKEND    TLS backend: openssl (default), mbedtls"
            echo "  -b, --base REF       Git ref to compare against (default: origin/main)"
            echo "  -j, --jobs N         Parallel build jobs (default: #CPU cores)"
            echo "  -B, --build DIR      Build directory (default: build-mac-<TLS>)"
            echo "  --all                Force test all modules"
            echo "  --ci                 CI mode: suppress colors, use base-sha for diff"
            echo "  --detect-only        Only output affected module names (one per line)"
            echo "  --asan               Enable AddressSanitizer"
            echo "  --base-sha SHA       Base SHA to diff against (CI mode, overrides -b)"
            echo "  -h, --help           Show this help"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build-mac-${TLS_BACKEND}}"

cd "$PROJECT_DIR"

# ── Module dependency map: module → direct dependencies ────────────────
# Derived from CMakeLists.txt target_link_libraries
declare -A MODULE_DEPS
MODULE_DEPS=(
    [xbase]=""
    [xlog]="xbase"
    [xbuf]="xbase"
    [xnet]="xbase xbuf"
    [xcrypto]="xbase"
    [xhttp]="xbase xbuf xnet"
    [xp2p]="xbase xnet xcrypto"
    [xfer]="xp2p xhttp xnet xcrypto xbase"
    [xagent]="xbase xnet xhttp"
)

ALL_MODULES=(xbase xlog xbuf xnet xcrypto xhttp xp2p xfer xagent)

# ── Compute reverse dependents (transitive) ────────────────────────────
# Given a set of changed modules, find all modules that depend on them.
compute_affected() {
    local -A changed
    local -A affected

    # Seed with directly changed modules
    for m in "$@"; do
        changed[$m]=1
        affected[$m]=1
    done

    # Iteratively expand: any module whose deps intersect with affected set
    local changed_again=1
    while [[ $changed_again -eq 1 ]]; do
        changed_again=0
        for m in "${ALL_MODULES[@]}"; do
            if [[ -z "${affected[$m]+_}" ]]; then
                for d in ${MODULE_DEPS[$m]}; do
                    if [[ -n "${affected[$d]+_}" ]]; then
                        affected[$m]=1
                        changed_again=1
                        break
                    fi
                done
            fi
        done
    done

    # Print result
    for m in "${ALL_MODULES[@]}"; do
        if [[ -n "${affected[$m]+_}" ]]; then
            echo "$m"
        fi
    done
}

# ── Detect changed modules ─────────────────────────────────────────────
detect_changed_modules() {
    step "Detecting changed modules (vs $BASE_REF)"

    if [[ $FORCE_ALL -eq 1 ]]; then
        info "Force all mode: testing every module"
        printf '%s\n' "${ALL_MODULES[@]}"
        return
    fi

    # In CI mode, use the explicit base SHA if provided
    local diff_target
    if [[ -n "$BASE_SHA" ]]; then
        diff_target="$BASE_SHA"
    elif git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
        diff_target="$BASE_REF"
    else
        warn "Base ref '$BASE_REF' not found, fetching..."
        git fetch origin "${BASE_REF#origin/}" 2>/dev/null || true
        if git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
            diff_target="$BASE_REF"
        else
            diff_target="HEAD~1"
            warn "Cannot find '$BASE_REF', falling back to $diff_target"
        fi
    fi

    local changed_files
    changed_files=$(git diff --name-only "$diff_target" HEAD 2>/dev/null || git diff --name-only HEAD 2>/dev/null || echo "")

    if [[ -z "$changed_files" ]]; then
        info "No changes detected, testing all modules"
        printf '%s\n' "${ALL_MODULES[@]}"
        return
    fi

    # Map changed files to modules
    local -A changed_mods=([__none__]=1)
    for f in $changed_files; do
        if [[ "$f" == libs/*/* ]]; then
            local mod="${f#libs/}"
            mod="${mod%%/*}"
            # Only count if it's a known module
            for m in "${ALL_MODULES[@]}"; do
                if [[ "$mod" == "$m" ]]; then
                    changed_mods[$mod]=1
                    break
                fi
            done
        elif [[ "$f" == examples/* ]]; then
            # Example / demo changes do not affect module tests
            continue
        elif [[ "$f" =~ ^(CMakeLists\.txt|cmake/) ]]; then
            # Root CMake change affects everything
            info "Root build system changed, testing all modules"
            printf '%s\n' "${ALL_MODULES[@]}"
            return
        fi
    done

    unset 'changed_mods[__none__]'
    if [[ ${#changed_mods[@]} -eq 0 ]]; then
        info "No module changes detected (only non-module files changed), skipping tests"
        return
    fi

    local direct_changes=""
    for k in "${(@k)changed_mods[@]}"; do
        [[ -n "$direct_changes" ]] && direct_changes+=" "
        direct_changes+="$k"
    done
    info "Directly changed: $direct_changes"

    # Expand to include dependents
    local affected
    affected=$(compute_affected "${(@k)changed_mods[@]}")
    info "Affected modules (with dependents): $(echo $affected | tr '\n' ' ')"

    echo "$affected"
}

# ── Main ────────────────────────────────────────────────────────────────
AFFECTED=()
local _line
detect_changed_modules | while IFS= read -r _line; do
    AFFECTED+=("$_line")
done

if [[ ${#AFFECTED[@]} -eq 0 ]]; then
    info "No modules to test"
    exit 0
fi

# --detect-only: just print affected module names and exit
if [[ $DETECT_ONLY -eq 1 ]]; then
    printf '%s\n' "${AFFECTED[@]}"
    exit 0
fi

# Collect test targets
TEST_TARGETS=()
for m in "${AFFECTED[@]}"; do
    TEST_TARGETS+=("${m}_test")
done

info "Test targets: ${TEST_TARGETS[*]}"

# ── CMake configure ────────────────────────────────────────────────────
step "Configuring build (TLS=$TLS_BACKEND, type=$BUILD_TYPE)"

CMAKE_EXTRA_ARGS=(-DMOO_TLS_BACKEND=$TLS_BACKEND)

if [[ $ASAN -eq 1 ]]; then
    CMAKE_EXTRA_ARGS+=(-DMOO_ENABLE_ASAN=ON)
fi

# Homebrew OpenSSL / mbedTLS are keg-only
OPENSSL_PREFIX="$(brew --prefix openssl 2>/dev/null || true)"
if [[ -n "$OPENSSL_PREFIX" ]]; then
    CMAKE_EXTRA_ARGS+=(-DOPENSSL_ROOT_DIR=$OPENSSL_PREFIX)
fi
MBEDTLS_PREFIX="$(brew --prefix mbedtls 2>/dev/null || true)"
if [[ -n "$MBEDTLS_PREFIX" ]]; then
    CMAKE_EXTRA_ARGS+=(-DMBEDTLS_ROOT_DIR=$MBEDTLS_PREFIX)
fi

cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTING=ON \
    "${CMAKE_EXTRA_ARGS[@]}"

# ── Build ──────────────────────────────────────────────────────────────
step "Building test targets"

cmake --build "$BUILD_DIR" --target ${TEST_TARGETS[@]} --parallel "$JOBS"

# ── Run tests ──────────────────────────────────────────────────────────
FAILED=0
for target in "${TEST_TARGETS[@]}"; do
    step "Running $target"
    if (cd "$BUILD_DIR" && ctest --output-on-failure -R "^${target}$"); then
        info "$target PASSED"
    else
        error "$target FAILED"
        FAILED=1
    fi
done

# ── Summary ────────────────────────────────────────────────────────────
echo ""
if [[ $FAILED -eq 0 ]]; then
    info "All tests passed! ✅"
else
    error "Some tests failed! ❌"
    exit 1
fi
