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
10. [Challenges](#10-challenges)
11. [Trade-off summary](#11-trade-off-summary)
12. [Known limitations and what I'd do next](#12-known-limitations-and-what-id-do-next)

---

## 1. Goals and non-goals

**Goals**

- Understand every line. If I can't explain a piece of the kernel, it doesn't belong in it.
- Cover the full path from power-on to a user-mode program that touches a disk: boot, traps, paging, processes, syscalls, a device driver, a filesystem.
- Stay small (~1,100 lines of C) and readable, with comments that say *why*.
- Run on QEMU only, so hardware quirks don't matter.

**Non-goals**

- Security, performance, or POSIX compatibility.
- Multi-core, interrupts, or preemption.
- Real hardware.

Nearly every trade-off below follows from these goals: **when simplicity and performance conflict, simplicity wins.**

---

## 2. System overview and memory map

```
   ┌────────────────────────────────────────────┐
   │ shell.c   (U-mode, runs at 0x1000000)      │   user land
   │  └─ user.c: syscall() via ecall            │
   ├────────────────────────────────────────────┤
   │ kernel.c  (S-mode, runs at 0x80200000)     │   kernel
   │  ├─ trap handler + syscalls                │
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
| User image | `0x1000000` (`USER_BASE`) | Code, data, bss and the user stack |
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

**Decision: the kernel is not re-entrant.** `sscratch` holds the kernel stack only while user code runs. A trap *inside* the kernel would swap `sp` with a stale value. I accept this because the kernel never enables any interrupt source and never expects to fault, so any trap other than a user `ecall` goes straight to `PANIC` (`handle_trap`). Fail loudly, not subtly.

**Helpers I wrote.** `READ_CSR` and `WRITE_CSR` are small macros so the code reads `WRITE_CSR(stvec, ...)` instead of repeating inline assembly everywhere.

---

## 4. Memory management

**Decision: a bump allocator.** `palloc(n)` hands out `n` contiguous 4 KB pages by advancing a static pointer, zeroes them, and panics on exhaustion.

| | |
|---|---|
| **Chosen** | Bump allocator, never frees |
| **Alternative** | Free list or bitmap allocator |
| **Why** | About 10 lines instead of ~60, and nothing in the kernel needs to free yet. Processes never exit for real (see §5), and page tables and the virtqueue live forever |
| **Cost** | Memory can't be reclaimed. 64 MB is plenty for 8 processes, but "exit" is not a real resource release |

**Zeroing is load-bearing.** `palloc` zeroes every page. Besides being good hygiene (no leaking stale data between processes), it means the user runtime doesn't need to clear its own `.bss`. The kernel already guarantees it. That contract is written down in a comment in `user.c`.

---

## 5. Processes and scheduling

Each process has a **Process Control Block** (`struct process`): pid, state, saved kernel `sp`, page table, current working directory, and a **private 8 KB kernel stack**.

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
| **Cost** | A process that never yields hogs the CPU. With only one real process (the shell), this is academic |

**Linear scan over a fixed table.** With `PROCS_MAX = 8`, scanning all slots each time is simpler than maintaining a run queue and just as fast at this scale.

### Blocking input without a blocking primitive

The SBI `getchar` call is *non-blocking* (it returns -1 if no key is waiting). `SYS_GETCHAR` turns that into a blocking syscall by looping: call SBI, and if nothing is there, `yield()` and try again. There are no wait queues, and waiting is "yield until it works".

### The idle process and process exit

The idle process (pid 0) is created with `create_proc(NULL, 0)`, so it gets page tables but no user image. It exists so `yield()` always has somewhere to go.

`SYS_EXIT` only sets `state = PROC_EXITED` and yields. The slot and memory are **not** reclaimed. When the shell exits, nothing is runnable, the scheduler picks idle, and `kernel_main` reports `PANIC: switched to idle process`. That message is this OS's version of "all done"; it's noisy on purpose, because real idle handling (a `wfi` loop) was out of scope.

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

- **Cost:** the binary is ~64 KB bigger than it needs to be, and there is **no guard page**. Overflowing the stack runs off the end of the image into unmapped memory, which faults. That fault reaches `handle_trap` and panics the kernel (there's no "kill the process" path yet).
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
| 4 | `SYS_READFILE` | name, buf, len | bytes copied, or -1 |
| 5 | `SYS_WRITEFILE` | name, buf, len | bytes written, or -1 |
| 6 | `SYS_GETCWD` | buf, len | length of path, or -1 if `buf` too small |

`SYS_READFILE` and `SYS_WRITEFILE` share one `case`. Their setup (look up the file, clamp the length) is identical, and only the copy direction and the flush differ, so a single `if` splits them. Less code, one place to fix bugs.

### Adding a syscall is a four-step recipe

The same four places change every time, which is the point of keeping the design boring:

1. Add a `SYS_*` number in `common.h`.
2. Add a wrapper in `user.c`, declared in `user.h`.
3. Add a `case` in `handle_syscall()`.
4. Call the wrapper from the shell.

### Decision: no user-pointer validation

The kernel dereferences pointers from user mode directly (thanks to `SUM`), trusting them.

- **Why:** validation means walking the user's page table to check that every byte of the buffer is mapped, `U`-marked, and writable. That is real code, and it teaches nothing the rest of the OS doesn't.
- **Cost:** a malicious program could pass a kernel address as `buf` and have the kernel write there for it. The kernel pages are mapped, and `SUM` only *adds* access. This is the single biggest security hole, and the first thing I'd fix (§12).

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

**Boot:** `fs_init` reads the whole image into `disk[]` a sector at a time, then walks the headers. For each one it checks the `ustar` magic (panicking on garbage), parses the octal size field (`octal2int`), and copies name and contents into a slot in the in-memory `files[]` table. Headers are 512 bytes and data is padded to the next 512-byte boundary.

**Write:** `SYS_WRITEFILE` copies into the in-memory file and then calls `fs_flush`, which **rebuilds the entire tar image** in `disk[]` (headers, octal sizes, checksums) and writes every sector back.

| | |
|---|---|
| **Chosen** | Cache everything in RAM, and write the whole image through on every write |
| **Alternative** | Partial, sector-level updates |
| **Why** | The in-memory `files[]` table is always the source of truth, so there is no cache-coherence problem. The flush code is one loop. Nothing can be half-written logically |
| **Cost** | Each `writefile` is O(whole disk). Files are capped at 1 KB and there are at most `FILES_MAX_LOADED` (2) of them. Fine for a demo, nonsense for a real disk |

**Semantics:** `writefile` *replaces* file contents (no append, no seek, no create, no delete). Names are matched with exact `strcmp`.

---

## 9. The shell and how my custom shell fits in

### The shell is a normal user program

`shell.c` has no special powers. It is built with `user.ld` into `shell.elf`, flattened to `shell.bin`, and then wrapped into an object file with `llvm-objcopy -Ibinary -Oelf32-littleriscv`. That object defines `_binary_shell_bin_start` and `_binary_shell_bin_size`, which the kernel links against and hands to `create_proc`.

| | |
|---|---|
| **Chosen** | Embed the shell binary in the kernel image |
| **Alternative** | Load programs from the disk (needs an executable loader and `exec`) |
| **Why** | With no `exec` or ELF loader, embedding is the shortest path to "a real user-mode process" |
| **Cost** | The shell can't be replaced without rebuilding the kernel, and there's only one program |

### Sharing code between kernel and user

`common.c` is compiled into **both** the kernel and the shell. It holds `printf`, `memcpy`, `memset`, `strcpy`, `strcmp`, and `strlen`. `printf` is written once against an extern `put_char()`; the kernel's `put_char` is an SBI call, and the user's `put_char` is the `SYS_PUTCHAR` syscall. The linker picks the right one per binary, so the same formatting code serves both worlds.

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

**Dispatch is an `if/else` chain, not a table.** With six commands, the chain is the simplest thing that works, and it keeps each command's code inline. A table of `{name, function}` pairs would be the right move around ten or more commands, or when `which` needs to enumerate built-ins.

---

## 10. Challenges

**Debugging with no debugger and no `printf`.** Until `printf` exists you can't print; until traps work you can't see faults. The toolkit that made it tractable was QEMU itself: `-d unimp,guest_errors,int,cpu_reset -D qemu.log` logs every trap and CPU reset, and `--no-reboot` freezes the machine at the moment of a crash instead of looping. Early boot problems, such as a missing `boot()` entry stub, produce no output at all, so the QEMU log is the only witness to what the CPU did.

**The `SUM` bit.** Syscalls that touch user buffers (`readfile`, `writefile`, `getcwd`) fault in the kernel unless `sstatus.SUM` is set. The symptom is a kernel panic on a perfectly reasonable-looking `memcpy`. The fix is one bit, but nothing in the failure obviously points at it.

**Register-level discipline.** Naked functions have no compiler help. The trap entry, `switch_context`, and `boot` are hand-written, and the byte offsets in assembly must match the C struct layouts exactly. Getting one offset wrong corrupts a register in a way that shows up far from the cause.

**Page-table subtleties.** The kernel must be mapped into every process, or the first trap faults. Changing `satp` needs `sfence.vma` on both sides, or the old translations linger.

**The final build errors.** The last round of compile errors before this commit were a good reminder of how C treats declarations. `fs_lookup` used `files[]` *above* the line that declared it, and `virtq_init` was called but never defined anywhere. Since C99 makes implicit function declarations an error, the compiler didn't guess quietly: it assumed `int virtq_init()`, which then clashed with the pointer it was assigned to. One missing function produced two errors. Lesson: in a freestanding build there is no linker-provided safety net, so define before use.

**Polling has a visible cost.** Because disk I/O spins, the whole system, including console input, is frozen during a flush. Driving the shell with scripted input sent faster than a flush completes loses characters. Typing by hand you'd rarely notice, but it's the clearest demonstration of why real drivers use interrupts.

---

## 11. Trade-off summary

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

---

## 12. Known limitations and what I'd do next

**Correctness gaps I know about**

- **`DISK_MAX_SIZE` is under-sized for full files.** It's derived from `sizeof(struct file)` (about 1.1 KB per file), but each file's tar footprint is a 512-byte header plus data padded to 512. Two files of 1 KB each would serialize to 3 KB and overflow the 2.5 KB `disk[]` buffer in `fs_flush`. Today's files are tiny, so it never triggers. The fix is to size the buffer from the tar footprint.
- **`readfile` returns the requested length, not the file's length** (when the request fits the 1 KB buffer). It works for the shell because the file's bytes are followed by zeros.
- **Only 2 files load** (`FILES_MAX_LOADED = 2`), so extra files in `disk/` are dropped from the image on the first flush.
- **`disk.tar` is rebuilt on each run**, so writes made inside the OS don't persist across runs.
- **`exit` ends in a panic.** It's harmless, but not graceful.

**Security and robustness**

- Validate user pointers in syscalls (check mappings and `U` bits before copying).
- Kill the faulting *process* on a user-mode exception instead of panicking the kernel.
- Add guard pages around the user stack, and stop mapping everything `RWX`.

**Features**

- **Directories and `cd`:** teach `fs_lookup` to resolve names relative to the process's `cwd` (the field and the `getcwd` syscall already exist), and add `SYS_CHDIR`.
- **`ls`, file create and delete:** the natural next syscalls, using the same four-step recipe.
- **Reclaim resources on `exit`:** free the page table, the image pages, and the PCB slot. This needs a real allocator (see §4).
- **Preemptive scheduling:** timer interrupts via SBI, and a trap path that can be re-entered.
- **Load programs from disk:** an `exec` syscall, at which point `which` and `env` become meaningful for the shell.

The whole design, in one sentence: **do the simplest thing that's correct, write down what it costs, and build the next layer on top.** Probably not a good strategy to follow along :)
