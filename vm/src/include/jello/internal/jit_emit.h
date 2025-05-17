// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#ifndef JELLO_JIT_EMIT_H
#define JELLO_JIT_EMIT_H

#include <jello/internal/jit_internal.h>

typedef enum jit_cond {
  JIT_JB = 0x82,
  JIT_JAE = 0x83,
  JIT_JE = 0x84,
  JIT_JNE = 0x85,
  JIT_JBE = 0x86,
  JIT_JA = 0x87,
  JIT_JL = 0x8C,
  JIT_JG = 0x8F,
  JIT_SETE = 0x94,
  JIT_SETNE = 0x95,
  JIT_SETA = 0x97,
  JIT_SETP = 0x9B,
  JIT_SETL = 0x9C,
} jit_cond;

typedef enum jit_alu {
  JIT_ALU_ADD = 0,
  JIT_ALU_OR = 1,
  JIT_ALU_AND = 4,
  JIT_ALU_SUB = 5,
  JIT_ALU_XOR = 6,
  JIT_ALU_CMP = 7,
} jit_alu;

typedef struct jit_branch_patch {
  size_t at;
  uint32_t bc_tgt;
} jit_branch_patch;

typedef enum jit_trap_kind {
  JIT_TRAP_ARITH = 0,
  JIT_TRAP_NULL,
  JIT_TRAP_BOUNDS,
  JIT_TRAP_TYPE,
  JIT_TRAP_PASSTHROUGH, /**< Code and location already recorded, by a callee or jit_run_handler. */
  JIT_TRAP_CALLEE,      /**< A callee's trap; the site records the call in the trace. */
  JIT_TRAP_HELPER,      /**< A C helper set the code; the site records only the location. */
  JIT_TRAP_LIMIT,
  JIT_TRAP_KIND_COUNT,
} jit_trap_kind;

typedef struct jit_trap_jump {
  size_t at;
  uint8_t kind; /**< A jit_trap_kind. */
} jit_trap_jump;

typedef struct emit_ctx {
  jit_exec_buf* buf;
  jit_reg_state* ra;
  const jello_bc_module* m;
  const jello_bc_function* f;
  uint32_t fi;
  uint32_t cur_bc_pc; /**< bc_pc of the instruction being emitted; trap sites record it. */
  jit_trap_jump* trap_jumps;
  size_t trap_jump_cap;
  size_t trap_jump_count;
  jit_branch_patch* branches;
  size_t branch_cap;
  size_t branch_count;
  size_t* ret_jumps;
  size_t ret_jump_cap;
  size_t ret_jump_count;
  size_t* stack_overflow_jumps;
  size_t stack_overflow_jump_cap;
  size_t stack_overflow_jump_count;
  const void* const* entries;
  uint32_t* catch_pcs;
  uint32_t ncatch;
  jello_type_kind ret_kind;
  int survive_any_armed;
  int oom;
} emit_ctx;

void jit_push_trap_jump(emit_ctx* ctx, size_t at, uint8_t kind);
void jit_push_branch(emit_ctx* ctx, size_t at, uint32_t bc_tgt);
void jit_push_ret_jump(emit_ctx* ctx, size_t at);
void jit_push_stack_overflow_jump(emit_ctx* ctx, size_t at);
uint32_t jit_branch_target(const emit_ctx* ctx, const jello_jit_ir_insn* ins);

void jit_emit_prologue(jit_exec_buf* buf, size_t* out_body_off, int any_armed);
int jit_emit_insn(emit_ctx* ctx, const jello_jit_ir_insn* ins);
void jit_emit_ok_exit(emit_ctx* ctx);
void jit_emit_trap_exit(emit_ctx* ctx, uintptr_t trap_fn);
void jit_emit_catch_dispatch(emit_ctx* ctx);
void jit_patch_jump(jit_exec_buf* buf, size_t at, size_t target);

#endif /* JELLO_JIT_EMIT_H */
