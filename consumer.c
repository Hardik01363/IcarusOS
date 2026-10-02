#include "user.h"

void main(void) {
    int id = pipe_open("demo", 0);
    if(id < 0) {printf("consumer: no free pipe\n"); return;}
    char b[16];
    int bol = 1;
    while(1) {
        int n = pipe_read(id, b, sizeof(b));
        if(n <= 0) {break;}
        for(int i = 0; i < n; i++) {
            if(bol) {printf("[consumer] got: ");}
            put_char(b[i]);
            bol = b[i] == '\n';
        }
    }
    printf("[consumer] end of stream\n");
}
