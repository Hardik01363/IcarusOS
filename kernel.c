//unimp is the unimplemented instruction (signifies an unimplemented operation) that triggers a trap in our kernel. its a pseudo instruction in RISC-V translating into: csrrw x0, cycle, x0. triggers an exception since cycle is a read-only register but we aretrying to write to it.

//uncomment the next line (or run `DEMO=1 ./run.sh`) to play the demo tour before the shell starts
//#define DEMO_TOUR

#include "kernel.h"
#include "common.h"

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t size_t;

extern char __kernel_base[], __bss[], __bss_end[], __stack_top[], __free_ram_start[], __free_ram_end[]; //__bss alone would mean value of 0th byte of .bss section. To get start address of .bss section, we add the [] at the end
extern char _binary_shell_bin_start[], _binary_shell_bin_size[]; //symbols to use the embedded raw binary in shell.bin.o
extern char _binary_spinner_bin_start[], _binary_spinner_bin_size[];
extern char _binary_ticker_bin_start[], _binary_ticker_bin_size[];
extern char _binary_crash_bin_start[], _binary_crash_bin_size[];
extern char _binary_producer_bin_start[], _binary_producer_bin_size[];
extern char _binary_consumer_bin_start[], _binary_consumer_bin_size[];
extern char _binary_evil_bin_start[], _binary_evil_bin_size[];
extern char _binary_recurse_bin_start[], _binary_recurse_bin_size[];


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

long get_char(void) {
    struct sbi_ret ret = sbi_call(0,0,0,0,0,0,0,2);
    return ret.error;
}

uint64_t rdtime(void) {
    uint32_t hi, lo, hi2;
    do {
        hi = READ_CSR(timeh);
        lo = READ_CSR(time);
        hi2 = READ_CSR(timeh);
    } while(hi != hi2);
    return ((uint64_t) hi << 32) | lo;
}

void sleep_ms(uint32_t ms) {
    uint32_t t = (uint32_t) rdtime();
    while((uint32_t) rdtime() - t < ms * (TICKS_PER_SEC / 1000)) {;}
}

//shift-and-subtract division, because a plain 64-bit `/` would need __udivdi3 from a library we don't link
uint32_t udiv64_32(uint64_t n, uint32_t d) {
    uint64_t r = 0;
    uint32_t q = 0;
    for(int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if(r >= d) {
            r -= d;
            if(i < 32) {q |= 1u << i;}
        }
    }
    return q;
}

//seconds since 1970-01-01 from the goldfish rtc of the qemu virt machine (it counts nanoseconds)
uint32_t rtc_seconds(void) {
    uint32_t lo = *(volatile uint32_t *) RTC_PADDR;
    uint32_t hi = *(volatile uint32_t *) (RTC_PADDR + 4);
    return udiv64_32(((uint64_t) hi << 32) | lo, 1000000000);
}

__attribute__((noreturn)) void halt(void) {
    printf("\n\033[1;33mIcarusOS halted. Goodbye (^o^)/\033[0m\n");
    sbi_call(0, 0, 0, 0, 0, 0, 0, 0x53525354);
    sbi_call(0, 0, 0, 0, 0, 0, 0, 8);
    for(;;) {__asm__ __volatile__("wfi");}
}

//the memory allocator will allocate contiguous memory in 4KB size pages/units. 
//bitmap allocator: one bit per 4KB page of free ram, 1 = in use. palloc() finds the first run of free pages, pfree() gives them back
uint8_t pmap[FREE_RAM_PAGES / 8];
int pg_total;
int pg_free;

void palloc_init(void) {
    pg_total = ((paddr_t) __free_ram_end - (paddr_t) __free_ram_start) / PAGE_SIZE;
    if(pg_total > FREE_RAM_PAGES) {PANIC("free ram is %d pages but FREE_RAM_PAGES is %d", pg_total, FREE_RAM_PAGES);}
    pg_free = pg_total;
}

int free_pages(void) {
    return pg_free;
}

paddr_t palloc(uint32_t n) {
    int run = 0;
    for(int i = 0; i < pg_total; i++) {
        if(pmap[i >> 3] & (1 << (i & 7))) {run = 0; continue;}
        if(++run == (int) n) {
            int first = i - run + 1;
            for(int j = first; j <= i; j++) {pmap[j >> 3] |= 1 << (j & 7);}
            pg_free -= n;
            paddr_t paddr = (paddr_t) __free_ram_start + first * PAGE_SIZE;
            memset((void *) paddr, 0, n * PAGE_SIZE);
            return paddr;
        }
    }
    PANIC("out of memory");
}

void pfree(paddr_t paddr, uint32_t n) {
    if(paddr < (paddr_t) __free_ram_start || paddr + n * PAGE_SIZE > (paddr_t) __free_ram_end || !is_aligned(paddr, PAGE_SIZE)) {
        PANIC("pfree of a bad address: %x", paddr);
    }
    int first = (paddr - (paddr_t) __free_ram_start) / PAGE_SIZE;
    for(int j = first; j < first + (int) n; j++) {
        if(!(pmap[j >> 3] & (1 << (j & 7)))) {PANIC("double free of page %x", (paddr_t) __free_ram_start + j * PAGE_SIZE);}
        pmap[j >> 3] &= ~(1 << (j & 7));
    }
    pg_free += n;
}

//map pages in 2-level page table
void map_page(uint32_t *table1, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    if(!is_aligned(vaddr, PAGE_SIZE)) {PANIC("unaligned vaddr: %x", vaddr);}
    if(!is_aligned(paddr, PAGE_SIZE)) {PANIC("unaligned paddr: %x", paddr);}
    
    uint32_t vpn1 = (vaddr >> 22) & 0x3ff; //0x3ff is the highest value of vpn1 (8 bits by the 2 f's, 2 bits by 3), taking & with it to convert it from 10-bit to 12-bit value
    if((table1[vpn1] & PAGE_V) == 0) {
        //create 1st level page table if it doesnt exist
        uint32_t pt_paddr = palloc(1);
        table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V;
    }

    //setting 2nd level PT entry to map to physical page
    uint32_t vpn0 = (vaddr >> 12) & 0x3ff;
    uint32_t *table0 = (uint32_t *) ((table1[vpn1] >> 10) * PAGE_SIZE);
    table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | flags | PAGE_V;
}

