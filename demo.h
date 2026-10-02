#ifdef DEMO_TOUR

void demo_wait(void) {
    printf("\n\033[2m[ press any key for the section of the demo ]\033[0m\n");
    while(get_char() < 0) {;}
}

void demo_title(int n, const char *t) {
    printf("\n\033[1;33m=== feature %d: %s ===\033[0m\n", n, t);
}

void demo_flags(uint32_t pte) {
    printf("%c%c%c%c%c", (pte & PAGE_V) ? 'V' : '-', (pte & PAGE_R) ? 'R' : '-', (pte & PAGE_W) ? 'W' : '-', (pte & PAGE_X) ? 'X' : '-', (pte & PAGE_U) ? 'U' : '-');
}

void demo_satp(uint32_t v) {
    __asm__ __volatile__("sfence.vma\ncsrw satp, %0\nsfence.vma" :: "r"(v));
}

struct process *demo_start(const char *name) {
    struct prog prog;
    if(!find_prog(name, &prog)) {PANIC("demo: no program called %s", name);}
    return create_proc(name, prog.start, prog.size);
}

void demo_files(const char *dir) {
    char nm[100];
    for(int i = 0; ; i++) {
        int r = fs_readdir(dir, i, nm);
        if(r < 0) {break;}
        if(nm[strlen(nm) - 1] == '/') {printf("  %s  <dir>\n", nm);}
        else {printf("  %s  (%d bytes)\n", nm, r);}
    }
}

