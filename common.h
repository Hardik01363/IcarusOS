#pragma once

//__builtin_'s provided by clang compiler
#define va_list __builtin_va_list
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg

void printf(const char* frmtd_str, ...);
