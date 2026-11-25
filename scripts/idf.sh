#!/usr/bin/env bash
set -e
project_dir=$(cd "$(dirname "$0")/.." && pwd)
export IDF_PATH="${IDF_PATH:-$HOME/.espressif/v5.5.4/esp-idf}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
  printf 'ESP-IDF v5.5.4 not found. Set IDF_PATH to its installation directory.\n' >&2
  exit 1
fi
# Use the installed IDF environment rather than the shell's unrelated Python version.
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
  for idf_env_path in "${IDF_TOOLS_PATH:-$HOME/.espressif}"/python_env/idf5.5_py*_env; do
    if [ -x "$idf_env_path/bin/python" ]; then
      export IDF_PYTHON_ENV_PATH="$idf_env_path"
      break
    fi
  done
fi
if [ -n "${IDF_PYTHON_ENV_PATH:-}" ]; then
  export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
fi
source "$IDF_PATH/export.sh"
cd "$project_dir"
exec idf.py "$@"
