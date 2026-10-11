#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/env.sh
idf.py -B build-vocat16 \
  -D SDKCONFIG=sdkconfig.vocat16 \
  -D SDKCONFIG_DEFAULTS=sdkconfig.vocat16.defaults \
  "$@" reconfigure
.venv/bin/python scripts/build_receipt.py before vocat16
idf.py -B build-vocat16 -D SDKCONFIG=sdkconfig.vocat16 "$@" build
.venv/bin/python scripts/build_receipt.py after vocat16