//some ease-of-use functions for accessing MMIO registers of virtio-blk. accessing MMIO registers risky and costly, so, i took these helper implementations from the virtio main documentations
uint32_t virtio_reg_read32(unsigned offset) {
    return *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset));
}

uint64_t virtio_reg_read64(unsigned offset) {
    return *((volatile uint64_t *) (VIRTIO_BLK_PADDR + offset));
}

void virtio_reg_write32(unsigned offset, uint32_t value) {
    *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset)) = value;
}

void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value) {
    virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
}

//entry point of exception handler (to be registered in stvec register)
__attribute__((naked))
__attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        //retrieving kernel stack of running process from sscratch
        "csrrw sp, sscratch, sp\n"

        "addi sp, sp, -4 * 31\n" // allocating space for the trap_frame struct
        "sw ra,  4 * 0(sp)\n"
        "sw gp,  4 * 1(sp)\n"
        "sw tp,  4 * 2(sp)\n"
        "sw t0,  4 * 3(sp)\n"
        "sw t1,  4 * 4(sp)\n"
        "sw t2,  4 * 5(sp)\n"
        "sw t3,  4 * 6(sp)\n"
        "sw t4,  4 * 7(sp)\n"
        "sw t5,  4 * 8(sp)\n"
        "sw t6,  4 * 9(sp)\n"
        "sw a0,  4 * 10(sp)\n"
        "sw a1,  4 * 11(sp)\n"
        "sw a2,  4 * 12(sp)\n"
        "sw a3,  4 * 13(sp)\n"
        "sw a4,  4 * 14(sp)\n"
        "sw a5,  4 * 15(sp)\n"
        "sw a6,  4 * 16(sp)\n"
        "sw a7,  4 * 17(sp)\n"
        "sw s0,  4 * 18(sp)\n"
        "sw s1,  4 * 19(sp)\n"
        "sw s2,  4 * 20(sp)\n"
        "sw s3,  4 * 21(sp)\n"
        "sw s4,  4 * 22(sp)\n"
        "sw s5,  4 * 23(sp)\n"
        "sw s6,  4 * 24(sp)\n"
        "sw s7,  4 * 25(sp)\n"
        "sw s8,  4 * 26(sp)\n"
        "sw s9,  4 * 27(sp)\n"
        "sw s10, 4 * 28(sp)\n"
        "sw s11, 4 * 29(sp)\n"

        //retrieve and save sp when exception occurs
        "csrr a0, sscratch\n"
        "sw a0, 4 * 30(sp)\n"

        //resetting the kernel stack
        "addi a0, sp, 4 * 31\n"
        "csrw sscratch, a0\n"

        "mv a0, sp\n"
        "call handle_trap\n"

        "lw ra,  4 * 0(sp)\n"
        "lw gp,  4 * 1(sp)\n"
        "lw tp,  4 * 2(sp)\n"
        "lw t0,  4 * 3(sp)\n"
        "lw t1,  4 * 4(sp)\n"
        "lw t2,  4 * 5(sp)\n"
        "lw t3,  4 * 6(sp)\n"
        "lw t4,  4 * 7(sp)\n"
        "lw t5,  4 * 8(sp)\n"
        "lw t6,  4 * 9(sp)\n"
        "lw a0,  4 * 10(sp)\n"
        "lw a1,  4 * 11(sp)\n"
        "lw a2,  4 * 12(sp)\n"
        "lw a3,  4 * 13(sp)\n"
        "lw a4,  4 * 14(sp)\n"
        "lw a5,  4 * 15(sp)\n"
        "lw a6,  4 * 16(sp)\n"
        "lw a7,  4 * 17(sp)\n"
        "lw s0,  4 * 18(sp)\n"
        "lw s1,  4 * 19(sp)\n"
        "lw s2,  4 * 20(sp)\n"
        "lw s3,  4 * 21(sp)\n"
        "lw s4,  4 * 22(sp)\n"
        "lw s5,  4 * 23(sp)\n"
        "lw s6,  4 * 24(sp)\n"
        "lw s7,  4 * 25(sp)\n"
        "lw s8,  4 * 26(sp)\n"
        "lw s9,  4 * 27(sp)\n"
        "lw s10, 4 * 28(sp)\n"
        "lw s11, 4 * 29(sp)\n"
        "lw sp,  4 * 30(sp)\n"
        "sret\n"
    );
}

//implementing context switching
//in RISC-V, s0-a11 are callee-saved and others (like a0) are caller-saved
__attribute__((naked)) void switch_context(uint32_t *prev_sp, uint32_t *next_sp) {
    __asm__ __volatile__(
        //save callee-saved registers onto the current process's stack.
        "addi sp, sp, -13 * 4\n" //allocate stack space for 13 4-byte registers
        "sw ra,  0  * 4(sp)\n"   //save callee-saved registers only
        "sw s0,  1  * 4(sp)\n"
        "sw s1,  2  * 4(sp)\n"
        "sw s2,  3  * 4(sp)\n"
        "sw s3,  4  * 4(sp)\n"
        "sw s4,  5  * 4(sp)\n"
        "sw s5,  6  * 4(sp)\n"
        "sw s6,  7  * 4(sp)\n"
        "sw s7,  8  * 4(sp)\n"
        "sw s8,  9  * 4(sp)\n"
        "sw s9,  10 * 4(sp)\n"
        "sw s10, 11 * 4(sp)\n"
        "sw s11, 12 * 4(sp)\n"

        //switch the stack pointer.
        "sw sp, (a0)\n"         //*prev_sp = sp;
        "lw sp, (a1)\n"         //switch stack pointer (sp) here

        //restore callee-saved registers from the next process's stack.
        "lw ra,  0  * 4(sp)\n"  //restore callee-saved registers only
        "lw s0,  1  * 4(sp)\n"
        "lw s1,  2  * 4(sp)\n"
        "lw s2,  3  * 4(sp)\n"
        "lw s3,  4  * 4(sp)\n"
        "lw s4,  5  * 4(sp)\n"
        "lw s5,  6  * 4(sp)\n"
        "lw s6,  7  * 4(sp)\n"
        "lw s7,  8  * 4(sp)\n"
        "lw s8,  9  * 4(sp)\n"
        "lw s9,  10 * 4(sp)\n"
        "lw s10, 11 * 4(sp)\n"
        "lw s11, 12 * 4(sp)\n"
        "addi sp, sp, 13 * 4\n"  //popped 13 4-byte registers from the stack
        "ret\n"
    );
}

