# IcarusOS Design Document

This document explains *why* IcarusOS is built the way it is: the decisions, the trade-offs, the things that bit me, and how my [custom shell](https://github.com/Hardik01363/custom-shell) ended up inside it. For *what* it does and how to run it, see the [README](README.md).

IcarusOS follows the structure of [*Operating System in 1,000 Lines*](https://1000os.seiya.me/en/) by Seiya Nuta. Where I made a different choice or added something of my own, I say so.

**Contents**

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [System overview and memory map](#2-system-overview-and-memory-map)
3. [Boot and trap handling](#3-boot-and-trap-handling)
4. [Memory management](#4-memory-management)
5. [Processes and scheduling](#5-processes-and-scheduling)
6. [Virtual memory and user mode](#6-virtual-memory-and-user-mode)
7. [System calls](#7-system-calls)
8. [Disk and filesystem](#8-disk-and-filesystem)
9. [The shell and how my custom shell fits in](#9-the-shell-and-how-my-custom-shell-fits-in)
10. [Crashes, the demo tour and the boot log](#10-crashes-the-demo-tour-and-the-boot-log)
11. [Challenges](#11-challenges)
12. [Trade-off summary](#12-trade-off-summary)
13. [Known limitations and what I'd do next](#13-known-limitations-and-what-id-do-next)

---

## 1. Goals and non-goals

**Goals**

- Understand every line. If I can't explain a piece of the kernel, it doesn't belong in it.
- Cover the full path from power-on to a user-mode program that touches a disk: boot, traps, paging, processes, syscalls, a device driver, a filesystem.
- Stay small (~1,700 lines of C) and readable, with comments that say *why*.
- Run on QEMU only, so hardware quirks don't matter.
- Be demoable: every internal (paging, scheduling, faults, the disk) should be something I can *show* an audience, not just claim.

**Non-goals**

- Security, performance, or POSIX compatibility.
- Multi-core, interrupts, or preemption.
- Real hardware.

Nearly every trade-off below follows from these goals: **when simplicity and performance conflict, simplicity wins.**

---

## 2. System overview and memory map

```
   ┌────────────────────────────────────────────┐
   │ shell, spinner, ticker, crash (U-mode)     │   user land
   │  └─ user.c: syscall() via ecall            │
   ├────────────────────────────────────────────┤
   │ kernel.c  (S-mode, runs at 0x80200000)     │   kernel
   │  ├─ trap handler, syscalls, fault killer   │
   │  ├─ page allocator, Sv32 page tables       │
   │  ├─ processes + scheduler                  │
   │  └─ virtio-blk driver + tar filesystem     │
   ├────────────────────────────────────────────┤
   │ OpenSBI   (M-mode firmware)                │   provided by QEMU
   ├────────────────────────────────────────────┤
   │ QEMU "virt" machine, virtio-blk disk       │
   └────────────────────────────────────────────┘
```

**Physical memory (from `kernel.ld`)**

| Region | Address | Notes |
|---|---|---|
| Kernel image | `0x80200000` (`__kernel_base`) | `.text`, `.rodata`, `.data`, `.bss` |
| Boot stack | 128 KB after `.bss` (`__stack_top`) | Used before any process exists |
| Free RAM | 64 MB from `__free_ram_start` | Owned by the page allocator |
| virtio-blk MMIO | `0x10001000` | Device registers, not RAM |

**Virtual memory of a user process**

| Region | Address | Notes |
|---|---|---|
| User image | `0x1000000` (`USER_BASE`) | Code, data, bss and the user stack. Every program is linked here and lives in its own address space |
| Kernel | identity-mapped, no `U` bit | Invisible to user mode, present for traps |

The user base is pinned in two places that must agree: `USER_BASE` in `kernel.h` and `. = 0x1000000` in `user.ld`.

---

## 3. Boot and trap handling

**Boot.** `boot()` is a `naked` function placed in `.text.boot` so the linker script's `KEEP(*(.text.boot))` puts it first. It sets `sp` and jumps to `kernel_main`, because a naked function may not have a C prologue that touches a stack that doesn't exist yet. `kernel_main` then zeroes `.bss` itself. Some bootloaders do this, but I can't count on it.

**The trap entry** (`kernel_entry`, registered in `stvec`) is the most delicate code in the kernel. On a trap the CPU gives me *no free register and no kernel stack*, so the entry does this:

1. `csrrw sp, sscratch, sp` swaps the user `sp` with the kernel stack pointer stashed in `sscratch`. One instruction, and now I have a stack.
2. Save all 31 general registers into a `struct trap_frame` on that stack.
3. Recover the original user `sp` from `sscratch` and store it in the frame.
4. Re-arm `sscratch` with the kernel stack top, call `handle_trap(frame)`, restore everything, `sret`.

**Decision: packed `trap_frame`.** The struct is `__attribute__((packed))`. The assembly addresses fields as `4 * n(sp)`, so the C struct and the assembly must agree byte for byte. Packing removes the chance of the compiler sneaking in padding. (31 words, no padding needed anyway, but the attribute documents the intent.)

**Decision: the kernel is not re-entrant.** `sscratch` holds the kernel stack only while user code runs. A trap *inside* the kernel would swap `sp` with a stale value. I accept this because the kernel never enables any interrupt source and never expects to fault, so any trap that comes from *kernel* mode goes straight to `PANIC` (`handle_trap`). A trap from *user* mode that isn't an `ecall` is a bug in that program, not in the kernel, and only kills that process (§10). Fail loudly when the kernel is wrong, and quietly contain the damage when a program is.

**Helpers I wrote.** `READ_CSR` and `WRITE_CSR` are small macros so the code reads `WRITE_CSR(stvec, ...)` instead of repeating inline assembly everywhere.

---

## 4. Memory management

**Decision: a bump allocator.** `palloc(n)` hands out `n` contiguous 4 KB pages by advancing a static pointer, zeroes them, and panics on exhaustion.

| | |
|---|---|
| **Chosen** | Bump allocator, never frees |
| **Alternative** | Free list or bitmap allocator |
| **Why** | About 10 lines instead of ~60, and nothing in the kernel needs to free yet. Processes never give their memory back (see §5), and page tables and the virtqueue live forever |
| **Cost** | Memory can't be reclaimed. 64 MB is plenty, but every process costs roughly 150 KB (a root table, about 18 second-level tables for the identity-mapped kernel, and the image pages) that never comes back. The `sysinfo` command makes this visible: free pages only go down |

**The bump pointer is a global, not a function-local static,** so that `free_pages()` can report how much is left. That is what `sysinfo` and the demo tour use to show the allocator working.

**Zeroing is load-bearing.** `palloc` zeroes every page. Besides being good hygiene (no leaking stale data between processes), it means the user runtime doesn't need to clear its own `.bss`. The kernel already guarantees it. That contract is written down in a comment in `user.c`.

---

## 5. Processes and scheduling

Each process has a **Process Control Block** (`struct process`): pid, state, a short name (for `ps`), saved kernel `sp`, page table, current working directory, and a **private 8 KB kernel stack**.

### Decision: one kernel stack per process

| | |
|---|---|
| **Chosen** | Each PCB embeds its own 8 KB kernel stack |
| **Alternative** | A single kernel stack shared by all processes |
| **Why** | With a per-process stack, a process blocked inside a syscall (like `getchar`) keeps its whole kernel call chain alive on its own stack. The scheduler can switch away mid-syscall and return later. With one shared stack I'd need continuations or to never block inside the kernel |
| **Cost** | 8 KB × 8 slots of `.bss`, used or not |

### Context switching

`switch_context` is `naked` assembly that saves only `ra` and `s0`–`s11` (13 words), the **callee-saved** registers. It's called like a normal C function, so the compiler has already spilled everything else. Saving less is both faster and less code.

**A trick worth noting:** `create_proc` fabricates a fresh process's stack to *look like* it was already switched out once: 12 zeroed `s`-registers and `ra = user_entry`. The very first `switch_context` into a new process "returns" straight into `user_entry`, which does `sret` into user mode. There is no special first-run path.

### Scheduling: cooperative round-robin

`yield()` scans the 8 slots starting **one past the current process**, picks the first `PROC_RUNNABLE` with `pid > 0`, and falls back to the **idle process** (pid 0) if there is none. It then loads the new page table into `satp` (with `sfence.vma` around it), points `sscratch` at the new process's kernel stack top, and switches.

| | |
|---|---|
| **Chosen** | Cooperative: a process runs until it calls `yield()` or blocks |
| **Alternative** | Preemptive, with timer interrupts |
| **Why** | Preemption needs timer setup via SBI, interrupt-safe kernel code, and a re-entrant trap path (see §3). It's a whole extra subsystem |
| **Cost** | A process that never yields hogs the CPU. With several programs running this is no longer academic: the spinner calls `yield` on every loop iteration, and a program that forgot to would freeze the shell |

**Linear scan over a fixed table.** With `PROCS_MAX = 8`, scanning all slots each time is simpler than maintaining a run queue and just as fast at this scale.

**Decision: a pid is not a slot number.** The scan used to compute the next slot from `pid`, which only worked because `pid = slot + 1`. Once slots are reused that breaks, so `yield` now derives the current slot from the pointer (`currently_running_proc - procs`) and pids come from a counter that only goes up. A pid therefore names one process for the whole run, which is what `kill` and `ps` need. The idle process is forced to pid 0 and the counter is reset to 1 right after it is created.

### Spawning, yielding and killing

Three syscalls turn the scheduler from a hidden mechanism into something the user can drive:

- **`spawn(name)`** looks the program up in a table of embedded binaries (`shell`, `spinner`, `ticker`, `crash`), calls `create_proc`, and then **yields once**. That makes the child run first, so its first output lands before the shell prints its next prompt instead of interleaving with it.
- **`yield()`** is the cooperative handoff. The spinner and ticker call it in a loop and check the clock in between.
- **`kill(pid)`** marks a runnable process `PROC_EXITED`. If a process kills itself it takes the exit path.

| | |
|---|---|
| **Chosen** | Reuse `PROC_EXITED` slots in `create_proc`, and return `NULL` (not `PANIC`) when none is free |
| **Alternative** | Never reuse a slot; panic when they run out |
| **Why** | With 7 usable slots and no reuse, a few spawns in a demo would exhaust them, and a user typing `spawn` too often should get an error message, not a dead kernel |
| **Cost** | Reusing the slot reuses the PCB and kernel stack, but the *memory* of the old process (page tables and image) is still leaked, since the allocator never frees |


### Blocking input without a blocking primitive

The SBI `getchar` call is *non-blocking* (it returns -1 if no key is waiting). `SYS_GETCHAR` turns that into a blocking syscall by looping: call SBI, and if nothing is there, `yield()` and try again. There are no wait queues, and waiting is "yield until it works".

### The idle process and process exit

The idle process (pid 0) is created with `create_proc(NULL, 0)`, so it gets page tables but no user image. It exists so `yield()` always has somewhere to go.

`SYS_EXIT` only sets `state = PROC_EXITED` and yields. The memory is **not** reclaimed (the slot can be reused, see above). When nothing is runnable, the scheduler picks idle, and control returns into `kernel_main` right after its first `yield()`. That used to be a `PANIC`; now `kernel_main` prints `no runnable processes left` and calls `halt()`.

**Decision: shutdown through SBI.** `halt()` first tries the SRST extension (system reset, type shutdown) and then the legacy shutdown call, and only if both return does it fall into a `wfi` loop. On QEMU's `virt` machine either one makes the emulator exit, so the talk can end on `shutdown` instead of `Ctrl-A X`. The same `halt()` backs the `SYS_SHUTDOWN` syscall.

---

## 6. Virtual memory and user mode

### Sv32 page tables

Two-level, 4 KB pages. `map_page` creates second-level tables on demand with `palloc(1)`. Every process gets its own root table.

### Decision: map the kernel into every process

Every process page table **identity-maps the entire kernel and all free RAM** (`__kernel_base` to `__free_ram_end`), *without* the `PAGE_U` bit.

| | |
|---|---|
| **Chosen** | Kernel mapped into every address space, supervisor-only |
| **Alternative** | Separate kernel page table, switching `satp` on every trap |
| **Why** | The trap handler runs on the *interrupted process's* page table. If the kernel weren't mapped there, the first instruction of `kernel_entry` would fault. Identity mapping also means physical addresses from `palloc` can be dereferenced directly |
| **Cost** | Each process's page tables map ~64 MB: about 17 second-level tables plus a root per process (on the order of 70 KB). The user can't touch any of it (no `U` bit), so isolation is preserved |

The virtio-blk MMIO page is mapped the same way (R/W, no `U`).

### The user image

`create_proc` copies the embedded shell image into fresh pages, page by page, and maps them at `USER_BASE` with `U|R|W|X`. The last page is partially filled, so `copy_size` is clamped to the remainder.

**Decision: the user stack lives inside the image.** `user.ld` reserves a 64 KB stack *inside `.bss`*, and the build uses `llvm-objcopy --set-section-flags .bss=alloc,contents` so `.bss` is emitted into the raw binary as real zeros. Result: "load the program" is a single copy loop, and there's no separate stack mapping to manage.

- **Cost:** the binary is ~64 KB bigger than it needs to be, and there is **no guard page**. Overflowing the stack runs off the end of the image into unmapped memory, which faults. That fault reaches `handle_trap`, which now kills just that process (§10).
- **Also:** pages are mapped `RWX`. There's no W^X separation, which is a deliberate simplification.

### Entering user mode

`user_entry` writes `sepc = USER_BASE` and `sstatus = SPIE | SUM`, then `sret`. Two bits matter:

- **`SPIE`** makes `sret` land in user mode with interrupts enabled.
- **`SUM`** (Supervisor User Memory access) lets the *kernel* read and write pages marked `U`. Without it, the moment a syscall like `readfile` tried to copy into the user's buffer, the kernel would fault on the user's own page (see §10).

---

## 7. System calls

### ABI

| | Register |
|---|---|
| Syscall number | `a3` |
| Arguments | `a0`, `a1`, `a2` |
| Return value | `a0` |

The number goes in `a3`, as in the book (not `a7` like Linux). Arguments and results travel through the saved `trap_frame`, so the handler just reads `f->a0` and writes `f->a0` back. After handling, `handle_trap` advances `sepc` by 4; otherwise `sret` would re-execute the `ecall` forever.

### The syscalls

| # | Name | Arguments | Returns |
|---|---|---|---|
| 1 | `SYS_PUTCHAR` | `a0` = char | — |
| 2 | `SYS_GETCHAR` | — | char (blocks via `yield`) |
| 3 | `SYS_EXIT` | — | never |
| 4 | `SYS_READFILE` | name, buf, len | bytes copied (never more than the file's size), or -1 |
| 5 | `SYS_WRITEFILE` | name, buf, len | bytes written (clamped to 1 KB), creating the file if needed, or -1 |
| 6 | `SYS_GETCWD` | buf, len | length of path, or -1 if `buf` too small |
| 7 | `SYS_YIELD` | — | — |
| 8 | `SYS_SPAWN` | name | pid, -1 unknown program, -2 no free slot |
| 9 | `SYS_LISTFILES` | index, name buf, len | size of the index-th file, or -1 past the end |
| 10 | `SYS_PROCINFO` | slot, `struct procinfo *` | 0, or -1 past the last slot |
| 11 | `SYS_SYSINFO` | `struct sysinfo *` | 0 |
| 12 | `SYS_TIME` | — | low 32 bits of the `time` counter (10 MHz) |
| 13 | `SYS_SHUTDOWN` | — | never |
| 14 | `SYS_KILL` | pid | 0, or -1 if no such runnable process |
| 15 | `SYS_GETPID` | — | the caller's pid |

`SYS_READFILE` and `SYS_WRITEFILE` used to share one `case`. They no longer do: the shared version returned the *requested* length on reads and left old bytes behind when a file was overwritten with something shorter (write `hello world`, then `hi`, and `cat` printed `hillo world`). Once `cat` and `write` existed that was visible, so reads now clamp to the file's real size, and writes go through `fs_write`, which zeroes the old contents first.

**Time without 64-bit division.** The `time` CSR is 64 bits wide on RV32, but the kernel is linked with `-nostdlib`, so there is no `__udivdi3` for a 64-bit `/`. `SYS_SYSINFO` computes uptime as `(ticks >> 7) / 78125` (since 10 MHz = 128 × 78125), which stays a 32-bit division. `SYS_TIME` just returns the low 32 bits: wrapping every ~7 minutes is fine because users only ever subtract two readings.

### Adding a syscall is a four-step recipe

The same four places change every time, which is the point of keeping the design boring:

1. Add a `SYS_*` number in `common.h`.
2. Add a wrapper in `user.c`, declared in `user.h`.
3. Add a `case` in `handle_syscall()`.
4. Call the wrapper from the shell.

### Decision: no user-pointer validation

The kernel dereferences pointers from user mode directly (thanks to `SUM`), trusting them.

- **Why:** validation means walking the user's page table to check that every byte of the buffer is mapped, `U`-marked, and writable. That is real code, and it teaches nothing the rest of the OS doesn't.
- **Cost:** a malicious program could pass a kernel address as `buf` and have the kernel write there for it. The kernel pages are mapped, and `SUM` only *adds* access. This is the single biggest security hole, and the first thing I'd fix (§13). Note the difference from the crash demo: a program that writes to a kernel address *itself* is killed by the MMU, but one that hands a kernel address to a syscall like `readfile` is not stopped, because the kernel's own access is allowed.

---

## 8. Disk and filesystem

### virtio-blk driver

virtio is a standard for virtual devices, and QEMU exposes a virtio-blk disk at a fixed MMIO address.

**Decision: the legacy (v1) interface.** The driver checks for device version 1. The legacy interface is slightly simpler than the modern one (no feature negotiation dance, a single queue-address register), and QEMU's `virt` machine speaks it.

**Initialization** follows the spec as a naive but correct sequence: reset, `ACKNOWLEDGE`, `DRIVER`, set page size, set up a virtqueue (`virtq_init`), `DRIVER_OK`. Then read the disk capacity from the config space.

**Requests** use a virtqueue (descriptor table + available ring + used ring). Each request is a chain of 3 descriptors, because each part has different access rules for the device:

1. Header (type + sector): device reads it.
2. Data buffer (512 B): device writes it on reads, reads it on writes.
3. Status byte: device writes it.

The structs are `packed`, since the device and the driver must agree on the layout exactly. MMIO access goes through `volatile` pointers so the compiler can't reorder or elide reads and writes that have side effects.

| | |
|---|---|
| **Chosen** | Synchronous polling: kick the device, then spin until the used ring advances |
| **Alternative** | Interrupt-driven completion |
| **Why** | Interrupts need a PLIC driver, interrupt handling in the trap path, and the re-entrancy I've ruled out |
| **Cost** | The entire system stalls during every disk operation, which is why characters typed during a flush can be dropped (§10). Also only one request can be in flight, so I always reuse the first three descriptors and a single static `blk_req` |

### Filesystem: tar, loaded whole

**Decision: the on-disk format is a tar archive (ustar).**

| | |
|---|---|
| **Chosen** | tar as the filesystem |
| **Alternative** | A custom format such as a superblock + inode table |
| **Why** | The host already has `tar`. `run.sh` builds the disk with one command, and I can inspect it with `tar tvf disk.tar`. No `mkfs` tool to write, and the format is self-describing (header per file, `ustar` magic) |
| **Cost** | No random allocation, no in-place growth or deletion, no directories (yet) |

**Sizing.** `DISK_MAX_SIZE` is now derived from the tar footprint: `FILES_MAX_LOADED * (512-byte header + 1 KB data)`, i.e. 12 KB. `fs_init` panics with a readable message if the disk is smaller than that, and `run.sh` pads `disk.tar` to 16 KB so it never is.

**Boot:** `fs_init` reads the whole image into `disk[]` a sector at a time, then walks the headers. For each one it checks the `ustar` magic (panicking on garbage), parses the octal size field (`octal2int`), and copies name and contents into a slot in the in-memory `files[]` table. Headers are 512 bytes and data is padded to the next 512-byte boundary.

**Write:** `SYS_WRITEFILE` copies into the in-memory file and then calls `fs_flush`, which **rebuilds the entire tar image** in `disk[]` (headers, octal sizes, checksums) and writes every sector back.

| | |
|---|---|
| **Chosen** | Cache everything in RAM, and write the whole image through on every write |
| **Alternative** | Partial, sector-level updates |
| **Why** | The in-memory `files[]` table is always the source of truth, so there is no cache-coherence problem. The flush code is one loop. Nothing can be half-written logically |
| **Cost** | Each `writefile` is O(whole disk). Files are capped at 1 KB and there are at most `FILES_MAX_LOADED` (8) of them. Fine for a demo, nonsense for a real disk |

**Semantics:** `writefile` *replaces* file contents and **creates** the file if the name is new and a slot is free (no append, no seek, no delete). Names are matched with exact `strcmp`, and empty or 100-character names are refused, since an empty name in a tar header means "end of archive". `SYS_LISTFILES` walks the in-use slots, which is all `ls` needs.

**Decision: the disk persists between runs.** `run.sh` only builds `disk.tar` if it doesn't exist (or on `./run.sh fresh`), so `write` a file, quit QEMU, boot again, and `cat` it back. This is what makes the filesystem believable in a demo.

| | |
|---|---|
| **Chosen** | Keep `disk.tar` between runs; QEMU writes straight into it |
| **Alternative** | Rebuild it from `disk/*.txt` on every run |
| **Why** | A disk that forgets everything isn't a disk. The test is the reboot |
| **Cost** | The old debugging lines in `kernel_main` that wrote "hello from kernel" to sector 0 had to go: with a persistent disk they overwrite the first tar header, and the next boot fails. State is also now something to reset on purpose (`./run.sh fresh`) |

---

## 9. The shell and how my custom shell fits in

### The shell is a normal user program

`shell.c` has no special powers. It is built with `user.ld` into `shell.elf`, flattened to `shell.bin`, and then wrapped into an object file with `llvm-objcopy -Ibinary -Oelf32-littleriscv`. That object defines `_binary_shell_bin_start` and `_binary_shell_bin_size`, which the kernel links against and hands to `create_proc`.

| | |
|---|---|
| **Chosen** | Embed every program's binary in the kernel image |
| **Alternative** | Load programs from the disk (needs an executable loader and `exec`) |
| **Why** | With no `exec` or ELF loader, embedding is the shortest path to "a real user-mode process" |
| **Cost** | Programs can't be added without rebuilding the kernel, and there are exactly four (`shell`, `spinner`, `ticker`, `crash`), found by name in `find_prog` |

### Sharing code between kernel and user

`common.c` is compiled into **both** the kernel and every user program. It holds `printf` (`%s %d %x %c %%`), `memcpy`, `memset`, `strcpy`, `strcmp`, and `strlen`. `printf` is written once against an extern `put_char()`; the kernel's `put_char` is an SBI call, and the user's `put_char` is the `SYS_PUTCHAR` syscall. The linker picks the right one per binary, so the same formatting code serves both worlds.

I wrote these myself because the compiler can emit calls to `memcpy`/`memset` on its own, even under `-ffreestanding`, so they have to exist.

### Bringing the custom shell over

Before IcarusOS I wrote [custom-shell](https://github.com/Hardik01363/custom-shell), a shell for Linux in C with built-ins `pwd`, `cd`, `env`, `which` and `echo`. Porting it was a good test of what a shell *really* needs from its OS.

**Every built-in needs either nothing from the kernel, or something specific:**

| Built-in | Needs from the OS | Result in IcarusOS |
|---|---|---|
| `echo` | Nothing. It's pure string handling plus output | **Ported** as shell code |
| `pwd` | The process's current directory | **Ported**, backed by a new syscall |
| `cd` | Directories and `chdir` | Not ported: the filesystem is flat |
| `env` | An environment block | Not ported: the OS has no environment |
| `which` | A `PATH` and executables on disk | Not ported: no `PATH`, no `exec` |

That split was the main lesson: **a built-in only touches the kernel when it touches OS state.** `echo` never enters the kernel beyond printing; `pwd` has to ask.

**What changed in the port**

- **`echo`**: the core logic (the `-n` flag, printing args separated by single spaces, trailing newline) is unchanged from the custom shell. The `$VAR` expansion branch is gone, since there is no environment to expand from.
- **`pwd`**: in the Linux version, `getcwd(NULL, 0)` allocates the result buffer and the shell `free`s it. IcarusOS has no `malloc`, so the port uses a fixed `CWD_MAX` buffer and a `getcwd` syscall that fills it and returns the length (or -1 if it doesn't fit). I kept the function name, the control flow, and the error message shape from the original, so the two read almost identically. The deliberate difference is the signature: POSIX returns `char *`, mine returns `int`, which avoids needing an allocator.
- **Argument handling**: the custom shell's built-ins take a NULL-terminated `char **args` array. IcarusOS's shell originally compared the *whole* command line with `strcmp`, which can't support `echo hello world`. I added a small in-place tokenizer that splits the line on spaces (overwriting them with `'\0'`) and builds the same `args[]` convention, terminated by NULL. With no allocation needed, the tokens are just pointers into the input buffer. The dispatcher now compares `args[0]`.
- **Guard for empty input**: with `args[0]` possibly NULL, an empty line must be handled before any `strcmp`, or the shell would dereference NULL and take the kernel down with it.
- **Style**: I kept the same conventions as the rest of the OS (`if(...) {...}` one-liners, lowercase `//` comments, `else if` chains), so the ported code isn't a stylistic stranger.

**Dispatch is an `if/else` chain, not a table.** The chain was the simplest thing that works with six commands, and it keeps each command's code inline. At fifteen it is about as long as I'd let it get; a table of `{name, function}` pairs is the next step, and `help` would then print itself from it.

### The line editor

Reading a line is now `readline()`, shared by the prompt and the guessing game. It echoes printable characters only and handles three more things:

- **Backspace.** Terminals send `0x7f` (or `0x08`). The editor drops the last character and prints `\b \b` (back, overwrite with a space, back) so the character disappears on screen.
- **History.** The last 8 distinct commands are kept in a small array. Up and down arrive as three-byte escape sequences (`ESC [ A` / `ESC [ B`). To show a recalled line, the editor prints `\r`, the ANSI "erase to end of line" code, the prompt, and the line.
- **Overflow.** Past the buffer size, extra characters ring the terminal bell instead of discarding the whole line.

The prompt colour, `clear`, and the spinner's position all come from plain ANSI escape sequences written through `printf`. There is no terminal driver: the host terminal is the screen.

### Programs besides the shell

The same build recipe makes four programs, and unknown commands fall through to `spawn(name)`, so `spinner` and `spawn spinner` are the same thing.

| Program | What it shows |
|---|---|
| `spinner` | Runs forever, drawing a rotating glyph at the top-right of the screen (save cursor, jump, draw, restore cursor), so the shell stays usable underneath. Its column depends on its pid, so two spinners sit side by side |
| `ticker` | Prints five numbered ticks about 300 ms apart and exits. Two of them interleave line by line |
| `crash` | Stores to a kernel address on purpose |

Both of the first two wait on the clock with `gettime()` and call `yield()` in between: cooperative multitasking in about fifteen lines.

---

## 10. Crashes, the demo tour and the boot log

### A faulting program kills only itself

`handle_trap` used to `PANIC` on anything that wasn't an `ecall`. It now asks *where the trap came from*: `sstatus.SPP` holds the privilege mode the CPU was in before the trap. If it is 0, the fault came from user mode.

| | |
|---|---|
| **Chosen** | On a user-mode fault, print `process N (name) killed: segmentation fault (...)`, mark the process `PROC_EXITED`, and `yield()` |
| **Alternative** | Keep panicking, or try to resume the process |
| **Why** | The trap entry already saved everything and the scheduler already knows how to stop running a process. The process's kernel stack is simply abandoned, which is safe because an exited process is never scheduled again |
| **Cost** | The process's memory is leaked like any other exit. A fault *inside the kernel* still panics: the kernel isn't re-entrant (§3) |

The message includes the cause (`store page fault`, `load page fault`, and so on), `stval` (the faulting address) and `sepc` (the instruction). The `crash` program stores to `0x80200000`. That page *is* mapped in its address space, but without the `U` bit, so the MMU refuses and the process dies while the shell keeps running. That is the whole point of §6, made visible.

### The demo tour

`demo.h` holds `demo_tour()`, compiled in only when `DEMO_TOUR` is defined (uncomment the line at the top of `kernel.c`, or run `DEMO=1 ./run.sh`). It runs after the idle process exists and before the shell starts, and waits for a key between stages:

1. The memory map (kernel image, boot stack, free RAM, MMIO).
2. The page allocator: three `palloc` calls, a check that a fresh page is all zeroes, free pages before and after.
3. **Sv32 paging, live.** It builds a page table, maps one physical page at two virtual addresses, walks the table by hand to print each PTE and its flag bits, then *turns the MMU on* and reads and writes through both virtual addresses. The kernel page's PTE visibly has no `U` bit.
4. The disk: capacity, the first tar header, a write through the driver, then a wipe of the RAM copy and a re-read from the disk image to prove the round trip.
5. User mode and syscalls: two `ticker` processes interleaving, and the fall back to idle when both exit.
6. The `crash` program, and the kernel surviving it.
7. The process table and the free-page count after all that.

| | |
|---|---|
| **Chosen** | A compile-time switch around one function, in its own header |
| **Alternative** | A runtime `demo` shell command, or a comment block to uncomment |
| **Why** | Most of the stages poke at kernel internals (page tables, `satp`, the allocator) that a user-mode shell can't reach. A header the kernel includes can use them directly, and when the switch is off none of it is even compiled |
| **Cost** | It runs before the shell, so it can't be launched mid-session. It also writes `demo.txt` to the disk |

Stage 3 maps its demo pages *without* `U`, on purpose: the kernel may only touch `U` pages when `SUM` is set, and `SUM` is only turned on in `user_entry`.

### The boot log

The boot log is a banner and a checklist (`[ ok ] paging`-style lines) printed by one macro, `BOOT_OK`, after each real step. Each line prints *after* the step it reports succeeded, so a hang points at the step that didn't. A short pause between lines (`SPLASH_MS`, using the `time` counter) makes it readable on stage; set it to 0 to boot instantly. The old test code in `kernel_main` (read sector 0 and print it, write a string over it) is gone.

---

## 11. Challenges

**Debugging with no debugger and no `printf`.** Until `printf` exists you can't print; until traps work you can't see faults. The toolkit that made it tractable was QEMU itself: `-d unimp,guest_errors,int,cpu_reset -D qemu.log` logs every trap and CPU reset, and `--no-reboot` freezes the machine at the moment of a crash instead of looping. Early boot problems, such as a missing `boot()` entry stub, produce no output at all, so the QEMU log is the only witness to what the CPU did.

**The `SUM` bit.** Syscalls that touch user buffers (`readfile`, `writefile`, `getcwd`) fault in the kernel unless `sstatus.SUM` is set. The symptom is a kernel panic on a perfectly reasonable-looking `memcpy`. The fix is one bit, but nothing in the failure obviously points at it.

**Register-level discipline.** Naked functions have no compiler help. The trap entry, `switch_context`, and `boot` are hand-written, and the byte offsets in assembly must match the C struct layouts exactly. Getting one offset wrong corrupts a register in a way that shows up far from the cause.

**Page-table subtleties.** The kernel must be mapped into every process, or the first trap faults. Changing `satp` needs `sfence.vma` on both sides, or the old translations linger.

**The final build errors.** The last round of compile errors before this commit were a good reminder of how C treats declarations. `fs_lookup` used `files[]` *above* the line that declared it, and `virtq_init` was called but never defined anywhere. Since C99 makes implicit function declarations an error, the compiler didn't guess quietly: it assumed `int virtq_init()`, which then clashed with the pointer it was assigned to. One missing function produced two errors. Lesson: in a freestanding build there is no linker-provided safety net, so define before use.

**ANSI escapes and C's hex escapes.** The obvious way to write "save cursor" is `"\x1b7"`. C reads *every* hex digit after `\x`, so that is one escape with the value `0x1b7`, not ESC followed by `7`. The fix is octal (`"\033"` is at most three digits, so `"\0337"` is ESC then `7`). Any `\x1b` followed by a digit or the letters a to f is affected, so every escape sequence in the OS is written in octal now.

**A persistent disk turns debugging code into a bug.** `kernel_main` used to read sector 0 and write `hello from kernel!!!` over it, as a driver test. With the disk rebuilt on every run that was harmless; with a persistent disk it destroys the first tar header, and the next boot sees a corrupt archive. Persistence made me audit everything that writes to the disk.

**`-d int` and polling don't mix.** `run.sh` asked QEMU to log every interrupt and exception (`-d int`). Every SBI `getchar` poll is an exception, and with several processes polling and yielding, that log grows very fast and can slow the emulator down, which is the last thing a timing-based spinner needs. Trap logging is now opt-in (`TRACE=1`).

**Polling has a visible cost.** Because disk I/O spins, the whole system, including console input, is frozen during a flush. Driving the shell with scripted input sent faster than a flush completes loses characters. Typing by hand you'd rarely notice, but it's the clearest demonstration of why real drivers use interrupts.

---

## 12. Trade-off summary

| Area | I chose | Instead of | Because |
|---|---|---|---|
| Memory allocator | Bump, never frees | Free list / bitmap | Nothing needs to free yet |
| Kernel stacks | One per process | One shared | Lets a process block mid-syscall |
| Scheduling | Cooperative, linear scan | Preemptive, run queue | No interrupts or timers needed |
| Kernel mapping | In every address space | Separate kernel table | Traps run on the process's page table |
| User stack | Inside the image's `.bss` | Separately mapped | Loading is one copy loop |
| User memory access | `SUM`, trust pointers | Validate every pointer | Much less code (but unsafe) |
| Disk I/O | Polling, one request | Interrupts, queue depth | No PLIC, no re-entrancy |
| Filesystem | tar, cached in RAM | Custom format | Host tools do the work |
| Write path | Flush whole image | Partial updates | One loop, always consistent |
| Program loading | Embedded binary | Load from disk | No `exec` or ELF loader |
| Shell dispatch | `if/else` chain | Command table | Six commands don't justify a table |
| Port of custom shell | Only what the OS can support | Stub everything | Every command should really work |
| User-mode faults | Kill the process | Panic the kernel | The MMU already caught the bug; the kernel can survive it |
| Process slots | Reuse exited ones | Never reuse | Seven slots run out fast in a demo |
| Spawn | Child runs first (yield once) | Return immediately | Its first output lands before the next prompt |
| Persistence | Keep `disk.tar` between runs | Rebuild every run | A disk that forgets isn't a disk |
| Demo | Compile-time switch + one function | Runtime command | The stages need kernel internals |

---

## 13. Known limitations and what I'd do next

**Correctness gaps I know about**

- **At most 8 files, 1 KB each.** Extra `.txt` files in `disk/` beyond the eighth are dropped from the image on the first flush.
- **No delete, no append, no directories.** `write` replaces a file and `ls` lists a flat namespace.
- **Typing during a disk flush can still drop characters** (and the spinner freezes), because disk I/O polls. After `write`, wait for the `wrote ... bytes` line.
- **Memory is never reclaimed.** Exited and killed processes release their slot but not their pages.
- **The `time` counter wraps** after ~7 minutes in `SYS_TIME` (users only subtract, so this is invisible), and uptime is accurate for about 15 hours.
- **The spinner and `kill` share a screen-position convention** (`SPIN_COL`) instead of any real window system.

**Security and robustness**

- Validate user pointers in syscalls (check mappings and `U` bits before copying).
- Add guard pages around the user stack, and stop mapping everything `RWX`.

**Features**

- **Directories and `cd`:** teach `fs_lookup` to resolve names relative to the process's `cwd` (the field and the `getcwd` syscall already exist), and add `SYS_CHDIR`.
- **File delete and append:** the natural next syscalls, using the same four-step recipe (`ls` and create already exist).
- **Reclaim resources on `exit`:** free the page table and the image pages (the slot is already reused). This needs a real allocator (see §4).
- **Preemptive scheduling:** timer interrupts via SBI, and a trap path that can be re-entered.
- **Load programs from disk:** an `exec` syscall, replacing the `find_prog` table of embedded binaries, at which point `which` and `env` become meaningful for the shell.
- **A command table in the shell,** now that there are fifteen commands.

The whole design, in one sentence: **do the simplest thing that's correct, write down what it costs, and build the next layer on top.** Probably not a good strategy to follow along :)
