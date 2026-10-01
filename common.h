#pragma once

typedef int bool;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t size_t;
typedef uint32_t paddr_t; //physical address
typedef uint32_t vaddr_t; //virual address

#define true 1
#define false 0
#define NULL ((void *) 0)
#define PAGE_SIZE 4096
#define FILE_DATA_MAX 1024 //max size of a file's contents in bytes
#define CWD_MAX 64 //max length of a process's current working directory path (including the '\0')
#define SYS_PUTCHAR 1 //as it is the first case in the switch-case in handle_syscall() function (in kernel.c). similar reasoning for those below
#define SYS_GETCHAR 2
#define SYS_EXIT 3
#define SYS_READFILE  4
#define SYS_WRITEFILE 5
#define SYS_GETCWD 6
#define SYS_YIELD 7
#define SYS_SPAWN 8
#define SYS_LISTFILES 9
#define SYS_PROCINFO 10
#define SYS_SYSINFO 11
#define SYS_TIME 12
#define SYS_SHUTDOWN 13
#define SYS_KILL 14
#define SYS_GETPID 15
#define PS_UNUSED 0
#define PS_READY 1
#define PS_EXITED 2
#define PS_RUNNING 3
#define TICKS_PER_SEC 10000000
#define SPIN_COL(pid) (80 - 4 * (pid))

//__builtin_'s provided by clang compiler, defined in C standard library's <stdarg.h>.
//we can use compiler builtins without relying on standard library :)
#define align_up(value, align)   __builtin_align_up(value, align)
#define is_aligned(value, align) __builtin_is_aligned(value, align)
#define offsetof(type, member)   __builtin_offsetof(type, member)
#define va_list __builtin_va_list
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg

struct procinfo {
    int pid;
    int state;
    char name[16];
};

struct sysinfo {
    int total_pages;
    int free_pages;
    int uptime_s;
    int procs;
};

void printf(const char* frmtd_str, ...);
void *memset(void *buf, char c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
char *strcpy(char *dst, const char *src);
int strcmp(const char *s1, const char *s2);
size_t strlen(const char *s);
