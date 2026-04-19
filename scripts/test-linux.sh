#!/usr/bin/env bash
#
# Run Linux unit tests locally on macOS via Apple Containerization.
#
# Prerequisites:
#   - macOS 26+ (Tahoe)
#   - brew install container
#   - container system start
#
# Usage:
#   ./scripts/test-linux.sh              # default: gcc:14, Debug, -j2
#   ./scripts/test-linux.sh -j4 -m 4G    # custom parallelism and memory
#   XK_TLS_BACKEND=openssl ./scripts/test-linux.sh  # test single backend
#
# Caching:
#   - APT dependencies are baked into a pre-built image (scripts/Dockerfile.test).
#     Rebuild with: ./scripts/test-linux.sh --rebuild
#   - FetchContent downloads are cached in a named volume (xkit-fetchcontent).
#     Clear with: container volume rm xkit-fetchcontent
#

set -euo pipefail

BASE_IMAGE="${BASE_IMAGE:-gcc:14}"
TEST_IMAGE="xkit-test:latest"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
MEMORY="2G"
JOBS="2"
TLS_BACKENDS="${XK_TLS_BACKEND:-openssl mbedtls}"
APT_MIRROR="${APT_MIRROR:-mirrors.tuna.tsinghua.edu.cn}"
FETCHCONTENT_VOLUME="xkit-fetchcontent"
REBUILD_IMAGE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j*)       JOBS="${1#-j}"; shift ;;
        -m)        MEMORY="$2"; shift 2 ;;
        --rebuild) REBUILD_IMAGE=1; shift ;;
        *)         echo "Unknown option: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- Build test image if needed ---
build_image() {
    echo "==> Building test image: $TEST_IMAGE"
    container build \
        -t "$TEST_IMAGE" \
        -f "$SCRIPT_DIR/Dockerfile.test" \
        --build-arg "BASE_IMAGE=$BASE_IMAGE" \
        --build-arg "APT_MIRROR=$APT_MIRROR" \
        "$SCRIPT_DIR"
    echo "==> ✅ Image built: $TEST_IMAGE"
    echo ""
}

if [[ "$REBUILD_IMAGE" -eq 1 ]]; then
    build_image
elif ! container image ls 2>/dev/null | grep -q "$TEST_IMAGE"; then
    echo "==> Test image not found, building..."
    build_image
fi

# --- Ensure FetchContent cache volume exists ---
if ! container volume ls 2>/dev/null | grep -q "$FETCHCONTENT_VOLUME"; then
    echo "==> Creating FetchContent cache volume: $FETCHCONTENT_VOLUME"
    container volume create "$FETCHCONTENT_VOLUME"
fi

# --- Run tests ---
for TLS in $TLS_BACKENDS; do
    BUILD_DIR="build-linux-${TLS}"

    echo "==> Running Linux tests in container (TLS=$TLS)"
    echo "    Image:      $TEST_IMAGE"
    echo "    Build type: $BUILD_TYPE"
    echo "    TLS backend: $TLS"
    echo "    Memory:     $MEMORY"
    echo "    Jobs:       $JOBS"
    echo ""

    container run --rm -m "$MEMORY" \
        -v "$PROJECT_DIR":/work \
        -v "$FETCHCONTENT_VOLUME":/fetchcontent-cache \
        -w /work \
        "$TEST_IMAGE" \
        bash -c "
            export XKIT_SKIP_NETWORK_TESTS=1 && \
            find /work -name '*.c' -o -name '*.cpp' -o -name '*.h' | xargs touch && \
            rm -rf $BUILD_DIR && mkdir -p $BUILD_DIR && cd $BUILD_DIR && \
            cmake .. \
              -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
              -DXK_TLS_BACKEND=$TLS \
              -DXK_ENABLE_ASAN=OFF \
              -DFETCHCONTENT_BASE_DIR=/fetchcontent-cache && \
            cmake --build . -j$JOBS && \
            ctest --output-on-failure
        "

    echo ""
    echo "==> ✅ TLS=$TLS passed"
    echo ""
done

echo "==> ✅ All TLS backends passed"
