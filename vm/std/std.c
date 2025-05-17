
#define B (uint8_t)JELLO_T_BOOL
#define I (uint8_t)JELLO_T_I32
#define Y (uint8_t)JELLO_T_BYTES
#define D (uint8_t)JELLO_T_DYNAMIC
#define W JELLO_JDLL_BLOCKING

static const jello_jdll_export STD_EXPORTS[] = {
    {"regexp_compile", std_regexp_compile, D, 2, {Y, Y}, 0},
    {"file_exists", std_file_exists, B, 1, {Y}, W},
    {"file_read", std_file_read, Y, 1, {Y}, W},
    {"file_write", std_file_write, B, 2, {Y, Y}, W},
    {"get_env", std_get_env, Y, 1, {Y}, 0},
    {"argv_count", std_argv_count, I, 0, {0}, 0},
};

JELLO_JDLL_LIBRARY(std, STD_EXPORTS);