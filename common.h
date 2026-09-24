#pragma once

typedef int bool
typedef unsigned char uint8_t
typedef unsigned short uint16_t
typedef unsigned int uint32_t
typedef unsigned long long uint64_t
typedef uint32_t size_t
typedef uint32_t paddr_t
typedef uint32_t vaddr_t

//__builtin_'s provided by clang compiler, defined in C standard library's <stdarg.h>.
//we can use compiler builtins without relying on standard library :)
#define va_list __builtin_va_list
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg

void printf(const char* frmtd_str, ...);
