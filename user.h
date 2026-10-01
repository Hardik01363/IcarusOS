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
int listfiles(int idx, char *name, int len);
int procinfo(int idx, struct procinfo *pi);
int sysinfo(struct sysinfo *si);
uint32_t gettime(void);
__attribute__((noreturn)) void shutdown(void);
int kill(int pid);
int getpid(void);
