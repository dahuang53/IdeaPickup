#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
.venv/bin/python tests/test_firmware_install.py
.venv/bin/python -m unittest discover -s tests -p test_usb_stdio_vfs.py -v
.venv/bin/python tests/test_sd_dma_reserve.py
