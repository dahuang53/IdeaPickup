#!/usr/bin/env bash
# Generic new installation / compatible application update; no personal registry.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# != 3 ]]; then
  echo "usage: $0 ROM_PORT MANIFEST {first-install|app-only}" >&2
  exit 2
fi
exec .venv/bin/python scripts/device_registry.py --board vocat16 --port "$1" --bundle "$2" --mode "$3"