struct process procs[PROCS_MAX]; //all the process control structures of our kernel

void user_entry(void) {
    __asm__ __volatile__(
        "csrw sepc, %[sepc]        \n" //setting pc (where sret jumps to)
        "csrw sstatus, %[sstatus]  \n" //setting SPIE bit in sstatus register (enables hardware interrupts when entering user mode)
        "sret                      \n" //switches from S-mode to U-mode
        :
        : [sepc] "r" (USER_BASE),
          [sstatus] "r" (SSTATUS_SPIE | SSTATUS_SUM)
    );
}

uint32_t *new_pt(void) {
    uint32_t *page_table = (uint32_t *) palloc(1);
    for(paddr_t paddr = (paddr_t) __kernel_base; paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE) {
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X); //no PAGE_U, so, processes cant access these pages in user mode
    }
    map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR, PAGE_R | PAGE_W);
    map_page(page_table, RTC_PADDR, RTC_PADDR, PAGE_R | PAGE_W);
    return page_table;
}

int next_pid = 1;

struct process *create_proc(const char *name, const void *image, size_t image_size) {
    //find and return an unused PCB
    struct process *unused_proc = NULL;
    int i;
    for(i = 0; i < PROCS_MAX; i++) {
        if(procs[i].state == PROC_UNUSED || procs[i].state == PROC_EXITED) {
            unused_proc = &procs[i];
            break;
        }
    }
    if(!unused_proc) {return NULL;}

    //stack callee-saved registers. restored in the first context switch in switch_context
    uint32_t *sp = (uint32_t *) &unused_proc->stack[sizeof(unused_proc->stack)];
    *--sp = 0;                      // s11
    *--sp = 0;                      // s10
    *--sp = 0;                      // s9
    *--sp = 0;                      // s8
    *--sp = 0;                      // s7
    *--sp = 0;                      // s6
    *--sp = 0;                      // s5
    *--sp = 0;                      // s4
    *--sp = 0;                      // s3
    *--sp = 0;                      // s2
    *--sp = 0;                      // s1
    *--sp = 0;                      // s0
    *--sp = (uint32_t) user_entry;  // ra
    
    //mapping kernel pages
    uint32_t *page_table = new_pt();

    //the page just below the user stack (see user.ld) is a guard page: it is simply never mapped, so a stack overflow faults instead of corrupting the program
    size_t guard = image_size >= USER_STACK_SIZE + PAGE_SIZE ? image_size - USER_STACK_SIZE - PAGE_SIZE : (size_t) -1;

    //mapping  user pages
    for(uint32_t offset = 0; offset < image_size; offset += PAGE_SIZE) {
        if(offset == guard) {continue;}
        paddr_t curr_page = palloc(1);
        
        //when data to be copied smaller than page size (for the last page being used for this image)
        size_t rem = image_size - offset;
        size_t copy_size = PAGE_SIZE <= rem ? PAGE_SIZE : rem; //using ternary operator like a pro *\(^o^)/*

        //filling and mapping the page
        memcpy((void *) curr_page, image + offset, copy_size);
        map_page(page_table, USER_BASE + offset, curr_page, PAGE_U | PAGE_R | PAGE_W | PAGE_X);
    }

    //initialising the fields of the PCB to be returned
    unused_proc->pid = next_pid++;
    unused_proc->state = PROC_RUNNABLE;
    unused_proc->sp = (uint32_t) sp;
    unused_proc->page_table = page_table;
    strcpy(unused_proc->cwd, "/"); //every process starts in the root directory
    int k = 0;
    while(name[k] && k < (int) sizeof(unused_proc->name) - 1) {unused_proc->name[k] = name[k]; k++;}
    unused_proc->name[k] = '\0';

    return unused_proc;
}

struct process *currently_running_proc;
struct process *idle_proc;

struct pipe pipes[PIPES_MAX];

//returns the page table entry for a virtual address, or 0 if the second level table is missing
uint32_t pt_walk(uint32_t *t1, vaddr_t va) {
    uint32_t e1 = t1[(va >> 22) & 0x3ff];
    if(!(e1 & PAGE_V)) {return 0;}
    uint32_t *t0 = (uint32_t *) ((e1 >> 10) * PAGE_SIZE);
    return t0[(va >> 12) & 0x3ff];
}

//frees a whole address space: every user page (the ones with PAGE_U), every second level table, and the root. the identity-mapped kernel pages have no PAGE_U and belong to nobody, so they stay
void pt_free(uint32_t *t1) {
    for(int i = 0; i < 1024; i++) {
        if(!(t1[i] & PAGE_V)) {continue;}
        uint32_t *t0 = (uint32_t *) ((t1[i] >> 10) * PAGE_SIZE);
        for(int j = 0; j < 1024; j++) {
            if((t0[j] & PAGE_V) && (t0[j] & PAGE_U)) {pfree((t0[j] >> 10) * PAGE_SIZE, 1);}
        }
        pfree((paddr_t) t0, 1);
    }
    pfree((paddr_t) t1, 1);
}

//how many pages of free ram a process owns
int proc_pages(struct process *p) {
    if(!p->page_table) {return 0;}
    int n = 1;
    for(int i = 0; i < 1024; i++) {
        if(!(p->page_table[i] & PAGE_V)) {continue;}
        n++;
        uint32_t *t0 = (uint32_t *) ((p->page_table[i] >> 10) * PAGE_SIZE);
        for(int j = 0; j < 1024; j++) {
            if((t0[j] & PAGE_V) && (t0[j] & PAGE_U)) {n++;}
        }
    }
    return n;
}

//gives back everything an exited process held. also closes the pipes it was writing to, so a reader waiting on it sees end-of-file instead of waiting forever
void reap(struct process *p) {
    for(int i = 0; i < PIPES_MAX; i++) {
        if(pipes[i].used && pipes[i].wpid == p->pid) {pipes[i].closed = true;}
    }
    if(!p->page_table) {return;}
    pt_free(p->page_table);
    p->page_table = NULL;
}

