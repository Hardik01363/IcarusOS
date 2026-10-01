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

__attribute__((noreturn)) void halt(void) {
    printf("\n\033[1;33mIcarusOS halted. Goodbye (^o^)/\033[0m\n");
    sbi_call(0, 0, 0, 0, 0, 0, 0, 0x53525354);
    sbi_call(0, 0, 0, 0, 0, 0, 0, 8);
    for(;;) {__asm__ __volatile__("wfi");}
}

//the memory allocator will allocate contiguous memory in 4KB size pages/units. 
paddr_t paddr_ptr = (paddr_t) __free_ram_start;

int free_pages(void) {
    return ((paddr_t) __free_ram_end - paddr_ptr) / PAGE_SIZE;
}

paddr_t palloc(uint32_t n) {
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

    //mapping  user pages
    for(uint32_t offset = 0; offset < image_size; offset += PAGE_SIZE) {
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

//reading the disk into memory. returns the number of files found
int fs_init(void) {
    if(blk_capacity < sizeof(disk)) {PANIC("disk image too small (%d bytes), delete disk.tar and run ./run.sh fresh", (int) blk_capacity);}

    for(unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++) {
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);
    }

    unsigned offset = 0;
    int i;
    //following ustar format of tar files, every file has a tar header and file data pair
    for(i = 0; i < FILES_MAX_LOADED; i++) {
        if(offset + sizeof(struct tar_header) > sizeof(disk)) {break;}
        struct tar_header *header = (struct tar_header *) &disk[offset];
        if(header->name[0] == '\0') {break;}
        if(strcmp(header->magic, "ustar") != 0) {
            PANIC("invalid tar header: magic=\"%s\"", header->magic);
        }

        int filesize = octal2int(header->size, sizeof(header->size));
        int copy = filesize > FILE_DATA_MAX ? FILE_DATA_MAX : filesize;
        struct file *file = &files[i];
        file->in_use = true;
        strcpy(file->name, header->name);
        memcpy(file->data, header->data, copy);
        file->size = copy;
        offset += align_up(sizeof(struct tar_header) + filesize, SECTOR_SIZE);
    }
    return i;
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
        strcpy(header->mode, "000644");
        strcpy(header->magic, "ustar");
        strcpy(header->version, "00");
        header->type = '0';

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

struct file *fs_create(const char *filename) {
    if(filename[0] == '\0' || strlen(filename) >= sizeof(files[0].name)) {return NULL;}
    for(int i = 0; i < FILES_MAX_LOADED; i++) {
        if(!files[i].in_use) {
            memset(&files[i], 0, sizeof(files[i]));
            files[i].in_use = true;
            strcpy(files[i].name, filename);
            return &files[i];
        }
    }
    return NULL;
}

int fs_write(const char *filename, const char *buf, int len) {
    struct file *file = fs_lookup(filename);
    if(!file) {file = fs_create(filename);}
    if(!file) {return -1;}
    if(len < 0) {len = 0;}
    if(len > FILE_DATA_MAX) {len = FILE_DATA_MAX;}
    memset(file->data, 0, sizeof(file->data));
    memcpy(file->data, buf, len);
    file->size = len;
    fs_flush();
    return len;
}

struct prog {char *start; size_t size;};

int find_prog(const char *name, struct prog *p) {
#define PROG(n) if(strcmp(name, #n) == 0) {p->start = _binary_##n##_bin_start; p->size = (size_t) _binary_##n##_bin_size; return 1;}
    PROG(shell)
    PROG(spinner)
    PROG(ticker)
    PROG(crash)
#undef PROG
    return 0;
}

void handle_syscall(struct trap_frame *f) {
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
        case SYS_EXIT: { //we only mark the process as exited for simplicity. in a more practical OS, resources held by the process must also be freed
            printf("process %d (%s) exited\n", currently_running_proc->pid, currently_running_proc->name);
            currently_running_proc->state = PROC_EXITED; //a process with this state never ran by the scheduler again
            yield();
            PANIC("unreachable"); //just in case this process does return again
        }
        case SYS_READFILE: {
            const char *filename = (const char *) f->a0;
            char *buf = (char *) f->a1;
            int len = f->a2;
            struct file *file = fs_lookup(filename);
            if(!file) {f->a0 = -1; break;}

            if(len > (int) file->size) {len = file->size;}
            memcpy(buf, file->data, len);
            f->a0 = len;
            break;
        }
        case SYS_WRITEFILE: {
            f->a0 = fs_write((const char *) f->a0, (const char *) f->a1, f->a2);
            break;
        }
        case SYS_GETCWD: {
            char *buf = (char *) f->a0;
            int len = f->a1;
            int cwd_len = strlen(currently_running_proc->cwd);
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
            struct prog prog;
            if(!find_prog((const char *) f->a0, &prog)) {f->a0 = -1; break;}
            struct process *p = create_proc((const char *) f->a0, prog.start, prog.size);
            if(!p) {f->a0 = -2; break;}
            f->a0 = p->pid;
            yield();
            break;
        }
        case SYS_LISTFILES: {
            int idx = f->a0;
            char *buf = (char *) f->a1;
            int len = f->a2;
            f->a0 = -1;
            for(int i = 0; i < FILES_MAX_LOADED; i++) {
                if(!files[i].in_use) {continue;}
                if(idx-- > 0) {continue;}
                if((int) strlen(files[i].name) + 1 > len) {break;}
                strcpy(buf, files[i].name);
                f->a0 = files[i].size;
                break;
            }
            break;
        }
        case SYS_PROCINFO: {
            int idx = f->a0;
            struct procinfo *pi = (struct procinfo *) f->a1;
            if(idx < 0 || idx >= PROCS_MAX) {f->a0 = -1; break;}
            struct process *p = &procs[idx];
            pi->pid = p->pid;
            pi->state = p == currently_running_proc ? PS_RUNNING : p->state;
            strcpy(pi->name, p->name);
            f->a0 = 0;
            break;
        }
        case SYS_SYSINFO: {
            struct sysinfo *si = (struct sysinfo *) f->a0;
            si->total_pages = ((paddr_t) __free_ram_end - (paddr_t) __free_ram_start) / PAGE_SIZE;
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
            f->a0 = 0;
            break;
        }
        case SYS_GETPID: {
            f->a0 = currently_running_proc->pid;
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
    printf("\\            |            / \n");
    printf("                                                ' .       |       . '\n");            
    printf("      ICARUS OS                            -        -   .---.   -        -\n");
    printf("please put up with my bad drawing (^.^)        '  .   /:::::::\\   .  '\n");
    printf("took me an hour for this masterpiece :)             - \\:::::::/ -          -\n");
    printf("                                               .  '   / '---' \\   '  .\n");
    printf("                                           -        -           -        -\n");
    printf("                                                . '       |       ' .\n");
    printf("                                              /            |            \\   \n");
    printf("\n");
    printf("                                                               *   .  ,\n");
    printf("                                                               ,    *\n");
    printf("                               *                               .    *\n");
    printf("         _/\\_            _/\\_                     .            .        ,\n");
    printf("        //  \\_  \\O/  _//  \\                             *       .\n");
    printf("        ||    |   |   |    ||                       .       ,\n");
    printf("         \\   \\  / \\  /   //                 *                 .\n");
    printf("          \\   \\ \\ / /   //                                  *\n");
    printf("           `    `   `    `                        .\n");
    printf("\033[0m\n");
    sleep_ms(SPLASH_MS * 3); 
}

void kernel_main(void) {
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss); //.bss section initialised to 0. Some bootloders may recognise and 0-clear the .bss section, but, we do it manually too just in case the bootloader doesnt.
    splash();

    //telling the CPU where the exception handler is located
    WRITE_CSR(stvec, (uint32_t) kernel_entry);
    BOOT_OK("trap vector installed");
    BOOT_OK("page allocator ready: %d pages (%d MB)", free_pages(), free_pages() / 256);

    //initializing virtio-blk
    virtio_blk_init();
    BOOT_OK("virtio-blk disk: %d bytes", (int) blk_capacity);

    //initializing filesystem
    int nfiles = fs_init();
    BOOT_OK("tar filesystem: %d files loaded", nfiles);

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
