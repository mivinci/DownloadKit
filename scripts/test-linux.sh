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
#

set -euo pipefail

IMAGE="${IMAGE:-gcc:14}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
BUILD_DIR="build-linux"
MEMORY="2G"
JOBS="2"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j*)  JOBS="${1#-j}"; shift ;;
        -m)   MEMORY="$2"; shift 2 ;;
        *)    echo "Unknown option: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "==> Running Linux tests in container"
echo "    Image:      $IMAGE"
echo "    Build type: $BUILD_TYPE"
echo "    Memory:     $MEMORY"
echo "    Jobs:       $JOBS"
echo ""

container run --rm -m "$MEMORY" \
    -v "$PROJECT_DIR":/work \
    -w /work \
    "$IMAGE" \
    bash -c "
        apt-get update -qq && \
        apt-get install -y -qq cmake libgtest-dev > /dev/null 2>&1 && \
        find /work -name '*.c' -o -name '*.cpp' -o -name '*.h' | xargs touch && \
        rm -rf $BUILD_DIR && mkdir -p $BUILD_DIR && cd $BUILD_DIR && \
        cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE && \
        cmake --build . -j$JOBS && \
        ctest --output-on-failure
    "
