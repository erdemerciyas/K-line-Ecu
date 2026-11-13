#pragma once
// Bağımsız (freestanding) derleme: basit olanlar derleyici yerleşikleri, gerisi msvcrt.dll'in double sürümleri
#define M_PI        3.14159265358979323846
#define NAN         __builtin_nanf("")
#define INFINITY    __builtin_inff()
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)
#define fabsf(x)    __builtin_fabsf(x)
#define fabs(x)     __builtin_fabs(x)
#define fminf(a, b) __builtin_fminf(a, b)
#define fmaxf(a, b) __builtin_fmaxf(a, b)
#define roundf(x)   __builtin_roundf(x)
#define lroundf(x)  __builtin_lroundf(x)
#define floorf(x)   __builtin_floorf(x)
#define ceilf(x)    __builtin_ceilf(x)
#define truncf(x)   __builtin_truncf(x)
#define sqrtf(x)    __builtin_sqrtf(x)
#define copysignf(a, b) __builtin_copysignf(a, b)
double atan2(double y, double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double exp(double x);
double log(double x);
double pow(double x, double y);
double sqrt(double x);
double fmod(double x, double y);
double floor(double x);
double ceil(double x);
#define atan2f(y, x)    ((float)atan2((y), (x)))
#define sinf(x)         ((float)sin(x))
#define cosf(x)         ((float)cos(x))
#define tanf(x)         ((float)tan(x))
#define asinf(x)        ((float)asin(x))
#define acosf(x)        ((float)acos(x))
#define atanf(x)        ((float)atan(x))
#define expf(x)         ((float)exp(x))
#define logf(x)         ((float)log(x))
#define powf(x, y)      ((float)pow((x), (y)))
#define fmodf(x, y)     ((float)fmod((x), (y)))
#define hypotf(x, y)    sqrtf((x) * (x) + (y) * (y))
