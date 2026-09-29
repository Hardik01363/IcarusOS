#include "user.h"

extern char __stack_top[];

//no need to clear the .bss section by filling it with 0's as the kernel already guarantees that when we use the palloc() function

__attribute__((noreturn)) void exit(void) {
    for(;;);
}

void put_char(char ch) {
    //will implement later (i promise)
}

__attribute__((section(".text.start")))
__attribute__((naked))
void start(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top] \n"
        "call main           \n"
        "call exit           \n"
        :: [stack_top] "r" (__stack_top)
    );
}
