#!/usr/bin/env bash
# Native Go: preserve zero-copy producer vertices and per-draw GE streaming.
# Reuse the SC-only feature vector, clean by default; simulation is unchanged.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
exec bash tools/build_go_r243_native_ge_20260921.sh \
  TH08_PSP_BUILD_ID=r244T-native-stream-20260922 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r244T-native-stream)' "$@"
