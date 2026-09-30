# IcarusOS

> *A tiny RISC-V operating system that flew too close to the sun, and then panicked gracefully.*

![arch](https://img.shields.io/badge/arch-RISC--V%20(rv32)-blue)
![language](https://img.shields.io/badge/language-C11-orange)
![runs on](https://img.shields.io/badge/runs%20on-QEMU%20virt-green)
![size](https://img.shields.io/badge/size-~1100%20lines%20of%20C-brightgreen)
![dependencies](https://img.shields.io/badge/libc-none%20(we%20wrote%20printf%20ourselves)-red)

IcarusOS is a from-scratch operating system for 32-bit RISC-V that boots on QEMU, runs a shell in **user mode**, and reads and writes files on a real (virtual) disk. It has **no libc** (`printf` included, written by hand), and no bootloader of its own beyond OpenSBI, which QEMU kindly provides.

It started life by following the excellent [*Operating System in 1,000 Lines*](https://1000os.seiya.me/en/) book by Seiya Nuta, then grew its own shell, its own syscall, and a slightly unhealthy attachment to kaomoji.

```
*(^o^)/* >> hello
Konnichiwa! Welcome to UserLand. This is your trusty shell (^o^)
*(^o^)/* >> echo it works on my machine
it works on my machine
*(^o^)/* >> pwd
/
*(^o^)/* >> writefile
wrote 2560 bytes to disk
*(^o^)/* >> readfile
Konnichiwa! The con
*(^o^)/* >> exit
process 2 exited
PANIC: kernel.c:650: switched to idle process
```

*(Yes, that last line is on purpose. See [Known quirks](#known-quirks).)*

---

## What's inside

| Layer | What IcarusOS does |
|---|---|
| **Boot** | Boots via OpenSBI (`-bios default`) into S-mode at `0x80200000`, sets up a stack, zeroes `.bss`, installs a trap handler |
| **Console** | Output and input through SBI calls. `printf` is hand-written (`%s %d %x %%`) |
| **Traps** | A naked-assembly entry point saves all 31 registers into a `trap_frame` and swaps stacks via `sscratch` |
| **Memory** | A page allocator over 64 MB of RAM, and **Sv32 two-level page tables**, one set per process |
| **Processes** | Up to 8 processes, each with its own page table and its own kernel stack, plus an idle process |
| **Scheduling** | Cooperative round-robin. A process runs until it yields or blocks on input |
| **User mode** | Apps run in U-mode and can only talk to the kernel through `ecall` |
| **Syscalls** | `putchar`, `getchar`, `exit`, `readfile`, `writefile`, `getcwd` |
| **Disk** | A **virtio-blk** device driver (legacy MMIO interface, polling) |
| **Filesystem** | Files live in a **tar (ustar) archive**. Load it at boot, flush it back on write |
| **Shell** | `hello`, `echo [-n]`, `pwd`, `readfile`, `writefile`, `exit` |

The shell isn't a kernel feature. It's a **separate user program**, compiled on its own, turned into a raw binary, and embedded inside the kernel image. The kernel loads it into a fresh address space at boot.

---

## Quick start

### Requirements

- `clang` with the RISC-V target, and `lld`
- `llvm-objcopy`
- `qemu-system-riscv32`
- `tar`

On Ubuntu/Debian:

```bash
sudo apt install clang lld llvm qemu-system-misc
```

On macOS:

```bash
brew install llvm lld qemu
```

### Run it

```bash
git clone https://github.com/Hardik01363/IcarusOS.git
cd IcarusOS
./run.sh
```

`run.sh` builds the shell, builds the kernel around it, packs `disk/*.txt` into a tar image, and starts QEMU.

> **Note:** `run.sh` hardcodes `/usr/bin/clang` and `/usr/bin/llvm-objcopy`. On macOS with Homebrew, change `CC` and `OBJCOPY` at the top of the script to your LLVM paths.

To quit QEMU, press **`Ctrl-A`** and then **`X`**.

### Commands

| Command | What it does |
|---|---|
| `hello` | Greets you from user land |
| `echo [-n] text...` | Prints its arguments. `-n` suppresses the trailing newline |
| `pwd` | Prints the current working directory (via the `getcwd` syscall) |
| `readfile` | Reads `konnichiwa.txt` from the disk |
| `writefile` | Writes to `konnichiwa.txt` and flushes the whole disk image |
| `exit` | Terminates the shell process |

Anything else prints `unknown command: <name>`. The shell is honest like that.

---

## Project layout

```
IcarusOS/
├── kernel.c      # the kernel: boot, traps, memory, processes, virtio, filesystem, syscalls
├── kernel.h      # kernel structs and constants (trap frame, PCB, virtio, tar)
├── kernel.ld     # kernel linker script (memory map)
├── user.c        # user-side runtime: syscall stubs and the program entry point
├── user.h        # user-side API
├── user.ld       # user program linker script
├── shell.c       # the shell (runs in U-mode)
├── common.c/.h   # code shared by kernel and user: printf, mem*/str*, syscall numbers
├── disk/         # files that end up on the virtual disk (*.txt)
└── run.sh        # build + run
```

---

## How it works, in 60 seconds

1. **QEMU** starts OpenSBI, which jumps to `boot()` in S-mode.
2. The kernel zeroes `.bss`, installs the trap vector, brings up **virtio-blk**, and loads the tar disk into RAM.
3. It creates an **idle process** (pid 0), then a **shell process** from the embedded `shell.bin`.
4. `yield()` switches to the shell: new page table loaded into `satp`, kernel stack address into `sscratch`, callee-saved registers swapped.
5. `sret` drops into **user mode** at `0x1000000`.
6. Whenever the shell does an `ecall`, the CPU traps to `kernel_entry`, registers are saved, `handle_trap()` dispatches the syscall, and `sret` returns.

For the long version, including every trade-off and what I'd do differently, read **[design.md](design.md)**.

---

## Known quirks

I'd rather tell you than have you discover them:

- **`exit` ends in a `PANIC`.** When the shell exits, the scheduler falls back to the idle process, and `kernel_main` treats "we ended up in idle" as a panic. It's the OS's way of saying *"there's nothing left to run."* Dramatic, but technically accurate.
- **The disk is flushed whole.** `writefile` rewrites every file in the tar image, synchronously. Input typed during a flush can get dropped, because the kernel is busy polling the disk.
- **Small filesystem.** At most 2 loaded files (`FILES_MAX_LOADED`), up to 1 KB each, names must match exactly, and there are no directories yet. So `pwd` always says `/`, and there's no `cd`.
- **`disk.tar` is rebuilt on every `./run.sh`.** The script re-packs `disk/*.txt`, so writes made inside IcarusOS don't survive the next run. Yes, that means your data has the lifespan of a mayfly.
- **No pointer validation in syscalls.** The kernel trusts pointers passed by user programs. Fine for a toy, not fine for production. See the design doc.

---

## Roadmap (a.k.a. things the next commit might fix)

- [ ] Directories and `cd` (the per-process `cwd` is already in place)
- [ ] `ls`, file creation and deletion
- [ ] Reclaiming memory and process slots after `exit`
- [ ] Validating user pointers in syscalls
- [ ] Timer-driven preemptive scheduling
- [ ] Loading programs from disk instead of embedding them

---

## Credits

- **[Operating System in 1,000 Lines](https://1000os.seiya.me/en/)** by Seiya Nuta. The book is CC BY 4.0, and its source code is MIT-licensed. IcarusOS follows its structure closely, and any good idea in here probably came from it first.
- The [RISC-V SBI specification](https://github.com/riscv-non-isa/riscv-sbi-doc) and the [virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html), which are the real authors of most of the hard parts.
- The shell's `echo` and `pwd` come from my own [custom-shell](https://github.com/Hardik01363/custom-shell), adapted to run without a libc. See [design.md](design.md#9-the-shell-and-how-my-custom-shell-fits-in).

## License

MIT. Do whatever you like, but if it breaks, you get to keep both pieces. ;)
