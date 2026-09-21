#!/usr/bin/env bash
# Keep r244 native rendering; repair PSP setup/BGM thread ownership only.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
exec bash tools/build_go_r244_native_stream_20260922.sh \
  TH08_PSP_BUILD_ID=r245T-native-reap-20260922 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r245T-native-reap)' "$@"
