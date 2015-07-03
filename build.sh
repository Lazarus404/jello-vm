#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT}/build"

mkdir -p "${BUILD_DIR}"
(cd "${BUILD_DIR}" && cmake "$@" "${ROOT}/vm")
cmake --build "${BUILD_DIR}" --target jello_vm
cmake --build "${BUILD_DIR}" --target jlo_tool
