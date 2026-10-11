#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
umask 077
command -v uv >/dev/null || { printf '%s\n' '需要 uv；本脚本不会安装全局工具。' >&2; exit 1; }
[[ "$(uv --version)" == 'uv 0.12.5 '* ]] || { printf '%s\n' '需要锁定的 uv 0.12.5。' >&2; exit 1; }
for memo_relay_var in $(compgen -e); do
  case "$memo_relay_var" in
    UV_*|PIP_*|IDF_*|ESP_IDF_*|PYTHONPATH|PYTHONHOME|PYTHONUSERBASE|PYTHONPYCACHEPREFIX|VIRTUAL_ENV|__PYVENV_LAUNCHER__) unset "$memo_relay_var" ;;
  esac
done
unset memo_relay_var
mkdir -p "$project_dir/private/bootstrap-tmp"
chmod 700 "$project_dir/private/bootstrap-tmp"
export TMPDIR="$project_dir/private/bootstrap-tmp"
export UV_MANAGED_PYTHON=1 UV_NO_CONFIG=1 UV_KEYRING_PROVIDER=disabled
export UV_PYTHON_INSTALL_DIR="$project_dir/.tools/relay-python"
export UV_PYTHON_BIN_DIR="$project_dir/.tools/bin"
export UV_CACHE_DIR="$project_dir/.tools/uv-cache"
export UV_PROJECT_ENVIRONMENT="$project_dir/.venv-relay"
export UV_DEFAULT_INDEX=https://pypi.org/simple
export PIP_CONFIG_FILE=/dev/null PIP_USER=no PYTHONNOUSERSITE=1
export PIP_CACHE_DIR="$project_dir/.tools/pip-cache"
uv python install --no-bin 3.11.16
uv sync --locked --project "$project_dir/server-relay" --python 3.11.16 --group test
