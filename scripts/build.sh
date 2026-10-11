#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/env.sh
# Refresh the embedded source commit even when only Git HEAD has changed.
idf.py "$@" reconfigure build
