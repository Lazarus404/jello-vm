// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

#include <jello/internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* -- jlo_tool -- */

#define JLO_MAGIC 0x4A4C5942u
#define JLO_VERSION 1u

/* -- name tables -- */
typedef struct kind_name_entry {
  const char* name;
  jello_type_kind kind;
} kind_name_entry;

static const kind_name_entry KIND_NAMES[] = {
  {"VOID", JELLO_T_VOID},
  {"BOOL", JELLO_T_BOOL},
  {"ATOM", JELLO_T_ATOM},
  {"I8", JELLO_T_I8},
  {"I16", JELLO_T_I16},
  {"I32", JELLO_T_I32},
  {"I64", JELLO_T_I64},
  {"F16", JELLO_T_F16},
  {"F32", JELLO_T_F32},
  {"F64", JELLO_T_F64},
  {"BYTES", JELLO_T_BYTES},
  {"STRUCT", JELLO_T_STRUCT},
  {"OBJECT", JELLO_T_OBJECT},
  {"DYNAMIC", JELLO_T_DYNAMIC},
  {"ENUM", JELLO_T_ENUM},
};
#define NKIND_NAMES (sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]))

typedef struct op_name_entry {
  const char* name;
  jello_op op;
} op_name_entry;

static const op_name_entry OP_NAMES[] = {
  {"NOP", JOP_NOP}, {"RET", JOP_RET}, {"JMP", JOP_JMP}, {"JMP_IF", JOP_JMP_IF},
  {"MOV", JOP_MOV}, {"TRY", JOP_TRY}, {"ENDTRY", JOP_ENDTRY}, {"THROW", JOP_THROW},
  {"ASSERT", JOP_ASSERT}, {"SWITCH_KIND", JOP_SWITCH_KIND}, {"CASE_KIND", JOP_CASE_KIND},
  {"CALL", JOP_CALL}, {"CALLR", JOP_CALLR}, {"TAILCALL", JOP_TAILCALL},
  {"TAILCALLR", JOP_TAILCALLR}, {"CONST_FUN", JOP_CONST_FUN}, {"CLOSURE", JOP_CLOSURE},
  {"BIND_THIS", JOP_BIND_THIS},
  {"CONST_I8_IMM", JOP_CONST_I8_IMM}, {"CONST_I32", JOP_CONST_I32}, {"CONST_I64", JOP_CONST_I64},
  {"CONST_F16", JOP_CONST_F16}, {"CONST_F32", JOP_CONST_F32}, {"CONST_F64", JOP_CONST_F64},
  {"CONST_BOOL", JOP_CONST_BOOL}, {"CONST_NULL", JOP_CONST_NULL}, {"CONST_ATOM", JOP_CONST_ATOM},
  {"CONST_BYTES", JOP_CONST_BYTES},
  {"ADD_I32", JOP_ADD_I32}, {"SUB_I32", JOP_SUB_I32}, {"MUL_I32", JOP_MUL_I32},
  {"DIV_I32", JOP_DIV_I32}, {"MOD_I32", JOP_MOD_I32}, {"ADD_I32_IMM", JOP_ADD_I32_IMM},
  {"SUB_I32_IMM", JOP_SUB_I32_IMM}, {"MUL_I32_IMM", JOP_MUL_I32_IMM}, {"NEG_I32", JOP_NEG_I32},
  {"SHL_I32", JOP_SHL_I32}, {"SHR_I32", JOP_SHR_I32}, {"BITAND_I32", JOP_BITAND_I32},
  {"BITOR_I32", JOP_BITOR_I32}, {"BITXOR_I32", JOP_BITXOR_I32},
  {"ADD_I64", JOP_ADD_I64}, {"SUB_I64", JOP_SUB_I64}, {"MUL_I64", JOP_MUL_I64},
  {"DIV_I64", JOP_DIV_I64}, {"MOD_I64", JOP_MOD_I64}, {"NEG_I64", JOP_NEG_I64},
  {"SHL_I64", JOP_SHL_I64}, {"SHR_I64", JOP_SHR_I64}, {"BITAND_I64", JOP_BITAND_I64},
  {"BITOR_I64", JOP_BITOR_I64}, {"BITXOR_I64", JOP_BITXOR_I64},
  {"ADD_F16", JOP_ADD_F16}, {"SUB_F16", JOP_SUB_F16}, {"MUL_F16", JOP_MUL_F16},
  {"ADD_F32", JOP_ADD_F32}, {"SUB_F32", JOP_SUB_F32}, {"MUL_F32", JOP_MUL_F32},
  {"DIV_F32", JOP_DIV_F32}, {"NEG_F32", JOP_NEG_F32}, {"ADD_F64", JOP_ADD_F64},
  {"SUB_F64", JOP_SUB_F64}, {"MUL_F64", JOP_MUL_F64}, {"DIV_F64", JOP_DIV_F64},
  {"NEG_F64", JOP_NEG_F64}, {"NOT_BOOL", JOP_NOT_BOOL},
  {"EQ_I32", JOP_EQ_I32}, {"LT_I32", JOP_LT_I32}, {"EQ_I32_IMM", JOP_EQ_I32_IMM},
  {"LT_I32_IMM", JOP_LT_I32_IMM}, {"EQ_I64", JOP_EQ_I64}, {"LT_I64", JOP_LT_I64},
  {"EQ_F32", JOP_EQ_F32}, {"LT_F32", JOP_LT_F32}, {"EQ_F64", JOP_EQ_F64}, {"LT_F64", JOP_LT_F64},
  {"SEXT_I64", JOP_SEXT_I64}, {"SEXT_I16", JOP_SEXT_I16}, {"TRUNC_I8", JOP_TRUNC_I8},
  {"TRUNC_I16", JOP_TRUNC_I16}, {"I32_FROM_I64", JOP_I32_FROM_I64},
  {"F64_FROM_I32", JOP_F64_FROM_I32}, {"I32_FROM_F64", JOP_I32_FROM_F64},
  {"F64_FROM_I64", JOP_F64_FROM_I64}, {"I64_FROM_F64", JOP_I64_FROM_F64},
  {"F32_FROM_I32", JOP_F32_FROM_I32}, {"I32_FROM_F32", JOP_I32_FROM_F32},
  {"F64_FROM_F32", JOP_F64_FROM_F32}, {"F32_FROM_F64", JOP_F32_FROM_F64},
  {"F32_FROM_I64", JOP_F32_FROM_I64}, {"I64_FROM_F32", JOP_I64_FROM_F32},
  {"F16_FROM_F32", JOP_F16_FROM_F32}, {"F32_FROM_F16", JOP_F32_FROM_F16},
  {"F16_FROM_I32", JOP_F16_FROM_I32}, {"I32_FROM_F16", JOP_I32_FROM_F16},
  {"TO_DYN", JOP_TO_DYN}, {"FROM_DYN_I8", JOP_FROM_DYN_I8}, {"FROM_DYN_I16", JOP_FROM_DYN_I16},
  {"FROM_DYN_I32", JOP_FROM_DYN_I32}, {"FROM_DYN_I64", JOP_FROM_DYN_I64},
  {"FROM_DYN_F16", JOP_FROM_DYN_F16}, {"FROM_DYN_F32", JOP_FROM_DYN_F32},
  {"FROM_DYN_F64", JOP_FROM_DYN_F64}, {"FROM_DYN_BOOL", JOP_FROM_DYN_BOOL},
  {"FROM_DYN_ATOM", JOP_FROM_DYN_ATOM}, {"FROM_DYN_PTR", JOP_FROM_DYN_PTR},
  {"SPILL_PUSH", JOP_SPILL_PUSH}, {"SPILL_POP", JOP_SPILL_POP},
  {"PHYSEQ", JOP_PHYSEQ}, {"KINDOF", JOP_KINDOF},
  {"LIST_NIL", JOP_LIST_NIL}, {"LIST_CONS", JOP_LIST_CONS}, {"LIST_HEAD", JOP_LIST_HEAD},
  {"LIST_TAIL", JOP_LIST_TAIL}, {"LIST_IS_NIL", JOP_LIST_IS_NIL}, {"ARRAY_NEW", JOP_ARRAY_NEW},
  {"ARRAY_LEN", JOP_ARRAY_LEN}, {"ARRAY_GET", JOP_ARRAY_GET}, {"ARRAY_SET", JOP_ARRAY_SET},
  {"ARRAY_CONCAT2", JOP_ARRAY_CONCAT2}, {"ARRAY_RESIZE", JOP_ARRAY_RESIZE},
  {"BYTES_NEW", JOP_BYTES_NEW}, {"BYTES_LEN", JOP_BYTES_LEN}, {"BYTES_GET_U8", JOP_BYTES_GET_U8},
  {"BYTES_SET_U8", JOP_BYTES_SET_U8}, {"BYTES_CONCAT2", JOP_BYTES_CONCAT2},
  {"BYTES_CONCAT_MANY", JOP_BYTES_CONCAT_MANY}, {"BYTES_EQ", JOP_BYTES_EQ},
  {"BYTES_BITAND2", JOP_BYTES_BITAND2}, {"BYTES_BITOR2", JOP_BYTES_BITOR2},
  {"BYTES_BITXOR2", JOP_BYTES_BITXOR2},
  {"BYTES_READ_U16_LE", JOP_BYTES_READ_U16_LE}, {"BYTES_READ_U16_BE", JOP_BYTES_READ_U16_BE},
  {"BYTES_READ_U32_LE", JOP_BYTES_READ_U32_LE}, {"BYTES_READ_U32_BE", JOP_BYTES_READ_U32_BE},
  {"BYTES_READ_I32_LE", JOP_BYTES_READ_I32_LE}, {"BYTES_READ_I32_BE", JOP_BYTES_READ_I32_BE},
  {"BYTES_READ_F32_LE", JOP_BYTES_READ_F32_LE}, {"BYTES_READ_F32_BE", JOP_BYTES_READ_F32_BE},
  {"BYTES_WRITE_U16_LE", JOP_BYTES_WRITE_U16_LE}, {"BYTES_WRITE_U16_BE", JOP_BYTES_WRITE_U16_BE},
  {"BYTES_WRITE_U32_LE", JOP_BYTES_WRITE_U32_LE}, {"BYTES_WRITE_U32_BE", JOP_BYTES_WRITE_U32_BE},
  {"BYTES_WRITE_I32_LE", JOP_BYTES_WRITE_I32_LE}, {"BYTES_WRITE_I32_BE", JOP_BYTES_WRITE_I32_BE},
  {"BYTES_WRITE_F32_LE", JOP_BYTES_WRITE_F32_LE}, {"BYTES_WRITE_F32_BE", JOP_BYTES_WRITE_F32_BE},
  {"BITS_WRITE", JOP_BITS_WRITE}, {"BITS_READ", JOP_BITS_READ},
  {"STRUCT_GET", JOP_STRUCT_GET}, {"STRUCT_SET", JOP_STRUCT_SET},
  {"OBJ_NEW", JOP_OBJ_NEW}, {"OBJ_HAS_ATOM", JOP_OBJ_HAS_ATOM}, {"OBJ_GET_ATOM", JOP_OBJ_GET_ATOM},
  {"OBJ_SET_ATOM", JOP_OBJ_SET_ATOM}, {"OBJ_GET", JOP_OBJ_GET}, {"OBJ_SET", JOP_OBJ_SET},
  {"OBJ_HAS", JOP_OBJ_HAS}, {"OBJ_REMOVE", JOP_OBJ_REMOVE}, {"OBJ_CLEAR", JOP_OBJ_CLEAR},
  {"OBJ_COPY", JOP_OBJ_COPY}, {"OBJ_KEYS", JOP_OBJ_KEYS},
  {"ENUM_NEW", JOP_ENUM_NEW}, {"ENUM_TAG", JOP_ENUM_TAG}, {"ENUM_GET", JOP_ENUM_GET},
};
#define NOP_NAMES (sizeof(OP_NAMES) / sizeof(OP_NAMES[0]))

