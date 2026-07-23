typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t size_t;

extern char __bss[], __bss_end[], __stack_top[]; //__bss alone would mean value of 0th byte of .bss section. To get start address of .bss section, we add the [] at the end

void* memset(void* buff, char c, size_t n) {
    uint8_t* p = (uint8_t*)buff;
    while(n--) {*p++ = c;}
    return buff;
}

void kernel_main(void) {
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss); //.bss section initialised to 0. Some bootloders may recognise and 0-clear the .bss section, but, we do it manually too just in case the bootloader doesnt.
    for(;;);
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
