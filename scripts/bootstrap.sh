#!/usr/bin/env bash
set -euo pipefail
memo_host_only=0
if [[ "$#" == 1 && "$1" == --host-only ]]; then
  memo_host_only=1
elif [[ "$#" != 0 ]]; then
  printf '%s\n' '用法：scripts/bootstrap.sh [--host-only]' >&2
  exit 2
fi
cd "$(dirname "$0")/.."
umask 077
command -v uv >/dev/null
[[ "$(uv --version)" == 'uv 0.12.5 '* ]] || { printf '%s\n' '需要锁定的 uv 0.12.5；本脚本不安装全局工具。' >&2; exit 1; }
# Inherited environments must not redirect installation, caches, or IDF activation.
for memo_bootstrap_var in $(compgen -e); do
  case "$memo_bootstrap_var" in
    UV_*|PIP_*|IDF_*|ESP_IDF_*|PYTHONPATH|PYTHONHOME|PYTHONUSERBASE|PYTHONPYCACHEPREFIX|VIRTUAL_ENV|__PYVENV_LAUNCHER__|DEFAULT_COMPONENT_SERVICE_URL) unset "$memo_bootstrap_var" ;;
  esac
done
unset memo_bootstrap_var
mkdir -p .tools private/bootstrap-tmp
chmod 700 private/bootstrap-tmp
export UV_MANAGED_PYTHON=1 UV_NO_CONFIG=1 UV_KEYRING_PROVIDER=disabled
export UV_PYTHON_INSTALL_DIR="$PWD/.tools/host-python"
export UV_PYTHON_BIN_DIR="$PWD/.tools/bin"
export UV_CACHE_DIR="$PWD/.tools/uv-cache"
export UV_DEFAULT_INDEX=https://pypi.org/simple
export TMPDIR="$PWD/private/bootstrap-tmp"
export PIP_CONFIG_FILE=/dev/null PIP_USER=no PYTHONNOUSERSITE=1
export PIP_CACHE_DIR="$PWD/.tools/pip-cache"
export IDF_COMPONENT_CACHE_PATH="$PWD/.tools/component-cache"
export IDF_CCACHE_ENABLE=0 CCACHE_DISABLE=1
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
uv python install --no-bin 3.12.14
memo_python="$(uv python find --no-project 3.12.14)"
# esptool's sdist uses setuptools>=64. Seed that build backend from the same
# hash lock, then disable build isolation so no unpinned build dependency resolves.
"$memo_python" - "$PWD/requirements-idf.lock.txt" "$TMPDIR/setuptools.lock.txt" <<'PY'
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text()
block = re.search(r'(?ms)^setuptools==84\.0\.0[^\n]*\n(?:[ \t]+[^\n]*\n)*', text)
assert block and '--hash=sha256:' in block[0], 'Pinned build backend is missing'
Path(sys.argv[2]).write_text(block[0])
PY
memo_sync_python() {
  uv pip install --require-hashes --no-deps --only-binary :all: --python "$1" -r "$TMPDIR/setuptools.lock.txt"
  uv pip sync --require-hashes --no-build-isolation --strict --python "$1" "$2"
  uv pip check --python "$1"
}
# Rebase only these project-owned virtual environments; preserve other files.
uv venv --allow-existing --python "$memo_python" .venv
memo_sync_python .venv/bin/python requirements-host.lock.txt
if [[ "$memo_host_only" == 1 ]]; then exit 0; fi
if [[ ! -d .tools/esp-idf/.git ]]; then
  git clone --branch v5.5.1 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .tools/esp-idf
fi
[[ "$(git -C .tools/esp-idf rev-parse HEAD)" == fcae32885b0296b32044cb99ecbdc50d98dddb83 ]]
[[ -z "$(git -C .tools/esp-idf status --porcelain --untracked-files=no)" ]]
export IDF_TOOLS_PATH="$PWD/.tools/espressif"
"$memo_python" .tools/esp-idf/tools/idf_tools.py install --targets esp32s3
memo_idf_env="$IDF_TOOLS_PATH/python_env/idf5.5_py3.12_env"
uv venv --allow-existing --python "$memo_python" "$memo_idf_env"
memo_sync_python "$memo_idf_env/bin/python" requirements-idf.lock.txt
# IDF 5.5.1 uses this major.minor marker and records core features during its
# dependency check. No install-python-env: it upgrades pip and fetches mutable
# constraints before installing requirements. Our complete hash lock owns versions.
printf '%s\n' '5.5' > "$memo_idf_env/idf_version.txt"
export IDF_PYTHON_CHECK_CONSTRAINTS=0
"$memo_python" .tools/esp-idf/tools/idf_tools.py check-python-dependencies --no-constraints
