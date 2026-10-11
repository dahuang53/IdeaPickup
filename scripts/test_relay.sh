#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
relay_python="$project_dir/.venv-relay/bin/python"
[[ -x "$relay_python" ]] || { printf '%s\n' '先运行 ./scripts/setup_relay.sh 建立锁定环境。' >&2; exit 1; }
"$relay_python" -c 'import sys; assert sys.version_info[:3] == (3, 11, 16), "relay Python version mismatch"'
install -d -m 0700 "$project_dir/private/oss-tests/tmp"
export TMPDIR="$project_dir/private/oss-tests/tmp"
export PYTHONPATH="$project_dir/server-relay"
if [[ "$#" == 0 ]]; then
  "$relay_python" -m unittest discover -s "$project_dir/server-relay" -p 'test_*.py' -v
else
  "$relay_python" -m unittest "$@" -v
fi
