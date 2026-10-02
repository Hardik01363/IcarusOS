#pragma once

#include "common.h"

__attribute__((noreturn)) void exit(void);
void put_char(char ch);
int get_char(void);
int readfile(const char *filename, char *buf, int len);
int writefile(const char *filename, const char *buf, int len);
int getcwd(char *buf, int len);
void yield(void);
int spawn(const char *name);
int readdir(const char *path, int idx, char *name);
int procinfo(int idx, struct procinfo *pi);
int sysinfo(struct sysinfo *si);
uint32_t gettime(void);
__attribute__((noreturn)) void shutdown(void);
int kill(int pid);
int getpid(void);
int appendfile(const char *path, const char *buf, int len);
int unlink(const char *path);
int mkdir(const char *path);
int chdir(const char *path);
int pipe_open(const char *name, int mode);
int pipe_write(int id, const char *buf, int len);
int pipe_read(int id, char *buf, int len);
int pipe_close(int id);
int reboot(void);
int poll_char(void);
uint32_t getdate(void);
