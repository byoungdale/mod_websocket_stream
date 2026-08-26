#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${SCRIPT_DIR}/.venv"

if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
	python3 -m venv "${VENV_DIR}"
fi

if ! "${VENV_DIR}/bin/python" -c 'import aiohttp, certifi' >/dev/null 2>&1; then
	"${VENV_DIR}/bin/python" -m pip install \
		-r "${SCRIPT_DIR}/requirements.txt"
fi

exec "${VENV_DIR}/bin/python" "${SCRIPT_DIR}/server.py" "$@"
