#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Jahred Love. All rights reserved.

# Every fixture that loads must come back byte for byte through disasm and asm again, so that
# asm reads everything disasm prints; the Bytes pool, quoted atoms, imports and exact doubles.

set -euo pipefail

JLO_TOOL="${1:?usage: asm_roundtrip.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

failures=0
checked=0

while IFS= read -r fixture; do
  name="${fixture#"${ROOT}/tests/bytecode/"}"
  base="${TMP}/$(tr '/' '_' <<<"${name}")"
  
  # roundtrip
  "${JLO_TOOL}" asm "${fixture}" -o "${base}.jlo"
  "${JLO_TOOL}" disasm "${base}.jlo" -o "${base}.txt"
  
  # check match
  if ! "${JLO_TOOL}" asm "${base}.txt" -o "${base}.again.jlo"; then
    echo "asm_roundtrip FAIL: ${name}: asm rejected disasm's output" >&2
    failures=$((failures + 1))
  elif ! cmp -s "${base}.jlo" "${base}.again.jlo"; then
    echo "asm_roundtrip FAIL: ${name}: $(cmp "${base}.jlo" "${base}.again.jlo" 2>&1 || true)" >&2
    failures=$((failures + 1))
  fi

  # loop
  checked=$((checked + 1))
done < <(find "${ROOT}/tests/bytecode" -name '*.jlo.txt' -not -path '*/corrupt/*' | sort)

if [[ ${failures} -gt 0 ]]; then
  # dagnabbit
  echo "asm_roundtrip: ${failures} of ${checked} fixture(s) failed" >&2
  exit 1
fi

# woohoo!
echo "asm_roundtrip: ${checked} fixtures round-trip byte for byte"