//is [ptr, ptr+len) entirely inside pages that the current process may use? every page must be mapped with PAGE_U (and PAGE_W if the kernel is going to write)
int uvalid(const void *ptr, size_t len, int write) {
    vaddr_t a = (vaddr_t) ptr;
    if(len == 0) {return 1;}
    if(a + len < a) {return 0;}
    for(vaddr_t pg = a & ~(PAGE_SIZE - 1); pg < a + len; pg += PAGE_SIZE) {
        uint32_t e = pt_walk(currently_running_proc->page_table, pg);
        if(!(e & PAGE_V) || !(e & PAGE_U)) {return 0;}
        if(write && !(e & PAGE_W)) {return 0;}
    }
    return 1;
}

//length of a user string. -2 if it runs into memory the process doesn't own, -1 if there is no '\0' within max bytes
int ustrlen(const char *s, int max) {
    for(int i = 0; i < max; i++) {
        vaddr_t a = (vaddr_t) s + i;
        if((i == 0 || is_aligned(a, PAGE_SIZE)) && !uvalid((const void *) a, 1, 0)) {return -2;}
        if(s[i] == '\0') {return i;}
    }
    return -1;
}

//optimistic scheduler to context switch when a process yields the CPU (calls yield()). works slightly inclined to round-robin principles
void yield(void) {
    //searching for a runnable process. since we have at max 8 processes, we dont need to maintain a separate list of available to run processes, we can just scan the whole process list and check running/unused status
    struct process *next_to_run = idle_proc;
    for(int i = 1; i <= PROCS_MAX; i++) {
        struct process *proc = &procs[((currently_running_proc - procs) + i) % PROCS_MAX];
        if(proc->state == PROC_RUNNABLE && proc->pid > 0) {
            next_to_run = proc;
            break;
        }
    }

    //if no process runnable other than current one, dont context switch, just run it
    if(next_to_run == currently_running_proc) {return;}
    
    //storing a pointer for the currently_running_proc to the bottom of the kernel stack in the sscratch register and switching the process's page table
    __asm__ __volatile__(
        //sfence.vma ensure changes to PT completed properly and clear/flush the TLB
        "sfence.vma\n"
        "csrw satp, %[satp]\n"
        "sfence.vma\n"
        "csrw sscratch, %[sscratch]\n"
        :
        : [satp] "r" (SATP_SV32 | ((uint32_t) next_to_run->page_table / PAGE_SIZE)),
          [sscratch] "r" ((uint32_t) &next_to_run->stack[sizeof(next_to_run->stack)])
    );

    //context switch
    struct process *prev_to_run = currently_running_proc;
    currently_running_proc = next_to_run;
    if(prev_to_run->state == PROC_EXITED) {reap(prev_to_run);} //satp already points at the next process, so the old page table is free to go
    switch_context(&prev_to_run->sp, &next_to_run->sp);
}

//delay() is made solely to test the context switching mechanism (not an integral part of the OS)
void delay(void) {
    for (int i = 0; i < 30000000; i++) {
        __asm__ __volatile__("nop"); // nop is an instruction that does nothing
    }
}

struct file files[FILES_MAX_LOADED];
uint8_t disk[DISK_MAX_SIZE];

struct file *fs_lookup(const char *filename) {
    for(int i = 0; i < FILES_MAX_LOADED; i++) {
        struct file *file = &files[i];
        if(file->in_use && strcmp(file->name, filename) == 0) {return file;}
    }
    return NULL;
}

//virtio-blk initialization as described in the spec. (this is a naive implementation, i MAY make it better later on)
//basic flow: reset the device, set the required parameters, then enable the device
struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
uint64_t blk_capacity;

//initializing a virtqueue. allocates the queue memory and tells the device its location (as a page frame number, not a physical address)
struct virtio_virtq *virtq_init(unsigned index) {
    //allocating a region for the virtqueue
    paddr_t virtq_paddr = palloc(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
    struct virtio_virtq *vq = (struct virtio_virtq *) virtq_paddr;
    vq->queue_index = index;
    vq->used_index = (volatile uint16_t *) &vq->used.index;

    //selecting the queue by writing its index (first queue is 0)
    virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);

    //telling the device the queue size (number of descriptors we will use)
    virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);

    //writing the physical page frame number of the queue
    virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr / PAGE_SIZE);
    return vq;
}

void virtio_blk_init(void) {
    if(virtio_reg_read32(VIRTIO_REG_MAGIC) != 0x74726976) {PANIC("virtio: invalid magic value");}
    if(virtio_reg_read32(VIRTIO_REG_VERSION) != 1) {PANIC("virtio: invalid version");}
    if(virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK) {PANIC("virtio: invalid device id");}

    //resetting the device
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);

    //setting the ACKNOWLEDGE status bit (basically says "we found the device")
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);

    //setting the DRIVER status bit (basically says "we know how to use the device")
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER);
    
    //setting the page size. we use 4KB pages. this defines PFN (page frame number) calculation
    virtio_reg_write32(VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

    //initializing a queue for disk read/write requests
    blk_request_vq = virtq_init(0);

    //setting the DRIVER_OK status bit (basically says "we can now use the device!!!!!!")
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER_OK);

    //getting the disk capacity
    blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;

    //allocating a region to store requests to the device
    blk_req_paddr = palloc(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
    blk_req = (struct virtio_blk_req *) blk_req_paddr;
}

