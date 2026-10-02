#include "user.h"

void main(void) {
    int id = pipe_open("demo", 1);
    if(id < 0) {printf("producer: no free pipe\n"); return;}
    char b[] = "message 0\n";
    uint32_t t = gettime();
    int k = 1;
    while(k <= 6) {
        if(gettime() - t >= TICKS_PER_SEC / 5) {
            b[8] = '0' + k;
            pipe_write(id, b, sizeof(b) - 1);
            printf("[producer] sent message %d\n", k);
            k++;
            t = gettime();
        }
        yield();
    }
    pipe_close(id);
    printf("[producer] done, pipe closed\n");
}
