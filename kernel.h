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
    uint8_t stack[STACK_SIZE]; //saves process state when context switching or handling trap/syscall
}
