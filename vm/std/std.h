
#define STD_MAX_PATH 4096u
#define STD_MAX_FILE (256u * 1024u * 1024u)

char** std_entries(jdlo_ctx* c, int index, char** store, size_t* count);

int std_temp_dir(char* out, size_t cap);

void std_regexp_compile(jdlo_ctx* c);

void std_file_exists(jdlo_ctx* c);
void std_file_read(jdlo_ctx* c);
void std_file_contents(jdlo_ctx* c);
void std_file_write(jdlo_ctx* c);
void std_file_remove(jdlo_ctx* c);
void std_dir_list(jdlo_ctx* c);
void std_dir_entries(jdlo_ctx* c);
void std_dir_make(jdlo_ctx* c);
void std_file_real_path(jdlo_ctx* c);
void std_process_executable(jdlo_ctx* c);
void std_get_env(jdlo_ctx* c);
void std_argv_count(jdlo_ctx* c);
void std_args_count(jdlo_ctx* c);
void std_args_get(jdlo_ctx* c);
void std_process_exit(jdlo_ctx* c);

void std_command_start(jdlo_ctx* c);
void std_command_wait(jdlo_ctx* c);
void std_command_stdout(jdlo_ctx* c);
void std_command_stderr(jdlo_ctx* c);
void std_command_done(jdlo_ctx* c);
void std_command_next(jdlo_ctx* c);
void std_cpu_count(jdlo_ctx* c);
void std_date_now(jdlo_ctx* c);
void std_date_offset(jdlo_ctx* c);

void std_f64_low_bits(jdlo_ctx* c);
void std_f64_high_bits(jdlo_ctx* c);
void std_f32_bits(jdlo_ctx* c);
void std_f16_bits(jdlo_ctx* c);

void std_float_text(char* text, size_t size, double value, int width);
double std_half_value(uint16_t bits);
void std_float_parse(jdlo_ctx* c);
void std_bytes_hex(jdlo_ctx* c);
void std_bytes_from_hex(jdlo_ctx* c);
void std_bytes_base64(jdlo_ctx* c);
void std_bytes_from_base64(jdlo_ctx* c);