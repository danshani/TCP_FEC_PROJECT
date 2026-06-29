#!/usr/bin/env bash
# Build the shared library that exposes the real GF(256) codec to the demo
# backend (called from server.py via ctypes). Run from the repo root.
set -euo pipefail
mkdir -p build
gcc -shared -fPIC -I include -O2 \
    -Wno-maybe-uninitialized -Wno-alloc-size-larger-than \
    tools/fec-demo/fecsim_shim.c src/fec_codec.c src/fec.c \
    -o build/libfecsim.so
echo "built build/libfecsim.so"
