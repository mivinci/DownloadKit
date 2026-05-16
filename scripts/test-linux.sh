#!/usr/bin/env bash
#
# test-linux.sh - Run unit tests for changed modules on Linux
#
# Detects which modules changed (vs main branch or a given base),
# resolves transitive dependents, then builds & runs only those tests.
#
# Local mode (macOS with Apple Containerization):
#   ./scripts/test-linux.sh                        # default: openssl, vs origin/main
#   ./scripts/test-linux.sh -t mbedtls             # use mbedTLS backend
#   ./scripts/test-linux.sh -t openssl -j4 -m 4G   # custom parallelism and memory
#   ./scripts/test-linux.sh --all                  # force test all modules
#
# CI mode (runs natively on Linux, no container):
#   ./scripts/test-linux.sh --ci --base-sha <SHA> -t openssl --asan
#   ./scripts/test-linux.sh --ci --detect-only --base-sha <SHA>
#
# Prerequisites (local):
#   - macOS 26+ (Tahoe)
#   - brew install container
#   - container system start
#
# Prerequisites (CI / native Linux):
#   apt-get install libgtest-dev libnghttp2-dev libssl-dev libmbedtls-dev ...

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────
BASE_IMAGE="${BASE_IMAGE:-gcc:14}"
TEST_IMAGE="moo-test:latest"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
TLS_BACKEND="openssl"
BASE_REF="origin/main"
JOBS="2"
MEMORY="2G"
REBUILD_IMAGE=0
FORCE_ALL=0
CI_MODE=0
DETECT_ONLY=0
ASAN=0
BASE_SHA=""
APT_MIRROR="${APT_MIRROR:-}"
GITHUB_MIRROR="${GITHUB_MIRROR:-}"

# ── Parse args ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--tls)     TLS_BACKEND="$2"; shift 2 ;;
        -b|--base)    BASE_REF="$2"; shift 2 ;;
        -j|--jobs)    JOBS="$2"; shift 2 ;;
        -m|--memory)  MEMORY="$2"; shift 2 ;;
        -B|--build)   BUILD_DIR="$2"; shift 2 ;;
        --rebuild)    REBUILD_IMAGE=1; shift ;;
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
            echo "  -j, --jobs N         Parallel build jobs (default: 2)"
            echo "  -m, --memory SIZE    Container memory limit (default: 2G, local only)"
            echo "  --rebuild            Rebuild the Docker image (local only)"
            echo "  --all                Force test all modules"
            echo "  --ci                 CI mode: run natively on Linux (no container)"
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

cd "$PROJECT_DIR"

# ── Colors (disabled in CI mode) ───────────────────────────────────────
if [[ "$CI_MODE" -eq 1 ]]; then
    GREEN='' YELLOW='' RED='' CYAN='' NC=''
else
    GREEN='\033[0;32m' YELLOW='\033[1;33m' RED='\033[0;31m' CYAN='\033[0;36m' NC='\033[0m'
fi

