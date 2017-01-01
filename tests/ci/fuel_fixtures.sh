#!/usr/bin/env bash
set -euo pipefail

JLO_TOOL="${1:?usage: fuel_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXDIR="${ROOT}/tests/bytecode/fuel"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
  echo "fuel_fixtures FAIL: $*" >&2
  failures=$((failures + 1))
}

assemble() {
  local base="$1"
  "${JLO_TOOL}" asm "${FIXDIR}/${base}.jlo.txt" -o "${TMP}/${base}.jlo" 2>"${TMP}/${base}.asm.err" \
    || { fail "${base}: asm failed -- $(cat "${TMP}/${base}.asm.err")"; return 1; }
  return 0
}

echo "== fuel: unlimited (default) matches the existing, pre-Chapter-18 result exactly =="
if assemble "loop_sum"; then
  line="$("${JLO_TOOL}" run "${TMP}/loop_sum.jlo" 2>"${TMP}/loop_sum.run.err" | head -1 || true)"
  if [[ "${line}" != "RESULT kind=I32 value=4950" ]]; then
    fail "loop_sum (no fuel): expected [RESULT kind=I32 value=4950] got [${line}] $(cat "${TMP}/loop_sum.run.err")"
  else
    echo "fuel_fixtures: loop_sum (no fuel) -> ${line}"
  fi
fi
if assemble "recursive_countdown"; then
  line="$("${JLO_TOOL}" run "${TMP}/recursive_countdown.jlo" 2>"${TMP}/rc.run.err" | head -1 || true)"
  if [[ "${line}" != "RESULT kind=I32 value=1275" ]]; then
    fail "recursive_countdown (no fuel): expected [RESULT kind=I32 value=1275] got [${line}] $(cat "${TMP}/rc.run.err")"
  else
    echo "fuel_fixtures: recursive_countdown (no fuel) -> ${line}"
  fi
fi

echo "== fuel: a budget far below the required iteration/call count halts cleanly with JELLO_TRAP_FUEL =="
line="$("${JLO_TOOL}" run "${TMP}/loop_sum.jlo" --fuel 10 2>"${TMP}/loop_sum.fuel.err" | head -1 || true)"
if [[ "${line}" != "TRAP code=FUEL" ]]; then
  fail "loop_sum (--fuel 10): expected [TRAP code=FUEL] got [${line}] $(cat "${TMP}/loop_sum.fuel.err")"
else
  echo "fuel_fixtures: loop_sum (--fuel 10) -> ${line}"
fi
line="$("${JLO_TOOL}" run "${TMP}/recursive_countdown.jlo" --fuel 10 2>"${TMP}/rc.fuel.err" | head -1 || true)"
if [[ "${line}" != "TRAP code=FUEL" ]]; then
  fail "recursive_countdown (--fuel 10): expected [TRAP code=FUEL] got [${line}] $(cat "${TMP}/rc.fuel.err")"
else
  echo "fuel_fixtures: recursive_countdown (--fuel 10) -> ${line}"
fi

if [[ ${failures} -gt 0 ]]; then
  echo "chapter18_fuel_and_limits: ${failures} failure(s)" >&2
  exit 1
fi

echo "chapter18_fuel_and_limits: passed"