typedef struct flag_name_entry {
  const char* name;
  jello_bc_feature_flags bit;
  int supported; /* 1 if this chapter's tool can actually emit/consume the section */
} flag_name_entry;

static const flag_name_entry FLAG_NAMES[] = {
  {"CONST64", JELLO_BC_FEAT_CONST64, 1},
  {"CONSTBYTES", JELLO_BC_FEAT_CONSTBYTES, 1},
  {"CAP_START", JELLO_BC_FEAT_CAP_START, 1},
  {"LINE_TABLE", JELLO_BC_FEAT_LINE_TABLE, 0},
  {"LINE_COL", JELLO_BC_FEAT_LINE_COL, 0},
  {"LINE_DELTA", JELLO_BC_FEAT_LINE_DELTA, 0},
  {"COMPRESSED_INSN", JELLO_BC_FEAT_COMPRESSED_INSN, 1},
  {"LOOP_ANCHOR", JELLO_BC_FEAT_LOOP_ANCHOR, 0},
  {"TRAP_MAP", JELLO_BC_FEAT_TRAP_MAP, 0},
  {"HANDLER_MAP", JELLO_BC_FEAT_HANDLER_MAP, 0},
  {"TYPE_PROFILE", JELLO_BC_FEAT_TYPE_PROFILE, 0},
  {"STACK_MAP", JELLO_BC_FEAT_STACK_MAP, 0},
  {"PKG_DEPS", JELLO_BC_FEAT_PKG_DEPS, 0},
};
#define NFLAG_NAMES (sizeof(FLAG_NAMES) / sizeof(FLAG_NAMES[0]))

