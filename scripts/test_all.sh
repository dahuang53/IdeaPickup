#!/usr/bin/env bash
# Public host regression: synthetic data, no devices or live providers.
set -euo pipefail
cd "$(dirname "$0")/.."
if ! command -v clang >/dev/null 2>&1; then
    echo "error: clang is required for the USB SDK regression; no suites started" >&2
    exit 1
fi
export PYTHON="$PWD/.venv/bin/python"
./tests/run_host_tests.sh
./scripts/test_relay.sh
./scripts/test_service_config.sh
./scripts/test_sync_pipeline.sh
./scripts/test_firmware_install.sh
# Retain rejection coverage for the disabled historical privileged publisher.
.venv/bin/python -m unittest discover -s tests -p test_legacy_maintenance_cli.py -v
node tests/test_admin_probe.cjs

.venv/bin/python -m unittest discover -s tests -p test_public_export.py -v
