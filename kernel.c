#include "kernel.h"

extern char __bss[], __bss_end[], __stack_top[]; //__bss alone would mean value of 0th byte of .bss section. To get start address of .bss section, we add the [] at the end

struct sbi_ret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a0") = arg1;
    register long a2 __asm__("a0") = arg2;
    register long a3 __asm__("a0") = arg3;
    register long a4 __asm__("a0") = arg4;
    register long a5 __asm__("a0") = arg5;
    register long a6 __asm__("a0") = fid;
    register long a7 __asm__("a0") = eid;
}
