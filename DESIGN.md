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
- Stay small (~2,600 lines of C) and readable, with comments that say *why*.
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
   │  ├─ bitmap allocator, Sv32 page tables     │
   │  ├─ processes, scheduler, pipes            │
   │  └─ virtio-blk driver, tar filesystem      │
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

**Decision: a bitmap allocator.** One bit per 4 KB page of free RAM (16,384 bits for 64 MB, so 2 KB of bitmap). `palloc(n)` finds the first run of `n` free pages, marks it used, zeroes it and returns it. `pfree(addr, n)` clears the bits again and panics on a double free or an address outside free RAM.

| | |
|---|---|
| **Chosen** | Bitmap, first fit, `pfree` supported |
| **Alternative** | The bump allocator it replaced (advance a pointer, never free), or a free list |
| **Why** | A process now gives its memory back when it exits (§5), and the bitmap is the least code that can do that. Contiguous multi-page requests (`palloc(n)`) still work, which a free list of single pages would make awkward. A bitmap also costs nothing per allocation and can't be corrupted by a wild write into freed memory, because the bookkeeping lives somewhere else |
| **Cost** | `palloc` is a linear scan, O(pages). It is fine at this size, where the used pages sit at the start and the scan stops early. First fit fragments over time, though nothing here runs long enough to care |

The allocation count is the thing the audience can *see*: `sysinfo` and `ps` report free pages and per-process pages, and the number goes back up when a process dies. The bitmap size is a compile-time constant (`FREE_RAM_PAGES`, tied to the 64 MB in `kernel.ld`), and `palloc_init` panics if the linker script ever disagrees.

**Zeroing is load-bearing.** `palloc` zeroes every page. Besides being good hygiene (no leaking stale data between processes, now that pages are actually reused), it means the user runtime doesn't need to clear its own `.bss`. The kernel already guarantees it. That contract is written down in a comment in `user.c`. A reused page is re-zeroed on allocation, not on free, so a page can hold stale bytes while it is on the free list and nothing can observe that.

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
| **Cost** | The PCB and its kernel stack are reused as they are. The old process's *memory* is not left behind: `reap` gives it back (see below) |


### Blocking input without a blocking primitive

The SBI `getchar` call is *non-blocking* (it returns -1 if no key is waiting). `SYS_GETCHAR` turns that into a blocking syscall by looping: call SBI, and if nothing is there, `yield()` and try again. There are no wait queues, and waiting is "yield until it works".

### Talking between processes: pipes

Until now two processes could run at the same time but could not talk. A pipe is a small kernel-owned ring buffer (256 bytes) with a name. Four syscalls: `pipe_open(name, mode)`, `pipe_write`, `pipe_read`, `pipe_close`.

| | |
|---|---|
| **Chosen** | *Named* pipes, opened by name for reading or for writing |
| **Alternative** | Anonymous pipes created by `pipe()` and inherited by a child |
| **Why** | There is no `fork`, so nothing could be inherited. Two independently started programs (`producer`, `consumer`) just agree on a name. The `mode` argument matters: opening for *writing* (re)opens the stream, while opening for *reading* never changes it, so a consumer that starts after the producer has already finished still sees the buffered data and then end-of-file |
| **Cost** | Only 4 pipes, one reader and one writer are really supported, and there is no permission check: anyone who knows the name can open it |

Blocking has no primitive behind it, in keeping with the rest of the kernel (see the next subsection). A read on an empty pipe and a write to a full one both loop on `yield()` inside the syscall, like `SYS_GETCHAR`. Each loop re-checks the pipe after it is resumed.

**End-of-file and a dying writer.** A reader that finds the pipe empty *and closed* returns 0 and frees the pipe. The writer closes it with `pipe_close`, but a writer can also die without doing so (killed, or a fault), so the pipe remembers which pid opened it for writing, and `reap` marks it closed when that process goes. A reader blocked on a dead producer therefore wakes up with end-of-file instead of waiting forever.

### The idle process and process exit

The idle process (pid 0) is created with `create_proc(NULL, 0)`, so it gets page tables but no user image. It exists so `yield()` always has somewhere to go.

`SYS_EXIT` only sets `state = PROC_EXITED` and yields; `yield()` does the cleanup (see *Reclaiming memory* below). When nothing is runnable, the scheduler picks idle, and control returns into `kernel_main` right after its first `yield()`. That used to be a `PANIC`; now `kernel_main` prints `no runnable processes left` and calls `halt()`.