static const char* kind_name_of(jello_type_kind k) {
  for(size_t i = 0; i < NKIND_NAMES; i++) if(KIND_NAMES[i].kind == k) return KIND_NAMES[i].name;
  return NULL;
}
static int kind_from_name(const char* name, jello_type_kind* out) {
  for(size_t i = 0; i < NKIND_NAMES; i++) {
    if(strcmp(KIND_NAMES[i].name, name) == 0) { *out = KIND_NAMES[i].kind; return 1; }
  }
  return 0;
}
static const char* op_name_of(jello_op op) {
  for(size_t i = 0; i < NOP_NAMES; i++) if(OP_NAMES[i].op == op) return OP_NAMES[i].name;
  return NULL;
}
static int op_from_name(const char* name, jello_op* out) {
  for(size_t i = 0; i < NOP_NAMES; i++) {
    if(strcmp(OP_NAMES[i].name, name) == 0) { *out = OP_NAMES[i].op; return 1; }
  }
  return 0;
}
static const flag_name_entry* flag_entry_from_name(const char* name) {
  for(size_t i = 0; i < NFLAG_NAMES; i++) if(strcmp(FLAG_NAMES[i].name, name) == 0) return &FLAG_NAMES[i];
  return NULL;
}
/* -- in-memory module (this chapter's own scope for the `asm` writer only -- */

typedef struct asm_type_entry {
  jello_type_kind kind;
  uint32_t p0; /* STRUCT: struct_sigs index. Every other supported kind: unused. */
} asm_type_entry;

typedef struct asm_struct_sig {
  uint16_t nfields;
  uint8_t named;
  uint8_t is_mutable;
  uint32_t* field_type;       /* type-table indices */
  uint32_t* field_name_atom;  /* atom indices, only when named */
} asm_struct_sig;

typedef struct func_t {
  uint32_t nregs;
  uint32_t cap_start;
  uint32_t* reg_type_idx;
  uint32_t ninsns;
  jello_insn* insns;
} func_t;

typedef struct module_t {
  uint32_t version;
  uint32_t features;
  uint32_t entry;
  uint32_t ntypes;
  asm_type_entry* types;
  uint32_t natoms;
  char** atoms;
  uint32_t nstruct_sigs;
  asm_struct_sig* struct_sigs;
  uint32_t nconst_i64;
  int64_t* const_i64;
  uint32_t nconst_f64;
  double* const_f64;
  uint32_t nfuncs;
  func_t* funcs;
} module_t;

static void module_free(module_t* m) {
  if(!m) return;
  free(m->types);
  for(uint32_t i = 0; i < m->natoms; i++) free(m->atoms[i]);
  free(m->atoms);
  for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
    free(m->struct_sigs[i].field_type);
    free(m->struct_sigs[i].field_name_atom);
  }
  free(m->struct_sigs);
  free(m->const_i64);
  free(m->const_f64);
  for(uint32_t i = 0; i < m->nfuncs; i++) {
    free(m->funcs[i].reg_type_idx);
    free(m->funcs[i].insns);
  }
  free(m->funcs);
  memset(m, 0, sizeof(*m));
}

/* -- text grammar: tokenizer + parser -- */

typedef struct cursor {
  const char* buf;
  size_t len;
  size_t pos;
  const char* filename;
} cursor;

