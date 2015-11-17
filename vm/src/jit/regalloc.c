// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal/jit_impl.h>
#include <stdlib.h>
#include <string.h>

/* -- HashLink-style home/current read side -- */

uint32_t jit_reg_reload_count;
uint32_t jit_reg_discard_count;

static const uint8_t jit_gpr_pool[] = {6, 7, 8, 9, 10, 11, 14, 15};

enum {
  JIT_GPR_POOL_LEN = 8,
  JIT_XMM_POOL_LEN = 6,
};

static int preg_is_xmm(int preg) {
  return preg >= (int)JIT_PREG_XMM0;
}

static int preg_xmm_index(int preg) {
  return preg - (int)JIT_PREG_XMM0;
}

static int preg_to_gpr_pool_index(int preg) {
  for(unsigned i = 0; i < JIT_GPR_POOL_LEN; i++) {
    if((int)jit_gpr_pool[i] == preg) return (int)i;
  }
  return -1;
}

/* [r13 + disp32] -- JIT_REG_BASE is R13 (rm=5 with REX.B), mod=10, no SIB. */
static void emit_load_gpr64_from_slot(jit_exec_buf* buf, unsigned dst_gpr, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  bytes[0] = (uint8_t)(0x48u | ((dst_gpr >= 8) ? 0x04u : 0) | 0x01u);
  bytes[1] = 0x8B;
  bytes[2] = (uint8_t)(0x80u | ((dst_gpr & 7u) << 3) | 5u);
  memcpy(&bytes[3], &disp, 4);
  jit_exec_buf_write(buf, bytes, 7);
}

