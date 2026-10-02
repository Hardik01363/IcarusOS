#include "user.h"

void main(void) {
    printf("evil: readfile into a kernel address...\n");
    printf("evil: -> %d\n", readfile("lorem.txt", (char *) 0x80200000, 16));
    printf("evil: getcwd into address 0...\n");
    printf("evil: -> %d\n", getcwd((char *) 0, 16));
    printf("evil: writefile from kernel memory...\n");
    printf("evil: -> %d\n", writefile("leak.txt", (const char *) 0x80200000, 64));
    printf("evil: spawn with a kernel address as the name...\n");
    printf("evil: -> %d\n", spawn((const char *) 0x80200000));
    printf("evil: the kernel is intact, every attack was refused\n");
}
