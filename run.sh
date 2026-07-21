#!/bin/bash
set -xue

# QEMU file path
QEMU=qemu-system-riscv32
# start QEMU
# can check other available machines with -machine '?'
# --no-reboot stops emulator without rebooting if vm crashes (useful for debugging)
$QEMU -machine virt -bios default -nographic -serial mon:stdio --no-reboot
