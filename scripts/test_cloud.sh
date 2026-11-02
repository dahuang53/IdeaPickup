#!/usr/bin/env bash
# Synthetic local HTTPS/WebSocket and SDK shims; no device or live provider.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p private/host-tests
.venv/bin/python tests/test_cloud_protocol.py
.venv/bin/python tests/test_cloud_esp.py
.venv/bin/python tests/test_cloud_ws_sdk.py
