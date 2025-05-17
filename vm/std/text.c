
typedef struct text_buffer {
  uint8_t* data;
  size_t len, cap;
  int failed;
} text_buffer;