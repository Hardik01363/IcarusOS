#include "common.h"

void putchar(char c);

void printf(const char* fmtd_str, ...) {
    va_list vargs;
    va_start(vargs ,fmtd_str);

    while(*fmtd_str) {
        if(*fmtd_str == '%') {}
        else {putchar(*fmtd_str);}
        fmtd_str++;
    }

end:
    va_end(vargs);
}
