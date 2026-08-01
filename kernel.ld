ENTRY(boot)

SECTIONS {
    . = 0x80200000; /*base address*/
                    /*'.' represents current address*/
    
    .text :{ /*section containing code of program*/
        KEEP(*(.text.boot));
        *(.text .text.*);
    }

    .rodata : ALIGN(4) { /*read-only data*/
        *(.rodata .rodata.*);
    }

    .data : ALIGN(4) { /*read/write data*/
        *(.data .data.*);
    }

    .bss : ALIGN(4) { /*read/write data with initial value 0*/
        __bss = .;
        *(.bss .bss.* .sbss .sbss.*);
        __bss_end = .;
    }

    . = ALIGN(4); /*aligns current address with a 4-byte boundary*/
    . += 128 * 1024; /*advance current address by 128KB*/
    __stack_top = .;
}
