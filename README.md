# IcarusOS

> *A tiny RISC-V operating system that flew too close to the sun, and then shut down gracefully.*

![arch](https://img.shields.io/badge/arch-RISC--V%20(rv32)-blue)
![language](https://img.shields.io/badge/language-C11-orange)
![runs on](https://img.shields.io/badge/runs%20on-QEMU%20virt-green)
![size](https://img.shields.io/badge/size-~2600%20lines%20of%20C-brightgreen)
![dependencies](https://img.shields.io/badge/libc-none%20(we%20wrote%20printf%20ourselves)-red)

IcarusOS is a from-scratch operating system for 32-bit RISC-V that boots on QEMU, runs a shell in **user mode**, runs **several programs at once**, lets them **talk through pipes**, survives a program crashing or **lying about a pointer**, **gives memory back** when a process dies, and keeps **directories and files** on a real (virtual) disk across reboots. It has **no libc** (`printf` included, written by hand), and no bootloader of its own beyond OpenSBI, which QEMU kindly provides.

It started life by following the excellent [*Operating System in 1,000 Lines*](https://1000os.seiya.me/en/) book by Seiya Nuta, then grew its own shell, its own syscalls, and a slightly unhealthy attachment to kaomoji.

```
\\            |            / 
                                                    ' .       |       . '            
          ICARUS OS                            -        -   .---.   -        -
    please put up with my bad drawing (^.^)        '  .   /:::::::\\   .  '
    took me an hour for this masterpiece :)             - \\:::::::/ -          -
                                                   .  '   / '---' \\   '  .
                                               -        -           -        -
                                                    . '       |       ' .
                                                  /            |            \\   
    
                                                                   *   .  ,
                                                                   ,    *
                                   *                               .    *
             _/\\_            _/\\_                     .            .        ,
            //  \\_  \\O/  _//  \\                             *       .
            ||    |   |   |    ||                       .       ,\n");
             \\   \\  / \\  /   //                 *                 .
              \\   \\ \\ / /   //                                  *
               `    `   `    `                        

[ ok ] trap vector installed
[ ok ] bitmap page allocator ready: 16384 pages (64 MB)
[ ok ] virtio-blk disk: 32768 bytes
[ ok ] tar filesystem: 4 entries loaded
[ ok ] idle process created (pid 0)
[ ok ] shell process created (pid 1)
[ ok ] entering user mode

*\(^o^)/* / >> mkdir notes
wrote 24576 bytes to disk
*\(^o^)/* / >> cd notes
*\(^o^)/* /notes >> write todo.txt ship the demo
wrote 24576 bytes to disk
*\(^o^)/* /notes >> append todo.txt then sleep
wrote 24576 bytes to disk
*\(^o^)/* /notes >> cat todo.txt
ship the demo
then sleep
*\(^o^)/* /notes >> cd /
*\(^o^)/* / >> spawn spinner
*\(^o^)/* / >> ps
PID  STATE    PAGES  NAME
0    ready    20     idle
1    running  40     shell
2    ready    38     spinner
*\(^o^)/* / >> spawn evil
evil: readfile into a kernel address...
process 3 (evil): bad pointer in syscall 4, rejected
evil: -> -1
...
evil: the kernel is intact, every attack was refused
*\(^o^)/* / >> reboot

IcarusOS rebooting...
```

*(While the spinner runs, a rotating `|/-\` glyph sits in the top-right corner of the terminal and the prompt stays fully usable. After `reboot`, the files are still there. See [Demo](#demo).)*

---

## What's inside

| Layer | What IcarusOS does |
|---|---|
| **Boot** | Boots via OpenSBI (`-bios default`) into S-mode at `0x80200000`, sets up a stack, zeroes `.bss`, installs a trap handler, and prints a banner plus a `[ ok ]` boot log |
| **Console** | Output and input through SBI calls. `printf` is hand-written (`%s %d %x %c %%`), and ANSI escape codes give colour, clearing and cursor-addressed screens |
| **Traps** | A naked-assembly entry point saves all 31 registers into a `trap_frame` and swaps stacks via `sscratch` |
| **Memory** | A **bitmap page allocator** over 64 MB of RAM (`palloc` / `pfree`), and **Sv32 two-level page tables**, one set per process |
| **Reclaiming** | When a process exits or is killed, its page table is walked and every page it owned goes back to the allocator |
| **Processes** | Up to 8 slots, each with its own page table and its own kernel stack, plus an idle process. Exited slots are reused |
| **Scheduling** | Cooperative round-robin. A process runs until it yields or blocks on input, and the user can `spawn`, `yield` and `kill` |
| **IPC** | Named **pipes** (256-byte ring buffers) with blocking reads and writes and end-of-file |
| **User mode** | Apps run in U-mode and can only talk to the kernel through `ecall` |
| **Protection** | The `U` bit keeps user code out of kernel memory, a **guard page** under every user stack turns overflows into clean faults, and every **syscall pointer is validated** before the kernel touches it |
| **Faults** | A fault in user mode kills only that process (`segmentation fault`). A fault in the kernel still panics |
| **Syscalls** | 26 of them: console, files and directories, processes, pipes, clocks, power |
| **Disk** | A **virtio-blk** device driver (legacy MMIO interface, polling) |
| **Filesystem** | Files **and directories** live in a **tar (ustar) archive**: up to 16 entries, 1 KB per file. Loaded at boot, flushed back on every change, and **kept across reboots** |
| **Clocks** | A time counter for uptime and timing, and the QEMU goldfish **RTC** for the calendar date |
| **Shell** | A table-driven shell with backspace, up/down history and Tab completion, and 25 commands (see below) |
| **Programs** | `shell`, `spinner`, `ticker`, `crash`, `producer`, `consumer`, `evil` and `recurse`, each built separately and embedded in the kernel image |
| **Power** | `shutdown` and `reboot` through the SBI reset extension |

The shell isn't a kernel feature. It's a **separate user program**, compiled on its own, turned into a raw binary, and embedded inside the kernel image, together with the other seven programs. The kernel loads each one into a fresh address space when it is started.

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

`run.sh` builds the eight user programs, builds the kernel around them, creates `disk.tar` **from everything in `disk/`** if it doesn't exist yet, and starts QEMU.

| Command | What it does |
|---|---|
| `./run.sh` | Build and run. The disk keeps whatever you wrote last time |
| `./run.sh fresh` | Rebuild `disk.tar` from `disk/` first, throwing away saved changes |
| `DEMO=1 ./run.sh` | Play the [demo tour](#the-demo-tour) before the shell starts |
| `TRACE=1 ./run.sh` | Also log every trap to `qemu.log` (huge; for debugging only) |
| `NOREBOOT=1 ./run.sh` | Make QEMU exit instead of restarting when the guest reboots (debugging) |

Put files and sub-directories in `disk/` (for example `disk/docs/readme.txt`) and they appear in the OS after `./run.sh fresh`.

> **Note:** `run.sh` hardcodes `/usr/bin/clang` and `/usr/bin/llvm-objcopy`. On macOS with Homebrew, change `CC` and `OBJCOPY` at the top of the script to your LLVM paths.

> **Upgrading from an older checkout?** Run `./run.sh fresh` once. Your old `disk.tar` is smaller than the new 16-entry filesystem needs (and may contain old test data), and the kernel will tell you so.

To quit QEMU without using `shutdown`, press **`Ctrl-A`** and then **`X`**.

### Commands

| Command | What it does |
|---|---|
| `hello` | Greets you from user land |
| `echo [-n] text...` | Prints its arguments. `-n` suppresses the trailing newline |
| `pwd` | Prints the working directory |
| `cd [dir]` | Changes directory (`.`, `..` and absolute paths work; no argument goes to `/`) |
| `ls [dir]` | Lists a directory, with sizes (directories show as `<dir>`) |
| `cat <file>` | Prints a file |
| `write <file> <text...>` | Replaces a file's contents with the text, creating the file if it's new |
| `append <file> <text...>` | Adds a line to the end of a file |
| `rm <path>` | Removes a file or an *empty* directory |
| `mkdir <dir>` | Creates a directory |
| `spawn <program>` | Runs a program in the background. Typing the bare program name works too |
| `ps` | Lists processes: pid, state, pages owned, name |
| `top` | Live process table, refreshed every second (`q` quits) |
| `kill <pid>` | Stops a process and frees its memory |
| `sysinfo` (or `mem`) | Uptime, free memory in pages, number of processes |
| `uptime` | How long the machine has been up |
| `date` | Date and time (UTC) from the hardware clock |
| `guess` | A number-guessing game, seeded from the hardware clock |
| `snake` | Snake, in the terminal (`wasd` or the arrow keys, `q` quits) |
| `clear` | Clears the screen |
| `help` | Lists the commands, generated from the same table the shell dispatches from |
| `reboot` | Restarts the machine |
| `shutdown` | Powers off the machine cleanly |
| `exit` | Terminates the shell process |

Programs you can `spawn`: `spinner`, `ticker`, `crash`, `producer`, `consumer`, `evil`, `recurse`.

Backspace edits the line, the up and down arrows walk through the last 8 commands, and **Tab** completes command and program names (several matches extend to their common prefix, then list). Anything else prints `unknown command: <name>`. The shell is honest like that.

> After a command that writes to the disk (`write`, `append`, `mkdir`, `rm`), wait for the `wrote ... bytes` line before typing again. The disk driver polls, so the whole system (input included) is frozen while a flush runs.

---

## Demo

### A five-minute demo arc

1. **Boot.** The banner and the `[ ok ]` checklist.
2. **`hello`, `help`, `echo`.** A real shell in user mode. Press Tab to complete.
3. **Files that persist.** `mkdir notes`, `cd notes`, `write todo.txt ship the demo`, `append todo.txt then sleep`, `cat todo.txt`. Then `reboot` and, when it comes back, `cat notes/todo.txt`. The file is still there.
4. **Two things at once.** `spawn spinner`, keep typing (the glyph keeps turning), then `top` to watch the table update live.
5. **Memory comes back.** Run `sysinfo`, `spawn spinner`, `sysinfo` again, `kill` it, `sysinfo` once more. The free-page count goes down by the program's pages and then back up.
6. **Processes that talk.** `spawn consumer`, then `spawn producer`: six messages cross a pipe, and the consumer sees end-of-file.
7. **Three ways to get stopped.** `spawn crash` (the `U` bit), `spawn recurse` (the guard page), `spawn evil` (pointer validation). The shell answers the next command every time.
8. **Play.** `snake`, or `guess`.
9. **Clean exit.** `kill` the spinners, then `shutdown`.

### The demo tour

For a talk about *how* it works rather than *what* it does, run `DEMO=1 ./run.sh` (or uncomment `//#define DEMO_TOUR` at the top of `kernel.c`). Before the shell starts, the kernel walks through its own powers one at a time, waiting for a key press between stages:

1. the memory map
2. the bitmap page allocator (a freed page comes straight back)
3. memory reclaimed when a process dies
4. Sv32 paging, live (the MMU is switched on and one physical page is read through two virtual addresses)
5. protection: a program storing into kernel memory, and one overflowing its stack into the guard page
6. a program passing the kernel addresses it doesn't own, all refused
7. the disk driver and filesystem round trip
8. directories and path resolution (`..`, `.`, repeated slashes)
9. user mode, syscalls and two processes interleaving
10. a pipe between a producer and a consumer
11. the clocks (time counter, uptime, RTC)
12. the process table, and a free-page count that proves nothing leaked

### Three kinds of "no"

- **`crash`** stores to a kernel address. The page is mapped in its address space but without the `U` bit, so the MMU refuses, the kernel sees a user-mode fault, and only that process dies.
- **`recurse`** calls itself until its stack runs out. A guard page sits just below the stack, never mapped, so the overflow becomes a page fault at an address inside it. Without it, the program would silently overwrite its own code.
- **`evil`** asks the *kernel* to read or write kernel memory for it. That would bypass the MMU, since the kernel is allowed to touch everything. Instead every syscall checks the pointers it gets, and each call returns `-1`.

---

## Project layout

```
IcarusOS/
├── kernel.c      # the kernel: boot, traps, memory, processes, pipes, virtio, filesystem, syscalls
├── kernel.h      # kernel structs and constants (trap frame, PCB, pipes, virtio, tar)
├── demo.h        # the demo tour (compiled only with DEMO_TOUR)
├── kernel.ld     # kernel linker script (memory map)
├── user.c        # user-side runtime: syscall stubs and the program entry point
├── user.h        # user-side API
├── user.ld       # user program linker script (reserves the stack and its guard page)
├── shell.c       # the shell (runs in U-mode)
├── spinner.c     # background spinner
├── ticker.c      # prints five ticks and exits
├── producer.c    # writes six messages into a pipe
├── consumer.c    # reads them back out
├── crash.c       # writes to kernel memory on purpose
├── recurse.c     # overflows its own stack on purpose
├── evil.c        # passes bad pointers to syscalls on purpose
├── common.c/.h   # code shared by kernel and user: printf, mem*/str*, syscall numbers
├── disk/         # files and directories that end up on the virtual disk
└── run.sh        # build + run
```

---

## How it works, in 60 seconds

1. **QEMU** starts OpenSBI, which jumps to `boot()` in S-mode.
2. The kernel zeroes `.bss`, sets up the page allocator, installs the trap vector, brings up **virtio-blk**, and loads the tar disk into RAM, logging each step.
3. It creates an **idle process** (pid 0), then a **shell process** from the embedded `shell.bin`.
4. `yield()` switches to the shell: new page table loaded into `satp`, kernel stack address into `sscratch`, callee-saved registers swapped.
5. `sret` drops into **user mode** at `0x1000000`.
6. Whenever a program does an `ecall`, the CPU traps to `kernel_entry`, registers are saved, `handle_trap()` dispatches the syscall (after validating any pointers it carries), and `sret` returns.
7. If a program does something illegal instead, `handle_trap()` sees that the trap came from user mode, kills that one process, and picks another to run.
8. When a process dies, `yield()` waits until it has switched to the next process's page table and then **reaps** the old one: it walks the dead process's page table and frees every page it owned.
9. When no process is left, control falls back to the idle process, the kernel says so, and the machine powers off.

For the long version, including every trade-off and what I'd do differently, read **[DESIGN.md](DESIGN.md)**.

---

## Known quirks

I'd rather tell you than have you discover them:

- **The disk is flushed whole.** Every change rewrites every entry in the tar image, synchronously. Input typed during a flush can get dropped (and the spinner pauses), because the kernel is busy polling the disk.
- **Small filesystem.** At most 16 entries (files and directories), up to 1 KB per file, and no rename, move or timestamps. `rm` only removes empty directories.
- **Small pipes.** 4 pipes of 256 bytes, one reader and one writer each.
- **Cooperative scheduling.** A program that never calls `yield` (or blocks on input) would freeze everything else. Preemption is the next big step.
- **`date` is UTC,** and as right as your computer's clock.
- **`disk.tar` persists on purpose.** To go back to the original files, run `./run.sh fresh`.
- **The kernel's own allocations are never freed:** the virtqueue, the request buffer and the disk cache live for the whole run.

---

## Roadmap (a.k.a. things the next commit might fix)

- [x] Run more than one process at once (`spawn`, `yield`, `ps`, `top`)
- [x] Kill a faulting process instead of panicking the kernel
- [x] `ls`, `cat`, `append`, `rm`, file creation
- [x] Directories and `cd`
- [x] Persistent disk across reboots, and a real `reboot`
- [x] A clean `shutdown` instead of ending in a panic
- [x] Reclaiming memory (page tables and images) after `exit`
- [x] Validating user pointers in syscalls
- [x] A guard page under the user stack
- [x] Pipes between processes
- [x] A command table, Tab completion, and a clock
- [ ] Timer-driven preemptive scheduling
- [ ] Loading programs from disk instead of embedding them (`exec`)
- [ ] Interrupt-driven disk I/O
- [ ] Rename, move, and anonymous pipes with `a | b` in the shell

---

## Credits

- **[Operating System in 1,000 Lines](https://1000os.seiya.me/en/)** by Seiya Nuta. The book is CC BY 4.0, and its source code is MIT-licensed. IcarusOS follows its structure closely, and any good idea in here probably came from it first.
- The [RISC-V SBI specification](https://github.com/riscv-non-isa/riscv-sbi-doc) and the [virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html), which are the real authors of most of the hard parts.
- The shell's `echo` and `pwd` come from my own [custom-shell](https://github.com/Hardik01363/custom-shell), adapted to run without a libc. See [DESIGN.md](DESIGN.md#9-the-shell-and-how-my-custom-shell-fits-in).

## License

MIT. Do whatever you like, but if it breaks, you get to keep both pieces. ;)