static void skip_ws(cursor* c) {
  while(c->pos < c->len) {
    char ch = c->buf[c->pos];
    if(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { c->pos++; continue; }
    break;
  }
}

static int perr(cursor* c, const char* msg) {
  size_t line = 1, col = 1;
  for(size_t i = 0; i < c->pos && i < c->len; i++) {
    if(c->buf[i] == '\n') { line++; col = 1; } else { col++; }
  }
  fprintf(stderr, "%s:%zu:%zu: parse error: %s\n", c->filename, line, col, msg);
  return 0;
}

static int match_ch(cursor* c, char ch) {
  skip_ws(c);
  if(c->pos < c->len && c->buf[c->pos] == ch) { c->pos++; return 1; }
  return 0;
}

static int expect_ch(cursor* c, char ch) {
  if(match_ch(c, ch)) return 1;
  char msg[64];
  snprintf(msg, sizeof(msg), "expected '%c'", ch);
  return perr(c, msg);
}

static int read_ident(cursor* c, char* out, size_t outsz) {
  skip_ws(c);
  size_t start = c->pos;
  while(c->pos < c->len) {
    char ch = c->buf[c->pos];
    if((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') {
      c->pos++;
    } else break;
  }
  size_t n = c->pos - start;
  if(n == 0 || n >= outsz) return perr(c, "expected identifier");
  memcpy(out, c->buf + start, n);
  out[n] = '\0';
  return 1;
}

static int match_ident(cursor* c, const char* literal) {
  size_t save = c->pos;
  char buf[64];
  if(!read_ident(c, buf, sizeof(buf))) { c->pos = save; return 0; }
  if(strcmp(buf, literal) != 0) { c->pos = save; return 0; }
  return 1;
}

static int expect_ident(cursor* c, const char* literal) {
  if(match_ident(c, literal)) return 1;
  char msg[80];
  snprintf(msg, sizeof(msg), "expected '%s'", literal);
  return perr(c, msg);
}

static int read_uint(cursor* c, uint32_t* out) {
  skip_ws(c);
  size_t start = c->pos;
  while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  if(c->pos == start) return perr(c, "expected an unsigned integer");
  char buf[16];
  size_t n = c->pos - start;
  if(n >= sizeof(buf)) return perr(c, "integer literal too long");
  memcpy(buf, c->buf + start, n);
  buf[n] = '\0';
  *out = (uint32_t)strtoul(buf, NULL, 10);
  return 1;
}

static int read_int(cursor* c, int32_t* out) {
  skip_ws(c);
  int neg = 0;
  if(c->pos < c->len && c->buf[c->pos] == '-') { neg = 1; c->pos++; }
  uint32_t mag = 0;
  if(!read_uint(c, &mag)) return 0;
  uint32_t bits = neg ? (0u - mag) : mag;
  memcpy(out, &bits, sizeof bits);
  return 1;
}

static int read_int64(cursor* c, int64_t* out) {
  skip_ws(c);
  size_t start = c->pos;
  if(c->pos < c->len && c->buf[c->pos] == '-') c->pos++;
  size_t digits_start = c->pos;
  while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  if(c->pos == digits_start) return perr(c, "expected a 64-bit integer");
  char buf[32];
  size_t n = c->pos - start;
  if(n >= sizeof(buf)) return perr(c, "integer literal too long");
  memcpy(buf, c->buf + start, n);
  buf[n] = '\0';
  *out = (int64_t)strtoll(buf, NULL, 10);
  return 1;
}

static int read_double(cursor* c, double* out) {
  skip_ws(c);
  size_t start = c->pos;
  if(c->pos < c->len && (c->buf[c->pos] == '-' || c->buf[c->pos] == '+')) c->pos++;
  size_t digits_start = c->pos;
  while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  if(c->pos < c->len && c->buf[c->pos] == '.') {
    c->pos++;
    while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
  }
  if(c->pos < c->len && (c->buf[c->pos] == 'e' || c->buf[c->pos] == 'E')) {
    size_t save = c->pos;
    c->pos++;
    if(c->pos < c->len && (c->buf[c->pos] == '-' || c->buf[c->pos] == '+')) c->pos++;
    size_t exp_digits = c->pos;
    while(c->pos < c->len && c->buf[c->pos] >= '0' && c->buf[c->pos] <= '9') c->pos++;
    if(c->pos == exp_digits) c->pos = save;
  }
  if(c->pos == digits_start) return perr(c, "expected a floating-point literal");
  char buf[64];
  size_t n = c->pos - start;
  if(n >= sizeof(buf)) return perr(c, "float literal too long");
  memcpy(buf, c->buf + start, n);
  buf[n] = '\0';
  *out = strtod(buf, NULL);
  return 1;
}

/* -- module-level parse -- */

static int parse_features(cursor* c, uint32_t* out_features) {
  uint32_t features = 0;
  if(!expect_ch(c, '[')) return 0;
  skip_ws(c);
  if(match_ch(c, ']')) { *out_features = 0; return 1; }
  for(;;) {
    char name[32];
    if(!read_ident(c, name, sizeof(name))) return 0;
    const flag_name_entry* fe = flag_entry_from_name(name);
    if(!fe) { fprintf(stderr, "%s: unknown feature flag '%s'\n", c->filename, name); return 0; }
    if(!fe->supported) {
      fprintf(stderr, "%s: feature flag '%s' has no section producer/consumer in Chapter 2's tool yet\n",
              c->filename, name);
      return 0;
    }
    features |= (uint32_t)fe->bit;
    if(match_ch(c, ',')) continue;
    break;
  }
  if(!expect_ch(c, ']')) return 0;
  *out_features = features;
  return 1;
}

static int parse_type_entry(cursor* c, asm_type_entry* out) {
  char name[32];
  if(!read_ident(c, name, sizeof(name))) return 0;
  jello_type_kind kind;
  if(!kind_from_name(name, &kind)) {
    fprintf(stderr, "%s: unknown or not-yet-supported type kind '%s' "
                    "(LIST/ARRAY/FUNCTION/ABSTRACT are still deferred past Chapter 3)\n",
            c->filename, name);
    return 0;
  }
  out->kind = kind;
  out->p0 = 0;
  if(kind == JELLO_T_STRUCT) {
    if(!expect_ch(c, '(')) return 0;
    if(!read_uint(c, &out->p0)) return 0;
    if(!expect_ch(c, ')')) return 0;
  }
  return 1;
}

static int parse_types(cursor* c, asm_type_entry** out_types, uint32_t* out_n) {
  if(!expect_ident(c, "types")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  asm_type_entry* types = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      asm_type_entry ent;
      if(!parse_type_entry(c, &ent)) { free(types); return 0; }
      if(n == cap) {
        cap = cap ? cap * 2 : 4;
        types = (asm_type_entry*)realloc(types, cap * sizeof(asm_type_entry));
      }
      types[n++] = ent;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) { free(types); return 0; }
  }
  *out_types = types;
  *out_n = n;
  return 1;
}

static int expect_empty_list_section(cursor* c, const char* name) {
  if(!expect_ident(c, name)) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  if(!expect_ch(c, ']')) {
    fprintf(stderr, "%s: '%s:' must be empty in this chapter's tool (no producer/consumer yet)\n",
            c->filename, name);
    return 0;
  }
  return 1;
}

static int parse_atoms(cursor* c, char*** out_atoms, uint32_t* out_n) {
  if(!expect_ident(c, "atoms")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  char** atoms = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[64];
      if(!read_ident(c, name, sizeof(name))) {
        for(uint32_t i = 0; i < n; i++) free(atoms[i]);
        free(atoms);
        return 0;
      }
      if(n == cap) { cap = cap ? cap * 2 : 4; atoms = (char**)realloc(atoms, cap * sizeof(char*)); }
      atoms[n] = (char*)malloc(strlen(name) + 1);
      strcpy(atoms[n], name);
      n++;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) {
      for(uint32_t i = 0; i < n; i++) free(atoms[i]);
      free(atoms);
      return 0;
    }
  }
  *out_atoms = atoms;
  *out_n = n;
  return 1;
}

static int parse_const_i64(cursor* c, int64_t** out_vals, uint32_t* out_n) {
  if(!expect_ident(c, "const_i64")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  int64_t* vals = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      int64_t v;
      if(!read_int64(c, &v)) { free(vals); return 0; }
      if(n == cap) { cap = cap ? cap * 2 : 4; vals = (int64_t*)realloc(vals, cap * sizeof(int64_t)); }
      vals[n++] = v;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) { free(vals); return 0; }
  }
  *out_vals = vals;
  *out_n = n;
  return 1;
}