### Reclaiming memory: `reap`

A process owns three kinds of pages: its user image pages, the second-level page tables, and the root table. `reap(p)` frees all of them by *walking the page table*:

- for each valid root entry, it visits the second-level table it points to;
- every leaf entry with the `PAGE_U` bit is a page the process owns: free it;
- free the second-level table itself;
- finally free the root.

The identity-mapped kernel and MMIO pages have no `U` bit, so they are skipped. That is the same bit that keeps user code out of kernel memory, and here it also answers "which pages are mine?" without keeping any per-process list. The cost is a walk of about 20 tables of 1,024 entries each, a few tens of thousands of loop iterations per exit.

| | |
|---|---|
| **Chosen** | Reap inside `yield()`, right after `satp` has switched to the next process |
| **Alternative** | Free the pages inside `SYS_EXIT`, or in a later pass over all exited processes |
| **Why** | A process that frees its own page table while it is still the active one is standing on the floor it is removing. By the time `yield` has loaded the next process's `satp`, the old tables are unused, and `reap` makes no allocations, so nothing can overwrite them before the switch finishes. The kernel stack lives in the PCB (static memory), so it is not affected either |
| **Cost** | Killing a *different* process takes a second path: `SYS_KILL` marks it exited and calls `reap` right away, since that process is not running and its tables are not in use |

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

The virtio-blk MMIO page and the goldfish RTC page are mapped the same way (R/W, no `U`). The RTC mapping matters because `SYS_DATE` reads it while the kernel is running on a process's page table.

### The user image

`create_proc` copies the embedded program image into fresh pages, page by page, and maps them at `USER_BASE` with `U|R|W|X`. The last page is partially filled, so `copy_size` is clamped to the remainder.

**Decision: the user stack lives inside the image.** `user.ld` reserves a 64 KB stack *inside `.bss`*, and the build uses `llvm-objcopy --set-section-flags .bss=alloc,contents` so `.bss` is emitted into the raw binary as real zeros. Result: "load the program" is a single copy loop, and there's no separate stack mapping to manage.

- **Cost:** the binary is ~68 KB bigger than it needs to be (the stack plus one page, see below).

### The guard page

Without help, a stack overflow would not fault at all. The stack is the *last* part of the image, and what sits just below it is the program's own `.bss`, `.data` and `.text`, all mapped `RWX`. A runaway recursion would quietly overwrite its own globals and then its own code.

| | |
|---|---|
| **Chosen** | Reserve one extra page just below the stack in `user.ld`, and have `create_proc` *skip* that page when mapping the image |
| **Alternative** | Map the stack as a separate region with an unmapped page next to it |
| **Why** | The stack still lives inside the image, so loading stays one copy loop (see above). The kernel finds the guard with arithmetic, `image_size - USER_STACK_SIZE - PAGE_SIZE`, because the stack is always the last `USER_STACK_SIZE` bytes. No new symbol has to cross from the user ELF to the kernel |
| **Cost** | The constant `USER_STACK_SIZE` exists twice (in `common.h` and in `user.ld`) and has to match. A frame larger than 4 KB could jump over the guard page. The `recurse` program shows the result instead: it dies with a page fault whose address is inside the guard, and the fault is handled like any other user fault (§10) |

