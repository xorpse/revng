#!/bin/bash
#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
set -euo pipefail

INSTALL_TARGET="${DESTDIR:-}$2/$4"

"$3" -m zipfile -e "$1" "$INSTALL_TARGET"
"$3" -m compileall -q "$INSTALL_TARGET"
