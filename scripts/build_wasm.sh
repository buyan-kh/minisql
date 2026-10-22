#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC1091
cd /tmp/emsdk && . ./emsdk_env.sh
cd "$ROOT"
emcmake cmake -S "$ROOT" -B "$ROOT/build-wasm" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKEEL_WASM=ON \
  -DKEEL_BUILD_TESTS=OFF \
  -DKEEL_BUILD_BENCH=OFF \
  -DKEEL_ASAN=OFF
cmake --build "$ROOT/build-wasm" -j4 --target keel_wasm
mkdir -p "$ROOT/web/public/keel"
cp "$ROOT/build-wasm/keel.js" "$ROOT/build-wasm/keel.wasm" "$ROOT/web/public/keel/"
