// Minimal AEABI polyfill to satisfy link-time references from compiled code.
// Provides __aeabi_f2d which converts float to double.

#include <stdint.h>

double __aeabi_f2d(float x) {
    return (double)x;
}
