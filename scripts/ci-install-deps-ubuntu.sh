#!/usr/bin/env bash
# Install build dependencies for Ubuntu-based CI runners.
set -euo pipefail

sudo apt-get update -q
sudo apt-get install -y libgtest-dev libgmock-dev libnghttp2-dev cmake \
  libssl-dev libcurl4-openssl-dev libmbedtls-dev libcjson-dev

# Install llhttp from source (no apt package available)
git clone --depth 1 --branch release/v9.3.1 https://github.com/nodejs/llhttp.git /tmp/llhttp
cmake -S /tmp/llhttp -B /tmp/llhttp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build /tmp/llhttp/build
sudo cmake --install /tmp/llhttp/build

# Install usrsctp from source (no apt package available)
git clone --depth 1 https://github.com/sctplab/usrsctp.git /tmp/usrsctp
cmake -S /tmp/usrsctp -B /tmp/usrsctp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -Dsctp_build_programs=OFF
cmake --build /tmp/usrsctp/build
sudo cmake --install /tmp/usrsctp/build