void demo_tour(void) {
    printf("\n\033[1;33m*** IcarusOS demo tour ***\033[0m\n");
    demo_wait();

    demo_title(1, "a hand-made memory map");
    printf("kernel image : %x - %x\n", (uint32_t) __kernel_base, (uint32_t) __bss_end);
    printf("boot stack   : up to %x\n", (uint32_t) __stack_top);
    printf("free ram     : %x - %x (%d pages)\n", (uint32_t) __free_ram_start, (uint32_t) __free_ram_end, pg_total);
    printf("virtio-blk   : %x, rtc: %x (device registers, not ram)\n", VIRTIO_BLK_PADDR, RTC_PADDR);
    demo_wait();

    demo_title(2, "bitmap page allocator");
    int f0 = free_pages();
    paddr_t a = palloc(1);
    paddr_t b = palloc(1);
    paddr_t c = palloc(3);
    printf("palloc(1) -> %x\npalloc(1) -> %x\npalloc(3) -> %x\n", a, b, c);
    uint32_t *w = (uint32_t *) a;
    int z = 1;
    for(int i = 0; i < PAGE_SIZE / 4; i++) {if(w[i]) {z = 0;}}
    printf("page %x is all zeroes: %s\n", a, z ? "yes" : "no");
    printf("free pages: %d -> %d\n", f0, free_pages());
    pfree(b, 1);
    paddr_t d = palloc(1);
    printf("pfree(%x), then palloc(1) -> %x (%s)\n", b, d, d == b ? "the same page came straight back" : "a different page");
    pfree(a, 1);
    pfree(d, 1);
    pfree(c, 3);
    printf("everything freed: %d free pages (started with %d)\n", free_pages(), f0);
    demo_wait();

    demo_title(3, "memory comes back when a process dies");
    int f1 = free_pages();
    struct process *sp = demo_start("spinner");
    int f2 = free_pages();
    printf("spawned %s (pid %d): free pages %d -> %d, it owns %d pages\n", sp->name, sp->pid, f1, f2, proc_pages(sp));
    sp->state = PROC_EXITED;
    reap(sp);
    printf("it exited and was reaped: free pages %d -> %d (%s)\n", f2, free_pages(), free_pages() == f1 ? "every page came back" : "pages leaked!");
    demo_wait();

    demo_title(4, "Sv32 paging, live");
    uint32_t *pt = new_pt();
    paddr_t pg = palloc(1);
    map_page(pt, 0x4000000, pg, PAGE_R | PAGE_W);
    map_page(pt, 0x5000000, pg, PAGE_R);
    *(volatile uint32_t *) pg = 0xc0ffee;
    uint32_t p1 = pt_walk(pt, 0x4000000);
    uint32_t p2 = pt_walk(pt, 0x5000000);
    uint32_t pk = pt_walk(pt, (vaddr_t) __kernel_base);
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
    pt_free(pt);
    pfree(pg, 1);
    demo_wait();

    demo_title(5, "protection: the U bit and the guard page");
    printf("a program that stores into kernel memory:\n");
    demo_start("crash");
    yield();
    struct prog rp;
    find_prog("recurse", &rp);
    vaddr_t gp = USER_BASE + rp.size - USER_STACK_SIZE - PAGE_SIZE;
    printf("\na program that overflows its stack (its guard page is %x - %x):\n", gp, gp + PAGE_SIZE - 1);
    demo_start("recurse");
    yield();
    printf("\nboth died alone. the kernel is still here\n");
    demo_wait();

    demo_title(6, "syscall pointer validation");
    printf("a program that hands the kernel addresses it does not own:\n");
    demo_start("evil");
    yield();
    demo_wait();

    demo_title(7, "disk driver + tar filesystem");
    printf("virtio-blk capacity: %d sectors (%d bytes)\n", (int) (blk_capacity / SECTOR_SIZE), (int) blk_capacity);
    struct tar_header *h = (struct tar_header *) disk;
    printf("first tar header on disk: name=%s magic=%s\n", h->name, h->magic);
    printf("files in memory:\n");
    demo_files("");
    printf("writing demo.txt through the driver...\n");
    fs_write("demo.txt", "written by the demo tour\n", 25);
    memset(files, 0, sizeof(files));
    memset(disk, 0, sizeof(disk));
    int n = fs_init();
    printf("wiped the ram copy, re-read the disk: %d entries\n", n);
    demo_files("");
    demo_wait();

    demo_title(8, "directories and paths");
    const char *cw[] = {"/", "/docs", "/docs/deep", "/docs"};
    const char *pa[] = {"a/b", "../x.txt", "../../..", "/etc//./hosts"};
    char out[PATH_MAX + 1];
    for(int i = 0; i < 4; i++) {
        path_resolve(cw[i], pa[i], out, sizeof(out));
        printf("cwd %s + %s -> %s\n", cw[i], pa[i], out);
    }
    printf("\ncreating demo-dir/ with a file inside:\n");
    fs_create("demo-dir", true);
    fs_write("demo-dir/note.txt", "inside a directory\n", 19);
    printf("/\n");
    demo_files("");
    printf("/demo-dir\n");
    demo_files("demo-dir");
    int r1x = fs_remove("demo-dir");
    printf("rm demo-dir (not empty) -> %d\n", r1x);
    int r2x = fs_remove("demo-dir/note.txt");
    int r3x = fs_remove("demo-dir");
    printf("rm demo-dir/note.txt -> %d, then rm demo-dir -> %d\n", r2x, r3x);
    demo_wait();

    demo_title(9, "user mode, syscalls, two processes at once");
    printf("starting two ticker programs; every printf char is an ecall\n\n");
    demo_start("ticker");
    demo_start("ticker");
    yield();
    printf("\nboth finished, the scheduler fell back to idle and returned here\n");
    demo_wait();

    demo_title(10, "pipes: a producer and a consumer");
    printf("the consumer starts first and blocks on an empty pipe\n\n");
    demo_start("consumer");
    demo_start("producer");
    yield();
    printf("\nthe pipe was closed, so the consumer saw end-of-file and exited\n");
    demo_wait();

    demo_title(11, "clocks");
    printf("time counter : %d ticks at 10 MHz\n", (uint32_t) rdtime());
    printf("uptime       : %d s\n", (uint32_t) (rdtime() >> 7) / (TICKS_PER_SEC / 128));
    printf("rtc          : %d seconds since 1970-01-01\n", rtc_seconds());
    printf("(the shell's date command turns that into a calendar date)\n");
    demo_wait();

    demo_title(12, "process table");
    for(int i = 0; i < PROCS_MAX; i++) {
        if(procs[i].state == PROC_UNUSED) {continue;}
        printf("slot %d: pid=%d  %s  pages=%d  %s\n", i, procs[i].pid, procs[i].state == PROC_EXITED ? "exited" : "ready ", proc_pages(&procs[i]), procs[i].name);
    }
    printf("free pages: %d of %d (exited processes were reaped, so nothing leaked)\n", free_pages(), pg_total);
    printf("\n\033[1;33m*** tour done, starting the shell ***\033[0m\n");
    demo_wait();
}

#endif