static int parse_const_f64(cursor* c, double** out_vals, uint32_t* out_n) {
  if(!expect_ident(c, "const_f64")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  double* vals = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      double v;
      if(!read_double(c, &v)) { free(vals); return 0; }
      if(n == cap) { cap = cap ? cap * 2 : 4; vals = (double*)realloc(vals, cap * sizeof(double)); }
      vals[n++] = v;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) { free(vals); return 0; }
  }
  *out_vals = vals;
  *out_n = n;
  return 1;
}

static int find_type_index_by_kind_name(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                                          const char* name, uint32_t* out_idx) {
  jello_type_kind want;
  if(!kind_from_name(name, &want)) {
    fprintf(stderr, "%s: unknown type kind '%s'\n", c->filename, name);
    return 0;
  }
  for(uint32_t i = 0; i < ntypes; i++) {
    if(types[i].kind == want) { *out_idx = i; return 1; }
  }
  fprintf(stderr, "%s: type kind '%s' used but not declared in 'types:'\n", c->filename, name);
  return 0;
}

static int find_atom_index_by_name(cursor* c, char* const* atoms, uint32_t natoms,
                                    const char* name, uint32_t* out_idx) {
  for(uint32_t i = 0; i < natoms; i++) {
    if(strcmp(atoms[i], name) == 0) { *out_idx = i; return 1; }
  }
  fprintf(stderr, "%s: atom '%s' used but not declared in 'atoms:'\n", c->filename, name);
  return 0;
}

static int parse_struct_sig_entry(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                                   char* const* atoms, uint32_t natoms, asm_struct_sig* out) {
  memset(out, 0, sizeof(*out));
  if(!expect_ch(c, '{')) return 0;
  if(!expect_ident(c, "nfields")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t nfields;
  if(!read_uint(c, &nfields)) return 0;
  if(!expect_ident(c, "named")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t named;
  if(!read_uint(c, &named)) return 0;
  if(!expect_ident(c, "mutable")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t is_mutable;
  if(!read_uint(c, &is_mutable)) return 0;
  if(!expect_ident(c, "fields")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!expect_ch(c, '[')) return 0;

  out->nfields = (uint16_t)nfields;
  out->named = (uint8_t)(named ? 1 : 0);
  out->is_mutable = (uint8_t)(is_mutable ? 1 : 0);
  uint32_t* field_type = nfields ? (uint32_t*)calloc(nfields, sizeof(uint32_t)) : NULL;
  uint32_t got = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[32];
      if(!read_ident(c, name, sizeof(name))) { free(field_type); return 0; }
      uint32_t ti;
      if(!find_type_index_by_kind_name(c, types, ntypes, name, &ti)) { free(field_type); return 0; }
      if(got >= nfields) { fprintf(stderr, "%s: struct_sig has more fields than nfields\n", c->filename); free(field_type); return 0; }
      field_type[got++] = ti;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) { free(field_type); return 0; }
  }
  if(got != nfields) {
    fprintf(stderr, "%s: struct_sig declares nfields=%u but fields=[...] has %u entries\n", c->filename, nfields, got);
    free(field_type);
    return 0;
  }
  out->field_type = field_type;

  if(out->named) {
    uint32_t* field_name_atom = NULL;
    int names_ok = expect_ident(c, "names") && expect_ch(c, '=') && expect_ch(c, '[');
    if(names_ok) {
      field_name_atom = nfields ? (uint32_t*)calloc(nfields, sizeof(uint32_t)) : NULL;
      uint32_t ngot = 0;
      skip_ws(c);
      if(!match_ch(c, ']')) {
        for(;;) {
          char name[64];
          uint32_t ai;
          if(!read_ident(c, name, sizeof(name)) || !find_atom_index_by_name(c, atoms, natoms, name, &ai)) {
            names_ok = 0;
            break;
          }
          if(ngot >= nfields) {
            fprintf(stderr, "%s: struct_sig has more names than nfields\n", c->filename);
            names_ok = 0;
            break;
          }
          field_name_atom[ngot++] = ai;
          if(match_ch(c, ',')) continue;
          break;
        }
        if(names_ok && !expect_ch(c, ']')) names_ok = 0;
      }
    }
    if(!names_ok) {
      free(field_name_atom);
      free(field_type);
      out->field_type = NULL;
      return 0;
    }
    out->field_name_atom = field_name_atom;
  }

  if(!expect_ch(c, '}')) return 0;
  return 1;
}

static int parse_struct_sigs(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                              char* const* atoms, uint32_t natoms,
                              asm_struct_sig** out_sigs, uint32_t* out_n) {
  if(!expect_ident(c, "struct_sigs")) return 0;
  if(!expect_ch(c, ':')) return 0;
  if(!expect_ch(c, '[')) return 0;
  asm_struct_sig* sigs = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      asm_struct_sig sig;
      if(!parse_struct_sig_entry(c, types, ntypes, atoms, natoms, &sig)) { free(sigs); return 0; }
      if(n == cap) { cap = cap ? cap * 2 : 4; sigs = (asm_struct_sig*)realloc(sigs, cap * sizeof(asm_struct_sig)); }
      sigs[n++] = sig;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) { free(sigs); return 0; }
  }
  *out_sigs = sigs;
  *out_n = n;
  return 1;
}

static int parse_reg_types(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                            uint32_t** out_idx, uint32_t* out_n) {
  if(!expect_ident(c, "reg_types")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!expect_ch(c, '[')) return 0;
  uint32_t* idx = NULL;
  uint32_t n = 0, cap = 0;
  skip_ws(c);
  if(!match_ch(c, ']')) {
    for(;;) {
      char name[32];
      if(!read_ident(c, name, sizeof(name))) { free(idx); return 0; }
      uint32_t ti;
      if(!find_type_index_by_kind_name(c, types, ntypes, name, &ti)) { free(idx); return 0; }
      if(n == cap) { cap = cap ? cap * 2 : 4; idx = (uint32_t*)realloc(idx, cap * sizeof(uint32_t)); }
      idx[n++] = ti;
      if(match_ch(c, ',')) continue;
      break;
    }
    if(!expect_ch(c, ']')) { free(idx); return 0; }
  }
  *out_idx = idx;
  *out_n = n;
  return 1;
}