info()  { echo -e "${GREEN}[INFO]${NC} $*" >&2; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
error() { echo -e "${RED}[ERR]${NC} $*" >&2; }
step()  { echo -e "${CYAN}[STEP]${NC} $*" >&2; }

# ── Module dependency map ──────────────────────────────────────────────
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
compute_affected() {
    local -A changed
    local -A affected

    for m in "$@"; do
        changed[$m]=1
        affected[$m]=1
    done

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

    local -A changed_mods=([__none__]=1)
    for f in $changed_files; do
        if [[ "$f" =~ ^libx/([^/]+)/ ]]; then
            local mod="${BASH_REMATCH[1]}"
            for m in "${ALL_MODULES[@]}"; do
                if [[ "$mod" == "$m" ]]; then
                    changed_mods[$mod]=1
                    break
                fi
            done
        elif [[ "$f" =~ ^examples/ ]]; then
            # Example / demo changes do not affect module tests
            continue
        elif [[ "$f" =~ ^(CMakeLists\.txt|cmake/) ]]; then
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

    local direct_changes="${!changed_mods[*]}"
    info "Directly changed: $direct_changes"

    local affected
    affected=$(compute_affected ${!changed_mods[@]})
    info "Affected modules (with dependents): $(echo $affected | tr '\n' ' ')"

    echo "$affected"
}

# ── Detect & prepare ───────────────────────────────────────────────────
AFFECTED=$(detect_changed_modules)

if [[ -z "$AFFECTED" ]]; then
    info "No modules to test"
    exit 0
fi

# --detect-only: just print affected module names and exit
if [[ $DETECT_ONLY -eq 1 ]]; then
    echo "$AFFECTED"
    exit 0
fi

# Collect test targets (skip modules with no test binary)
TEST_TARGETS=()
for m in $AFFECTED; do
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

# ── CI mode: run natively on Linux ─────────────────────────────────────
if [[ "$CI_MODE" -eq 1 ]]; then
    BUILD_DIR="${PROJECT_DIR}/build-linux-${TLS_BACKEND}"

    step "Configuring build (TLS=$TLS_BACKEND, type=$BUILD_TYPE, CI mode)"

    CMAKE_EXTRA_ARGS="-DX_TLS_BACKEND=$TLS_BACKEND"
    if [[ $ASAN -eq 1 ]]; then
        CMAKE_EXTRA_ARGS="$CMAKE_EXTRA_ARGS -DMOO_ENABLE_ASAN=ON"
    fi

    GITHUB_MIRROR="${GITHUB_MIRROR}" cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_TESTING=ON \
        $CMAKE_EXTRA_ARGS

    step "Building test targets"
    cmake --build "$BUILD_DIR" --target ${TEST_TARGETS[@]} -j"$JOBS"

    # Suppress known third-party library leaks (OpenSSL, libcurl) under ASan
    if [[ $ASAN -eq 1 ]]; then
        LSAN_SUPPRESSIONS="$SCRIPT_DIR/lsan_suppressions.txt"
        if [[ -f "$LSAN_SUPPRESSIONS" ]]; then
            export LSAN_OPTIONS="suppressions=$LSAN_SUPPRESSIONS"
            info "LSAN suppressions loaded from $LSAN_SUPPRESSIONS"
        fi
        # After fork/forkpty, child processes inherit inconsistent ASAN shadow
        # memory.  halt_on_error=0 lets children survive ASAN false-positives
        # (e.g. in login_tty dup2/close wrappers) and reach execvp().
        export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=0"
        info "ASAN_OPTIONS set: halt_on_error=0"
    fi

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

    echo ""
    if [[ $FAILED -eq 0 ]]; then
        info "All tests passed! ✅"
    else
        error "Some tests failed! ❌"
        exit 1
    fi
    exit 0
fi

# ── Local mode: run in Apple container ─────────────────────────────────

# Build container image if needed
build_image() {
    echo "==> Building test image: $TEST_IMAGE"
    container build \
        -t "$TEST_IMAGE" \
        -f "$SCRIPT_DIR/Dockerfile.test" \
        --build-arg "BASE_IMAGE=$BASE_IMAGE" \
        --build-arg "APT_MIRROR=$APT_MIRROR" \
        --build-arg "GITHUB_MIRROR=$GITHUB_MIRROR" \
        "$PROJECT_DIR"
    echo "==> ✅ Image built: $TEST_IMAGE"
    echo ""
}

if [[ "$REBUILD_IMAGE" -eq 1 ]]; then
    build_image
elif ! container image ls 2>/dev/null | grep -q "$TEST_IMAGE"; then
    echo "==> Test image not found, building..."
    build_image
fi

BUILD_DIR="build-linux-${TLS_BACKEND}"

# Build the test target list for the container command
TEST_TARGETS_STR=""
for m in $AFFECTED; do
    TEST_TARGETS_STR="${TEST_TARGETS_STR} ${m}_test"
done
TEST_TARGETS_STR="${TEST_TARGETS_STR# }"  # trim leading space

# ── Run tests in container ─────────────────────────────────────────────
step "Running Linux tests in container (TLS=$TLS_BACKEND)"
echo "    Image:       $TEST_IMAGE"
echo "    Build type:  $BUILD_TYPE"
echo "    TLS backend: $TLS_BACKEND"
echo "    Memory:      $MEMORY"
echo "    Jobs:        $JOBS"
echo "    Targets:    $TEST_TARGETS_STR"
echo ""

container run --rm -m "$MEMORY" \
    -v "$PROJECT_DIR":/work \
    -w /work \
    -e GITHUB_MIRROR="${GITHUB_MIRROR}" \
    "$TEST_IMAGE" \
    bash -c "
        set -euo pipefail && \
        export MOO_SKIP_NETWORK_TESTS=1 && \
        container-setup.sh && \
        BUILD_DIR=$BUILD_DIR && \
        if [ ! -d \$BUILD_DIR ]; then \
            mkdir -p \$BUILD_DIR && cd \$BUILD_DIR && \
            cmake .. \
              -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
              -DX_TLS_BACKEND=$TLS_BACKEND \
              -DMOO_ENABLE_ASAN=OFF \
              -DFETCHCONTENT_BASE_DIR=/fetchcontent-cache; \
        else \
            cd \$BUILD_DIR; \
        fi && \
        cmake --build . --target $TEST_TARGETS_STR -j$JOBS && \
        for target in $TEST_TARGETS_STR; do \
            echo '── Running \$target ──' && \
            ctest --output-on-failure -R \"^\${target}\$\" || exit 1; \
        done
    "

echo ""
info "All affected module tests passed! ✅"
