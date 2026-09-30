#pragma once

#include "common.h"

struct sbi_ret {long error, value;};

#define PANIC(fmt, ...)                                                         \
    do {                                                                        \
        printf("PANIC: %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);   \
        while(1) {}                                                             \
    } while(0)

//macros used in trap handler
//the trap_frame struct is almost all the state of a process we are saving when handling traps
struct trap_frame {
    uint32_t ra; uint32_t gp; uint32_t tp; uint32_t t0;
    uint32_t t1; uint32_t t2; uint32_t t3; uint32_t t4;
    uint32_t t5; uint32_t t6; uint32_t a0; uint32_t a1;
    uint32_t a2; uint32_t a3; uint32_t a4; uint32_t a5;
    uint32_t a6; uint32_t a7; uint32_t s0; uint32_t s1;
    uint32_t s2; uint32_t s3; uint32_t s4; uint32_t s5;
    uint32_t s6; uint32_t s7; uint32_t s8; uint32_t s9;
    uint32_t s10; uint32_t s11; uint32_t sp;
} __attribute__((packed)); //making this packed to save some overall space, no padding needed as they sum up to a good value

//READ_CSR and WRITE_CSR implementations written by me to read/write the csr register (just written for convenience of use, not an integral part of the OS)
#define READ_CSR(reg)                                                          \
    ({                                                                         \
        unsigned long __tmp;                                                   \
        __asm__ __volatile__("csrr %0, " #reg : "=r"(__tmp));                  \
        __tmp;                                                                 \
    })

#define WRITE_CSR(reg, value)                                                  \
    do {                                                                       \
        uint32_t __tmp = (value);                                              \
        __asm__ __volatile__("csrw " #reg ", %0" ::"r"(__tmp));                \
    } while (0)

//Process Control Block (PCB)
//we give a kernel stack to each process, instead of having a single kernel stack for the CPU
#define PROCS_MAX 8
#define PROC_UNUSED 0
#define PROC_RUNNABLE 1
#define STACK_SIZE 8192 //8KB stack for each process

struct process {
    int pid;
    int state; //unused(0) or runnable(1)
    vaddr_t sp;
    uint32_t *page_table;
    uint8_t stack[STACK_SIZE]; //saves process state when context switching or handling trap/syscall
};

//Constructing the Sv32 Page Table
#define SATP_SV32 (1u << 31)
#define PAGE_V    (1 << 0)   //"Valid" bit (of page table, not of OS)
#define PAGE_R    (1 << 1)   //page is readable
#define PAGE_W    (1 << 2)   //page is writable
#define PAGE_X    (1 << 3)   //page is executable
#define PAGE_U    (1 << 4)   //accessible in user mode

//base virtual address of an application image. matches the starting address defined in user.ld
#define USER_BASE 0x1000000
#define SSTATUS_SPIE (1 << 5)

#define SCAUSE_ECALL 8 //to check that if the scause of an exception is 8, it is a syscall and not an illegal use
#define PROC_EXITED 2

//some virtio-related definitions
#define SECTOR_SIZE       512
#define VIRTQ_ENTRY_NUM   16
#define VIRTIO_DEVICE_BLK 2
#define VIRTIO_BLK_PADDR  0x10001000
#define VIRTIO_REG_MAGIC         0x00
#define VIRTIO_REG_VERSION       0x04
#define VIRTIO_REG_DEVICE_ID     0x08
#define VIRTIO_REG_PAGE_SIZE     0x28
#define VIRTIO_REG_QUEUE_SEL     0x30
#define VIRTIO_REG_QUEUE_NUM_MAX 0x34
#define VIRTIO_REG_QUEUE_NUM     0x38
#define VIRTIO_REG_QUEUE_PFN     0x40
#define VIRTIO_REG_QUEUE_READY   0x44
#define VIRTIO_REG_QUEUE_NOTIFY  0x50
#define VIRTIO_REG_DEVICE_STATUS 0x70
#define VIRTIO_REG_DEVICE_CONFIG 0x100
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

//virtqueue descriptor table entry
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

//virtqueue available ring
struct virtq_avail {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

//virtqueue used ring entry
struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

//virtqueue used ring
struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_elem ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

//virtqueue
struct virtio_virtq {
    struct virtq_desc descs[VIRTQ_ENTRY_NUM];
    struct virtq_avail avail;
    struct virtq_used used __attribute__((aligned(PAGE_SIZE)));
    int queue_index;
    volatile uint16_t *used_index;
    uint16_t last_used_index;
} __attribute__((packed));

//virtio-blk request (3 descriptors defined as is used by kernel.c and defined in the specifications)
struct virtio_blk_req {
    //first descriptor (read-only from the device)
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;

    //second descriptor (writable by the device if it's a read operation (VIRTQ_DESC_F_WRITE))
    uint8_t data[512];

    //third descriptor (writable by the device (VIRTQ_DESC_F_WRITE))
    uint8_t status;
} __attribute__((packed));

//defining filesystem structs and functions
#define FILES_MAX_LOADED 2
#define DISK_MAX_SIZE  align_up(sizeof(struct file) * FILES_MAX_LOADED, SECTOR_SIZE)

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
    char data[];
} __attribute__((packed));

struct file {
    bool in_use;
    char name[100];
    char data[1024];
    size_t size;
};