static int parse_reg_operand(cursor* c, uint8_t* out) {
  if(!expect_ch(c, '=')) return 0;
  if(!match_ch(c, 'r')) return perr(c, "expected register operand 'rN'");
  uint32_t v;
  if(!read_uint(c, &v)) return 0;
  if(v > 255) return perr(c, "register index exceeds 255 (Chapter 2's frozen ceiling)");
  *out = (uint8_t)v;
  return 1;
}

static int parse_insn(cursor* c, uint32_t expect_pc, jello_insn* out) {
  uint32_t pc_label;
  if(!read_uint(c, &pc_label)) return 0;
  if(pc_label != expect_pc) {
    fprintf(stderr, "%s: PC label %u out of sequence (expected %u, labels must be sequential from 0)\n",
            c->filename, pc_label, expect_pc);
    return 0;
  }
  if(!expect_ch(c, ':')) return 0;
  char opname[32];
  if(!read_ident(c, opname, sizeof(opname))) return 0;
  jello_op op;
  if(!op_from_name(opname, &op)) {
    fprintf(stderr, "%s: unknown opcode '%s'\n", c->filename, opname);
    return 0;
  }
  if(!expect_ident(c, "a")) return 0;
  uint8_t a; if(!parse_reg_operand(c, &a)) return 0;
  if(!expect_ident(c, "b")) return 0;
  uint8_t b; if(!parse_reg_operand(c, &b)) return 0;
  if(!expect_ident(c, "c")) return 0;
  uint8_t cc; if(!parse_reg_operand(c, &cc)) return 0;
  if(!expect_ident(c, "imm")) return 0;
  if(!expect_ch(c, '=')) return 0;
  int32_t imm;
  if(!read_int(c, &imm)) return 0;
  out->op = (uint8_t)op;
  out->a = a;
  out->b = b;
  out->c = cc;
  out->imm = (uint32_t)imm;
  return 1;
}

static int parse_func(cursor* c, const asm_type_entry* types, uint32_t ntypes,
                       uint32_t* out_index, func_t* out_fn) {
  if(!expect_ident(c, "func")) return 0;
  if(!expect_ch(c, '[')) return 0;
  uint32_t idx;
  if(!read_uint(c, &idx)) return 0;
  if(!expect_ch(c, ']')) return 0;
  if(!expect_ident(c, "nregs")) return 0;
  if(!expect_ch(c, '=')) return 0;
  uint32_t nregs;
  if(!read_uint(c, &nregs)) return 0;
  if(nregs > 256) return perr(c, "nregs exceeds the frozen 256-register ceiling");

  uint32_t cap_start = nregs; /* default: no captures */
  size_t save = c->pos;
  if(match_ident(c, "cap_start")) {
    if(!expect_ch(c, '=')) return 0;
    if(!read_uint(c, &cap_start)) return 0;
  } else {
    c->pos = save;
  }

  uint32_t* reg_type_idx = NULL;
  uint32_t nrt = 0;
  if(!parse_reg_types(c, types, ntypes, &reg_type_idx, &nrt)) return 0;
  if(nrt != nregs) {
    fprintf(stderr, "%s: reg_types has %u entries but nregs=%u\n", c->filename, nrt, nregs);
    free(reg_type_idx);
    return 0;
  }

  jello_insn* insns = NULL;
  uint32_t ninsns = 0, cap = 0;
  for(;;) {
    size_t before = c->pos;
    skip_ws(c);
    if(c->pos >= c->len) break;
    size_t probe = c->pos;
    while(probe < c->len && c->buf[probe] >= '0' && c->buf[probe] <= '9') probe++;
    if(probe == c->pos || probe >= c->len || c->buf[probe] != ':') { c->pos = before; break; }

    jello_insn insn;
    if(!parse_insn(c, ninsns, &insn)) { free(reg_type_idx); free(insns); return 0; }
    if(ninsns == cap) { cap = cap ? cap * 2 : 8; insns = (jello_insn*)realloc(insns, cap * sizeof(jello_insn)); }
    insns[ninsns++] = insn;
  }

  out_fn->nregs = nregs;
  out_fn->cap_start = cap_start;
  out_fn->reg_type_idx = reg_type_idx;
  out_fn->ninsns = ninsns;
  out_fn->insns = insns;
  *out_index = idx;
  return 1;
}

static int parse_module(cursor* c, module_t* out) {
  memset(out, 0, sizeof(*out));
  if(!expect_ch(c, '.')) return 0;
  if(!expect_ident(c, "jlo")) return 0;
  if(!expect_ident(c, "module")) return 0;
  if(!expect_ident(c, "version")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!read_uint(c, &out->version)) return 0;
  if(!expect_ident(c, "features")) return 0;
  if(!expect_ch(c, '=')) return 0;
  if(!parse_features(c, &out->features)) return 0;

  out->entry = 0;
  size_t save = c->pos;
  if(match_ident(c, "entry")) {
    if(!expect_ch(c, '=')) return 0;
    if(!read_uint(c, &out->entry)) return 0;
  } else {
    c->pos = save;
  }

  if(!parse_types(c, &out->types, &out->ntypes)) return 0;
  if(!expect_empty_list_section(c, "sigs")) { module_free(out); return 0; }
  if(!parse_atoms(c, &out->atoms, &out->natoms)) { module_free(out); return 0; }
  if(!parse_struct_sigs(c, out->types, out->ntypes, out->atoms, out->natoms,
                         &out->struct_sigs, &out->nstruct_sigs)) {
    module_free(out);
    return 0;
  }
  if(out->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    if(!parse_const_i64(c, &out->const_i64, &out->nconst_i64)) { module_free(out); return 0; }
    if(!parse_const_f64(c, &out->const_f64, &out->nconst_f64)) { module_free(out); return 0; }
  }

  func_t* funcs = NULL;
  uint32_t nfuncs = 0, cap = 0;
  for(;;) {
    skip_ws(c);
    if(c->pos >= c->len) break;
    uint32_t idx;
    func_t fn;
    if(!parse_func(c, out->types, out->ntypes, &idx, &fn)) { module_free(out); free(funcs); return 0; }
    if(idx != nfuncs) {
      fprintf(stderr, "%s: func[%u] out of order; expected func[%u]\n", c->filename, idx, nfuncs);
      module_free(out); free(funcs); free(fn.reg_type_idx); free(fn.insns);
      return 0;
    }
    if(nfuncs == cap) { cap = cap ? cap * 2 : 4; funcs = (func_t*)realloc(funcs, cap * sizeof(func_t)); }
    funcs[nfuncs++] = fn;
  }

  out->nfuncs = nfuncs;
  out->funcs = funcs;

  if(out->entry >= out->nfuncs) {
    fprintf(stderr, "%s: entry=%u out of range (nfuncs=%u)\n", c->filename, out->entry, out->nfuncs);
    module_free(out);
    return 0;
  }
  return 1;
}

