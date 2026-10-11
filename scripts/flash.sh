#!/usr/bin/env bash
# Historical 32 MB VoCat flashing is excluded from the Route 1 public targets.
set -euo pipefail
echo '32 MB vocat is unsupported. Use the two-board installation entry in docs/DEVICE_INSTALL.md.' >&2
exit 2
