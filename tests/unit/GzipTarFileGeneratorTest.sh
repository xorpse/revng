#!/bin/bash
#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

set -euo pipefail

TEMP_FILE=$(mktemp -t revng.GzipTarFileGeneratorTest)
trap 'rm -f -- "$TEMP_FILE"' EXIT

"$1/test_gzip_tar_fileGenerator" foo foo2 bar bar2 > "$TEMP_FILE"
CONTENTS=$(tar -tf "$TEMP_FILE")

[[ $(wc -l <<< "$CONTENTS") -eq 2 ]]

grep -qF 'foo' <<< "$CONTENTS"
grep -qF 'bar' <<< "$CONTENTS"

[[ $(tar -xOf "$TEMP_FILE" foo) = "foo2" ]]
[[ $(tar -xOf "$TEMP_FILE" bar) = "bar2" ]]
