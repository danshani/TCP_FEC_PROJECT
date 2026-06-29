#!/usr/bin/env bash
# Build the real-codec shared lib and launch the live demo server.
# Run from the repo root under WSL:  bash tools/fec-demo/run_demo.sh
# Then open http://localhost:8088 in your browser.
set -euo pipefail
bash tools/fec-demo/build_demo.sh
exec python3 tools/fec-demo/server.py