/* -- binary writer -- */

typedef struct byte_buf {
  uint8_t* data;
  size_t len;
  size_t cap;
} byte_buf;

static void bb_push(byte_buf* bb, const void* p, size_t n) {
  if(bb->len + n > bb->cap) {
    size_t newcap = bb->cap ? bb->cap * 2 : 256;
    while(newcap < bb->len + n) newcap *= 2;
    bb->data = (uint8_t*)realloc(bb->data, newcap);
    bb->cap = newcap;
  }
  memcpy(bb->data + bb->len, p, n);
  bb->len += n;
}

static void bb_u8(byte_buf* bb, uint8_t v) { bb_push(bb, &v, 1); }
static void bb_u16(byte_buf* bb, uint16_t v) {
  uint8_t le[2] = {(uint8_t)(v), (uint8_t)(v >> 8)};
  bb_push(bb, le, 2);
}
static void bb_u32(byte_buf* bb, uint32_t v) {
  uint8_t le[4] = {(uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  bb_push(bb, le, 4);
}

static void write_insn_canonical(byte_buf* bb, const jello_insn* in) {
  bb_u8(bb, in->op);
  bb_u8(bb, in->a);
  bb_u8(bb, in->b);
  bb_u8(bb, in->c);
  bb_u32(bb, in->imm);
}

static void write_insn_compressed(byte_buf* bb, const jello_insn* in) {
  if(in->imm == 0) {
    bb_u8(bb, 1); /* COMPACT */
    bb_u8(bb, in->op);
    bb_u8(bb, in->a);
    bb_u8(bb, in->b);
    bb_u8(bb, in->c);
  } else {
    bb_u8(bb, 0); /* FULL */
    bb_u8(bb, in->op);
    bb_u8(bb, in->a);
    bb_u8(bb, in->b);
    bb_u8(bb, in->c);
    bb_u32(bb, in->imm);
  }
}

static int write_module(const module_t* m, byte_buf* bb) {
  int compressed = (m->features & (uint32_t)JELLO_BC_FEAT_COMPRESSED_INSN) != 0;

  bb_u32(bb, JLO_MAGIC);
  bb_u32(bb, m->version);
  bb_u32(bb, m->features);
  bb_u32(bb, m->ntypes);
  bb_u32(bb, 0); /* nsigs -- function signatures are still deferred past Chapter 3 */
  bb_u32(bb, m->nstruct_sigs);
  bb_u32(bb, m->natoms);
  bb_u32(bb, m->nfuncs);
  bb_u32(bb, m->entry);

  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    bb_u32(bb, m->nconst_i64);
    bb_u32(bb, m->nconst_f64);
  }
  if(m->features & (uint32_t)JELLO_BC_FEAT_CONSTBYTES) {
    bb_u32(bb, 0); /* nconst_bytes */
  }

  for(uint32_t i = 0; i < m->ntypes; i++) {
    bb_u8(bb, (uint8_t)m->types[i].kind);
    bb_u8(bb, 0);  /* pad1 (u8) */
    bb_u16(bb, 0); /* pad2 (u16) */
    bb_u32(bb, m->types[i].p0);
    bb_u32(bb, 0); /* p1 */
  }

  /* Signature table: empty (nsigs=0 always, this chapter). */
  for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
    const asm_struct_sig* sig = &m->struct_sigs[i];
    bb_u16(bb, sig->nfields);
    bb_u8(bb, sig->named);
    bb_u8(bb, sig->is_mutable);
    for(uint32_t j = 0; j < sig->nfields; j++) bb_u32(bb, sig->field_type[j]);
    if(sig->named) {
      for(uint32_t j = 0; j < sig->nfields; j++) bb_u32(bb, sig->field_name_atom[j]);
    }
  }

  /* Atoms: length-prefixed UTF-8, per the frozen container spec. */
  for(uint32_t i = 0; i < m->natoms; i++) {
    uint32_t len = (uint32_t)strlen(m->atoms[i]);
    bb_u32(bb, len);
    bb_push(bb, m->atoms[i], len);
  }

  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    for(uint32_t i = 0; i < m->nconst_i64; i++) {
      uint64_t bits = (uint64_t)m->const_i64[i];
      bb_u32(bb, (uint32_t)bits);
      bb_u32(bb, (uint32_t)(bits >> 32));
    }
    for(uint32_t i = 0; i < m->nconst_f64; i++) {
      uint64_t bits;
      memcpy(&bits, &m->const_f64[i], sizeof bits);
      bb_u32(bb, (uint32_t)bits);
      bb_u32(bb, (uint32_t)(bits >> 32));
    }
  }

  for(uint32_t f = 0; f < m->nfuncs; f++) {
    const func_t* fn = &m->funcs[f];
    bb_u32(bb, fn->nregs);
    bb_u32(bb, fn->cap_start);
    bb_u32(bb, fn->ninsns);
    for(uint32_t r = 0; r < fn->nregs; r++) bb_u32(bb, fn->reg_type_idx[r]);
    for(uint32_t k = 0; k < fn->ninsns; k++) {
      if(compressed) write_insn_compressed(bb, &fn->insns[k]);
      else write_insn_canonical(bb, &fn->insns[k]);
    }
  }
  return 1;
}

/* -- text printer (disassembler) -- */

