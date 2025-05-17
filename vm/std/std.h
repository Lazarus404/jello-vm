
#define STD_MAX_PATH 4096u
#define STD_MAX_FILE (256u * 1024u * 1024u)

void std_regexp_compile(jdlo_ctx* c);

void std_file_exists(jdlo_ctx* c);
void std_file_read(jdlo_ctx* c);
void std_file_write(jdlo_ctx* c);
void std_get_env(jdlo_ctx* c);
void std_argv_count(jdlo_ctx* c);