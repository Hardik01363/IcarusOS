#ifdef DEMO_TOUR

void demo_wait(void) {
    printf("\n\033[2m[ press any key for the next step]\033[0m\n");
    while(get_char() < 0) {;}
}

void demo_title(int n, const char *t) {
    printf("\n\033[1;33m=== power %d: %s ===\033[0m\n", n, t);
}

void demo_flags(uint32_t pte) {
    printf("%c%c%c%c%c", (pte & PAGE_V) ? 'V' : '-', (pte & PAGE_R) ? 'R' : '-', (pte & PAGE_W) ? 'W' : '-', (pte & PAGE_X) ? 'X' : '-', (pte & PAGE_U) ? 'U' : '-');
}

uint32_t demo_walk(uint32_t *t1, vaddr_t va) {
    uint32_t e1 = t1[(va >> 22) & 0x3ff];
    if(!(e1 & PAGE_V)) {return 0;}
    uint32_t *t0 = (uint32_t *) ((e1 >> 10) * PAGE_SIZE);
    return t0[(va >> 12) & 0x3ff];
}

void demo_satp(uint32_t v) {
    __asm__ __volatile__("sfence.vma\ncsrw satp, %0\nsfence.vma" :: "r"(v));
}

void demo_files(void) {
    for(int i = 0; i < FILES_MAX_LOADED; i++) {
        if(files[i].in_use) {printf("  %s  (%d bytes)\n", files[i].name, (int) files[i].size);}
    }
}

void demo_tour(void) {
    printf("\n\033[1;33m*** IcarusOS demo tour ***\033[0m\n");
    demo_wait();

    demo_title(1, "a hand-made memory map");
    printf("kernel image : %x - %x\n", (uint32_t) __kernel_base, (uint32_t) __bss_end);
    printf("boot stack   : up to %x\n", (uint32_t) __stack_top);
    printf("free ram     : %x - %x (%d pages)\n", (uint32_t) __free_ram_start, (uint32_t) __free_ram_end, free_pages());
    printf("virtio-blk   : %x (device registers, not ram)\n", VIRTIO_BLK_PADDR);
    demo_wait();

    demo_title(2, "page allocator");
    paddr_t a = palloc(1);
    paddr_t b = palloc(1);
    paddr_t c = palloc(2);
    printf("palloc(1) -> %x\npalloc(1) -> %x\npalloc(2) -> %x\n", a, b, c);
    uint32_t *w = (uint32_t *) a;
    int z = 1;
    for(int i = 0; i < PAGE_SIZE / 4; i++) {if(w[i]) {z = 0;}}
    printf("page %x is all zeroes: %s\n", a, z ? "yes" : "no");
    printf("free pages now: %d (the allocator never gives them back)\n", free_pages());
    demo_wait();

    demo_title(3, "Sv32 paging, live");
    uint32_t *pt = new_pt();
    paddr_t pg = palloc(1);
    map_page(pt, 0x4000000, pg, PAGE_R | PAGE_W);
    map_page(pt, 0x5000000, pg, PAGE_R);
    *(volatile uint32_t *) pg = 0xc0ffee;
    uint32_t p1 = demo_walk(pt, 0x4000000);
    uint32_t p2 = demo_walk(pt, 0x5000000);
    uint32_t pk = demo_walk(pt, (vaddr_t) __kernel_base);
    printf("vaddr 4000000 -> pte %x -> paddr %x  flags ", p1, (p1 >> 10) * PAGE_SIZE); demo_flags(p1); printf("\n");
    printf("vaddr 5000000 -> pte %x -> paddr %x  flags ", p2, (p2 >> 10) * PAGE_SIZE); demo_flags(p2); printf("\n");
    printf("kernel page   -> pte %x -> paddr %x  flags ", pk, (pk >> 10) * PAGE_SIZE); demo_flags(pk); printf("   <- no U bit\n");
    demo_satp(SATP_SV32 | ((uint32_t) pt / PAGE_SIZE));
    uint32_t r1 = *(volatile uint32_t *) 0x4000000;
    *(volatile uint32_t *) 0x4000000 = 0xbeef;
    uint32_t r2 = *(volatile uint32_t *) 0x5000000;
    demo_satp(0);
    printf("MMU on: read via 4000000 = %x, wrote beef, read via 5000000 = %x\n", r1, r2);
    printf("two virtual pages, one physical page: the MMU did the translation\n");
    demo_wait();

    demo_title(4, "disk driver + tar filesystem");
    printf("virtio-blk capacity: %d sectors (%d bytes)\n", (int) (blk_capacity / SECTOR_SIZE), (int) blk_capacity);
    struct tar_header *h = (struct tar_header *) disk;
    printf("first tar header on disk: name=%s magic=%s\n", h->name, h->magic);
    printf("files in memory:\n");
    demo_files();
    printf("writing demo.txt through the driver...\n");
    fs_write("demo.txt", "written by the demo tour\n", 25);
    memset(files, 0, sizeof(files));
    memset(disk, 0, sizeof(disk));
    int n = fs_init();
    printf("wiped ram copy, re-read the disk: %d files\n", n);
    demo_files();
    demo_wait();

    demo_title(5, "user mode, syscalls, two processes at once");
    printf("starting two ticker programs; every printf char is an ecall\n\n");
    create_proc("ticker", _binary_ticker_bin_start, (size_t) _binary_ticker_bin_size);
    create_proc("ticker", _binary_ticker_bin_start, (size_t) _binary_ticker_bin_size);
    yield();
    printf("\nboth finished, scheduler fell back to idle and returned here\n");
    demo_wait();

    demo_title(6, "a crashing program does not take the kernel down");
    create_proc("crash", _binary_crash_bin_start, (size_t) _binary_crash_bin_size);
    yield();
    printf("kernel is still alive: the U bit kept user code out of kernel memory\n");
    demo_wait();

    demo_title(7, "process table");
    for(int i = 0; i < PROCS_MAX; i++) {
        if(procs[i].state == PROC_UNUSED) {continue;}
        printf("slot %d: pid=%d  %s  %s\n", i, procs[i].pid, procs[i].state == PROC_EXITED ? "exited" : "ready ", procs[i].name);
    }
    printf("free pages: %d (exited processes keep their memory)\n", free_pages());
    printf("\n\033[1;33m*** tour done, starting the shell ***\033[0m\n");
    demo_wait();
}

#endif