static void print_real_module(const jello_bc_module* m, FILE* out) {
  fprintf(out, ".jlo module version=%u features=[", m->version);
  int first = 1;
  for(size_t i = 0; i < NFLAG_NAMES; i++) {
    if(m->features & (uint32_t)FLAG_NAMES[i].bit) {
      if(!first) fprintf(out, ",");
      fprintf(out, "%s", FLAG_NAMES[i].name);
      first = 0;
    }
  }
  fprintf(out, "] entry=%u\n", m->entry);

  fprintf(out, "types: [");
  for(uint32_t i = 0; i < m->ntypes; i++) {
    if(i) fprintf(out, ", ");
    jello_type_kind kind = m->types[i].kind;
    fprintf(out, "%s", kind_name_of(kind));
    if(kind == JELLO_T_STRUCT) fprintf(out, "(%u)", m->types[i].as.strct.struct_sig_id);
  }
  fprintf(out, "]\n");
  fprintf(out, "sigs: []\n");

  fprintf(out, "atoms: [");
  for(uint32_t i = 0; i < m->natoms; i++) {
    if(i) fprintf(out, ", ");
    fprintf(out, "%s", m->atoms[i]);
  }
  fprintf(out, "]\n");

  fprintf(out, "struct_sigs: [");
  for(uint32_t i = 0; i < m->nstruct_sigs; i++) {
    if(i) fprintf(out, ", ");
    const jello_struct_sig* sig = &m->struct_sigs[i];
    fprintf(out, "{ nfields=%u named=%u mutable=%u fields=[", sig->nfields, sig->named, sig->is_mutable);
    for(uint32_t j = 0; j < sig->nfields; j++) {
      if(j) fprintf(out, ", ");
      fprintf(out, "%s", kind_name_of(m->types[sig->field_type[j]].kind));
    }
    fprintf(out, "]");
    if(sig->named) {
      fprintf(out, " names=[");
      for(uint32_t j = 0; j < sig->nfields; j++) {
        if(j) fprintf(out, ", ");
        fprintf(out, "%s", m->atoms[sig->field_name_atom[j]]);
      }
      fprintf(out, "]");
    }
    fprintf(out, " }");
  }
  fprintf(out, "]\n");

  if(m->features & (uint32_t)JELLO_BC_FEAT_CONST64) {
    fprintf(out, "const_i64: [");
    for(uint32_t i = 0; i < m->nconst_i64; i++) {
      if(i) fprintf(out, ", ");
      fprintf(out, "%lld", (long long)m->const_i64[i]);
    }
    fprintf(out, "]\n");
    fprintf(out, "const_f64: [");
    for(uint32_t i = 0; i < m->nconst_f64; i++) {
      if(i) fprintf(out, ", ");
      fprintf(out, "%g", m->const_f64[i]);
    }
    fprintf(out, "]\n");
  }

  for(uint32_t f = 0; f < m->nfuncs; f++) {
    const jello_bc_function* fn = &m->funcs[f];
    fprintf(out, "func[%u] nregs=%u", f, fn->nregs);
    if(fn->cap_start != fn->nregs) fprintf(out, " cap_start=%u", fn->cap_start);
    fprintf(out, " reg_types=[");
    for(uint32_t r = 0; r < fn->nregs; r++) {
      if(r) fprintf(out, ", ");
      jello_type_kind kind = m->types[fn->reg_types[r]].kind;
      fprintf(out, "%s", kind_name_of(kind));
    }
    fprintf(out, "]\n");
    for(uint32_t k = 0; k < fn->ninsns; k++) {
      const jello_insn* in = &fn->insns[k];
      fprintf(out, "  %u: %s a=r%u b=r%u c=r%u imm=%d\n",
              k, op_name_of((jello_op)in->op), in->a, in->b, in->c, (int32_t)in->imm);
    }
  }
}

/* -- file I/O helpers -- */

static char* read_whole_file(const char* path, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if(!f) { fprintf(stderr, "%s: cannot open: %s\n", path, strerror(errno)); return NULL; }
  if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long size = ftell(f);
  if(size < 0) { fclose(f); return NULL; }
  if(fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char* buf = (char*)malloc((size_t)size + 1);
  if(!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)size, f);
  fclose(f);
  buf[n] = '\0';
  *out_len = n;
  return buf;
}

/* -- CLI -- */

static void usage(const char* prog) {
  fprintf(stderr, "usage: %s asm <in.jlo.txt> -o <out.jlo>\n", prog);
  fprintf(stderr, "       %s disasm <in.jlo> [-o <out.jlo.txt>]\n", prog);
}

static int cmd_asm(const char* in_path, const char* out_path) {
  size_t len = 0;
  char* text = read_whole_file(in_path, &len);
  if(!text) return 1;

  cursor c = {text, len, 0, in_path};
  module_t m;
  if(!parse_module(&c, &m)) { free(text); return 1; }
  free(text);

  byte_buf bb = {0};
  write_module(&m, &bb);
  module_free(&m);

  FILE* out = fopen(out_path, "wb");
  if(!out) { fprintf(stderr, "%s: cannot open for writing\n", out_path); free(bb.data); return 1; }
  fwrite(bb.data, 1, bb.len, out);
  fclose(out);
  free(bb.data);
  return 0;
}

static int cmd_disasm(const char* in_path, const char* out_path) {
  FILE* f = fopen(in_path, "rb");
  if(!f) { fprintf(stderr, "%s: cannot open\n", in_path); return 1; }
  if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
  long size = ftell(f);
  if(size < 0) { fclose(f); return 1; }
  fseek(f, 0, SEEK_SET);
  uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
  size_t n = fread(data, 1, (size_t)size, f);
  fclose(f);

  jello_bc_module* m = NULL;
  jello_bc_result rr = jello_bc_read(data, n, &m);
  free(data);
  if(rr.err) {
    fprintf(stderr, "%s: %s (offset %zu)\n", in_path, rr.msg, rr.offset);
    return 1;
  }

  FILE* out = out_path ? fopen(out_path, "w") : stdout;
  if(!out) { fprintf(stderr, "%s: cannot open for writing\n", out_path); jello_bc_free(m); return 1; }
  print_real_module(m, out);
  if(out_path) fclose(out);
  jello_bc_free(m);
  return 0;
}

int main(int argc, char** argv) {
  if(argc < 3) { usage(argv[0]); return 1; }

  const char* subcmd = argv[1];
  const char* in_path = argv[2];
  const char* out_path = NULL;
  for(int i = 3; i < argc; i++) {
    if(strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out_path = argv[i + 1]; i++; }
  }

  if(strcmp(subcmd, "asm") == 0) {
    if(!out_path) { usage(argv[0]); return 1; }
    return cmd_asm(in_path, out_path);
  }
  if(strcmp(subcmd, "disasm") == 0) {
    return cmd_disasm(in_path, out_path);
  }
  usage(argv[0]);
  return 1;
}
