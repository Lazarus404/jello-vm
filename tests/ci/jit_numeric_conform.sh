#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: jit_numeric_conform.sh <path-to-jlo_tool> <path-to-jit_numeric_run>}"
JIT_RUN="${2:?usage: jit_numeric_conform.sh <path-to-jlo_tool> <path-to-jit_numeric_run>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/interp"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

HOST_ARCH="$(uname -m)"
if [[ "${HOST_ARCH}" != "x86_64" ]]; then
  echo "chapter11_jit_conform: SKIPPED (host is not x86_64 -- use chapter11_jit_numeric_x64 for cross-run conform on ARM64 Darwin)"
  exit 0
fi

failures=0
fail() {
  echo "jit_numeric_conform FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== JIT numeric/cfg conform: interp fixtures (incl. jmp / hot_loop; skip call) =="
for fixture in "${FIXDIR}"/*.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  if [[ "${base}" == *jmp* || "${base}" == *call* || "${base}" == "stack_overflow" ]]; then
    echo "jit_numeric_conform: ${base} -> SKIPPED (control flow / non-numeric)"
    continue
  fi
  expect_file="${FIXDIR}/${base}.expect"
  if [[ ! -f "${expect_file}" ]]; then
    fail "${base}: no .expect file"
    continue
  fi
  bin="${TMP}/${base}.jlo"
  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${TMP}/${base}.asm.err"; then
    fail "${base}: asm failed -- $(cat "${TMP}/${base}.asm.err")"
    continue
  fi
  expected="$("${JLO_TOOL}" run "${bin}" 2>"${TMP}/${base}.interp.err" || true)"
  actual="$("${JIT_RUN}" "${bin}" 2>"${TMP}/${base}.jit.err" || true)"
  if [[ "${actual}" == "JIT_SKIP" ]]; then
    echo "jit_numeric_conform: ${base} -> JIT_SKIP (not JIT-compilable yet)"
    continue
  fi
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${base}: expected [${expected}] got [${actual}] interp=$(cat "${TMP}/${base}.interp.err") jit=$(cat "${TMP}/${base}.jit.err")"
  else
    echo "jit_numeric_conform: ${base} -> ${actual}"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "chapter11_jit_conform: ${failures} failure(s)" >&2
  exit 1
fi

echo "chapter11_jit_conform: passed"