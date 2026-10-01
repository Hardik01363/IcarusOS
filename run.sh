#!/bin/bash
set -xue

# usage:
#   ./run.sh         - build and run (the disk keeps your writes between runs)
#   ./run.sh fresh   - rebuild disk.tar from disk/*.txt first (throws away saved writes)
#   DEMO=1 ./run.sh  - play the demo tour before the shell starts
#   TRACE=1 ./run.sh - also log every trap to qemu.log (huge, only for debugging)

# QEMU file path
QEMU=qemu-system-riscv32

# Path to clang and compiler flags
CC=/usr/bin/clang
CFLAGS="-std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib"

OBJCOPY=/usr/bin/llvm-objcopy

KDEFS=""
if [ "${DEMO:-}" = "1" ]; then KDEFS="-DDEMO_TOUR"; fi

DBG="unimp,guest_errors,cpu_reset"
if [ "${TRACE:-}" = "1" ]; then DBG="unimp,guest_errors,int,cpu_reset"; fi

# Build every user program (each one becomes a raw binary embedded in the kernel)
for p in shell spinner ticker crash; do
    $CC $CFLAGS -Wl,-Tuser.ld -Wl,-Map=$p.map -o $p.elf $p.c user.c common.c
    $OBJCOPY --set-section-flags .bss=alloc,contents -O binary $p.elf $p.bin
    $OBJCOPY -Ibinary -Oelf32-littleriscv $p.bin $p.bin.o
done

# Build the kernel
$CC $CFLAGS $KDEFS -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf \
    kernel.c common.c shell.bin.o spinner.bin.o ticker.bin.o crash.bin.o

# Build the disk only if it is missing (or on `./run.sh fresh`), so files survive reboots.
# The image is padded to 16 KB: the kernel needs room for FILES_MAX_LOADED files
if [ "${1:-}" = "fresh" ] || [ ! -f disk.tar ]; then
    (cd disk && tar cf ../disk.tar --format=ustar *.txt)
    dd if=/dev/null of=disk.tar bs=16384 seek=1
fi

# start QEMU
# can check other available machines with -machine '?'
# --no-reboot stops emulator without rebooting if vm crashes (useful for debugging)
# I'm using virtio-blk with virtio-mmio-bus and virtqueue as the data structure of choice
$QEMU -machine virt -bios default -nographic -serial mon:stdio --no-reboot \
    -d $DBG -D qemu.log \
    -drive id=drive0,file=disk.tar,format=raw,if=none \
    -device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
    -kernel kernel.elf
