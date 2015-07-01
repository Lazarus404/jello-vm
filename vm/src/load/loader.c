// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>
#include <stdlib.h>
#include <string.h>

/* -- loader -- */

#define JELLO_JLO_MAGIC 0x4A4C5942u
#define JELLO_JLO_VERSION 1u

typedef struct rd {
  const uint8_t* p;
  size_t n;
  size_t off;
} rd;

static jello_bc_result ok(void) {
  jello_bc_result r = { JELLO_BC_OK, "ok", 0 };
  return r;
}

static jello_bc_result err(jello_bc_error e, const char* msg, size_t off) {
  jello_bc_result r = { e, msg, off };
  return r;
}

static int rd_need(rd* r, size_t k) {
  return r->off + k <= r->n;
}

static jello_bc_result rd_u8(rd* r, uint8_t* out) {
  if(!rd_need(r, 1)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  *out = r->p[r->off++];
  return ok();
}

static jello_bc_result rd_u16(rd* r, uint16_t* out) {
  if(!rd_need(r, 2)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  *out = (uint16_t)((uint16_t)r->p[r->off] | ((uint16_t)r->p[r->off + 1] << 8));
  r->off += 2;
  return ok();
}

static jello_bc_result rd_u32(rd* r, uint32_t* out) {
  if(!rd_need(r, 4)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  *out = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1] << 8) |
         ((uint32_t)r->p[r->off + 2] << 16) | ((uint32_t)r->p[r->off + 3] << 24);
  r->off += 4;
  return ok();
}

static jello_bc_result rd_bytes(rd* r, uint8_t* out, size_t n) {
  if(!rd_need(r, n)) return err(JELLO_BC_EOF, "unexpected EOF", r->off);
  memcpy(out, r->p + r->off, n);
  r->off += n;
  return ok();
}

static jello_bc_result read_insn_canonical(rd* r, jello_insn* out) {
  jello_bc_result rr;
  uint8_t op, a, b, c;
  uint32_t imm;
  if((rr = rd_u8(r, &op)).err) return rr;
  if((rr = rd_u8(r, &a)).err) return rr;
  if((rr = rd_u8(r, &b)).err) return rr;
  if((rr = rd_u8(r, &c)).err) return rr;
  if((rr = rd_u32(r, &imm)).err) return rr;
  out->op = op; out->a = a; out->b = b; out->c = c; out->imm = imm;
  return ok();
}

static jello_bc_result read_insn_compressed(rd* r, jello_insn* out) {
  jello_bc_result rr;
  uint8_t tag, op, a, b, c;
  if((rr = rd_u8(r, &tag)).err) return rr;
  if(tag != 0 && tag != 1) return err(JELLO_BC_BAD_FORMAT, "bad compressed insn tag", r->off - 1);
  if((rr = rd_u8(r, &op)).err) return rr;
  if((rr = rd_u8(r, &a)).err) return rr;
  if((rr = rd_u8(r, &b)).err) return rr;
  if((rr = rd_u8(r, &c)).err) return rr;
  uint32_t imm = 0;
  if(tag == 0) { if((rr = rd_u32(r, &imm)).err) return rr; }
  out->op = op; out->a = a; out->b = b; out->c = c; out->imm = imm;
  return ok();
}

void jello_bc_free(jello_bc_module* m) {
  if(!m) return;
  free((void*)m->types);
  if(m->sigs) {
    for(uint32_t i = 0; i < m->nsigs; i++) free((void*)m->sigs[i].args);
    free((void*)m->sigs);
  }
  if(m->struct_sigs) {
    for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
      free((void*)m->struct_sigs[i].field_type);
      free((void*)m->struct_sigs[i].field_name_atom);
    }
    free((void*)m->struct_sigs);
  }
  free((void*)m->atoms_data);
  free((void*)m->atoms);
  free((void*)m->const_i64);
  free((void*)m->const_f64);
  free((void*)m->const_bytes_len);
  free((void*)m->const_bytes_off);
  free((void*)m->const_bytes_data);
  if(m->funcs) {
    for(uint32_t i = 0; i < m->nfuncs; i++) {
      free((void*)m->funcs[i].reg_types);
      free((void*)m->funcs[i].insns);
    }
    free((void*)m->funcs);
  }
  free(m);
}

