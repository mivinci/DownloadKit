#!/usr/bin/env bash
#
# container-setup.sh - One-time setup for moo test containers
#
# Apple Containerization's "container build" RUN steps have no network,
# so package installation and FetchContent prefetch must happen at runtime.
# This script installs apt packages and pre-fetches CMake dependencies
# on first run, then creates a marker file to skip on subsequent runs.
#
set -euo pipefail

MARKER="/var/lib/moo-container-setup-done"

if [ -f "$MARKER" ]; then
    echo "[container-setup] Already set up, skipping."
    exit 0
fi

echo "[container-setup] Installing apt packages..."
apt-get update -qq && \
    apt-get install -y -qq \
      cmake pkg-config \
      libssl-dev libcurl4-openssl-dev libmbedtls-dev libunwind-dev \
      > /dev/null 2>&1 && \
    rm -rf /var/lib/apt/lists/*

echo "[container-setup] Pre-fetching CMake FetchContent dependencies..."
GITHUB_MIRROR="${GITHUB_MIRROR:-}"
PREFETCH_DIR="/tmp/moo-prefetch"
mkdir -p "$PREFETCH_DIR"

# Copy only what cmake needs to resolve FetchContent
cp -r /work/cmake "$PREFETCH_DIR/"
cp /work/CMakeLists.txt "$PREFETCH_DIR/"
cp -r /work/libs "$PREFETCH_DIR/"
cp -r /work/bench "$PREFETCH_DIR/"

cd "$PREFETCH_DIR" && \
    GITHUB_MIRROR="$GITHUB_MIRROR" cmake -S . -B /tmp/moo-build \
      -DCMAKE_BUILD_TYPE=Debug \
      -DMOO_TLS_BACKEND=openssl \
      -DBUILD_TESTING=ON \
      -DFETCHCONTENT_BASE_DIR=/fetchcontent-cache

rm -rf "$PREFETCH_DIR" /tmp/moo-build

touch "$MARKER"
echo "[container-setup] ✅ Setup complete."
