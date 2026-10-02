#include "user.h"

extern char __stack_top[];

//no need to clear the .bss section by filling it with 0's as the kernel already guarantees that when we use the palloc() function

int syscall(int sysno, int arg0, int arg1, int arg2) {
    register int a0 __asm__("a0") = arg0;
    register int a1 __asm__("a1") = arg1;
    register int a2 __asm__("a2") = arg2;
    register int a3 __asm__("a3") = sysno;

    __asm__ __volatile__("ecall"
                         : "=r"(a0)
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3)
                         : "memory");

    return a0;
}

__attribute__((noreturn)) void exit(void) {
    syscall(SYS_EXIT, 0, 0, 0);
    for(;;); //a just in case measure (hopefully doesnt get called)
}

void put_char(char ch) {
    syscall(SYS_PUTCHAR, ch, 0, 0);
}

int get_char(void) {
    return syscall(SYS_GETCHAR, 0, 0, 0);
}

int readfile(const char *filename, char *buf, int len) {
    return syscall(SYS_READFILE, (int) filename, (int) buf, len);
}

int writefile(const char *filename, const char *buf, int len) {
    return syscall(SYS_WRITEFILE, (int) filename, (int) buf, len);
}

int getcwd(char *buf, int len) {
    return syscall(SYS_GETCWD, (int) buf, len, 0);
}

void yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

int spawn(const char *name) {
    return syscall(SYS_SPAWN, (int) name, 0, 0);
}

int readdir(const char *path, int idx, char *name) {
    return syscall(SYS_READDIR, (int) path, idx, (int) name);
}

int procinfo(int idx, struct procinfo *pi) {
    return syscall(SYS_PROCINFO, idx, (int) pi, 0);
}

int sysinfo(struct sysinfo *si) {
    return syscall(SYS_SYSINFO, (int) si, 0, 0);
}

uint32_t gettime(void) {
    return syscall(SYS_TIME, 0, 0, 0);
}

__attribute__((noreturn)) void shutdown(void) {
    syscall(SYS_SHUTDOWN, 0, 0, 0);
    for(;;);
}

int kill(int pid) {
    return syscall(SYS_KILL, pid, 0, 0);
}

int getpid(void) {
    return syscall(SYS_GETPID, 0, 0, 0);
}

int appendfile(const char *path, const char *buf, int len) {
    return syscall(SYS_APPENDFILE, (int) path, (int) buf, len);
}

int unlink(const char *path) {
    return syscall(SYS_UNLINK, (int) path, 0, 0);
}

int mkdir(const char *path) {
    return syscall(SYS_MKDIR, (int) path, 0, 0);
}

int chdir(const char *path) {
    return syscall(SYS_CHDIR, (int) path, 0, 0);
}

int pipe_open(const char *name, int mode) {
    return syscall(SYS_PIPE_OPEN, (int) name, mode, 0);
}

int pipe_write(int id, const char *buf, int len) {
    return syscall(SYS_PIPE_WRITE, id, (int) buf, len);
}

int pipe_read(int id, char *buf, int len) {
    return syscall(SYS_PIPE_READ, id, (int) buf, len);
}

int pipe_close(int id) {
    return syscall(SYS_PIPE_CLOSE, id, 0, 0);
}

int reboot(void) {
    return syscall(SYS_REBOOT, 0, 0, 0);
}

int poll_char(void) {
    return syscall(SYS_POLLCHAR, 0, 0, 0);
}

uint32_t getdate(void) {
    return syscall(SYS_DATE, 0, 0, 0);
}

__attribute__((section(".text.start")))
__attribute__((naked))
void start(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top] \n"
        "call main           \n"
        "call exit           \n"
        :: [stack_top] "r" (__stack_top)
    );
}
