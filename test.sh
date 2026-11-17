#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT}/build"

python3 "${ROOT}/vm/tools/gen_unicode_norm.py" > "${BUILD_DIR}/unicode_norm.h"
cmp -s "${BUILD_DIR}/unicode_norm.h" "${ROOT}/vm/std/unicode_norm.h" || {
  echo "vm/std/unicode_norm.h differs from vm/tools/gen_unicode_norm.py's output; regenerate it" >&2
  exit 1
}

"${ROOT}/build.sh"

(cd "${BUILD_DIR}" && ctest --output-on-failure "$@")
