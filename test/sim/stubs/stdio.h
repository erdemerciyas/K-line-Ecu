#pragma once
#include <stdarg.h>
#include <stddef.h>
typedef struct _iobuf FILE;
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
FILE *fopen(const char *path, const char *mode);
size_t fwrite(const void *p, size_t s, size_t n, FILE *f);
int fclose(FILE *f);
int fflush(FILE *f);
