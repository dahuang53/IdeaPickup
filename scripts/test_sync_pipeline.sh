#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
.venv-relay/bin/python -c 'import sys; assert sys.version_info[:3] == (3, 11, 16), "relay Python version mismatch"'
mkdir -p private/host-tests
chmod 700 private/host-tests
suite_dir="$(mktemp -d private/host-tests/sync-pipeline.XXXXXX)"
trap 'rm -rf -- "$suite_dir"' EXIT
"${CC:-cc}" -std=c11 -DMEMO_HOST_TEST -Wall -Wextra -Werror -pedantic -g -O1 \
 -fsanitize=address,undefined -I main main/memo_record_ids.c main/memo_cardless_record.c \
 tests/test_record_ids.c -o "$suite_dir/ids"
"$suite_dir/ids"
.venv-relay/bin/python tests/test_sync_pipeline.py
.venv-relay/bin/python tests/test_cardless_runtime.py
.venv-relay/bin/python tests/test_record_ids_nvs.py
