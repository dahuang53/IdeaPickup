#!/usr/bin/env bash
# Source this file from a shell before idf.py; all tools stay in this project.
MEMO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for memo_env_var in $(compgen -e); do
  case "$memo_env_var" in
    UV_*|PIP_*|IDF_*|ESP_IDF_*|PYTHONPATH|PYTHONHOME|PYTHONUSERBASE|PYTHONPYCACHEPREFIX|VIRTUAL_ENV|__PYVENV_LAUNCHER__|DEFAULT_COMPONENT_SERVICE_URL) unset "$memo_env_var" ;;
  esac
done
unset memo_env_var
mkdir -p "$MEMO_ROOT/private/bootstrap-tmp"
chmod 700 "$MEMO_ROOT/private/bootstrap-tmp"
export TMPDIR="$MEMO_ROOT/private/bootstrap-tmp"
export IDF_PATH="$MEMO_ROOT/.tools/esp-idf"
export IDF_TOOLS_PATH="$MEMO_ROOT/.tools/espressif"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env/idf5.5_py3.12_env"
export IDF_COMPONENT_CACHE_PATH="$MEMO_ROOT/.tools/component-cache"
export PIP_CACHE_DIR="$MEMO_ROOT/.tools/pip-cache"
export PIP_CONFIG_FILE=/dev/null PIP_USER=no PYTHONNOUSERSITE=1
export UV_MANAGED_PYTHON=1 UV_NO_CONFIG=1 UV_KEYRING_PROVIDER=disabled
export UV_PYTHON_INSTALL_DIR="$MEMO_ROOT/.tools/host-python"
export UV_PYTHON_BIN_DIR="$MEMO_ROOT/.tools/bin"
export UV_CACHE_DIR="$MEMO_ROOT/.tools/uv-cache"
export UV_DEFAULT_INDEX=https://pypi.org/simple
export IDF_CCACHE_ENABLE=0 CCACHE_DISABLE=1
# IDF's supported self-managed dependency mode; still check its core requirements.
export IDF_PYTHON_CHECK_CONSTRAINTS=0
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export PATH="$MEMO_ROOT/.venv/bin:$PATH"
"$IDF_PYTHON_ENV_PATH/bin/python" - "$MEMO_ROOT" <<'PY' || return 1
from importlib.metadata import version
from pathlib import Path
import re
import sys
root = Path(sys.argv[1]).resolve()
assert sys.version_info[:3] == (3, 12, 14), 'Run scripts/bootstrap.sh: wrong IDF Python'
assert Path(sys.base_prefix).resolve().is_relative_to(root / '.tools/host-python'), 'Run scripts/bootstrap.sh: external IDF Python'
for name, expected in re.findall(r'^([A-Za-z0-9_.-]+)==([^\s\\]+)', (root / 'requirements-idf.lock.txt').read_text(), re.M):
    assert version(name) == expected, f'Run scripts/bootstrap.sh: wrong version of {name}'
PY
[[ "$(git -C "$IDF_PATH" rev-parse HEAD)" == fcae32885b0296b32044cb99ecbdc50d98dddb83 ]] || return 1
[[ -z "$(git -C "$IDF_PATH" status --porcelain --untracked-files=no)" ]] || return 1
"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf_tools.py" check-python-dependencies --no-constraints || return 1
. "$IDF_PATH/export.sh"
