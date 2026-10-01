#include "user.h"

void main(void) {
    printf("crash: writing to kernel memory at %x ...\n", 0x80200000);
    *(volatile int *) 0x80200000 = 1;
    printf("crash: this line should never print\n");
}
