#include "kernel.h"
#include "common.h"

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t size_t;

extern char __kernel_base[], __bss[], __bss_end[], __stack_top[], __free_ram_start[], __free_ram_end[]; //__bss alone would mean value of 0th byte of .bss section. To get start address of .bss section, we add the [] at the end

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

//the memory allocator will allocate contiguous memory in 4KB size pages/units. 
paddr_t palloc(uint32_t n) {
    static paddr_t paddr_ptr = (paddr_t) __free_ram_start;
    paddr_t start_paddr = paddr_ptr;
    paddr_ptr += n * PAGE_SIZE;

    if(paddr_ptr > (paddr_t) __free_ram_end) {PANIC("out of memory");}

    memset((void *) start_paddr, 0, n * PAGE_SIZE);
    return start_paddr;
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

void handle_trap(struct trap_frame *f) {
    uint32_t scause = READ_CSR(scause);
    uint32_t stval = READ_CSR(stval);
    uint32_t sepc = READ_CSR(sepc); //sepc is basically user_pc
    PANIC("Unexpected trap: scause=%x, stval=%x, sepc=%x\n", scause, stval, sepc);
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

struct process *assign_pcb(uint32_t pc) {
    //find and return an unused PCB
    struct process *unused_proc = NULL;
    int i;
    for(i = 0; i < PROCS_MAX; i++) {
        if(procs[i].state == PROC_UNUSED) {
            unused_proc = &procs[i];
            break;
        }
    }
    if(!unused_proc) {PANIC("no free process slots available");}

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
    *--sp = (uint32_t) pc;          // ra
    
    //mapping kernel pages
    uint32_t *page_table = (uint32_t *) palloc(1);
    for(paddr_t paddr = (paddr_t) __kernel_base; paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE) {
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X); //no PAGE_U, so, processes cant access these pages in user mode
    }

    //initialising the fields of the PCB to be returned
    unused_proc->pid = i + 1;
    unused_proc->state = PROC_RUNNABLE;
    unused_proc->sp = (uint32_t) sp;
    unused_proc->page_table = page_table;

    return unused_proc;
}

struct process *currently_running_proc;
struct process *idle_proc;

//optimistic scheduler to context switch when a process yields the CPU (calls yield()). works slightly inclined to round-robin principles
void yield(void) {
    //searching for a runnable process. since we have at max 8 processes, we dont need to maintain a separate list of available to run processes, we can just scan the whole process list and check running/unused status
    struct process *next_to_run = idle_proc;
    for(int i = 0; i < PROCS_MAX; i++) {
        struct process *proc = &procs[(currently_running_proc->pid + i) % PROCS_MAX];
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
    switch_context(&prev_to_run->sp, &next_to_run->sp);
}

//delay(), a_entry(), b_entry(), proc_a, proc_b, num_context_switches are made solely to test the context switching mechanism (not an integral part of the OS)
void delay(void) {
    for (int i = 0; i < 900000000; i++) {
        __asm__ __volatile__("nop"); // nop is an instruction that does nothing
    }
}

struct process *proc_a;
struct process *proc_b;
int num_context_switches = 0;

void a_entry(void) {
    printf("starting process A\n");
    while (1) {
        delay();
        if(num_context_switches >= 30) {break;}
        num_context_switches++;
        put_char('A');
        yield();
    }
}

void b_entry(void) {
    printf("starting process B\n");
    while (1) {
        delay();
        if(num_context_switches >= 30) {break;}
        num_context_switches++;
        put_char('B');
        yield();
    }
}

void kernel_main(void) {
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss); //.bss section initialised to 0. Some bootloders may recognise and 0-clear the .bss section, but, we do it manually too just in case the bootloader doesnt.
    printf("\n\n");
    
    //telling the CPU where the exception handler is located
    WRITE_CSR(stvec, (uint32_t) kernel_entry);

    //creating an initial idle process with pid 0. this is the root process of IcarusOS
    idle_proc = assign_pcb((uint32_t) NULL);
    idle_proc->pid = 0;
    currently_running_proc = idle_proc;

    //testing printf()
    printf("Beat the odds. Go Beyond!\n");
    
    //testing memory allocator
    paddr_t mem_region1 = palloc(2);
    paddr_t mem_region2 = palloc(1);
    printf("palloc test; mem_region1 starts at address paddr=%x\n", mem_region1);
    printf("palloc test; mem_region2 starts at address paddr=%x\n", mem_region2);
    
    //testing context switching
    proc_a = assign_pcb((uint32_t) a_entry);
    proc_b = assign_pcb((uint32_t) b_entry);
    yield();
    PANIC("switched to idle process");

    //testing trap/exception handler and kernel PANIC
    //triggering an exception. testing if exception works in turn also tests if PANIC works correctly
    __asm__ __volatile__("unimp"); //unimp is the unimplemented instruction (signifies an unimplemented operation) that triggers a trap in our kernel. its a pseudo instruction in RISC-V translating into: csrrw x0, cycle, x0. triggers an exception since cycle is a read-only register but we aretrying to write to it. 
    printf("this shouldnt be printed if PANIC works correctly\n");
    
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
