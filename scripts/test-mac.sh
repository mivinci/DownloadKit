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
BASE_REF=""
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
    [xline]="xbase"
    [xjs]="xbase"
    [xtui]="xbase"
)

ALL_MODULES=(xbase xlog xbuf xnet xcrypto xhttp xp2p xfer xagent xline xjs xtui)

# Modules that have no test binary (no ${module}_test CMake target).
NO_TEST_MODULES=(xline)

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
                for d in ${=MODULE_DEPS[$m]}; do
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
    elif [[ -n "$BASE_REF" ]]; then
        if git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
            diff_target="$BASE_REF"
        else
            warn "Base ref '$BASE_REF' not found, fetching..."
            local remote_ref="${BASE_REF#origin/}"
            local remote_name
            remote_name="$(git remote | head -1)"
            git fetch "$remote_name" "$remote_ref" 2>/dev/null || true
            if git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
                diff_target="$BASE_REF"
            else
                diff_target="HEAD~1"
                warn "Cannot find '$BASE_REF', falling back to $diff_target"
            fi
        fi
    else
        # Auto-detect: find the main branch from any remote
        local main_branch=""
        for ref in "refs/remotes/gh/main" "refs/remotes/origin/main" "refs/remotes/upstream/main"; do
            if git rev-parse --verify "$ref" >/dev/null 2>&1; then
                main_branch="${ref#refs/remotes/}"
                break
            fi
        done
        if [[ -n "$main_branch" ]]; then
            diff_target="$main_branch"
            info "Auto-detected base: $diff_target"
        else
            diff_target="HEAD~1"
            warn "Cannot find main branch, falling back to $diff_target"
        fi
    fi

    local changed_files
    changed_files=("${(@f)$(git diff --name-only "$diff_target" HEAD 2>/dev/null || git diff --name-only HEAD 2>/dev/null || echo "")}")

    if [[ ${#changed_files[@]} -eq 0 ]]; then
        info "No changes detected, testing all modules"
        printf '%s\n' "${ALL_MODULES[@]}"
        return
    fi

    # Map changed files to modules
    local -A changed_mods=([__none__]=1)
    local libxpp_changed=0
    for f in "${changed_files[@]}"; do
        if [[ "$f" == libx/x/*/* ]]; then
            # libx/x/<mod>/...  -> module key is "x<mod>" (xbase, xlog, ...)
            local rest="${f#libx/x/}"
            local mod="x${rest%%/*}"
            for m in "${ALL_MODULES[@]}"; do
                if [[ "$mod" == "$m" ]]; then
                    changed_mods[$mod]=1
                    break
                fi
            done
        elif [[ "$f" == libx/examples/* || "$f" == libx/bench/* ]]; then
            # Example / bench changes do not affect module tests
            continue
        elif [[ "$f" == libx/cmake/* ]]; then
            # libx-internal CMake helpers — affect every libx module (and libx++)
            info "libx CMake helpers changed, testing all modules"
            printf '%s\n' "${ALL_MODULES[@]}"
            printf '%s\n' "__libxpp__"
            return
        elif [[ "$f" == libx/CMakeLists.txt ]]; then
            info "libx top-level CMake changed, testing all modules"
            printf '%s\n' "${ALL_MODULES[@]}"
            printf '%s\n' "__libxpp__"
            return
        elif [[ "$f" == libx++/* ]]; then
            libxpp_changed=1
        elif [[ "$f" =~ ^(CMakeLists\.txt|cmake/) ]]; then
            # Repo-root CMake change affects everything (libx + libx++)
            info "Root build system changed, testing all modules"
            libxpp_changed=1
            printf '%s\n' "${ALL_MODULES[@]}"
            printf '%s\n' "__libxpp__"
            return
        fi
    done

    if [[ $libxpp_changed -eq 1 ]]; then
        changed_mods[__libxpp__]=1
    fi

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

    # Pop libx++ pseudo-module before computing libx-graph dependents
    local include_libxpp=0
    if [[ -n "${changed_mods[__libxpp__]+_}" ]]; then
        include_libxpp=1
        unset 'changed_mods[__libxpp__]'
    fi

    # Expand to include dependents
    local affected
    if [[ ${#changed_mods[@]} -gt 0 ]]; then
        affected=$(compute_affected "${(@k)changed_mods[@]}")
    fi
    if [[ $include_libxpp -eq 1 ]]; then
        affected="${affected}"$'\n'"__libxpp__"
    fi
    info "Affected modules (with dependents): $(echo $affected | tr '\n' ' ')"

    echo "$affected"
}

# ── Main ────────────────────────────────────────────────────────────────
AFFECTED=()
local _result
_result="$(detect_changed_modules)"
for _line in "${(@f)_result}"; do
    [[ -n "$_line" ]] && AFFECTED+=("$_line")
done

if [[ ${#AFFECTED[@]} -eq 0 ]]; then
    info "No modules to test"
    exit 0
fi

# --detect-only: just print affected module names and exit
if [[ $DETECT_ONLY -eq 1 ]]; then
    for m in "${AFFECTED[@]}"; do
        if [[ "$m" == "__libxpp__" ]]; then
            echo "libx++"
        else
            echo "$m"
        fi
    done
    exit 0
fi

# Collect test targets (skip modules with no test binary)
TEST_TARGETS=()
for m in "${AFFECTED[@]}"; do
    if [[ "$m" == "__libxpp__" ]]; then
        TEST_TARGETS+=("x++_test")
        continue
    fi
    skip=0
    for nt in "${NO_TEST_MODULES[@]}"; do
        if [[ "$m" == "$nt" ]]; then skip=1; break; fi
    done
    if [[ $skip -eq 0 ]]; then
        TEST_TARGETS+=("${m}_test")
    fi
done

info "Test targets: ${TEST_TARGETS[*]}"

# If all affected modules have no test binary, nothing to do.
if [[ ${#TEST_TARGETS[@]} -eq 0 ]]; then
    info "No test targets to build — all affected modules lack test binaries"
    exit 0
fi

# ── CMake configure ────────────────────────────────────────────────────
step "Configuring build (TLS=$TLS_BACKEND, type=$BUILD_TYPE)"

CMAKE_EXTRA_ARGS=(-DX_TLS_BACKEND=$TLS_BACKEND)

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
    # Escape regex metacharacters in target name for ctest -R (e.g. x++_test)
    target_re="${target//+/\\+}"
    if (cd "$BUILD_DIR" && ctest --output-on-failure -R "^${target_re}$" --no-tests=error); then
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
