#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: gc_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/gc"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "gc_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== correctness fixtures: Bytes opcodes, traps, root survival (Sec 2.8/2.10) =="
for fixture in "${FIXDIR}"/bytes_new_len.jlo.txt "${FIXDIR}"/bytes_set_get_u8.jlo.txt \
               "${FIXDIR}"/bytes_null_deref_len.jlo.txt "${FIXDIR}"/bytes_null_deref_get.jlo.txt \
               "${FIXDIR}"/bytes_null_deref_set.jlo.txt "${FIXDIR}"/bytes_bounds_get.jlo.txt \
               "${FIXDIR}"/bytes_bounds_set_index.jlo.txt "${FIXDIR}"/bytes_bounds_set_value.jlo.txt \
               "${FIXDIR}"/bytes_call_arg.jlo.txt "${FIXDIR}"/bytes_gc_survives.jlo.txt; do
  base="$(basename "${fixture}" .jlo.txt)"
  expect_file="${FIXDIR}/${base}.expect"
  bin="${TMP}/${base}.jlo"
  if ! "${JLO_TOOL}" asm "${fixture}" -o "${bin}" 2>"${TMP}/${base}.asm.err"; then
    fail "${base}: asm failed -- $(cat "${TMP}/${base}.asm.err")"
    continue
  fi
  actual="$("${JLO_TOOL}" run "${bin}" 2>"${TMP}/${base}.run.err" || true)"
  expected="$(cat "${expect_file}")"
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${base}: expected [${expected}] got [${actual}] $(cat "${TMP}/${base}.run.err")"
  else
    echo "gc_fixtures: ${base} -> ${actual}"
  fi
done

if [[ ${failures} -gt 0 ]]; then
  echo "chapter4_gc_v1: ${failures} failure(s)" >&2
  exit 1
fi

echo "chapter4_gc_v1: passed"
