#include "common.h"

void put_char(char c);

void printf(const char* fmtd_str, ...) {
    va_list vargs;
    va_start(vargs, fmtd_str);

    //putchar() is being used to send char's to stdout 1 char at a time
    while(*fmtd_str) {
        if(*fmtd_str == '%') {
            fmtd_str++; //skipping '%' char
            switch(*fmtd_str) { //applying cases to char after '%'
                case '\0': {//means the sensed '%'  was at the end of a formatted string
                    put_char('%');
                    goto end;
                }
                case '%': {//just print a '%'
                    put_char('%');
                    break;
                }
                case 's': {
                    const char *s = va_arg(vargs, const char *);
                    while(*s) {put_char(*s); s++;}
                    break;
                }
                case 'd': {
                    int value = va_arg(vargs, int);
                    unsigned mag = value; //mag ~= magnitude

                    //putting '-' sign for negative int
                    if(value < 0) {put_char('-'); mag = -mag;}

                    unsigned div = 1;
                    while(mag/div > 9) {div *= 10;}

                    while(div > 0) {
                        put_char('0' + mag/div);
                        mag %= div; div /= 10;
                    }
                    break;
                }
                case 'x': {
                    unsigned value = va_arg(vargs, unsigned);
                    for(int i = 7; i >= 0; i--) {
                        //like bit for binary, digit for decimal, similarly nibble for hexadecimal
                        unsigned nibble = (value >> (i*4)) & 0xf;
                        put_char("0123456789abcdef"[nibble]);
                    }
                }
            }
        }
        else {put_char(*fmtd_str);}
        fmtd_str++;
    }

end:
    va_end(vargs);
}
