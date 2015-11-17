#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM_CMAKE="${ROOT}/vm"
TOOLCHAIN_X64="${VM_CMAKE}/cmake/Toolchain-x64.cmake"
TMP="${TMPDIR:-/tmp}"
BUILD_DIR="${TMP}/jello_ch11_conform_x64"

HOST_ARCH="$(uname -m)"
HOST_OS="$(uname -s)"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cmake -S "${VM_CMAKE}" -B "${BUILD_DIR}" -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_X64}" -DJELLO_BUILD_TESTS=ON >/dev/null
cmake --build "${BUILD_DIR}" --target jlo_tool jit_numeric_run

JLO_TOOL="$(find "${BUILD_DIR}" -type f -name jlo_tool | head -n1)"
JIT_RUN="$(find "${BUILD_DIR}" -type f -name jit_numeric_run | head -n1)"
if [[ -z "${JLO_TOOL}" || -z "${JIT_RUN}" ]]; then
  echo "chapter11_jit_conform_x64: build did not produce jlo_tool / jit_numeric_run" >&2
  exit 1
fi

FIXDIR="${ROOT}/tests/bytecode/interp"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

failures=0
fail() {
  echo "jit_numeric_conform_x64 FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== JIT numeric/cfg conform (x64): interp fixtures (incl. jmp) =="
for fixture in "${FIXDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  if [[ "${base}" == *call* || "${base}" == "stack_overflow" ]]; then
    echo "jit_numeric_conform_x64: ${base} -> SKIPPED (call / non-numeric)"
    continue
  fi
  expect_file="${FIXDIR}/${base}.expect"
  if [[ ! -f "${expect_file}" ]]; then
    fail "${base}: no .expect file"
    continue
  fi
  bin="${WORKDIR}/${base}.jlo"
  if ! arch -x86_64 "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${WORKDIR}/${base}.asm.err"; then
    fail "${base}: asm failed -- $(cat "${WORKDIR}/${base}.asm.err")"
    continue
  fi
  expected="$(arch -x86_64 "${JLO_TOOL}" run "${bin}" 2>"${WORKDIR}/${base}.interp.err" || true)"
  actual="$(arch -x86_64 "${JIT_RUN}" "${bin}" 2>"${WORKDIR}/${base}.jit.err" || true)"
  if [[ "${actual}" == "JIT_SKIP" ]]; then
    echo "jit_numeric_conform_x64: ${base} -> JIT_SKIP (not JIT-compilable yet)"
    continue
  fi
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${base}: expected [${expected}] got [${actual}] interp=$(cat "${WORKDIR}/${base}.interp.err") jit=$(cat "${WORKDIR}/${base}.jit.err")"
  else
    echo "jit_numeric_conform_x64: ${base} -> ${actual}"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "chapter11_jit_conform_x64: ${failures} failure(s)" >&2
  exit 1
fi

echo "chapter11_jit_conform_x64: passed"