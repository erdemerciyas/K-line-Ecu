#pragma once
// Bağımsız (freestanding) derleme: libm yerine derleyici yerleşikleri
#define NAN         __builtin_nanf("")
#define INFINITY    __builtin_inff()
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define fabsf(x)    __builtin_fabsf(x)
#define fabs(x)     __builtin_fabs(x)
#define fminf(a, b) __builtin_fminf(a, b)
#define fmaxf(a, b) __builtin_fmaxf(a, b)
#define roundf(x)   __builtin_roundf(x)
#define lroundf(x)  __builtin_lroundf(x)
#define floorf(x)   __builtin_floorf(x)
#define sqrtf(x)    __builtin_sqrtf(x)