uint32_t jello_bc_get_entry(const jello_bc_module* m) {
  return m ? m->entry : 0u;
}

jello_bc_result jello_bc_read(const uint8_t* data, size_t size, jello_bc_module** out) {
  if(!out) return err(JELLO_BC_BAD_FORMAT, "out is NULL", 0);
  *out = (jello_bc_module*)calloc(1, sizeof(jello_bc_module));
  if(!*out) return err(JELLO_BC_OUT_OF_MEMORY, "oom module", 0);
  jello_bc_module* m = *out;

  rd r = { data, size, 0 };
  jello_bc_result rr;

  uint32_t magic = 0;
  if((rr = rd_u32(&r, &magic)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if(magic != JELLO_JLO_MAGIC) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_MAGIC, "bad magic", r.off - 4); }

  if((rr = rd_u32(&r, &m->version)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if(m->version != JELLO_JLO_VERSION) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_UNSUPPORTED_VERSION, "unsupported version", r.off - 4); }

  if((rr = rd_u32(&r, &m->features)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if((rr = rd_u32(&r, &m->ntypes)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if((rr = rd_u32(&r, &m->nsigs)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if((rr = rd_u32(&r, &m->nstruct_sigs)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if((rr = rd_u32(&r, &m->natoms)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if((rr = rd_u32(&r, &m->nfuncs)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if((rr = rd_u32(&r, &m->entry)).err) { jello_bc_free(m); *out = NULL; return rr; }
  if(m->entry >= m->nfuncs) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "entry out of range", r.off - 4); }

  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    if((rr = rd_u32(&r, &m->nconst_i64)).err) { jello_bc_free(m); *out = NULL; return rr; }
    if((rr = rd_u32(&r, &m->nconst_f64)).err) { jello_bc_free(m); *out = NULL; return rr; }
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_CONSTBYTES) {
    if((rr = rd_u32(&r, &m->nconst_bytes)).err) { jello_bc_free(m); *out = NULL; return rr; }
  }

  // --- type table
  if(m->ntypes > 0) {
    jello_type_entry* types = (jello_type_entry*)calloc(m->ntypes, sizeof(jello_type_entry));
    if(!types) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom types", r.off); }
    m->types = types;
    for(uint32_t i = 0; i < m->ntypes; i++) {
      uint8_t kind_u8 = 0, pad1 = 0;
      uint16_t pad2 = 0;
      uint32_t p0 = 0, p1 = 0;
      if((rr = rd_u8(&r, &kind_u8)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u8(&r, &pad1)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u16(&r, &pad2)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u32(&r, &p0)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u32(&r, &p1)).err) { jello_bc_free(m); *out = NULL; return rr; }
      (void)pad1; (void)pad2; (void)p1;
      if(!jello_bc_type_kind_is_valid(kind_u8)) {
        jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "invalid type kind", r.off - 12);
      }
      types[i].kind = (jello_type_kind)kind_u8;
      switch(types[i].kind) {
        case JELLO_T_LIST:
        case JELLO_T_ARRAY:
          if(p0 >= m->ntypes) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "elem type out of range", r.off - 8); }
          types[i].as.unary.elem = (jello_type_id)p0;
          break;
        case JELLO_T_FUNCTION:
          if(p0 >= m->nsigs) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "sig id out of range", r.off - 8); }
          types[i].as.fun.sig_id = p0;
          break;
        case JELLO_T_STRUCT:
          if(p0 >= m->nstruct_sigs) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "struct sig id out of range", r.off - 8); }
          types[i].as.strct.struct_sig_id = p0;
          break;
        case JELLO_T_ABSTRACT:
          types[i].as.abs.abstract_id = p0; /* raw identifier, no table to be in range against */
          break;
        default:
          break;
      }
    }
  }

  // --- signature table
  if(m->nsigs > 0) {
    jello_fun_sig* sigs = (jello_fun_sig*)calloc(m->nsigs, sizeof(jello_fun_sig));
    if(!sigs) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom sigs", r.off); }
    m->sigs = sigs;
    for(uint32_t i = 0; i < m->nsigs; i++) {
      uint32_t ret = 0;
      uint16_t nargs = 0;
      if((rr = rd_u32(&r, &ret)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if(ret >= m->ntypes) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "sig ret type out of range", r.off - 4); }
      if((rr = rd_u16(&r, &nargs)).err) { jello_bc_free(m); *out = NULL; return rr; }
      sigs[i].ret = (jello_type_id)ret;
      sigs[i].nargs = nargs;
      if(nargs > 0) {
        jello_type_id* args = (jello_type_id*)calloc(nargs, sizeof(jello_type_id));
        if(!args) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom sig args", r.off); }
        sigs[i].args = args;
        for(uint16_t j = 0; j < nargs; j++) {
          uint32_t at = 0;
          if((rr = rd_u32(&r, &at)).err) { jello_bc_free(m); *out = NULL; return rr; }
          if(at >= m->ntypes) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "sig arg type out of range", r.off - 4); }
          ((jello_type_id*)args)[j] = (jello_type_id)at;
        }
      }
    }
  }

  // --- struct-layout table (new, no reference precedent)
  if(m->nstruct_sigs > 0) {
    jello_struct_sig* sigs = (jello_struct_sig*)calloc(m->nstruct_sigs, sizeof(jello_struct_sig));
    if(!sigs) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom struct_sigs", r.off); }
    m->struct_sigs = sigs;
    for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
      uint16_t nfields = 0;
      uint8_t named = 0, is_mutable = 0;
      if((rr = rd_u16(&r, &nfields)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u8(&r, &named)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u8(&r, &is_mutable)).err) { jello_bc_free(m); *out = NULL; return rr; }
      sigs[i].nfields = nfields;
      sigs[i].named = named;
      sigs[i].is_mutable = is_mutable;
      if(nfields > 0) {
        uint32_t* field_type = (uint32_t*)calloc(nfields, sizeof(uint32_t));
        if(!field_type) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom struct field_type", r.off); }
        sigs[i].field_type = field_type;
        for(uint16_t j = 0; j < nfields; j++) {
          uint32_t ft = 0;
          if((rr = rd_u32(&r, &ft)).err) { jello_bc_free(m); *out = NULL; return rr; }
          if(ft >= m->ntypes) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "struct field type out of range", r.off - 4); }
          ((uint32_t*)field_type)[j] = ft;
        }
        if(named) {
          uint32_t* field_name_atom = (uint32_t*)calloc(nfields, sizeof(uint32_t));
          if(!field_name_atom) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom struct field_name_atom", r.off); }
          sigs[i].field_name_atom = field_name_atom;
          for(uint16_t j = 0; j < nfields; j++) {
            uint32_t fa = 0;
            if((rr = rd_u32(&r, &fa)).err) { jello_bc_free(m); *out = NULL; return rr; }
            if(fa >= m->natoms) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "struct field name atom out of range", r.off - 4); }
            for(uint16_t k = 0; k < j; k++) {
              if(((uint32_t*)field_name_atom)[k] == fa) {
                jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "duplicate struct field name", r.off - 4);
              }
            }
            ((uint32_t*)field_name_atom)[j] = fa;
          }
        }
      }
    }
  }

  // --- atoms
  if(m->natoms > 0) {
    uint32_t* atom_offsets = (uint32_t*)calloc(m->natoms, sizeof(uint32_t));
    if(!atom_offsets) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom atom offsets", r.off); }
    size_t cap = 256, used = 0;
    char* blob = (char*)malloc(cap);
    if(!blob) { free(atom_offsets); jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom atoms_data", r.off); }
    for(uint32_t i = 0; i < m->natoms; i++) {
      uint32_t len = 0;
      if((rr = rd_u32(&r, &len)).err) { free(blob); free(atom_offsets); jello_bc_free(m); *out = NULL; return rr; }
      size_t need = (size_t)len + 1u;
      if(used + need > cap) {
        while(used + need > cap) cap *= 2;
        char* grown = (char*)realloc(blob, cap);
        if(!grown) { free(blob); free(atom_offsets); jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom atoms_data grow", r.off); }
        blob = grown;
      }
      if((rr = rd_bytes(&r, (uint8_t*)(blob + used), len)).err) { free(blob); free(atom_offsets); jello_bc_free(m); *out = NULL; return rr; }
      blob[used + len] = '\0';
      atom_offsets[i] = (uint32_t)used;
      used += need;
    }
    m->atoms_data = blob;
    const char** atoms = (const char**)calloc(m->natoms, sizeof(const char*));
    if(!atoms) { free(atom_offsets); jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom atoms", r.off); }
    for(uint32_t i = 0; i < m->natoms; i++) atoms[i] = m->atoms_data + atom_offsets[i];
    m->atoms = atoms;
    free(atom_offsets);
  }

  // --- constant pools
  if(m->nconst_i64 > 0) {
    int64_t* pool = (int64_t*)calloc(m->nconst_i64, sizeof(int64_t));
    if(!pool) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom const_i64", r.off); }
    m->const_i64 = pool;
    for(uint32_t i = 0; i < m->nconst_i64; i++) {
      uint32_t lo = 0, hi = 0;
      if((rr = rd_u32(&r, &lo)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u32(&r, &hi)).err) { jello_bc_free(m); *out = NULL; return rr; }
      ((int64_t*)pool)[i] = (int64_t)(((uint64_t)hi << 32) | (uint64_t)lo);
    }
  }
  if(m->nconst_f64 > 0) {
    double* pool = (double*)calloc(m->nconst_f64, sizeof(double));
    if(!pool) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom const_f64", r.off); }
    m->const_f64 = pool;
    for(uint32_t i = 0; i < m->nconst_f64; i++) {
      uint32_t lo = 0, hi = 0;
      if((rr = rd_u32(&r, &lo)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u32(&r, &hi)).err) { jello_bc_free(m); *out = NULL; return rr; }
      uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
      memcpy(&((double*)pool)[i], &bits, sizeof(double));
    }
  }
  if(m->nconst_bytes > 0) {
    uint32_t* lens = (uint32_t*)calloc(m->nconst_bytes, sizeof(uint32_t));
    uint32_t* offs = (uint32_t*)calloc(m->nconst_bytes, sizeof(uint32_t));
    if(!lens || !offs) { free(lens); free(offs); jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom const_bytes index", r.off); }
    m->const_bytes_len = lens;
    m->const_bytes_off = offs;
    size_t cap = 256, used = 0;
    uint8_t* bytes_data = (uint8_t*)malloc(cap);
    if(!bytes_data) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom const_bytes data", r.off); }
    for(uint32_t i = 0; i < m->nconst_bytes; i++) {
      uint32_t len = 0;
      if((rr = rd_u32(&r, &len)).err) { free(bytes_data); jello_bc_free(m); *out = NULL; return rr; }
      if(used + len > cap) {
        while(used + len > cap) cap *= 2;
        uint8_t* grown = (uint8_t*)realloc(bytes_data, cap);
        if(!grown) { free(bytes_data); jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom const_bytes grow", r.off); }
        bytes_data = grown;
      }
      if((rr = rd_bytes(&r, bytes_data + used, len)).err) { free(bytes_data); jello_bc_free(m); *out = NULL; return rr; }
      ((uint32_t*)lens)[i] = len;
      ((uint32_t*)offs)[i] = (uint32_t)used;
      used += len;
    }
    m->const_bytes_data = bytes_data;
  }

  // --- functions
  if(m->nfuncs > 0) {
    jello_bc_function* funcs = (jello_bc_function*)calloc(m->nfuncs, sizeof(jello_bc_function));
    if(!funcs) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom funcs", r.off); }
    m->funcs = funcs;
    for(uint32_t i = 0; i < m->nfuncs; i++) {
      uint32_t nregs = 0, cap_start = 0, ninsns = 0;
      if((rr = rd_u32(&r, &nregs)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if(nregs > 256) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "nregs > 256", r.off - 4); }
      if((rr = rd_u32(&r, &cap_start)).err) { jello_bc_free(m); *out = NULL; return rr; }
      if((rr = rd_u32(&r, &ninsns)).err) { jello_bc_free(m); *out = NULL; return rr; }

      jello_type_id* reg_types = NULL;
      if(nregs > 0) {
        reg_types = (jello_type_id*)calloc(nregs, sizeof(jello_type_id));
        if(!reg_types) { jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom reg_types", r.off); }
        for(uint32_t j = 0; j < nregs; j++) {
          uint32_t tid = 0;
          if((rr = rd_u32(&r, &tid)).err) { free(reg_types); jello_bc_free(m); *out = NULL; return rr; }
          if(tid >= m->ntypes) { free(reg_types); jello_bc_free(m); *out = NULL; return err(JELLO_BC_BAD_FORMAT, "reg type out of range", r.off - 4); }
          ((jello_type_id*)reg_types)[j] = (jello_type_id)tid;
        }
      }

      jello_insn* insns = NULL;
      if(ninsns > 0) {
        insns = (jello_insn*)calloc(ninsns, sizeof(jello_insn));
        if(!insns) { free(reg_types); jello_bc_free(m); *out = NULL; return err(JELLO_BC_OUT_OF_MEMORY, "oom insns", r.off); }
        int compressed = (m->features & (uint32_t)JELLO_BC_FEAT_COMPRESSED_INSN) != 0;
        for(uint32_t j = 0; j < ninsns; j++) {
          rr = compressed ? read_insn_compressed(&r, &((jello_insn*)insns)[j])
                           : read_insn_canonical(&r, &((jello_insn*)insns)[j]);
          if(rr.err) { free(insns); free(reg_types); jello_bc_free(m); *out = NULL; return rr; }
          if(!jello_bc_op_is_valid(insns[j].op)) {
            free(insns); free(reg_types); jello_bc_free(m); *out = NULL;
            return err(JELLO_BC_BAD_FORMAT, "invalid opcode", r.off);
          }
        }
      }

      funcs[i].nregs = nregs;
      funcs[i].cap_start = cap_start;
      funcs[i].reg_types = reg_types;
      funcs[i].ninsns = ninsns;
      funcs[i].insns = insns;
    }

    for(uint32_t i = 0; i < m->nfuncs; i++) {
      const jello_bc_function* f = &funcs[i];
      for(uint32_t j = 0; j < f->ninsns; j++) {
        jello_bc_result vr = jello_bc_validate_insn(m, f->reg_types, &f->insns[j], f->nregs, j, f->ninsns, m->nfuncs);
        if(vr.err) { jello_bc_free(m); *out = NULL; return vr; }
      }

      if(f->ninsns == 0 ||
         (f->insns[f->ninsns - 1u].op != (uint8_t)JOP_RET && f->insns[f->ninsns - 1u].op != (uint8_t)JOP_JMP)) {
        jello_bc_free(m);
        *out = NULL;
        return err(JELLO_BC_BAD_FORMAT, "function does not end in a terminator (RET or JMP)", r.off);
      }
    }
  }

  return ok();
}
