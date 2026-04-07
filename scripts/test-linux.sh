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

set -euo pipefail

IMAGE="${IMAGE:-gcc:14}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
MEMORY="2G"
JOBS="2"
TLS_BACKENDS="${XK_TLS_BACKEND:-openssl mbedtls}"
APT_MIRROR="${APT_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j*)  JOBS="${1#-j}"; shift ;;
        -m)   MEMORY="$2"; shift 2 ;;
        *)    echo "Unknown option: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Cache llhttp source in tmp/ to avoid repeated git clone
LLHTTP_DIR="$PROJECT_DIR/tmp/llhttp"
if [[ -d "$LLHTTP_DIR" ]]; then
    echo "==> Using cached llhttp at tmp/llhttp"
else
    echo "==> Cloning llhttp to tmp/llhttp ..."
    mkdir -p "$PROJECT_DIR/tmp"
    git  "$LLHTTP_DIR"
fi

for TLS in $TLS_BACKENDS; do
    BUILD_DIR="build-linux-${TLS}"

    echo "==> Running Linux tests in container (TLS=$TLS)"
    echo "    Image:      $IMAGE"
    echo "    Build type: $BUILD_TYPE"
    echo "    TLS backend: $TLS"
    echo "    Memory:     $MEMORY"
    echo "    Jobs:       $JOBS"
    echo ""

    # Build sed command to swap APT mirror if APT_MIRROR is set
    MIRROR_CMD=""
    if [[ -n "$APT_MIRROR" ]]; then
        MIRROR_CMD="sed -i 's|deb.debian.org|${APT_MIRROR#https://}|g' /etc/apt/sources.list.d/debian.sources 2>/dev/null || sed -i 's|deb.debian.org|${APT_MIRROR#https://}|g' /etc/apt/sources.list 2>/dev/null; "
    fi

    container run --rm -m "$MEMORY" \
        -v "$PROJECT_DIR":/work \
        -w /work \
        "$IMAGE" \
        bash -c "
            export XKIT_SKIP_NETWORK_TESTS=1 && \
            ${MIRROR_CMD}
            apt-get update -qq && \
            apt-get install -y -qq cmake libgtest-dev libssl-dev libcurl4-openssl-dev \
              libnghttp2-dev libmbedtls-dev > /dev/null 2>&1 && \
            # Install llhttp from cached source in tmp/llhttp
            cmake -S /work/tmp/llhttp -B /tmp/llhttp-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON > /dev/null 2>&1 && \
            cmake --build /tmp/llhttp-build > /dev/null 2>&1 && \
            cmake --install /tmp/llhttp-build > /dev/null 2>&1 && \
            ldconfig && \
            find /work -name '*.c' -o -name '*.cpp' -o -name '*.h' | xargs touch && \
            rm -rf $BUILD_DIR && mkdir -p $BUILD_DIR && cd $BUILD_DIR && \
            cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DXK_TLS_BACKEND=$TLS -DXK_ENABLE_ASAN=OFF && \
            cmake --build . -j$JOBS && \
            ctest --output-on-failure
        "

    echo ""
    echo "==> ✅ TLS=$TLS passed"
    echo ""
done

echo "==> ✅ All TLS backends passed"
