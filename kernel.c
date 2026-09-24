#include "kernel.h"
#include "common.h"

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t size_t;

extern char __bss[], __bss_end[], __stack_top[]; //__bss alone would mean value of 0th byte of .bss section. To get start address of .bss section, we add the [] at the end

void* memset(void* buff, char c, size_t n) {
    uint8_t* p = (uint8_t*)buff;
    while(n--) {*p++ = c;}
    return buff;
}

//sbi_call implemented accordin to OpenSBI calling convention. SBI can only change values of a0, a1 registers. a2-a7 reg values remain same after the call.
struct sbi_ret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid; //SBI function ID
    register long a7 __asm__("a7") = eid; //SBI extension ID

    __asm__ __volatile__(
        "ecall"
        : "=r"(a0), "=r"(a1)
        : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
        : "memory"
    );

    return (struct sbi_ret){.error = a0, .value = a1};
}

void put_char(char c) {
    sbi_call(c, 0,0,0,0,0,0, 1);
}

void kernel_main(void) {
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss); //.bss section initialised to 0. Some bootloders may recognise and 0-clear the .bss section, but, we do it manually too just in case the bootloader doesnt.
    printf("\n\nBeat the odds. Go Beyond!%s\n");
    for(;;) {__asm__ __volatile("wfi");}
}

__attribute__((section(".text.boot")))
__attribute__((naked))
void boot(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top]\n" //setting the stack pointer
        "j kernel_main\n"
        : //jump conditional protocols to follow
        : [stack_top] "r" (__stack_top) //passing stack_top address as %[stack_top]
    );
}