static void emit_store_gpr32_to_slot(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  size_t n = 0;
  bytes[n++] = (uint8_t)(0x40u | ((src_gpr >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x89;
  bytes[n++] = (uint8_t)(0x80u | ((src_gpr & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_gpr64_to_slot(jit_exec_buf* buf, uint8_t vreg, unsigned src_gpr) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[8];
  bytes[0] = (uint8_t)(0x48u | ((src_gpr >= 8) ? 0x04u : 0) | 0x01u);
  bytes[1] = 0x89;
  bytes[2] = (uint8_t)(0x80u | ((src_gpr & 7u) << 3) | 5u);
  memcpy(&bytes[3], &disp, 4);
  jit_exec_buf_write(buf, bytes, 7);
}

static void emit_load_xmm32_from_slot(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  /* Legacy SSE prefixes (F3/F2/66) must precede REX. */
  bytes[n++] = 0xF3;
  bytes[n++] = (uint8_t)(0x40u | ((dst_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((dst_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_load_xmm64_from_slot(jit_exec_buf* buf, unsigned dst_xmm, uint8_t vreg) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  bytes[n++] = 0xF2;
  bytes[n++] = (uint8_t)(0x40u | ((dst_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x10;
  bytes[n++] = (uint8_t)(0x80u | ((dst_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_xmm32_to_slot(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  bytes[n++] = 0xF3;
  bytes[n++] = (uint8_t)(0x40u | ((src_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((src_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

static void emit_store_xmm64_to_slot(jit_exec_buf* buf, uint8_t vreg, unsigned src_xmm) {
  uint32_t disp = (uint32_t)vreg * REG_SLOT_BYTES;
  uint8_t bytes[10];
  size_t n = 0;
  bytes[n++] = 0xF2;
  bytes[n++] = (uint8_t)(0x40u | ((src_xmm >= 8) ? 0x04u : 0) | 0x01u);
  bytes[n++] = 0x0F;
  bytes[n++] = 0x11;
  bytes[n++] = (uint8_t)(0x80u | ((src_xmm & 7u) << 3) | 5u);
  memcpy(&bytes[n], &disp, 4);
  n += 4;
  jit_exec_buf_write(buf, bytes, n);
}

void jit_reg_state_init(jit_reg_state* st, uint32_t nregs, jit_reg_emit_ctx emit) {
  st->nregs = nregs;
  st->emit = emit;
  st->current = (int8_t*)malloc(nregs ? nregs : 1u);
  if(st->current) {
    for(uint32_t i = 0; i < nregs; i++) st->current[i] = (int8_t)JIT_PREG_NONE;
  }
  for(unsigned i = 0; i < JIT_GPR_POOL_LEN; i++) st->gpr_vreg[i] = (int8_t)JIT_PREG_NONE;
  for(unsigned i = 0; i < JIT_XMM_POOL_LEN; i++) st->xmm_vreg[i] = (int8_t)JIT_PREG_NONE;
  memset(st->gpr_pinned, 0, sizeof st->gpr_pinned);
  memset(st->xmm_pinned, 0, sizeof st->xmm_pinned);
  st->next_gpr_victim = 0;
  st->next_xmm_victim = 0;
}

void jit_reg_state_free(jit_reg_state* st) {
  free(st->current);
  st->current = NULL;
}

void jit_reg_state_clobber_call(jit_reg_state* st) {
  for(uint32_t v = 0; v < st->nregs; v++) {
    if(st->current[v] != (int8_t)JIT_PREG_NONE) {
      int preg = (int)st->current[v];
      if(preg_is_xmm(preg)) {
        int xi = preg_xmm_index(preg);
        if(xi >= 0 && xi < (int)JIT_XMM_POOL_LEN) st->xmm_vreg[xi] = (int8_t)JIT_PREG_NONE;
      } else {
        int gi = preg_to_gpr_pool_index(preg);
        if(gi >= 0) st->gpr_vreg[gi] = (int8_t)JIT_PREG_NONE;
      }
      st->current[v] = (int8_t)JIT_PREG_NONE;
    }
  }
}

void jit_reg_state_discard_currents(jit_reg_state* st) {
  jit_reg_discard_count++;
  jit_reg_state_clobber_call(st);
}

void jit_reg_unbind_preg(jit_reg_state* st, int preg) {
  if(preg_is_xmm(preg)) {
    int xi = preg_xmm_index(preg);
    if(xi >= 0 && xi < (int)JIT_XMM_POOL_LEN) {
      int8_t v = st->xmm_vreg[xi];
      st->xmm_vreg[xi] = (int8_t)JIT_PREG_NONE;
      if(v >= 0 && (uint32_t)v < st->nregs) st->current[v] = (int8_t)JIT_PREG_NONE;
    }
  } else {
    int gi = preg_to_gpr_pool_index(preg);
    if(gi >= 0) {
      int8_t v = st->gpr_vreg[gi];
      st->gpr_vreg[gi] = (int8_t)JIT_PREG_NONE;
      if(v >= 0 && (uint32_t)v < st->nregs) st->current[v] = (int8_t)JIT_PREG_NONE;
    }
  }
}

void jit_reg_unbind_vreg(jit_reg_state* st, uint8_t vreg) {
  if((uint32_t)vreg >= st->nregs) return;
  int preg = (int)st->current[vreg];
  if(preg == JIT_PREG_NONE) return;
  jit_reg_unbind_preg(st, preg);
}

void jit_reg_unpin_all(jit_reg_state* st) {
  memset(st->gpr_pinned, 0, sizeof st->gpr_pinned);
  memset(st->xmm_pinned, 0, sizeof st->xmm_pinned);
}

static void pin_gpr_index(jit_reg_state* st, unsigned i) {
  if(i < (unsigned)(sizeof st->gpr_pinned)) st->gpr_pinned[i] = 1;
}

static void pin_xmm_index(jit_reg_state* st, unsigned i) {
  if(i < (unsigned)(sizeof st->xmm_pinned)) st->xmm_pinned[i] = 1;
}

static void pin_preg(jit_reg_state* st, int preg) {
  if(preg_is_xmm(preg)) {
    int xi = preg_xmm_index(preg);
    if(xi >= 0) pin_xmm_index(st, (unsigned)xi);
  } else {
    int gi = preg_to_gpr_pool_index(preg);
    if(gi >= 0) pin_gpr_index(st, (unsigned)gi);
  }
}

static unsigned pick_unpinned_victim(const uint8_t* pinned, unsigned len, unsigned start) {
  unsigned i = start % len;
  for(unsigned n = 0; n < len; n++) {
    if(!pinned[i]) return i;
    i = (i + 1u) % len;
  }
  return start % len;
}

void jit_reg_bind(jit_reg_state* st, uint8_t vreg, int preg) {
  if((uint32_t)vreg >= st->nregs) return;
  jit_reg_unbind_vreg(st, vreg);
  jit_reg_unbind_preg(st, preg);
  st->current[vreg] = (int8_t)preg;
  if(preg_is_xmm(preg)) {
    int xi = preg_xmm_index(preg);
    if(xi >= 0 && xi < (int)JIT_XMM_POOL_LEN) st->xmm_vreg[xi] = (int8_t)vreg;
  } else {
    int gi = preg_to_gpr_pool_index(preg);
    if(gi >= 0) st->gpr_vreg[gi] = (int8_t)vreg;
  }
}

static int alloc_from_gpr_pool(jit_reg_state* st, uint8_t vreg, int and_load) {
  for(unsigned i = 0; i < JIT_GPR_POOL_LEN; i++) {
    if(st->gpr_vreg[i] == (int8_t)JIT_PREG_NONE) {
      int preg = (int)jit_gpr_pool[i];
      jit_reg_bind(st, vreg, preg);
      pin_gpr_index(st, i);
      if(and_load) {
        emit_load_gpr64_from_slot(st->emit.buf, (unsigned)preg, vreg);
        jit_reg_reload_count++;
      }
      return preg;
    }
  }
  {
    unsigned i = pick_unpinned_victim(st->gpr_pinned, JIT_GPR_POOL_LEN, st->next_gpr_victim);
    int8_t victim = st->gpr_vreg[i];
    if(victim != (int8_t)JIT_PREG_NONE) st->current[(uint8_t)victim] = (int8_t)JIT_PREG_NONE;
    st->gpr_vreg[i] = (int8_t)JIT_PREG_NONE;
    int preg = (int)jit_gpr_pool[i];
    jit_reg_bind(st, vreg, preg);
    pin_gpr_index(st, i);
    if(and_load) {
      emit_load_gpr64_from_slot(st->emit.buf, (unsigned)preg, vreg);
      jit_reg_reload_count++;
    }
    st->next_gpr_victim = (i + 1u) % JIT_GPR_POOL_LEN;
    return preg;
  }
}

static int alloc_from_xmm_pool(jit_reg_state* st, uint8_t vreg, int and_load, int wide) {
  for(unsigned i = 0; i < JIT_XMM_POOL_LEN; i++) {
    if(st->xmm_vreg[i] == (int8_t)JIT_PREG_NONE) {
      int preg = (int)JIT_PREG_XMM0 + (int)i;
      jit_reg_bind(st, vreg, preg);
      pin_xmm_index(st, i);
      if(and_load) {
        if(wide) emit_load_xmm64_from_slot(st->emit.buf, (unsigned)i, vreg);
        else emit_load_xmm32_from_slot(st->emit.buf, (unsigned)i, vreg);
        jit_reg_reload_count++;
      }
      return preg;
    }
  }
  {
    unsigned i = pick_unpinned_victim(st->xmm_pinned, JIT_XMM_POOL_LEN, st->next_xmm_victim);
    int8_t victim = st->xmm_vreg[i];
    if(victim != (int8_t)JIT_PREG_NONE) st->current[(uint8_t)victim] = (int8_t)JIT_PREG_NONE;
    st->xmm_vreg[i] = (int8_t)JIT_PREG_NONE;
    int preg = (int)JIT_PREG_XMM0 + (int)i;
    jit_reg_bind(st, vreg, preg);
    pin_xmm_index(st, i);
    if(and_load) {
      if(wide) emit_load_xmm64_from_slot(st->emit.buf, (unsigned)i, vreg);
      else emit_load_xmm32_from_slot(st->emit.buf, (unsigned)i, vreg);
      jit_reg_reload_count++;
    }
    st->next_xmm_victim = (i + 1u) % JIT_XMM_POOL_LEN;
    return preg;
  }
}

int jit_reg_alloc_gpr(jit_reg_state* st, uint8_t vreg, int and_load) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  jit_reg_unbind_vreg(st, vreg);
  return alloc_from_gpr_pool(st, vreg, and_load);
}

int jit_reg_alloc_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  jit_reg_unbind_vreg(st, vreg);
  return alloc_from_xmm_pool(st, vreg, and_load, wide);
}

int jit_reg_get_gpr(jit_reg_state* st, uint8_t vreg, int and_load) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  int preg = (int)st->current[vreg];
  if(preg != JIT_PREG_NONE && !preg_is_xmm(preg)) {
    pin_preg(st, preg);
    return preg;
  }
  return jit_reg_alloc_gpr(st, vreg, and_load);
}

int jit_reg_get_xmm(jit_reg_state* st, uint8_t vreg, int and_load, int wide) {
  if((uint32_t)vreg >= st->nregs) return JIT_PREG_NONE;
  int preg = (int)st->current[vreg];
  if(preg != JIT_PREG_NONE && preg_is_xmm(preg)) {
    pin_preg(st, preg);
    return preg;
  }
  return jit_reg_alloc_xmm(st, vreg, and_load, wide);
}

void jit_reg_store_home_gpr32(jit_reg_state* st, uint8_t vreg, int preg) {
  if(preg_is_xmm(preg)) return;
  emit_store_gpr32_to_slot(st->emit.buf, vreg, (unsigned)preg);
  jit_reg_bind(st, vreg, preg);
}

void jit_reg_store_home_gpr64(jit_reg_state* st, uint8_t vreg, int preg) {
  if(preg_is_xmm(preg)) return;
  emit_store_gpr64_to_slot(st->emit.buf, vreg, (unsigned)preg);
  jit_reg_bind(st, vreg, preg);
}

void jit_reg_store_home_xmm32(jit_reg_state* st, uint8_t vreg, int preg) {
  if(!preg_is_xmm(preg)) return;
  emit_store_xmm32_to_slot(st->emit.buf, vreg, (unsigned)preg_xmm_index(preg));
  jit_reg_bind(st, vreg, preg);
}

void jit_reg_store_home_xmm64(jit_reg_state* st, uint8_t vreg, int preg) {
  if(!preg_is_xmm(preg)) return;
  emit_store_xmm64_to_slot(st->emit.buf, vreg, (unsigned)preg_xmm_index(preg));
  jit_reg_bind(st, vreg, preg);
}