//notifies the device that there is a new request. `desc_index` is index of the head descriptor of the new request
void virtq_kick(struct virtio_virtq *vq, int desc_index) {
    vq->avail.ring[vq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
    vq->avail.index++;
    __sync_synchronize();
    virtio_reg_write32(VIRTIO_REG_QUEUE_NOTIFY, vq->queue_index);
    vq->last_used_index++;
}

bool virtq_is_busy(struct virtio_virtq *vq) {
    return vq->last_used_index != *vq->used_index;
}

void read_write_disk(void *buf, unsigned sector, int is_write) {
    if(sector >= blk_capacity / SECTOR_SIZE) {
        printf("virtio: tried to read/write sector=%d, but capacity is %d\n",
              sector, blk_capacity / SECTOR_SIZE);
        return;
    }

    //constructing the request according to the virtio-blk specifications
    blk_req->sector = sector;
    blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    if(is_write) {memcpy(blk_req->data, buf, SECTOR_SIZE);}

    //constructing the virtqueue 3 descriptors
    struct virtio_virtq *vq = blk_request_vq;
    vq->descs[0].addr = blk_req_paddr;
    vq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
    vq->descs[0].flags = VIRTQ_DESC_F_NEXT;
    vq->descs[0].next = 1;

    vq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
    vq->descs[1].len = SECTOR_SIZE;
    vq->descs[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    vq->descs[1].next = 2;

    vq->descs[2].addr = blk_req_paddr + offsetof(struct virtio_blk_req, status);
    vq->descs[2].len = sizeof(uint8_t);
    vq->descs[2].flags = VIRTQ_DESC_F_WRITE;

    //notifying the device that there is a new request
    virtq_kick(vq, 0);

    //waiting until the device finishes processing
    while(virtq_is_busy(vq)) {;}

    if(blk_req->status != 0) {
        printf("virtio: warn: failed to read/write sector=%d status=%d\n", sector, blk_req->status);
        return;
    }

    //for read operations, copy the data into the buffer
    if(!is_write) {memcpy(buf, blk_req->data, SECTOR_SIZE);}
}

int octal2int(char* oct, int len) {
    int dec = 0;
    for(int i = 0; i < len; i++) {
        if(oct[i] < '0' || oct[i] > '7') {break;}
        dec = (dec * 8) + (oct[i] - '0');
    }
    return dec;
}

//reading the disk into memory. returns the number of files and directories found
int fs_init(void) {
    if(blk_capacity < sizeof(disk)) {PANIC("disk image too small (%d bytes), delete disk.tar and run ./run.sh fresh", (int) blk_capacity);}

    for(unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++) {
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);
    }

    unsigned offset = 0;
    int n = 0;
    //following ustar format of tar files, every file has a tar header and file data pair
    while(n < FILES_MAX_LOADED) {
        if(offset + sizeof(struct tar_header) > sizeof(disk)) {break;}
        struct tar_header *header = (struct tar_header *) &disk[offset];
        if(header->name[0] == '\0') {break;}
        if(strcmp(header->magic, "ustar") != 0) {
            PANIC("invalid tar header: magic=\"%s\"", header->magic);
        }

        int filesize = octal2int(header->size, sizeof(header->size));
        unsigned next = offset + align_up(sizeof(struct tar_header) + filesize, SECTOR_SIZE);
        bool is_dir = header->type == '5';
        if(header->type != '0' && header->type != '\0' && !is_dir) {offset = next; continue;} //anything that is neither a file nor a directory (like a pax header) is skipped

        int copy = filesize > FILE_DATA_MAX ? FILE_DATA_MAX : filesize;
        struct file *file = &files[n++];
        file->in_use = true;
        file->is_dir = is_dir;
        strcpy(file->name, header->name);
        int nl = strlen(file->name);
        if(nl > 0 && file->name[nl - 1] == '/') {file->name[nl - 1] = '\0';}
        if(!is_dir) {memcpy(file->data, header->data, copy);}
        file->size = is_dir ? 0 : copy;
        offset = next;
    }
    return n;
}

//writing to disk is implemented by writing the contents/value of the files variable back to the disk in tar format
void fs_flush(void) {
    //copying all file contents into disk buffer
    memset(disk, 0, sizeof(disk));
    unsigned offset = 0;
    for(int file_i = 0; file_i < FILES_MAX_LOADED; file_i++) {
        struct file *file = &files[file_i];
        if(!file->in_use) {continue;}

        struct tar_header *header = (struct tar_header *) &disk[offset];
        memset(header, 0, sizeof(*header));
        strcpy(header->name, file->name);
        if(file->is_dir) {header->name[strlen(file->name)] = '/';} //tar marks directories with a trailing slash and type '5'
        strcpy(header->mode, file->is_dir ? "000755" : "000644");
        strcpy(header->magic, "ustar");
        strcpy(header->version, "00");
        header->type = file->is_dir ? '5' : '0';

        //turning the file size into an octal string
        int filesize = file->size;
        for(int i = sizeof(header->size); i > 0; i--) {
            header->size[i - 1] = (filesize % 8) + '0';
            filesize /= 8;
        }

        //calculating the checksum
        int checksum = ' ' * sizeof(header->checksum);
        for(unsigned i = 0; i < sizeof(struct tar_header); i++) {
            checksum += (unsigned char) disk[offset + i];
        }

        for(int i = 5; i >= 0; i--) {
            header->checksum[i] = (checksum % 8) + '0';
            checksum /= 8;
        }

        //copying file data (finally!!!)
        memcpy(header->data, file->data, file->size);
        offset += align_up(sizeof(struct tar_header) + file->size, SECTOR_SIZE);
    }

    //writing disk buffer into the virtio-blk
    for(unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++) {
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, true);
    }

    printf("wrote %d bytes to disk\n", sizeof(disk));
}

//file names are stored as canonical paths with no leading slash: "notes.txt", "docs/a.txt". the root directory is the empty string
int fs_is_child(const char *name, const char *dir) {
    int dl = strlen(dir);
    if(dl > 0) {
        if((int) strlen(name) <= dl + 1) {return 0;}
        for(int i = 0; i < dl; i++) {if(name[i] != dir[i]) {return 0;}}
        if(name[dl] != '/') {return 0;}
        name += dl + 1;
    }
    for(int i = 0; name[i]; i++) {if(name[i] == '/') {return 0;}}
    return name[0] != '\0';
}

int fs_dir_ok(const char *dir) {
    if(dir[0] == '\0') {return 1;}
    struct file *d = fs_lookup(dir);
    return d && d->is_dir;
}

struct file *fs_create(const char *name, int is_dir) {
    int n = strlen(name);
    if(n == 0 || n > PATH_MAX) {return NULL;}
    int last = -1;
    for(int i = 0; i < n; i++) {if(name[i] == '/') {last = i;}}
    char parent[PATH_MAX + 1];
    if(last >= 0) {memcpy(parent, name, last);}
    parent[last < 0 ? 0 : last] = '\0';
    if(!fs_dir_ok(parent) || fs_lookup(name)) {return NULL;}

