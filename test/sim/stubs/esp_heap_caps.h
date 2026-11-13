#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define heap_caps_malloc(n, caps)       malloc(n)
#define heap_caps_calloc(n, s, caps)    calloc(n, s)
#define heap_caps_free(p)               free(p)
