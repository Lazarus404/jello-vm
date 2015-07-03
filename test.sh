#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT}/build"

"${ROOT}/build.sh"

(cd "${BUILD_DIR}" && ctest --output-on-failure "$@")
