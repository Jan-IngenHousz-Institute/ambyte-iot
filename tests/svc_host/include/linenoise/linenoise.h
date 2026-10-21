#pragma once
#include <unistd.h>
typedef ssize_t (*linenoise_read_bytes_fn)(int,void*,size_t);
void linenoiseSetReadFunction(linenoise_read_bytes_fn function);
