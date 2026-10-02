#include "user.h"

volatile int go = 1;

int dive(int n) {
    volatile char pad[512];
    pad[0] = (char) n;
    if(n % 16 == 0) {printf("recurse: depth %d\n", n);}
    if(go) {return dive(n + 1) + pad[0];}
    return 0;
}

void main(void) {
    printf("recurse: recursing until the stack runs out...\n");
    dive(1);
}
