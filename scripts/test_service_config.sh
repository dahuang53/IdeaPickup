#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
relay_python="$project_dir/.venv-relay/bin/python"
"$relay_python" -c 'import sys; assert sys.version_info[:3] == (3, 11, 16), "relay Python version mismatch"'
mkdir -p "$project_dir/private/host-tests"
chmod 700 "$project_dir/private/host-tests"
test_dir="$(mktemp -d "$project_dir/private/host-tests/service-config.XXXXXX")"
trap 'rm -rf -- "$test_dir"' EXIT
"$relay_python" "$project_dir/tests/test_service_runtime.py"
"$relay_python" "$project_dir/tests/test_cloud_input_helper.py"

"$relay_python" "$project_dir/tests/test_service_portal.py"
"$relay_python" "$project_dir/tests/test_provisioning.py"
"$relay_python" "$project_dir/tests/test_portal_socket.py"