    for(int i = 0; i < FILES_MAX_LOADED; i++) {
        if(!files[i].in_use) {
            memset(&files[i], 0, sizeof(files[i]));
            files[i].in_use = true;
            files[i].is_dir = is_dir;
            strcpy(files[i].name, name);
            return &files[i];
        }
    }
    return NULL;
}

//replaces the contents of a file (or appends to them), creating the file if needed, then flushes the disk. returns the number of bytes written
int fs_put(const char *name, const char *buf, int len, int append) {
    struct file *file = fs_lookup(name);
    if(file && file->is_dir) {return -1;}
    if(!file) {file = fs_create(name, false);}
    if(!file) {return -1;}
    int off = append ? (int) file->size : 0;
    if(len < 0) {len = 0;}
    if(off + len > FILE_DATA_MAX) {len = FILE_DATA_MAX - off;}
    if(!append) {memset(file->data, 0, sizeof(file->data));}
    memcpy(file->data + off, buf, len);
    file->size = off + len;
    fs_flush();
    return len;
}

int fs_write(const char *filename, const char *buf, int len) {
    return fs_put(filename, buf, len, false);
}

//0 on success, -1 if there is no such entry, -2 if it is a directory that isn't empty
int fs_remove(const char *name) {
    struct file *file = fs_lookup(name);
    if(!file) {return -1;}
    if(file->is_dir) {
        for(int i = 0; i < FILES_MAX_LOADED; i++) {
            if(files[i].in_use && fs_is_child(files[i].name, name)) {return -2;}
        }
    }
    file->in_use = false;
    fs_flush();
    return 0;
}

//copies the name of the idx-th entry of a directory into out (directories get a trailing '/'). returns its size, -1 past the end, -2 if dir doesn't exist
int fs_readdir(const char *dir, int idx, char *out) {
    if(!fs_dir_ok(dir)) {return -2;}
    for(int i = 0; i < FILES_MAX_LOADED; i++) {
        struct file *file = &files[i];
        if(!file->in_use || !fs_is_child(file->name, dir)) {continue;}
        if(idx-- > 0) {continue;}
        strcpy(out, dir[0] ? file->name + strlen(dir) + 1 : file->name);
        if(file->is_dir) {int l = strlen(out); out[l] = '/'; out[l + 1] = '\0';}
        return file->is_dir ? 0 : (int) file->size;
    }
    return -1;
}

//turns cwd + path into a canonical absolute path in out: "/" or "/a/b". handles absolute paths, ".", ".." and repeated slashes. returns the length, or -1 if it doesn't fit
int path_resolve(const char *cwd, const char *path, char *out, int max) {
    char in[CWD_MAX + 130];
    int cl = path[0] == '/' ? 0 : strlen(cwd);
    int pl = strlen(path);
    if(cl + pl + 2 > (int) sizeof(in) || max < 2) {return -1;}
    if(cl) {memcpy(in, cwd, cl); in[cl] = '/';}
    memcpy(in + (cl ? cl + 1 : 0), path, pl + 1);

    out[0] = '/';
    int n = 1;
    int i = 0;
    while(in[i]) {
        while(in[i] == '/') {i++;}
        if(!in[i]) {break;}
        int s = i;
        while(in[i] && in[i] != '/') {i++;}
        int l = i - s;
        if(l == 1 && in[s] == '.') {continue;}
        if(l == 2 && in[s] == '.' && in[s + 1] == '.') {
            while(n > 1 && out[n - 1] != '/') {n--;}
            if(n > 1) {n--;}
            continue;
        }
        if(n + l + 1 > PATH_MAX || n + l + 1 >= max) {return -1;}
        if(n > 1) {out[n++] = '/';}
        memcpy(out + n, in + s, l);
        n += l;
    }
    out[n] = '\0';
    return n;
}

//a user pointer to a path becomes a canonical absolute path. 0 on success, -2 if the pointer is bad, -1 if the path is too long
int user_path(const char *up, char *out) {
    int l = ustrlen(up, 128);
    if(l == -2) {return -2;}
    if(l < 0) {return -1;}
    char tmp[128];
    memcpy(tmp, up, l + 1);
    return path_resolve(currently_running_proc->cwd, tmp, out, PATH_MAX + 1) < 0 ? -1 : 0;
}

struct prog {char *start; size_t size;};

int find_prog(const char *name, struct prog *p) {
#define PROG(n) if(strcmp(name, #n) == 0) {p->start = _binary_##n##_bin_start; p->size = (size_t) _binary_##n##_bin_size; return 1;}
    PROG(shell)
    PROG(spinner)
    PROG(ticker)
    PROG(crash)
    PROG(producer)
    PROG(consumer)
    PROG(evil)
    PROG(recurse)
#undef PROG
    return 0;
}

void badptr(struct trap_frame *f) {
    printf("process %d (%s): bad pointer in syscall %d, rejected\n", currently_running_proc->pid, currently_running_proc->name, f->a3);
    f->a0 = -1;
}

//SYS_WRITEFILE and SYS_APPENDFILE only differ in one flag
void sys_put(struct trap_frame *f, int append) {
    char p[PATH_MAX + 1];
    int len = f->a2;
    if(len < 0 || !uvalid((void *) f->a1, len, 0)) {badptr(f); return;}
    int r = user_path((const char *) f->a0, p);
    if(r == -2) {badptr(f); return;}
    f->a0 = r < 0 ? -1 : fs_put(p + 1, (const char *) f->a1, len, append);
}

void sys_readdir(struct trap_frame *f) {
    char p[PATH_MAX + 1];
    if(!uvalid((void *) f->a2, sizeof(files[0].name), 1)) {badptr(f); return;}
    int r = user_path((const char *) f->a0, p);
    if(r == -2) {badptr(f); return;}
    f->a0 = r < 0 ? -2 : fs_readdir(p + 1, f->a1, (char *) f->a2);
}

