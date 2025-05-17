
static int c_string(jdlo_ctx* c, int index, char* out, uint32_t cap) {
  const uint8_t* data = jdl_arg_bytes_data(c, index);
  uint32_t len = jdl_arg_bytes_len(c, index);
  if(len == 0 || len >= cap || memchr(data, 0, len)) return 0;
  memcpy(out, data, len);
  out[len] = '\0';
  return 1;
}

static FILE* std_stream(const char* path) {
  if(strcmp(path, "/dev/stdout") == 0) return stdout;
  if(strcmp(path, "/dev/stderr") == 0) return stderr;
  return NULL;
}

static int null_device(const char* path) { return strcmp(path, "/dev/null") == 0; }

static FILE* std_open(const char* path, const char* mode) {
#ifdef _WIN32
  if(null_device(path)) path = "NUL";
#endif
  return fopen(path, mode);
}

void std_file_exists(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  struct stat st;
  int ok = c_string(c, 0, path, sizeof path) &&
           (std_stream(path) || null_device(path) || stat(path, &st) == 0);
  jdl_return_bool(c, ok);
}

static int read_file(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  FILE* f = c_string(c, 0, path, sizeof path) ? std_open(path, "rb") : NULL;
  if(!f) return 0;
  uint8_t* data = NULL;
  long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
  int ok = size >= 0 && (unsigned long)size <= STD_MAX_FILE && fseek(f, 0, SEEK_SET) == 0;
  if(ok && size > 0) {
    data = (uint8_t*)malloc((size_t)size);
    ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
  }
  fclose(f);
  if(ok) jdl_return_bytes_copy(c, data, (uint32_t)size);
  free(data);
  return ok;
}

void std_file_read(jdlo_ctx* c) {
  if(!read_file(c)) jdl_fail(c, "cannot read file");
}

void std_file_write(jdlo_ctx* c) {
  char path[STD_MAX_PATH];
  const uint8_t* body = jdl_arg_bytes_data(c, 1);
  uint32_t len = jdl_arg_bytes_len(c, 1);
  if(!c_string(c, 0, path, sizeof path)) {
    jdl_return_bool(c, 0);
    return;
  }
  FILE* stream = std_stream(path);
  if(stream) {
    jdl_return_bool(c, (len == 0 || fwrite(body, 1, len, stream) == len) && fflush(stream) == 0);
    return;
  }
  FILE* f = std_open(path, "wb");
  if(!f) {
    jdl_return_bool(c, 0);
    return;
  }
  int ok = len == 0 || fwrite(body, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  jdl_return_bool(c, ok);
}

void std_get_env(jdlo_ctx* c) {
  char name[STD_MAX_PATH];
  const char* value = c_string(c, 0, name, sizeof name) ? getenv(name) : NULL;
  jdl_return_bytes_copy(c, (const uint8_t*)value, value ? (uint32_t)strlen(value) : 0u);
}

void std_argv_count(jdlo_ctx* c) {
#if defined(__APPLE__)
  jdl_return_i32(c, *_NSGetArgc());
#elif defined(_WIN32)
  jdl_return_i32(c, __argc);
#else
  int32_t count = 0;
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if(f) {
    int ch;
    while((ch = fgetc(f)) != EOF) count += ch == 0;
    fclose(f);
  }
  jdl_return_i32(c, count);
#endif
}