The page is never allocated (the image loop just `continue`s over it), so it also costs no RAM.
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
| 4 | `SYS_READFILE` | path, buf, len | bytes copied (never more than the file's size), or -1 |
| 5 | `SYS_WRITEFILE` | path, buf, len | bytes written (clamped to 1 KB), creating the file if needed, or -1 |
| 6 | `SYS_GETCWD` | buf, len | length of path, or -1 if `buf` too small |
| 7 | `SYS_YIELD` | — | — |
| 8 | `SYS_SPAWN` | name | pid, -1 unknown program, -2 no free slot |
| 9 | `SYS_READDIR` | path, index, name buf (100 bytes) | size of the index-th entry (0 for a directory, whose name ends in `/`), -1 past the end, -2 no such directory |
| 10 | `SYS_PROCINFO` | slot, `struct procinfo *` | 0, or -1 past the last slot |
| 11 | `SYS_SYSINFO` | `struct sysinfo *` | 0 |
| 12 | `SYS_TIME` | — | low 32 bits of the `time` counter (10 MHz) |
| 13 | `SYS_SHUTDOWN` | — | never |
| 14 | `SYS_KILL` | pid | 0, or -1 if no such runnable process |
| 15 | `SYS_GETPID` | — | the caller's pid |
| 16 | `SYS_APPENDFILE` | path, buf, len | bytes appended (clamped so the file stays within 1 KB), creating the file if needed, or -1 |
| 17 | `SYS_UNLINK` | path | 0, -1 no such entry, -2 directory not empty |
| 18 | `SYS_MKDIR` | path | 0, -1 parent missing or table full, -2 already exists |
| 19 | `SYS_CHDIR` | path | 0, or -1 if it isn't a directory |
| 20 | `SYS_PIPE_OPEN` | name, mode (0 read, 1 write) | pipe id, or -1 |
| 21 | `SYS_PIPE_WRITE` | id, buf, len | bytes written (blocks while full), or -1 |
| 22 | `SYS_PIPE_READ` | id, buf, len | bytes read (blocks while empty), 0 at end-of-file, or -1 |
| 23 | `SYS_PIPE_CLOSE` | id | 0, or -1 |
| 24 | `SYS_REBOOT` | — | never, or -1 if the firmware can't |
| 25 | `SYS_POLLCHAR` | — | a character, or -1 if none is waiting (never blocks) |
| 26 | `SYS_DATE` | — | seconds since 1970-01-01 from the hardware clock |

`SYS_READFILE` and `SYS_WRITEFILE` used to share one `case`. They no longer do: the shared version returned the *requested* length on reads and left old bytes behind when a file was overwritten with something shorter (write `hello world`, then `hi`, and `cat` printed `hillo world`). Once `cat` and `write` existed that was visible, so reads now clamp to the file's real size, and writes go through `fs_put`, which zeroes the old contents first.

**Time without 64-bit division.** The `time` CSR is 64 bits wide on RV32, but the kernel is linked with `-nostdlib`, so there is no `__udivdi3` for a 64-bit `/`. `SYS_SYSINFO` computes uptime as `(ticks >> 7) / 78125` (since 10 MHz = 128 × 78125), which stays a 32-bit division. `SYS_TIME` just returns the low 32 bits: wrapping every ~7 minutes is fine because users only ever subtract two readings.

### Adding a syscall is a four-step recipe

The same four places change every time, which is the point of keeping the design boring:

1. Add a `SYS_*` number in `common.h`.
2. Add a wrapper in `user.c`, declared in `user.h`.
3. Add a `case` in `handle_syscall()`.
4. Call the wrapper from the shell.

### Decision: validate every user pointer

The kernel dereferences pointers from user mode directly (thanks to `SUM`), so a pointer that arrives in a syscall must be checked *before* the kernel touches it. Without that, a program could pass a kernel address as `buf` and have the kernel write there for it: `SUM` only *adds* access, and the kernel's own pages are mapped.

`uvalid(ptr, len, write)` walks the *current process's* page table for every page the range touches and requires that each is mapped, has the `U` bit, and (if the kernel is going to write) has `W`. It also rejects a range that wraps around the 4 GB boundary. Strings are checked by `ustrlen`, which re-checks at each page boundary while it looks for the `'\0'`, so a string that runs off the end of its mapped pages is refused instead of read.

| | |
|---|---|
| **Chosen** | Check in the kernel, per syscall, before use; return -1 and print `bad pointer in syscall N, rejected` |
| **Alternative** | Let the access fault and recover, or kill the process |
| **Why** | The kernel can't recover from a fault in its own code (the trap handler is not re-entrant, §3). Checking first keeps that rule intact. Returning -1 rather than killing mirrors a real `EFAULT`: a buggy program learns about it, and a probing one doesn't take the machine down |
| **Cost** | A page-table walk per page of every buffer. And it is a time-of-check problem in principle: the mapping could change between the check and the use. That cannot happen here, because there is one CPU, the kernel is not preemptible and nothing unmaps user pages while a syscall runs |

The `evil` program demonstrates it: it passes a kernel address as the buffer of `readfile` and `writefile` and as the name of `spawn`, and a null pointer to `getcwd`. Each call returns -1. Compare with the crash demo (§10): a program that writes to a kernel address *itself* is killed by the MMU, and one that asks the kernel to do it is now refused by the kernel.

**Paths are resolved in the kernel, once.** Every syscall that takes a path first copies it into a kernel buffer (so the user can't change it mid-call) and turns `cwd + path` into a canonical absolute path (§8).

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
| **Cost** | No random allocation and no in-place growth. Every change rewrites the image |

**Sizing.** `DISK_MAX_SIZE` is now derived from the tar footprint: `FILES_MAX_LOADED * (512-byte header + 1 KB data)`, i.e. 24 KB for 16 entries. `fs_init` panics with a readable message if the disk is smaller than that, and `run.sh` pads `disk.tar` to 32 KB so it never is. Directories take a slot and a 512-byte header, with no data.

**Boot:** `fs_init` reads the whole image into `disk[]` a sector at a time, then walks the headers. For each one it checks the `ustar` magic (panicking on garbage), parses the octal size field (`octal2int`), and copies name and contents into a slot in the in-memory `files[]` table. Headers are 512 bytes and data is padded to the next 512-byte boundary.

**Write:** `SYS_WRITEFILE` and `SYS_APPENDFILE` copy into the in-memory file through `fs_put` and then call `fs_flush`, which **rebuilds the entire tar image** in `disk[]` (headers, octal sizes, checksums) and writes every sector back.

| | |
|---|---|
| **Chosen** | Cache everything in RAM, and write the whole image through on every write |
| **Alternative** | Partial, sector-level updates |
| **Why** | The in-memory `files[]` table is always the source of truth, so there is no cache-coherence problem. The flush code is one loop. Nothing can be half-written logically |
| **Cost** | Each `writefile` is O(whole disk). Files are capped at 1 KB and there are at most `FILES_MAX_LOADED` (16) entries. Fine for a demo, nonsense for a real disk |

**Semantics:** `writefile` *replaces* a file's contents and **creates** the file if the name is new and a slot is free. `appendfile` adds to the end (and stops at the 1 KB cap). `unlink` removes a file or an *empty* directory by clearing its slot and flushing. There is no seek. Names are matched with exact `strcmp`, and empty names or names over 96 characters are refused, since an empty name in a tar header means "end of archive".

### Directories and paths

Directories are real tar entries (type `'5'`, name with a trailing `/`), so `tar tvf disk.tar` shows them and the host's `tar` can build a directory tree for the disk. Inside the kernel the table stays **flat**: each slot holds a file or a directory, its name is the whole path (`docs/deep/x.txt`, no leading slash), and "the entries of directory D" means the slots whose name is `D/` plus one more component.

| | |
|---|---|
| **Chosen** | A flat table of full paths, with a parent/child test (`fs_is_child`) |
| **Alternative** | A real tree: per-directory entry lists with inode-style links |
| **Why** | The on-disk format is already a flat sequence of paths. Keeping the in-memory table flat means loading and flushing stay the same loops, and `ls`, `rm` and `mkdir` each become a scan of at most 16 slots |
| **Cost** | Listing a directory scans the whole table, and "does this directory exist" is a lookup. Neither matters at 16 entries. A directory can't be renamed or moved cheaply, because every descendant's name would change, but there is no `mv` anyway |

**Resolving a path** (`path_resolve`) is a small state machine over the components of `cwd + "/" + path` (or just `path` if it starts with `/`). `.` and empty components are skipped, `..` removes the last component already written (and stops at the root), anything else is appended. The result is always canonical: `/` or `/a/b`, never a trailing slash, never `..`. Because it is purely textual it doesn't need the files to exist, and it is the same code for every syscall. `cd` then checks that the result names a directory, and a process's `cwd` is simply that string. Children started with `spawn` inherit it.

The `fs_create` rules are what keep the table consistent: the parent directory must exist, the name must not already exist, and the path must fit. Removing a directory only works when no slot has it as a parent.

**Decision: the disk persists between runs.** `run.sh` only builds `disk.tar` if it doesn't exist (or on `./run.sh fresh`), so `write` a file, quit QEMU, boot again, and `cat` it back. This is what makes the filesystem believable in a demo.

| | |
|---|---|
| **Chosen** | Keep `disk.tar` between runs; QEMU writes straight into it |
| **Alternative** | Rebuild it from `disk/` on every run |
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

**Dispatch is a command table.** With six commands an `if/else` chain was the simplest thing that worked. At twenty-five it was not: each command is now a function `void c_name(int argc, char **a)`, and one table of `{name, function, usage, description}` records drives *three* things: dispatch (a loop with `strcmp`), `help` (it prints the table, so it cannot drift out of date) and tab completion (the same names). A table entry with no description (`mem`, an alias for `sysinfo`) is dispatched but hidden from `help`.

### The line editor

Reading a line is now `readline()`, shared by the prompt and the guessing game. It echoes printable characters only and handles three more things:

- **Backspace.** Terminals send `0x7f` (or `0x08`). The editor drops the last character and prints `\b \b` (back, overwrite with a space, back) so the character disappears on screen.
- **History.** The last 8 distinct commands are kept in a small array. Up and down arrive as three-byte escape sequences (`ESC [ A` / `ESC [ B`). To show a recalled line, the editor prints `\r`, the ANSI "erase to end of line" code, the prompt, and the line.
- **Overflow.** Past the buffer size, extra characters ring the terminal bell instead of discarding the whole line.
- **Tab completion.** On Tab, if the line is a single word, the editor collects every command and program name with that prefix. One match completes it (and adds a space), several matches extend the line to their longest common prefix, and if that doesn't help it lists the candidates and redraws the prompt.

The prompt colour, `clear`, and the spinner's position all come from plain ANSI escape sequences written through `printf`. There is no terminal driver: the host terminal is the screen.

### Programs besides the shell

The same build recipe makes eight programs, and unknown commands fall through to `spawn(name)`, so `spinner` and `spawn spinner` are the same thing.

| Program | What it shows |
|---|---|
| `spinner` | Runs forever, drawing a rotating glyph at the top-right of the screen (save cursor, jump, draw, restore cursor), so the shell stays usable underneath. Its column depends on its pid, so two spinners sit side by side |
| `ticker` | Prints five numbered ticks about 300 ms apart and exits. Two of them interleave line by line |
| `producer`, `consumer` | Talk through the named pipe `demo`: six messages, 200 ms apart. The consumer reads in 16-byte chunks, which don't line up with the 10-byte messages, so the output shows the byte-stream nature of a pipe, then sees end-of-file |
| `crash` | Stores to a kernel address on purpose: stopped by the `U` bit |
| `recurse` | Overflows its own stack: stopped by the guard page |
| `evil` | Hands the kernel addresses it doesn't own: stopped by pointer validation |

Both of `spinner` and `ticker` wait on the clock with `gettime()` and call `yield()` in between: cooperative multitasking in about fifteen lines.

### Two commands that need a non-blocking read

`SYS_GETCHAR` blocks (it yields until a key arrives), which is exactly wrong for a program that must keep doing something while it waits for input. `SYS_POLLCHAR` returns -1 at once if nothing is waiting. Two commands are built on it:

- **`top`** redraws the process table once a second with the same cursor tricks as the spinner (home the cursor, overwrite each line and clear to its end, then clear below). It checks for `q` between redraws and yields while it waits, so everything else keeps running.
- **`snake`** is a 30 x 14 board drawn by cursor addressing. Each frame moves the head, erases the tail, and only redraws the cells that changed. It reads keys without blocking, accepts `wasd` and the arrow keys (`ESC [ A`..`D` is three bytes, read with three polls), refuses a 180-degree turn, and seeds its food placement from the clock with a xorshift generator. A frame is 125 ms of the clock; between frames it yields.

Both live in the shell rather than as separate programs because they need the keyboard, and the keyboard belongs to the shell.

### Clock commands

`uptime` and `sysinfo` use `SYS_SYSINFO`. `date` reads the **goldfish RTC** that QEMU's `virt` machine provides at `0x101000`, which counts nanoseconds since the Unix epoch. Turning that into seconds needs a 64-bit divide, and the kernel has no `__udivdi3`, so `udiv64_32` does shift-and-subtract division. The shell then turns seconds into a calendar date with the standard days-to-civil-date algorithm, which is a handful of 32-bit divisions and no tables. QEMU's RTC reports the host's UTC time, so `date` prints `UTC`.

`reboot` calls the SBI reset extension with type "cold reboot". For that to restart the OS, QEMU must not be run with `--no-reboot`, so `run.sh` leaves that flag off unless `NOREBOOT=1`. Because `disk.tar` persists, `write`, `reboot`, `cat` shows persistence without leaving QEMU.

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
2. **The bitmap allocator:** three `palloc` calls, a zero check, then `pfree` of the middle page and a `palloc` that gets the same page back.
3. **Memory comes back:** spawn a `spinner`, show free pages and the pages it owns, mark it exited and `reap` it, and show the free count return to where it started.
4. **Sv32 paging, live.** It builds a page table, maps one physical page at two virtual addresses, walks the table by hand to print each PTE and its flag bits, then *turns the MMU on* and reads and writes through both virtual addresses. The kernel page's PTE visibly has no `U` bit. The tables are freed afterwards.
5. **Protection:** the `crash` program (the `U` bit), then `recurse` with the address range of its guard page printed first, so the fault address in the next line can be seen to fall inside it.
6. **Pointer validation:** the `evil` program, with the kernel's `bad pointer ... rejected` lines interleaved with its output.
7. The disk: capacity, the first tar header, a write through the driver, then a wipe of the RAM copy and a re-read from the disk image to prove the round trip.
8. **Directories:** a table of `path_resolve` examples (`cwd`, path, result), then `demo-dir/` is created, filled, listed, refused removal while non-empty, and removed.
9. User mode and syscalls: two `ticker` processes interleaving, and the fall back to idle when both exit.
10. **Pipes:** `consumer` starts first and blocks, `producer` sends six messages, and the consumer sees end-of-file.
11. **Clocks:** the time counter, uptime and the RTC.
12. The process table with pages per process, and the final free-page count: nothing leaked.

| | |
|---|---|
| **Chosen** | A compile-time switch around one function, in its own header |
| **Alternative** | A runtime `demo` shell command, or a comment block to uncomment |
| **Why** | Most of the stages poke at kernel internals (page tables, `satp`, the allocator, `reap`) that a user-mode shell can't reach. A header the kernel includes can use them directly, and when the switch is off none of it is even compiled |
| **Cost** | It runs before the shell, so it can't be launched mid-session. It writes `demo.txt` to the disk (and creates and removes `demo-dir`) |

Stage 4 maps its demo pages *without* `U`, on purpose: the kernel may only touch `U` pages when `SUM` is set, and `SUM` is only turned on in `user_entry`.

### The boot log

The boot log is a banner and a checklist (`[ ok ] paging`-style lines) printed by one macro, `BOOT_OK`, after each real step. Each line prints *after* the step it reports succeeded, so a hang points at the step that didn't. A short pause between lines (`SPLASH_MS`, using the `time` counter) makes it readable on stage; set it to 0 to boot instantly. The old test code in `kernel_main` (read sector 0 and print it, write a string over it) is gone. The allocator is initialised first thing in `kernel_main`, before the banner, because everything after it allocates.

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

**Freeing the page table you are standing on.** The obvious place to reclaim a process's memory is inside `SYS_EXIT`. But at that moment the CPU is still running with that process's page table in `satp`. Freeing the pages does no harm by itself (the bytes stay where they are), but any `palloc` that follows would hand one of them out and zero it, and the very next instruction fetch could go through a table that no longer exists. Moving the `reap` into `yield`, after the new `satp` is loaded, removes the hazard without any special case.

**The allocator caught my own bug.** Writing the test for `reap`, I freed the pages a process owned and then freed one of them again by hand. `pfree` panicked with `double free of page ...`, which is exactly the check I'd added for the kernel's sake. The test was wrong, the allocator was right, and I would not have known if `pfree` had silently accepted it.

**An overflow that doesn't fault.** The guard page matters because of what is *below* the stack: the program's own code and data, all mapped read-write-execute. Without it a runaway recursion doesn't crash, it corrupts the program and keeps going. Getting the guard right was arithmetic: the stack is the last `USER_STACK_SIZE` bytes of the image, so the guard is the page before that, and `user.ld` has to align and reserve it, or the page the kernel skips isn't the page that was meant as the guard.

**Time of check, time of use, and why it doesn't apply.** Checking a pointer and then using it is the textbook race. Here the kernel runs one syscall at a time on one CPU, and nothing unmaps user pages in the middle of one, so a check stays true until the syscall returns. The `yield()` inside a *blocking* pipe call is the one place that could have broken this, which is why `sys_pipe_write` keeps reading the user's buffer only after being resumed on the same process's page table.

**Polling has a visible cost.** Because disk I/O spins, the whole system, including console input, is frozen during a flush. Driving the shell with scripted input sent faster than a flush completes loses characters. Typing by hand you'd rarely notice, but it's the clearest demonstration of why real drivers use interrupts.

---

## 12. Trade-off summary

| Area | I chose | Instead of | Because |
|---|---|---|---|
| Memory allocator | Bitmap, first fit, `pfree` | Bump, never frees | Processes now give their memory back |
| Kernel stacks | One per process | One shared | Lets a process block mid-syscall |
| Scheduling | Cooperative, linear scan | Preemptive, run queue | No interrupts or timers needed |
| Kernel mapping | In every address space | Separate kernel table | Traps run on the process's page table |
| User stack | Inside the image's `.bss` | Separately mapped | Loading is one copy loop |
| User memory access | `SUM`, validate every pointer first | Trust pointers | A syscall must not be a way around the `U` bit |
| Disk I/O | Polling, one request | Interrupts, queue depth | No PLIC, no re-entrancy |
| Filesystem | tar, cached in RAM | Custom format | Host tools do the work |
| Write path | Flush whole image | Partial updates | One loop, always consistent |
| Program loading | Embedded binary | Load from disk | No `exec` or ELF loader |
| Shell dispatch | Command table | `if/else` chain | One table drives dispatch, `help` and Tab completion |
| Port of custom shell | Only what the OS can support | Stub everything | Every command should really work |
| User-mode faults | Kill the process | Panic the kernel | The MMU already caught the bug; the kernel can survive it |
| Process slots | Reuse exited ones | Never reuse | Seven slots run out fast in a demo |
| Spawn | Child runs first (yield once) | Return immediately | Its first output lands before the next prompt |
| Persistence | Keep `disk.tar` between runs | Rebuild every run | A disk that forgets isn't a disk |
| Demo | Compile-time switch + one function | Runtime command | The stages need kernel internals |
| Reclaiming memory | Walk the page table, free `U` pages | A per-process list of pages | The page table already says which pages are mine |
| Stack overflow | Unmapped guard page inside the image | Separate stack region | Loading stays one copy loop |
| Directories | Flat table of full paths | A real tree | Matches the tar format; every operation is a short scan |
| Path handling | One `path_resolve`, in the kernel | Per-syscall parsing | One place to get `..` right |
| IPC | Named pipes | Anonymous pipes | There is no `fork` to inherit them through |
| Reboot | `reboot` command, no `--no-reboot` | Always exit QEMU | Persistence can be shown without leaving QEMU |

---

## 13. Known limitations and what I'd do next

**Correctness gaps I know about**

- **At most 16 entries (files and directories), 1 KB per file.** Beyond that, `write` and `mkdir` fail.
- **No rename, no move, no permissions, no timestamps.** `rm` only removes empty directories.
- **Typing during a disk flush can still drop characters** (and the spinner freezes), because disk I/O polls. After `write` or `mkdir`, wait for the `wrote ... bytes` line.
- **Pipes are small and few.** 4 pipes of 256 bytes, one reader and one writer. A pipe whose reader never arrives stays allocated.
- **Pages are reclaimed, but fragmentation isn't managed.** First fit is fine here and would not be for a long-running system.
- **The kernel's own allocations are never freed:** the virtqueue, the request buffer and the disk cache live for the whole run.
- **The `time` counter wraps** after ~7 minutes in `SYS_TIME` (users only subtract, so this is invisible), and uptime is accurate for about 15 hours.
- **`date` is UTC only**, and is only as right as the host's clock.
- **The spinner and `kill` share a screen-position convention** (`SPIN_COL`) instead of any real window system.
- **`USER_STACK_SIZE` is defined twice** (`common.h` and `user.ld`) and must be kept equal by hand.

**Security and robustness**

- Stop mapping every user page `RWX` (W^X).
- Syscall arguments are validated, but a process can still exhaust the 7 process slots or the 4 pipes. There are no per-process limits.
- A user program that never calls `yield` or blocks still freezes the system (no preemption).

**Features**

- **Preemptive scheduling:** timer interrupts via SBI, and a trap path that can be re-entered. This is the biggest remaining step, and the first thing it would fix is the cooperative-scheduling cost above.
- **Load programs from disk:** an `exec` syscall, replacing the `find_prog` table of embedded binaries, at which point `which` and `env` become meaningful for the shell.
- **Interrupt-driven disk I/O** (needs the PLIC), so a flush no longer freezes the console.
- **Rename and move**, and a real in-memory tree if the entry limit goes up.
- **Anonymous pipes and shell pipelines** (`a | b`), once there is some form of `exec`.

The whole design, in one sentence: **do the simplest thing that's correct, write down what it costs, and build the next layer on top.** Probably not a good strategy to follow along :)
