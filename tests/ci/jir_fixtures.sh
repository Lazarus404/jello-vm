#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Jahred Love. All rights reserved.

set -euo pipefail

JLO="${1:?usage: jir_fixtures.sh <path-to-jlo_tool>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IR_DIR="$ROOT/tests/jit/ir"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

check_pair() {
  local src="$1" golden="$2" label="$3"
  local bin="$TMP/$label.jlo"
  local got="$TMP/$label.jir.txt"
  if ! "$JLO" asm "$src" -o "$bin"; then
    echo "FAIL ($label): asm of $src failed"
    fail=1
    return
  fi
  if ! "$JLO" dumpir "$bin" > "$got" 2>"$TMP/$label.err"; then
    echo "FAIL ($label): dumpir failed: $(cat "$TMP/$label.err")"
    fail=1
    return
  fi
  if ! diff -u "$golden" "$got" > "$TMP/$label.diff"; then
    echo "FAIL ($label): dumpir output does not match golden fixture $golden"
    cat "$TMP/$label.diff"
    fail=1
    return
  fi
  echo "PASS ($label)"
}

check_pair "$ROOT/tests/bytecode/bench/fib.jlo.txt" \
           "$IR_DIR/fib_bin_i32_call_self_call_direct.jir.txt" \
           "fib_bin_i32_call_self_call_direct"

check_pair "$ROOT/tests/bytecode/bench/tak.jlo.txt" \
           "$IR_DIR/tak_call_self.jir.txt" \
           "tak_call_self"

check_pair "$ROOT/tests/bytecode/dyn/obj_set_get_atom_typed.jlo.txt" \
           "$IR_DIR/obj_set_get_atom_not_numeric_fast.jir.txt" \
           "obj_set_get_atom_not_numeric_fast"

check_pair "$ROOT/tests/bytecode/packet/blit_mid_offset.jlo.txt" \
           "$IR_DIR/blit_mid_offset_bytes_read_write_blit.jir.txt" \
           "blit_mid_offset_bytes_read_write_blit"

check_pair "$ROOT/tests/bytecode/bench/gc_churn.jlo.txt" \
           "$IR_DIR/gc_churn_backward_jmp_safepoint.jir.txt" \
           "gc_churn_backward_jmp_safepoint"

check_pair "$ROOT/tests/bytecode/bits_accept.jlo.txt" \
           "$IR_DIR/bits_accept_width_is_a_literal_not_a_register.jir.txt" \
           "bits_accept_width_is_a_literal_not_a_register"

exit $fail
