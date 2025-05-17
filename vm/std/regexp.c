
#define REGEXP_MAX_PATTERN_LEN 8192u
#define REGEXP_FLAGS "the flags are i, m, s, x and U"

typedef struct regexp {
  jdl_refs refs;
  pcre2_code* code;
  uint8_t* text; /* the pattern, then the flags */
  uint32_t pattern_len;
  uint32_t flags_len;
  uint32_t name_count; /* entries of name_size bytes in the name table, names */
  uint32_t name_size;
  PCRE2_SPTR names;
} regexp;

typedef struct regexp_match {
  jdl_refs refs;
  uint32_t name_count;
  uint32_t name_size;
} regexp_match;

static void regexp_free(void* payload) {
  if(!jdl_release(payload)) return;
  regexp* re = (regexp*)payload;
  pcre2_code_free(re->code);
  free(re->text);
  free(re);
}

typedef struct regexp_text {
  jdl_refs refs;
  uint32_t len;
  uint8_t data[];
} regexp_text;

static const jello_jdll_handle_class REGEXP = {"regexp", regexp_free, jdl_retain};

static uint32_t options(const uint8_t* flags, uint32_t len, uint8_t* bad) {
  uint32_t opts = PCRE2_UTF;
  for(uint32_t i = 0; i < len; i++) {
    switch(flags[i]) {
      case 'i': opts |= PCRE2_CASELESS; break;
      case 'm': opts |= PCRE2_MULTILINE; break;
      case 's': opts |= PCRE2_DOTALL; break;
      case 'x': opts |= PCRE2_EXTENDED; break;
      case 'U': opts |= PCRE2_UNGREEDY; break;
      default: *bad = flags[i]; return 0;
    }
  }
  return opts;
}

static pcre2_code* compile(const uint8_t* pattern, uint32_t pattern_len, const uint8_t* flags,
                           uint32_t flags_len, char* message, size_t size) {
  if(pattern_len > REGEXP_MAX_PATTERN_LEN) {
    snprintf(message, size, "regexp pattern is longer than %u bytes", REGEXP_MAX_PATTERN_LEN);
    return NULL;
  }
  uint8_t bad = 0;
  uint32_t opts = options(flags, flags_len, &bad);
  if(!opts) {
    if(bad > ' ' && bad < 0x7f)
      snprintf(message, size, "unknown regexp flag %c; " REGEXP_FLAGS, bad);
    else snprintf(message, size, "unknown regexp flag \\x%02X; " REGEXP_FLAGS, bad);
    return NULL;
  }
  int error = 0;
  PCRE2_SIZE error_offset = 0;
  pcre2_code* code = pcre2_compile(pattern_len ? (PCRE2_SPTR)pattern : (PCRE2_SPTR) "",
                                   (PCRE2_SIZE)pattern_len, opts, &error, &error_offset, NULL);
  if(!code) {
    char reason[160];
    pcre2_get_error_message(error, (PCRE2_UCHAR*)reason, sizeof reason);
    snprintf(message, size, "invalid regexp pattern: %s, at byte %u", reason,
             (unsigned)error_offset);
  }
  return code;
}

void std_regexp_compile(jdlo_ctx* c) {
  const uint8_t* pattern = jdl_arg_bytes_data(c, 0);
  uint32_t pattern_len = jdl_arg_bytes_len(c, 0);
  const uint8_t* flags = jdl_arg_bytes_data(c, 1);
  uint32_t flags_len = jdl_arg_bytes_len(c, 1);
  pcre2_code* code = compile(pattern, pattern_len, flags, flags_len, NULL, 0);
  // Null lets std.core.RegExp.compile ask regexp_check why, compiling twice only then.
  if(!code) {
    jdl_return_null(c);
    return;
  }
  regexp* re = (regexp*)calloc(1, sizeof(regexp));
  uint8_t* text = (uint8_t*)malloc((size_t)pattern_len + flags_len + 1u);
  if(!re || !text) {
    free(re);
    free(text);
    pcre2_code_free(code);
    jdl_fail(c, "out of memory");
    return;
  }
  if(pattern_len) memcpy(text, pattern, pattern_len);
  if(flags_len) memcpy(text + pattern_len, flags, flags_len);
  re->refs = 1;
  re->code = code;
  re->text = text;
  re->pattern_len = pattern_len;
  re->flags_len = flags_len;
  pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &re->name_count);
  pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &re->name_size);
  pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &re->names);
  size_t size = 0;
  pcre2_pattern_info(code, PCRE2_INFO_SIZE, &size);
  jdl_return_handle(c, &REGEXP, re, (uint32_t)(size + pattern_len + flags_len));
}