# IcarusOS

> *A tiny RISC-V operating system that flew too close to the sun, and then shut down gracefully.*

![arch](https://img.shields.io/badge/arch-RISC--V%20(rv32)-blue)
![language](https://img.shields.io/badge/language-C11-orange)
![runs on](https://img.shields.io/badge/runs%20on-QEMU%20virt-green)
![size](https://img.shields.io/badge/size-~1700%20lines%20of%20C-brightgreen)
![dependencies](https://img.shields.io/badge/libc-none%20(we%20wrote%20printf%20ourselves)-red)

IcarusOS is a from-scratch operating system for 32-bit RISC-V that boots on QEMU, runs a shell in **user mode**, runs **several programs at once**, survives a program crashing, and reads and writes files on a real (virtual) disk that **keeps them between boots**. It has **no libc** (`printf` included, written by hand), and no bootloader of its own beyond OpenSBI, which QEMU kindly provides.

It started life by following the excellent [*Operating System in 1,000 Lines*](https://1000os.seiya.me/en/) book by Seiya Nuta, then grew its own shell, its own syscalls, and a slightly unhealthy attachment to kaomoji.

```
            \   |   /
         ~~~ '-.:::.-' ~~~
      <=====( .:::::. )=====>      I c a r u s O S
         ~~~ '-:::::-' ~~~         a tiny RISC-V OS that flew
            /   |   \              too close to the sun

[ ok ] trap vector installed
[ ok ] page allocator ready: 16384 pages (64 MB)
[ ok ] virtio-blk disk: 16384 bytes
[ ok ] tar filesystem: 3 files loaded
[ ok ] idle process created (pid 0)
[ ok ] shell process created (pid 1)
[ ok ] entering user mode

*\(^o^)/* >> echo it works on my machine
it works on my machine
*\(^o^)/* >> write notes.txt ship it on friday
wrote 12288 bytes to disk
*\(^o^)/* >> spawn spinner
*\(^o^)/* >> ps
PID  STATE    NAME
0    ready    idle
1    running  shell
2    ready    spinner
*\(^o^)/* >> spawn crash
crash: writing to kernel memory at 80200000 ...
process 3 (crash) killed: segmentation fault (store page fault, addr=0x80200000, pc=0x010002a4)
*\(^o^)/* >> shutdown

IcarusOS halted. Goodbye (^o^)/
```

*(While the spinner runs, a rotating `|/-\` glyph sits in the top-right corner of the terminal and the prompt stays fully usable. Yes, the crash is on purpose. See [Surviving a crash](#surviving-a-crash).)*

---

## What's inside

| Layer | What IcarusOS does |
|---|---|
| **Boot** | Boots via OpenSBI (`-bios default`) into S-mode at `0x80200000`, sets up a stack, zeroes `.bss`, installs a trap handler, and prints a banner plus a `[ ok ]` boot log |
| **Console** | Output and input through SBI calls. `printf` is hand-written (`%s %d %x %c %%`), and ANSI escape codes give colour, clearing and a cursor-addressed spinner |
| **Traps** | A naked-assembly entry point saves all 31 registers into a `trap_frame` and swaps stacks via `sscratch` |
| **Memory** | A page allocator over 64 MB of RAM, and **Sv32 two-level page tables**, one set per process |
| **Processes** | Up to 8 slots, each with its own page table and its own kernel stack, plus an idle process. Exited slots are reused |
| **Scheduling** | Cooperative round-robin. A process runs until it yields or blocks on input, and the user can `spawn`, `yield` and `kill` |
| **User mode** | Apps run in U-mode and can only talk to the kernel through `ecall` |
| **Faults** | A fault in user mode kills only that process (`segmentation fault`). A fault in the kernel still panics |
| **Syscalls** | `putchar`, `getchar`, `exit`, `readfile`, `writefile`, `getcwd`, `yield`, `spawn`, `listfiles`, `procinfo`, `sysinfo`, `time`, `shutdown`, `kill`, `getpid` |
| **Disk** | A **virtio-blk** device driver (legacy MMIO interface, polling) |
| **Filesystem** | Files live in a **tar (ustar) archive**: up to 8 files of 1 KB. Loaded at boot, flushed back on every write, and **kept across reboots** |
| **Shell** | A line editor with backspace and up/down history, and 15 commands (see below) |
| **Programs** | `shell`, `spinner`, `ticker` and `crash`, each built separately and embedded in the kernel image |
| **Shutdown** | The SBI shutdown call, so QEMU exits cleanly when nothing is left to run |

The shell isn't a kernel feature. It's a **separate user program**, compiled on its own, turned into a raw binary, and embedded inside the kernel image, together with the other three programs. The kernel loads each one into a fresh address space when it is started.

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

`run.sh` builds the four user programs, builds the kernel around them, creates `disk.tar` **if it doesn't exist yet**, and starts QEMU.

| Command | What it does |
|---|---|
| `./run.sh` | Build and run. The disk keeps whatever you wrote last time |
| `./run.sh fresh` | Rebuild `disk.tar` from `disk/*.txt` first, throwing away saved writes |
| `DEMO=1 ./run.sh` | Play the [demo tour](#the-demo-tour) before the shell starts |
| `TRACE=1 ./run.sh` | Also log every trap to `qemu.log` (huge; for debugging only) |

> **Note:** `run.sh` hardcodes `/usr/bin/clang` and `/usr/bin/llvm-objcopy`. On macOS with Homebrew, change `CC` and `OBJCOPY` at the top of the script to your LLVM paths.

> **Upgrading from an older checkout?** Run `./run.sh fresh` once. Your old `disk.tar` is too small for the 8-file filesystem (and may have a damaged first sector), and the kernel will tell you so.

To quit QEMU without using `shutdown`, press **`Ctrl-A`** and then **`X`**.

### Commands

| Command | What it does |
|---|---|
| `hello` | Greets you from user land |
| `echo [-n] text...` | Prints its arguments. `-n` suppresses the trailing newline |
| `pwd` | Prints the current working directory (via the `getcwd` syscall) |
| `ls` | Lists the files on the disk, with sizes |
| `cat <file>` | Prints a file |
| `write <file> <text...>` | Replaces a file's contents with the text, creating the file if it's new, and flushes the disk |
| `spawn <program>` | Runs `spinner`, `ticker` or `crash` in the background. Typing the bare program name works too |
| `ps` | Lists processes: pid, state, name |
| `kill <pid>` | Stops a process |
| `sysinfo` (or `mem`) | Uptime, free memory in pages, number of processes |
| `guess` | A number-guessing game, seeded from the hardware clock |
| `clear` | Clears the screen |
| `help` | Lists the commands |
| `shutdown` | Powers off the machine cleanly |
| `exit` | Terminates the shell process |

Backspace edits the line, and the up and down arrows walk through the last 8 commands. Anything else prints `unknown command: <name>`. The shell is honest like that.

> After `write`, wait for the `wrote ... bytes` line before typing again. The disk driver polls, so the whole system (input included) is frozen while a flush runs.

---

## Demo

### A five-minute demo arc

1. **Boot.** The banner and the `[ ok ]` checklist.
2. **`hello`, `echo`, `help`.** A real shell in user mode.
3. **Files that persist.** `ls`, then `write notes.txt hello from icarus`, wait for `wrote ... bytes`, `cat notes.txt`. Then `shutdown`, run `./run.sh` again, and `cat notes.txt`. The file is still there.
4. **Two things at once.** `spawn spinner`, keep typing (the glyph keeps turning), then `ps`. Add a second spinner and run `ps` again.
5. **Surviving a crash.** `spawn crash`. The program dies with a segmentation fault and the shell answers the next command.
6. **Clean exit.** `kill` the spinners, then `shutdown`.

### The demo tour

For a talk about *how* it works rather than *what* it does, run `DEMO=1 ./run.sh` (or uncomment `//#define DEMO_TOUR` at the top of `kernel.c`). Before the shell starts, the kernel walks through its own powers one at a time, waiting for a key press between stages:

1. the memory map
2. the page allocator
3. Sv32 paging, live (the MMU is switched on and one physical page is read through two virtual addresses)
4. the disk driver and filesystem round trip
5. user mode, syscalls and two processes interleaving
6. a crashing program that doesn't take the kernel down
7. the process table

### Surviving a crash

The `crash` program stores to a kernel address. That page is mapped in its address space, but without the `U` bit, so the MMU refuses the store, the kernel sees a user-mode fault, kills just that process, and carries on. That is what the `U` bit buys you.

---

## Project layout

```
IcarusOS/
├── kernel.c      # the kernel: boot, traps, memory, processes, virtio, filesystem, syscalls
├── kernel.h      # kernel structs and constants (trap frame, PCB, virtio, tar)
├── demo.h        # the demo tour (compiled only with DEMO_TOUR)
├── kernel.ld     # kernel linker script (memory map)
├── user.c        # user-side runtime: syscall stubs and the program entry point
├── user.h        # user-side API
├── user.ld       # user program linker script
├── shell.c       # the shell (runs in U-mode)
├── spinner.c     # background spinner (runs in U-mode)
├── ticker.c      # prints five ticks and exits (runs in U-mode)
├── crash.c       # writes to kernel memory on purpose (runs in U-mode)
├── common.c/.h   # code shared by kernel and user: printf, mem*/str*, syscall numbers
├── disk/         # files that end up on the virtual disk (*.txt)
└── run.sh        # build + run
```

---

## How it works, in 60 seconds

1. **QEMU** starts OpenSBI, which jumps to `boot()` in S-mode.
2. The kernel zeroes `.bss`, installs the trap vector, brings up **virtio-blk**, and loads the tar disk into RAM, logging each step.
3. It creates an **idle process** (pid 0), then a **shell process** from the embedded `shell.bin`.
4. `yield()` switches to the shell: new page table loaded into `satp`, kernel stack address into `sscratch`, callee-saved registers swapped.
5. `sret` drops into **user mode** at `0x1000000`.
6. Whenever a program does an `ecall`, the CPU traps to `kernel_entry`, registers are saved, `handle_trap()` dispatches the syscall, and `sret` returns.
7. If a program does something illegal instead, `handle_trap()` sees that the trap came from user mode, kills that one process, and picks another to run.
8. When no process is left, control falls back to the idle process, the kernel says so, and the machine powers off.

For the long version, including every trade-off and what I'd do differently, read **[DESIGN.md](DESIGN.md)**.

---

## Known quirks

I'd rather tell you than have you discover them:

- **The disk is flushed whole.** `write` rewrites every file in the tar image, synchronously. Input typed during a flush can get dropped (and the spinner pauses), because the kernel is busy polling the disk.
- **Small filesystem.** At most 8 files, up to 1 KB each, names must match exactly, and there are no directories yet. So `pwd` always says `/`, and there's no `cd`. There's also no delete and no append.
- **Memory is never given back.** Killing or exiting a process frees its slot, but not its pages. `sysinfo` will show free pages only going down.
- **Cooperative scheduling.** A program that never calls `yield` (or blocks on input) would freeze everything else.
- **No pointer validation in syscalls.** The kernel trusts pointers passed by user programs. A program can't *write* to kernel memory itself (see the crash demo), but it can ask the kernel to do it for it. Fine for a toy, not fine for production. See the design doc.
- **`disk.tar` persists on purpose.** To go back to the original files, run `./run.sh fresh`.

---

## Roadmap (a.k.a. things the next commit might fix)

- [x] Run more than one process at once (`spawn`, `yield`, `ps`)
- [x] Kill a faulting process instead of panicking the kernel
- [x] `ls`, `cat`, and file creation
- [x] Persistent disk across reboots
- [x] A clean `shutdown` instead of ending in a panic
- [ ] Directories and `cd` (the per-process `cwd` is already in place)
- [ ] File deletion and append
- [ ] Reclaiming memory (page tables and images) after `exit`
- [ ] Validating user pointers in syscalls
- [ ] Timer-driven preemptive scheduling
- [ ] Loading programs from disk instead of embedding them

---

## Credits

- **[Operating System in 1,000 Lines](https://1000os.seiya.me/en/)** by Seiya Nuta. The book is CC BY 4.0, and its source code is MIT-licensed. IcarusOS follows its structure closely, and any good idea in here probably came from it first.
- The [RISC-V SBI specification](https://github.com/riscv-non-isa/riscv-sbi-doc) and the [virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html), which are the real authors of most of the hard parts.
- The shell's `echo` and `pwd` come from my own [custom-shell](https://github.com/Hardik01363/custom-shell), adapted to run without a libc. See [DESIGN.md](DESIGN.md#9-the-shell-and-how-my-custom-shell-fits-in).

## License

MIT. Do whatever you like, but if it breaks, you get to keep both pieces. ;)
