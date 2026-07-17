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
  --upgrade
  --no-deps
  --target "${DESTDIR:-}$2/$4"
)
"$3" -m pip install "${ARGS[@]}" "$1"