void sys_pipe_open(struct trap_frame *f) {
    char nm[16];
    int l = ustrlen((const char *) f->a0, sizeof(nm));
    if(l == -2) {badptr(f); return;}
    if(l <= 0) {f->a0 = -1; return;}
    memcpy(nm, (const char *) f->a0, l + 1);
    int mode = f->a1;

    int id = -1;
    for(int i = 0; i < PIPES_MAX; i++) {
        if(pipes[i].used && strcmp(pipes[i].name, nm) == 0) {id = i;}
    }
    if(id < 0) {
        for(int i = 0; i < PIPES_MAX && id < 0; i++) {
            if(!pipes[i].used) {id = i;}
        }
        if(id < 0) {f->a0 = -1; return;}
        memset(&pipes[id], 0, sizeof(pipes[id]));
        pipes[id].used = true;
        strcpy(pipes[id].name, nm);
    }
    if(mode == 1) { //opening for writing (re)opens the stream
        pipes[id].closed = false;
        pipes[id].wpid = currently_running_proc->pid;
    }
    f->a0 = id;
}

void sys_pipe_write(struct trap_frame *f) {
    int id = f->a0;
    const char *src = (const char *) f->a1;
    int len = f->a2;
    if(id < 0 || id >= PIPES_MAX || !pipes[id].used || pipes[id].closed) {f->a0 = -1; return;}
    if(len < 0 || !uvalid(src, len, 0)) {badptr(f); return;}
    struct pipe *p = &pipes[id];
    int w = 0;
    while(w < len) {
        if(p->count == PIPE_BUF) { //full: let the reader run, then try again
            yield();
            if(!p->used || p->closed) {break;}
            continue;
        }
        p->buf[(p->head + p->count) % PIPE_BUF] = src[w++];
        p->count++;
    }
    f->a0 = w;
}

void sys_pipe_read(struct trap_frame *f) {
    int id = f->a0;
    char *dst = (char *) f->a1;
    int len = f->a2;
    if(id < 0 || id >= PIPES_MAX || !pipes[id].used) {f->a0 = -1; return;}
    if(len < 0 || !uvalid(dst, len, 1)) {badptr(f); return;}
    struct pipe *p = &pipes[id];
    int r = 0;
    while(len > 0) {
        if(p->count > 0) {
            while(r < len && p->count > 0) {
                dst[r++] = p->buf[p->head];
                p->head = (p->head + 1) % PIPE_BUF;
                p->count--;
            }
            break;
        }
        if(p->closed) {p->used = false; break;} //empty and closed: end of file, and the pipe is gone
        yield();
    }
    f->a0 = r;
}

void handle_syscall(struct trap_frame *f) {
    char p[PATH_MAX + 1];
    switch (f->a3) {
        case SYS_PUTCHAR: {
            put_char(f->a0);
            break;
        }
        case SYS_GETCHAR: {
            while(1) {
                long ch = get_char();
                if(ch >= 0) {f->a0 = ch; break;}
                yield();
            }
            break;
        }
        case SYS_EXIT: { //the process is only marked as exited here. yield() frees its memory once it has switched away from the process's page table
            printf("process %d (%s) exited\n", currently_running_proc->pid, currently_running_proc->name);
            currently_running_proc->state = PROC_EXITED; //a process with this state never ran by the scheduler again
            yield();
            PANIC("unreachable"); //just in case this process does return again
        }
        case SYS_READFILE: {
            int len = f->a2;
            if(len < 0 || !uvalid((void *) f->a1, len, 1)) {badptr(f); break;}
            int r = user_path((const char *) f->a0, p);
            if(r == -2) {badptr(f); break;}
            struct file *file = r < 0 ? NULL : fs_lookup(p + 1);
            if(!file || file->is_dir) {f->a0 = -1; break;}

            if(len > (int) file->size) {len = file->size;}
            memcpy((char *) f->a1, file->data, len);
            f->a0 = len;
            break;
        }
        case SYS_WRITEFILE: {
            sys_put(f, false);
            break;
        }
        case SYS_APPENDFILE: {
            sys_put(f, true);
            break;
        }
        case SYS_GETCWD: {
            char *buf = (char *) f->a0;
            int len = f->a1;
            int cwd_len = strlen(currently_running_proc->cwd);
            if(len < 0 || !uvalid(buf, len, 1)) {badptr(f); break;}
            if(cwd_len + 1 > len) { //+1 for the '\0'
                f->a0 = -1;
                break;
            }

            strcpy(buf, currently_running_proc->cwd);
            f->a0 = cwd_len;
            break;
        }
        case SYS_YIELD: {
            yield();
            break;
        }
        case SYS_SPAWN: {
            char nm[16];
            int l = ustrlen((const char *) f->a0, sizeof(nm));
            if(l == -2) {badptr(f); break;}
            if(l < 0) {f->a0 = -1; break;}
            memcpy(nm, (const char *) f->a0, l + 1);
            struct prog prog;
            if(!find_prog(nm, &prog)) {f->a0 = -1; break;}
            struct process *np = create_proc(nm, prog.start, prog.size);
            if(!np) {f->a0 = -2; break;}
            strcpy(np->cwd, currently_running_proc->cwd);
            f->a0 = np->pid;
            yield();
            break;
        }
        case SYS_READDIR: {
            sys_readdir(f);
            break;
        }
        case SYS_PROCINFO: {
            int idx = f->a0;
            struct procinfo *pi = (struct procinfo *) f->a1;
            if(!uvalid(pi, sizeof(*pi), 1)) {badptr(f); break;}
            if(idx < 0 || idx >= PROCS_MAX) {f->a0 = -1; break;}
            struct process *pr = &procs[idx];
            pi->pid = pr->pid;
            pi->state = pr == currently_running_proc ? PS_RUNNING : pr->state;
            pi->pages = proc_pages(pr);
            strcpy(pi->name, pr->name);
            f->a0 = 0;
            break;
        }
        case SYS_SYSINFO: {
            struct sysinfo *si = (struct sysinfo *) f->a0;
            if(!uvalid(si, sizeof(*si), 1)) {badptr(f); break;}
            si->total_pages = pg_total;
            si->free_pages = free_pages();
            si->uptime_s = (uint32_t) (rdtime() >> 7) / (TICKS_PER_SEC / 128);
            si->procs = 0;
            for(int i = 0; i < PROCS_MAX; i++) {
                if(procs[i].state == PROC_RUNNABLE && procs[i].pid > 0) {si->procs++;}
            }
            f->a0 = 0;
            break;
        }
        case SYS_TIME: {
            f->a0 = (uint32_t) rdtime();
            break;
        }
        case SYS_SHUTDOWN: {
            halt();
        }
        case SYS_KILL: {
            int pid = f->a0;
            struct process *t = NULL;
            for(int i = 0; i < PROCS_MAX; i++) {
                if(pid > 0 && procs[i].pid == pid && procs[i].state == PROC_RUNNABLE) {t = &procs[i];}
            }
            if(!t) {f->a0 = -1; break;}
            printf("process %d (%s) killed\n", t->pid, t->name);
            t->state = PROC_EXITED;
            if(t == currently_running_proc) {
                yield();
                PANIC("unreachable");
            }
            reap(t);
            f->a0 = 0;
            break;
        }
        case SYS_GETPID: {
            f->a0 = currently_running_proc->pid;
            break;
        }
        case SYS_UNLINK: {
            int r = user_path((const char *) f->a0, p);
            if(r == -2) {badptr(f); break;}
            f->a0 = r < 0 ? -1 : fs_remove(p + 1);
            break;
        }
        case SYS_MKDIR: {
            int r = user_path((const char *) f->a0, p);
            if(r == -2) {badptr(f); break;}
            if(r < 0) {f->a0 = -1; break;}
            if(fs_lookup(p + 1)) {f->a0 = -2; break;}
            struct file *d = fs_create(p + 1, true);
            if(d) {fs_flush();}
            f->a0 = d ? 0 : -1;
            break;
        }
        case SYS_CHDIR: {
            int r = user_path((const char *) f->a0, p);
            if(r == -2) {badptr(f); break;}
            if(r < 0 || !fs_dir_ok(p + 1)) {f->a0 = -1; break;}
            strcpy(currently_running_proc->cwd, p);
            f->a0 = 0;
            break;
        }
        case SYS_PIPE_OPEN: {
            sys_pipe_open(f);
            break;
        }
        case SYS_PIPE_WRITE: {
            sys_pipe_write(f);
            break;
        }
        case SYS_PIPE_READ: {
            sys_pipe_read(f);
            break;
        }
        case SYS_PIPE_CLOSE: {
            int id = f->a0;
            if(id < 0 || id >= PIPES_MAX || !pipes[id].used) {f->a0 = -1; break;}
            pipes[id].closed = true;
            f->a0 = 0;
            break;
        }
        case SYS_REBOOT: {
            printf("\nIcarusOS rebooting...\n");
            sbi_call(1, 0, 0, 0, 0, 0, 0, 0x53525354);
            printf("reboot is not supported by this firmware\n");
            f->a0 = -1;
            break;
        }
        case SYS_POLLCHAR: {
            f->a0 = get_char();
            break;
        }
        case SYS_DATE: {
            f->a0 = rtc_seconds();
            break;
        }
        default: {
            PANIC("unexpected syscall a3=%x\n", f->a3);
        }
    }
}

