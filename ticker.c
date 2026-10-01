#include "user.h"

void main(void) {
    int me = getpid();
    uint32_t t = gettime();
    int k = 1;
    while(k <= 5) {
        if(gettime() - t >= TICKS_PER_SEC / 3) {
            printf("[pid %d] tick %d\n", me, k);
            k++;
            t = gettime();
        }
        yield();
    }
}
