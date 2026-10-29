// The same shape app/build.py generates (app/dist/webapp_files.h).
#pragma once
#include <stddef.h>
typedef struct {
  const char *path;          // "/index.html"; serve "/" as "/index.html"
  const char *content_type;
  const unsigned char *data; // gzip
  size_t len;
} webapp_file_t;
extern const webapp_file_t webapp_files[];
extern const size_t webapp_files_count;
