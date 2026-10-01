#include "user.h"

void main(void) {
    const char fr[] = "|/-\\";
    int col = SPIN_COL(getpid());
    if(col < 1) {col = 1;}
    uint32_t t = gettime();
    int k = 0;
    while(1) {
        if(gettime() - t >= TICKS_PER_SEC / 10) {
            printf("\0337\033[1;%dH%c\0338", col, fr[k & 3]);
            k++;
            t = gettime();
        }
        yield();
    }
}
