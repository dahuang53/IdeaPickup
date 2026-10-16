#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
relay_python="$project_dir/.venv-relay/bin/python"
"$relay_python" -c 'import sys; assert sys.version_info[:3] == (3, 11, 16), "relay Python version mismatch"'
mkdir -p "$project_dir/private/host-tests"
chmod 700 "$project_dir/private/host-tests"
test_dir="$(mktemp -d "$project_dir/private/host-tests/service-config.XXXXXX")"
trap 'rm -rf -- "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 \
  -fsanitize=address,undefined -I "$project_dir/main" \
  "$project_dir/main/memo_network_config.c" "$project_dir/tests/test_network_config.c" \
  -o "$test_dir/config"
"$test_dir/config"
"$relay_python" "$project_dir/tests/test_firmware_pairing.py"
"$relay_python" "$project_dir/tests/test_service_runtime.py"
"$relay_python" "$project_dir/tests/test_pairing_request.py"

"$relay_python" "$project_dir/tests/test_service_portal.py"
"$relay_python" "$project_dir/tests/test_provisioning.py"
"$relay_python" "$project_dir/tests/test_portal_socket.py"
