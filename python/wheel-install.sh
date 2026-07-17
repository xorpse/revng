#!/bin/bash
#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
set -euo pipefail

ARGS=(
  --quiet
  --compile
  --no-index
  --no-build-isolation
  --ignore-installed
  --no-deps
  --prefix "$2"
)
if test -n "${DESTDIR:-}"; then
  ARGS+=(--root "$DESTDIR")
fi
python -m pip install "${ARGS[@]}" "$1"
