#!/bin/bash
set -xue

# usage:
#   ./run.sh          build and run (the disk keeps your writes between runs)
#   ./run.sh fresh    rebuild disk.tar from disk/ first (throws away saved writes)
#   DEMO=1 ./run.sh   play the demo tour before the shell starts
#   TRACE=1 ./run.sh  also log every trap to qemu.log (huge, only for debugging)
#   NOREBOOT=1 ./run.sh  make QEMU exit instead of restarting when the guest reboots (debugging)

# QEMU file path
QEMU=qemu-system-riscv32

# Path to clang and compiler flags
CC=/usr/bin/clang
CFLAGS="-std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib"

OBJCOPY=/usr/bin/llvm-objcopy

KDEFS=""
if [ "${DEMO:-}" = "1" ]; then KDEFS="-DDEMO_TOUR"; fi

NR="";
if [ "${NOREBOOT:-}" = "1" ]; then NR="--no-reboot"; fi

DBG="unimp,guest_errors,cpu_reset"
if [ "${TRACE:-}" = "1" ]; then DBG="unimp,guest_errors,int,cpu_reset"; fi

# Build every user program (each one becomes a raw binary embedded in the kernel)
for p in shell spinner ticker crash producer consumer evil recurse; do
    $CC $CFLAGS -Wl,-Tuser.ld -Wl,-Map=$p.map -o $p.elf $p.c user.c common.c
    $OBJCOPY --set-section-flags .bss=alloc,contents -O binary $p.elf $p.bin
    $OBJCOPY -Ibinary -Oelf32-littleriscv $p.bin $p.bin.o
done

# Build the kernel
$CC $CFLAGS $KDEFS -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf \
    kernel.c common.c shell.bin.o spinner.bin.o ticker.bin.o crash.bin.o \
    producer.bin.o consumer.bin.o evil.bin.o recurse.bin.o

# Build the disk only if it is missing (or on `./run.sh fresh`), so files survive reboots.
# Everything in disk/ goes in (files and sub-directories). The image is padded to 32 KB:
# the kernel needs room for FILES_MAX_LOADED entries
if [ "${1:-}" = "fresh" ] || [ ! -f disk.tar ]; then
    (cd disk && tar cf ../disk.tar --format=ustar *)
    dd if=/dev/null of=disk.tar bs=32768 seek=1
fi

# start QEMU
# can check other available machines with -machine '?'
# --no-reboot (NOREBOOT=1) stops the emulator instead of restarting it when the guest reboots (useful for debugging)
# without it, the shell's `reboot` command really restarts the OS and the disk keeps its files
# I'm using virtio-blk with virtio-mmio-bus and virtqueue as the data structure of choice
$QEMU -machine virt -bios default -nographic -serial mon:stdio $NR \
    -d $DBG -D qemu.log \
    -drive id=drive0,file=disk.tar,format=raw,if=none \
    -device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
    -kernel kernel.elf
