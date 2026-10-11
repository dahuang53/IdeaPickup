#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/env.sh
idf.py -B build-spotpear \
  -D SDKCONFIG=sdkconfig.spotpear \
  -D SDKCONFIG_DEFAULTS=sdkconfig.spotpear.defaults \
  "$@" reconfigure
.venv/bin/python scripts/build_receipt.py before spotpear
idf.py -B build-spotpear -D SDKCONFIG=sdkconfig.spotpear "$@" build
.venv/bin/python scripts/build_receipt.py after spotpear