void handle_trap(struct trap_frame *f) {
    uint32_t scause = READ_CSR(scause);
    uint32_t stval = READ_CSR(stval);
    uint32_t sepc = READ_CSR(sepc); //sepc is basically user_pc
    uint32_t sstatus = READ_CSR(sstatus);
    
    if(scause == SCAUSE_ECALL) {
        handle_syscall(f);
        sepc += 4; //to move an instruction ahead, otherwise, syscalls will be called infinitely
    }
    else if((sstatus & SSTATUS_SPP) == 0) { //SPP = 0 means the trap came from user mode, so only that process is at fault
        const char *why = scause == 12 ? "instruction page fault" : scause == 13 ? "load page fault" : scause == 15 ? "store page fault" : scause == 2 ? "illegal instruction" : "exception";
        printf("process %d (%s) killed: segmentation fault (%s, addr=0x%x, pc=0x%x)\n", currently_running_proc->pid, currently_running_proc->name, why, stval, sepc);
        currently_running_proc->state = PROC_EXITED;
        yield();
        PANIC("unreachable");
    }
    else {PANIC("Unexpected trap: scause=%x, stval=%x, sepc=%x\n", scause, stval, sepc);}

    WRITE_CSR(sepc, sepc);
}

#include "demo.h"

#define SPLASH_MS 120
#define BOOT_OK(...) do {printf("[\033[1;32m ok \033[0m] "); printf(__VA_ARGS__); printf("\n"); sleep_ms(SPLASH_MS);} while(0)

void splash(void) {
    printf("\033[2J\033[H\033[1;33m\n");
    printf("            \\   |   /\n");
    printf("         ~~~ '-.:::.-' ~~~\n");
    printf("      <=====( .:::::. )=====>      I c a r u s O S\n");
    printf("         ~~~ '-:::::-' ~~~         a tiny RISC-V OS that flew\n");
    printf("            /   |   \\              too close to the sun\n");
    printf("\033[0m\n");
    sleep_ms(SPLASH_MS * 3);
}

void kernel_main(void) {
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss); //.bss section initialised to 0. Some bootloders may recognise and 0-clear the .bss section, but, we do it manually too just in case the bootloader doesnt.
    palloc_init();
    splash();

    //telling the CPU where the exception handler is located
    WRITE_CSR(stvec, (uint32_t) kernel_entry);
    BOOT_OK("trap vector installed");
    BOOT_OK("bitmap page allocator ready: %d pages (%d MB)", free_pages(), free_pages() / 256);

    //initializing virtio-blk
    virtio_blk_init();
    BOOT_OK("virtio-blk disk: %d bytes", (int) blk_capacity);

    //initializing filesystem
    int nfiles = fs_init();
    BOOT_OK("tar filesystem: %d entries loaded", nfiles);

    //creating an initial idle process with pid 0. this is the root process of IcarusOS
    idle_proc = create_proc("idle", NULL, 0);
    idle_proc->pid = 0;
    next_pid = 1;
    currently_running_proc = idle_proc;
    BOOT_OK("idle process created (pid 0)");

#ifdef DEMO_TOUR
    demo_tour();
#endif

    struct process *shell = create_proc("shell", _binary_shell_bin_start, (size_t) _binary_shell_bin_size);
    BOOT_OK("shell process created (pid %d)", shell->pid);
    BOOT_OK("entering user mode");
    printf("\n");

    yield();
    printf("\nno runnable processes left\n");
    halt();
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
