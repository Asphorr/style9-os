# style9-os

A BSD `style(9)` hobby kernel for x86_64.  Boots via PVH (QEMU `-kernel`
for ELF64) or Multiboot1/2, runs preemptive multitasking with
Mach-style ports as the IPC primitive.

Monolithic and XNU-shaped — a BSD/Mach hybrid in one address space, not a
microkernel.  Ring-3 programs load from ELF *or* Mach-O containers, and a
Mach-O that declares macOS as its platform runs under a Darwin syscall
personality: the kernel decodes Apple's class-encoded `syscall`s (BSD
calls + Mach traps) directly.  That is the working base for running XNU
binaries — see *XNU binary compatibility* below.

Built from scratch — no upstream tree, no glue from another OS.

## Tree layout

```
arch/amd64/   boot, GDT/IDT/PIC, ISR asm, pmap, syscall entry
kern/         kernel core: tasks, threads, sched, locks, syscalls, loaders, ddb
vm/           physical + virtual memory: pmm, vm_map, vm_object
fs/           block cache, transactions, and the neutral file layer
fs/apfs/      the APFS reader and writer, and its self-tests
fs/fat/       the FAT reader
mach/         Mach IPC: ports, bootstrap, services, launchd, klog
dev/          drivers: tty, kbd, mouse, uart, ata, rtc
user/ lib/    ring-3 programs and libstyle9
test/         boot-time stress harness
```

`vm/` and `fs/` sit beside `kern/` rather than inside it, which is the split
4.4BSD drew and FreeBSD still keeps: `sys/kern` stays flat and large while
`sys/vm` and `sys/fs` are siblings.  The claim is that memory and file systems
are subsystems the kernel core *uses*, not parts of it — the same claim `mach/`
and `dev/` already make here.  Every directory is on the include path, so a
file moving between them changes no `#include` anywhere.

A file system gets a directory of its own, which is that same split one level
down: `fs/` holds what every file system uses — the block cache, the
transaction layer, the neutral layer that dispatches to whichever volume
mounted — and each format holds itself.  APFS is thirty times the size of FAT,
so a flat `fs/` had stopped saying which files belonged to what.

## What's in it

| layer | files | what |
|---|---|---|
| boot | `arch/amd64/boot.S`, `linker.ld` | MB2 header + PVH ELF note, 32→64 mode transition, identity-map low 1 GiB with 2 MiB huge pages |
| gdt | `arch/amd64/gdt.c` | 8-entry GDT laid out for `SYSCALL`/`SYSRET` MSR arithmetic; 104-byte TSS with `rsp0` for the ring-3 → ring-0 stack switch on IRQs/exceptions |
| syscall | `arch/amd64/syscall_entry.S`, `kern/syscall.c` | `SYSCALL` entry stub: stashes user RSP, switches to per-thread kernel stack via `syscall_kernel_rsp`, builds `struct syscall_frame`, calls the C dispatcher, `SYSRETQ` back; MSRs (EFER.SCE / STAR / LSTAR / FMASK) wired in `syscall_init`.  Caller-save registers are restored from the frame on the way out so user code matches the Linux x86_64 ABI |
| syscalls list | `kern/syscall.c` | `SYS_PRINT`, `SYS_EXIT`, `SYS_YIELD`, `SYS_PORT_ALLOC`, `SYS_PORT_DEALLOC`, `SYS_MSG_SEND`, `SYS_MSG_RECV`, `SYS_MSG_RECV_TIMED`, `SYS_MSG_RPC`, `SYS_SPAWN`, `SYS_TASK_ALIVE` -- Mach IPC surface fully exposed plus minimal task lifecycle |
| usermode | `arch/amd64/usermode.c` | per-task PML4 staged at task creation; the launcher sniffs the image's 4-byte magic and routes ELF to `elf_load` or Mach-O to `macho_load` (shared `(task, image, size, &entry)` contract, shared argv/stack/port-injection path); `iretq` lands at the entry RIP with a fresh user stack |
| elf loader | `kern/elf.c` | static ELF64 parser.  Walks PT_LOAD program headers, allocates 4 KiB user pages and maps them via the task's pmap with R/W/X taken from p_flags, copies file data via `pmm_kva_from_pa` of the freshly-allocated frame |
| macho loader | `kern/macho.c`, `tools/elf2macho.c` | XNU binary compat, S1: thin x86-64 Mach-O + fat/universal slice picker, mapping each `LC_SEGMENT_64` the way the ELF loader maps PT_LOAD and resolving the entry from `LC_UNIXTHREAD`/`LC_MAIN`.  The build host has no Darwin cross-toolchain, so the host tool `elf2macho` rewraps a style9 ELF into a spec-shaped Mach-O; `-Ikern` shares the wire structs so loader and converter never drift |
| darwin abi | `kern/darwin.c` | XNU binary compat, S2/S3: a per-task syscall personality, through which **ring 3 can now change the disk** -- `open(2)` honours `O_CREAT`/`O_TRUNC`/`O_APPEND`, `write(2)` on a file descriptor reaches the APFS writer, `unlink(2)` takes a name back out, `mkdir(2)`/`rmdir(2)` make and remove a directory with the mode they were given less this task's `umask(2)`, `chmod(2)`/`fchmod(2)` change one afterwards, a directory can be OPENED (a descriptor that names a place: `read(2)` on it answers EISDIR, and `openat`/`fdopendir`/`fchdir`/`fchmod` are built on it), and a real Apple `dash` redirecting with `>` makes a file that survives the machine being switched off.  **The terminal can be told what to do**: `ioctl(2)` at 54 carries `TIOCGETA`/`TIOCSETA`/`TIOCGWINSZ`, the kernel keeps a `struct termios` in Apple's exact layout (asserted, not assumed -- the size is encoded in the ioctl number), and the line discipline asks the flags instead of assuming them, so a program that turns ICANON and ECHO off reads one keystroke with no Return behind it.  A file and a pipe answer ENOTTY, which is what `isatty(3)` is built out of.  A Mach-O carrying an `LC_BUILD_VERSION` for macOS is tagged `TASK_PERSONALITY_DARWIN`, and `syscall_dispatch` routes it to `darwin_dispatch`, which decodes Apple's class-encoded `%rax` -- class 2 = BSD `write`/`getpid`/`exit` with the carry-flag errno convention; class 1 = Mach `task_self_trap`/`mach_reply_port`/`mach_msg` traps -- and translates each onto the style9 primitive.  The `mach_msg` trap drives the kernel's existing message queue, so a Darwin task does real IPC; the native style9 syscall table is left untouched |
| progreg | `kern/progreg.c` | "program registry" -- two dozen user programs embedded in the kernel image via objcopy, delivered as ELF or (for the Mach-O loader + Darwin demos) Mach-O containers.  `progreg_spawn(name)` creates a task and loads the matching image into it; `SYS_SPAWN` is the userspace door |
| traps | `arch/amd64/idt.c`, `intr.c`, `isr.S` | 48-vector IDT, trap-frame dispatcher, symbolicated autopsy on exception |
| irqs | `arch/amd64/pic.c`, `pit.c` | 8259 remap to 0x20/0x28, PIT @ 100 Hz with quantum tracking |
| clock | `kern/tsc.c`, `clock.c` | rdtsc + PIT-anchored calibration, `uptime_ms`, busy-sleep |
| memory map | `kern/memmap.c` | parses MB1 / MB2 / PVH boot info into one sorted table |
| pmm | `vm/pmm.c` | bitmap page allocator, first-fit, capped at boot identity-map |
| pmap | `arch/amd64/pmap.c` | per-task 4-level page tables (kernel half shared, user half private); `pmap_kenter / kremove / kextract` for kernel mappings, `pmap_enter / remove / extract` for per-task user mappings |
| vm map | `vm/vm.c` | per-task `vm_map` records anon user-VA ranges; `vm_map_find_space` picks free holes, `vm_map_release_anon` walks `VME_F_ANON` entries at task teardown and `pmm_free_page`s each leaf before `pmap_destroy` rips the page-table tree |
| kmem | `kern/kmem.c` | power-of-two bucketed allocator with `0xFE` red zones around each chunk and `0xDE` freelist poison; tripwires catch heap UAF + OOB writes |
| ddb | `kern/ddb.c`, `kprintf.c` | in-kernel debugger.  `ps`, `s task / thread / sched / ports / vm / mem / locks` introspection on whatever the kernel was doing when it dropped |
| panic | `kern/panic.c` | KASSERT, RBP-chain backtrace with symbolicated frames, fault/panic autopsy |
| spinlock + witness | `kern/spinlock.c`, `kern/witness.c` | holder tracking, preempt-counter integration, WITNESS-lite lock-order graph that screams on cycle attempt |
| ports | `mach/port_object.c`, `mach/port_space.c`, `mach/port_msg.c` | Mach `mach_msg_header_t` wire format, SEND / RECEIVE / SEND_ONCE rights, port_descriptor + OOL descriptor in messages, blocking send on full queue, port sets (`port_set_allocate / insert / remove` for recv-on-many), no-senders DEAD notification, `MOVE_RECEIVE` rebinding |
| OOL descriptors | `mach/port_msg.c` (`send_capture_ool`, `recv_install_ool`) | bulk-memory descriptor carries `{type, copy, deallocate, size, address}` (16 B, packed).  Variable-stride wire format: byte 0 of every descriptor is the type tag (PORT=0 / OOL=1).  Send copies sender bytes into a kmalloc'd staging buffer; recv allocates fresh user-VA in the receiver via `vm_map_find_space`, copies bytes into pmm-allocated frames, `pmap_enter`s them, `vm_map_enter`s the range, patches the descriptor's `address` to the receiver VA |
| recv timeout | `mach/port_msg.c`, `kern/sched.c` | `mach_msg_recv_timed(..., timeout_ms)` parks the thread on the port's waiter list AND on a global sleeper list; PIT IRQ tail (`sched_check_timeouts`) walks the list under `spin_trylock` and posts an IRQ wake for any thread past its deadline.  Returns `MACH_E_TIMEOUT` on expiry; the wake path absorbs the sender-vs-PIT race by re-checking the queue after detach |
| rpc | `mach/port_msg.c` | `mach_msg_rpc(req, reply_buf, ..., timeout_ms)` allocates a fresh reply port, splices it into `req->msgh_local` with `MAKE_SEND` (preserving `MACH_MSGH_BITS_COMPLEX` so body descriptors survive the recompute), sends, recv-timeds the reply, deallocs the reply port |
| inline-reply stash | `mach/port_msg.c` | `mach_msg_rpc` arms the reply port's stash; if the destination is a synchronous dispatcher (special-port intercept), the dispatcher's reply lands in the caller's reply buffer with zero kmalloc and zero enqueue |
| special ports | `mach/port_msg.c` | `p_special` tag (`PORT_SPECIAL_TASK_SELF` / `PORT_SPECIAL_BOOTSTRAP` / `PORT_SPECIAL_SERVICE`) routes a send synchronously to a per-tag dispatcher instead of queueing -- same pattern Mach uses for `task_port` / `host_port` |
| task_self | `kern/task.c`, `mach/port_object.c` | every task carries a `t_self_port`; SEND right at well-known name `MACH_PORT_TASK_SELF=1` in its own `port_space`.  Op `TASK_OP_GET_INFO` returns `{task_id, nthreads, name[32]}` |
| bootstrap | `mach/bootstrap.c` | global service-registry port at `MACH_PORT_BOOTSTRAP=2`.  `bootstrap_register(name, kernel_name)` publishes; op `BOOTSTRAP_OP_LOOKUP` returns a port_descriptor (`COPY_SEND`) to the named service via cross-space install |
| kernel services | `mach/services.c` | `svc/clock` (uptime), `svc/stats` (pmm/kmem/task counts), `svc/tasks` (task list), `svc/echool` (OOL round-trip oracle).  Each is a `PORT_SPECIAL_SERVICE` port with a synchronous dispatcher, registered under its string name in the bootstrap port |
| klog | `mach/klog.c` | structured ring + `klog` Mach service; same machinery as the regular services |
| tasks | `kern/task.c` | resource container, owns one `port_space`, one `pmap`, one `vm_map`.  `task_deref` cascades: `port_space_destroy` → `vm_map_release_anon` (frees user-VA leaves) → `pmap_destroy` (frees page-table tree) → `vm_map_destroy` |
| threads | `kern/thread.c`, `arch/amd64/switch.S` | callee-saved context switch, kstack-backed, per-thread `syscall_kernel_rsp` for SYSCALL entry |
| sched | `kern/sched.c` | cooperative + preemptive round-robin, idle thread reaps zombies, timed-waiter list for `mach_msg_recv_timed` and `task_is_alive` polling |
| dev/NAME | `dev/dev_subsystem.c` | generic driver protocol -- each driver registers a control port under `dev/<short>`; ops `DEV_OP_INFO` (kind + flags) and `DEV_OP_OPEN_STREAM` (returns a stream port via MOVE_RECEIVE).  Ring-3 client wrapper is `dev_open_stream(name)` in libstyle9 |
| kbd drv | `dev/kbd_drv.c` | bridges the PS/2 IRQ ring to a stream port; sh.elf opens `dev/kbd` and recv's keypresses one at a time |
| uart drv | `dev/uart_drv.c` | COM1 RX IRQ to a stream port via the same `dev/uart` protocol |
| ata drv | `dev/ata_drv.c` | LBA28+LBA48 ATA PIO driver, exposed as `dev/disk0`, and the door the filesystems below come in through.  Every write ends in FLUSH CACHE, which is what lets a checkpoint rely on the order it wrote its blocks in |
| block cache | `fs/bio.c` | 4 KiB buffers over `dev/disk0`.  A write goes to the device first and patches the resident page after, rather than invalidating the cache -- otherwise every write would re-read the tree it had just walked |
| fs | `fs/fs.c`, `fs/fs.h` | the neutral layer, and the only door: one volume, one sleeping lock, handles that carry the backend's own name for a file's bytes plus a volume generation, so a length copied before somebody else changed it is noticed rather than trusted.  `fs_slurp / open / pread / pwrite / truncate / stat / readdir / sync` |
| apfs | `fs/apfs/apfs.c` (8.5 kloc), `fs/apfs/apfs_test.c` (2 kloc), `fs/apfs/apfs_priv.h`, `fs/fs_txn.c` | a clean-room APFS **writer**, on a container `mkapfs` made.  Reads: checkpoint ring, object maps, the file-system B-tree **by descending on the key** -- binary search per node, the leftmost path pruned, so a run of records (one file's extents, one directory's names) comes back consecutively -- extents, extended fields.  Writes: Fletcher-64 forwards, an allocator over the chunk bitmaps, copy-on-write of every object from the edited leaf up to the container superblock, checkpoints (the superblock landing is the commit), free queues that hold a released block for as long as an older checkpoint still names it, records inserted and removed, nodes split at any depth, a tree that **grows a level** when its root fills (the root keeps its oid and splits downward, since the volume superblock names it), nodes that **leave the tree** when their last record goes -- cascading up through parents left holding nothing -- edits that span **as many leaves as the records need** with the index keys above them corrected when a node stops starting where its parent said it did, files that grow and shrink, files **created and unlinked** and directories **made and removed** (which is not the same edit with the mode changed: a directory's inode carries no data stream, its entry's type must agree with its mode, and it is counted where a file is not) -- including the directory-entry hash, which is not in any published layout and was recovered from the names already on the volume.  Fourteen boot self-tests -- `apfs-write / alloc / spine / ckpt / data / trunc / grow / make / dirs / shell / split / index / drop / seek` -- the ones that reach inside the format living in a file of their own (`apfs_test.c`, a fifth of everything the format code was) and the rest going through the neutral layer like any other caller, of which `index` and `drop` ARRANGE the shape they need (a node made to start at a record, then a node left holding only one file) rather than waiting for one that used to turn up as a volume the checker rejected, and `seek` is the one apfsck cannot stand in for: it seeks every record on the volume by its own key and demands the same record, out of the same leaf, with the rest of the tree behind it in the same order -- the descent checked against the whole-tree walk it replaced.  `apfsck` from `apfsprogs` is the outside oracle: silent on every image this kernel has written |
| fat | `fs/fat/fat.c` | FAT16/32 reader for the smoke-test image; writes answer `FS_E_ROFS`, which is a different answer from "that write was too ambitious" and means a different thing |
| tty | `dev/tty.c` | VT-style ANSI CSI state machine over the VGA console: CUP/CUU-CUB/ED/EL/SGR, DECSTBM scrolling region, DECTCEM cursor visibility, DEC's deferred wrap (a line exactly 80 columns wide costs one row, not two), and a hardware CRTC cursor programmed once per write rather than once per byte.  Three boot selftests read the CRTC and the cell grid back rather than asking the driver what it believes |
| user shell | `user/sh.c` | sh.elf, the ring-3 shell.  Apple/BSD-flavoured manpage TUI: NAME/SYSTEM/SEE ALSO sections, gray-on-black with bold-white labels, horizontal rule, and a status bar with uptime that lives above the scrolling region so no amount of output can carry it away.  Full line editor (arrows, Home/End, Delete, emacs control keys, 16 lines of history, Tab completion) and a less(1)-shaped pager for `man`.  Builtins: `help / echo / clear / about / ool / man / kill`.  Spawnable: any program in the registry, listed via `svc/progreg` |
| user demos | `user/hello.c`, `user/clock.c`, `user/tasks.c` | ring-3 exercises: port self-send + round-trip, recv_timed, task_self RPC, bootstrap_lookup chain, OOL round-trip via svc/echool, clock service consumer, task list service consumer |
| legacy shell | `kern/shell.c`, `kern/cmds.c` | kernel-side interactive shell kept as fallback if sh.elf fails to spawn; same commands surface for ddb-style introspection |

## Style

Project-wide `style(9)`:

- SPDX-License-Identifier headers on every file
- Function names at column 0, prototypes separate from definitions
- Tab indentation, parenthesised returns: `return (expr);`
- Per-struct field prefixes: `p_` for `struct port`, `th_` for `struct
  thread`, `me_` for `struct memmap_entry`, etc.
- Lock-key annotations in struct definitions: `(p)` protected,
  `(a)` atomic, `(c)` const after init
- `_Static_assert` on every wire-format struct
- `KASSERT` on every invariant the code relies on but cannot enforce
  with types

## Build + run

```
cd os
make            # build kernel.elf
make log        # boot headless, capture serial output to obj/boot.log
make run        # boot interactive in QEMU
```

Header dependency tracking via `-MMD -MP`, so editing a `.h` triggers
exactly the right `.o` recompiles — no `make clean` needed for
incremental changes.

Requires `gcc` with `-mcmodel=kernel`, GNU `ld`, and `qemu-system-x86_64`.
Built+tested with the toolchain in WSL on a Windows host; a PowerShell
wrapper for QEMU launching is in `tools/runlog.ps1`, and
`tools/accept.ps1` boots the kernel N times over and runs `apfsck` after
each, which is what a filesystem change is held to.

`make hostcheck` runs the **APFS subsystem and its self-tests on the
host**, with no kernel and no QEMU: `fs/apfs` reaches outside itself for
five symbols and no assembler — `bio_read`, `bio_write`, `kmalloc`,
`kfree`, `kprintf`, measured with `nm` rather than assumed — and each has
a one-line answer over a file.  Fourteen seconds for the whole ladder
twice plus `apfsck`, against about four minutes for one boot, which is
longer than most changes take to write.  The list runs **twice on
purpose**: a mounted volume legitimately keeps buffers it never frees, so
a leak is not "something still held at the end" but the same work costing
more the second time.  Its limits are written where it is defined —
notably that every test leaves the volume as it found it, so a defect
that exists only mid-run is gone before `apfsck` is called.

## Stress tests

The kernel runs a 14-test stress pass at boot, end-to-end exercising
every subsystem and checking conservation invariants:

```
stress mem 10000        mixed alloc/free, verify pmm-used minus kmem-cached
                        returns to baseline (no leaked chunks)
stress mem boundary     every interesting size around bucket edges,
                        post-write live re-verify to catch cross-talk
stress timer 2s         PIT drift vs TSC under kmalloc/kfree load
stress port 1000        mach_msg round-trip with descriptor capability
                        passing, single-thread
stress thread 200       cross-thread RPC via mach_msg_recv_block, server
                        thread spawn / exit / reap accounted
stress preempt 4/1s     4 CPU-bound workers that never yield, verify
                        the PIT preempts and rotates between them fairly
stress sendonce 500     MAKE_SEND_ONCE then MOVE_SEND_ONCE reply, verify
                        the right is consumed by use (no name leak)
stress portset 4x100    one port set, 4 member ports, 1 server thread
                        on the set; per-source attribution check
stress intertask 200    two tasks (kernel + worker), each with its own
                        port_space, RPC via cross-space port descriptors
stress moverecv 200     MOVE_RECEIVE in a descriptor: the receive right
                        rebinds onto a new name, the old name keeps SEND
stress nosenders 100    last sender drops while a receiver is parked in
                        mach_msg_recv_block; receiver wakes with E_DEAD
stress sendblock 2000   producer thread races to send into a 1024-slot
                        queue while the consumer drains one at a time;
                        producer blocks on full and resumes when freed
stress rpc 200          200 mach_msg_rpc rounds against a server thread
                        + a 50 ms recv_timed probe that must return
                        MACH_E_TIMEOUT within the PIT-scan latency
stress ool 4            4 rounds x 8 OOL payload sizes (1 B .. 65535 B),
                        mixed OOL + port descriptor in the same message,
                        FNV-1a checksum verified across the round-trip,
                        worker_task destroyed at the end and pmm count
                        confirmed back at baseline -- catches a missing
                        leaf-frame reclaim in pmap teardown
```

Sample boot output:

```
[13/14] stress rpc 200
stress_rpc: 200 rounds + 1 timeout probe
stress_rpc: timeout probe slept 50 ms
stress_rpc: names 2 -> 2, conserved 328 -> 328
stress_rpc: PASS

[14/14] stress ool 4 (parent <-> worker OOL transfer)
stress_ool: 4 rounds x 8 sizes, max payload 65535 bytes
stress_ool: kernel inuse 2 -> 2, conserved 328 -> 328
stress_ool: PASS
```

(`names 2 -> 2` is the per-test baseline: `MACH_PORT_TASK_SELF` and
`MACH_PORT_BOOTSTRAP` are already populated in `kernel_space` when the
stress pass starts, so the conservation check folds them in.  `conserved`
is the pmm page count after subtracting kmem's cached buckets.)

After the kernel stress pass, `hello.elf` is spawned once before sh.elf
takes over -- a deterministic ring-3 smoke test (port self-send,
recv_timed, task_self RPC, bootstrap chain, OOL round-trip via
svc/echool) so headless boots validate the userspace surface too.

## Shell

After the boot pass + the `hello.elf` ring-3 smoke run, you land in
sh.elf -- a ring-3 shell in an Apple/BSD-flavoured manpage TUI.  The
title bar and the rule under it sit above a DECSTBM scrolling region,
so they stay where they are while everything below them scrolls, and
the rest is drawn with the CP437 line glyphs the VGA font already has:

```
 style9-os(9)                                                 4 tasks   0:00:39
────────────────────────────────────────────────────────────────────────────────

  ┌── style9-os(9) ──────────────────────────────────────────────────────────┐
  │                                                                          │
  │   NAME       style9-os -- BSD-flavoured x86_64 kernel                    │
  │              with Mach IPC                                               │
  │                                                                          │
  │   SYSTEM     arch     x86_64                                             │
  │              memory   ▓░░░░░░░░░░░░░░░░░░░  4 / 127 MiB                  │
  │              tasks    4 live, 8 threads                                  │
  │              programs 39 in the registry                                 │
  │                                                                          │
  │   SEE ALSO   style(9), help(1)                                           │
  │                                                                          │
  └──────────────────────────────────────────────────────────────────────────┘

$
```

Builtins:

```
help                    list commands + every program in the registry
echo                    print arguments
clear                   erase + repaint splash
about                   version banner + live counters
ool                     OOL Mach IPC round-trip via svc/echool
man                     render a docs/man page through the built-in pager
kill                    terminate a child of this shell by task_id
```

Anything else is a `SYS_SPAWN`.  `help` gets its list from `svc/progreg`
rather than a hard-coded array, so a program added to `kern/progreg.c`
appears without the shell being touched.  `clock` and `tasks` themselves
are ring-3 consumers of `svc/clock` and `svc/tasks` (registered kernel
services reachable via `bootstrap_lookup`).

Line editing is whatever the keyboard already sends: arrows, Home, End,
Delete, `^A ^B ^E ^F ^D ^K ^U ^W ^L`, insertion anywhere in the line,
sixteen entries of history on Up/Down, and Tab completion across the
builtins and the registry.  A line longer than the screen scrolls
sideways instead of wrapping.

`help` and `man` are drawn as panels -- a frame with the title inlaid in
its top edge, and for the pager the position and key legend inlaid in the
bottom one.  In the program list a cyan name is a Mach-O that comes up
under the clean-room dyld and a gray one is not; the kernel decides which
by the image’s own first four bytes, the same sniff the loader makes.

The legacy `kern/shell.c` stays in the tree as a fallback for the case
where sh.elf fails to spawn -- it has the full ddb-style introspection
surface (`mem`, `memmap`, `pmap`, `task`, `thread`, `sched`, `port list`,
`stress <subcommand>`, `crash <variant>`, `panic`).  The same surface is
reachable interactively via `ddb` once that's invoked from a panic path.

## XNU binary compatibility

style9-os is monolithic and XNU-shaped (BSD + Mach in one address space,
not a microkernel), so the long game is running binaries built for XNU.
That is staged as a four-rung ladder:

- **S1 — Mach-O container (landed).**  `kern/macho.c` loads thin x86-64
  and fat/universal Mach-O images alongside ELF; the spawn launcher sniffs
  the 4-byte magic and dispatches.  With no Darwin cross-toolchain on the
  build host, `tools/elf2macho` rewraps a style9 ELF into a spec-shaped
  Mach-O so the loader has genuine containers to parse.
- **S2 — Darwin syscall personality (landed).**  A Mach-O that declares
  macOS via `LC_BUILD_VERSION` is tagged `TASK_PERSONALITY_DARWIN`;
  `kern/darwin.c` then decodes Apple's class-encoded `syscall`s (BSD calls
  in class 2, Mach traps in class 1) and honours the carry-flag errno
  convention — the same bytes a macOS x86-64 binary's libSystem stubs
  emit.  The freestanding `user/darwinhello` stub exercises it end to end,
  and the native style9 syscall path is left untouched.
- **S3 — Mach IPC trap (landed).**  The `mach_msg` trap (class 1, trap 31)
  drives Darwin messages through the kernel's existing queue; the
  `mach_msg_header_t` is byte-exact, so a Darwin task's own header round-
  trips unchanged.  The freestanding `user/darwinmsg` stub does a real
  send+receive on its own port.  MIG stub *generation* stays a userspace
  concern, and `mach_msg2`/overwrite plus the 7th (`notify`) arg are deferred.
- **S4 — libSystem + dyld (ahead).**  A minimal `libSystem` shim and
  dynamic linker — the point where unmodified Apple binaries run, and
  where standing up a Darwin cross-toolchain finally becomes worth it.

The ABI *inside* a plain (non-macOS) Mach-O is still style9: only a binary
that declares the macOS platform opts into the Darwin personality, so the
existing ELF programs and the style9 Mach-O demos are unaffected.

## Design notes

Loosely Mach-shape rather than BSD-shape:

- Task and thread are separate structs from day one.  No `proc` that
  conflates them.
- Ports + messages are the universal IPC primitive; there are no
  separate pipe / socket / fd abstractions.  Even kernel-internal RPC
  uses `mach_msg_send` against `kernel_space`.
- Both input devices (PS/2 keyboard and COM1 serial) ship bytes over
  Mach: each driver IRQ pushes into its own ring, a per-driver kernel
  thread parks via the new `sched_post_irq_wake` / `sched_drain_irq_wakes`
  primitive (lock-free LIFO drained at `preempt_enable` / intr tail,
  so IRQ context never touches `sched_lock`), and the shell recv's on
  a port set whose members are `kbd_input_port` and `uart_input_port`.
  Adding a third input source -- mouse, network console, scripted
  injector -- is just one more `mach_msg_send` from somewhere with
  the SEND right.
- The wire format matches real Mach in shape (`mach_msg_header_t`, 24
  bytes; `mach_msg_port_descriptor`, 8 bytes; `mach_msg_ool_descriptor`,
  16 bytes; `MACH_MSGH_BITS_COMPLEX`).  Descriptor area is variable-
  stride -- byte 0 of every descriptor is the type tag, the walker
  dispatches on it and advances by the descriptor's size.  Port
  descriptors carry capabilities; OOL descriptors carry bulk memory,
  and carrying it is a page-table operation rather than a copy -- the
  sender's frames are write-protected and handed over, the receiver
  maps them read-only under an entry that says writable, and only a
  receiver that actually writes takes the copy-on-write fault.  There
  was a staging buffer here once, which cost two copies of every
  payload for a middle step neither party looked at.
- Port descriptors in messages translate names between the sender's
  `port_space` and the receiver's, transferring capabilities.  The
  same code path serves single-space (kernel ↔ kernel) and multi-space
  (kernel ↔ worker-task) IPC — it's just two different `port_space`
  arguments.
- Kernel-implemented Mach objects (task_self, bootstrap) use a single
  synchronous-dispatch hook in `mach_msg_send`: if the destination
  port's `p_special` tag is non-zero, the sender's call body never
  queues -- the per-tag handler reads `msgh_id`, synthesises a reply,
  and sends it back to `msgh_local` from inside the same call, so the
  client's `mach_msg_rpc` returns one scheduler hop later with the
  reply already in its buffer.  Same pattern Mach uses for `task_port`
  / `host_port` / `processor_set_port`; we use it today for
  `MACH_PORT_TASK_SELF` (`task_self_dispatch`) and
  `MACH_PORT_BOOTSTRAP` (`bootstrap_dispatch`).
- The bootstrap port closes the discovery gap: there is no header
  file with `kbd_input_port` etc. baked in for ring-3 to find.  A
  task that wants a service does `mach_msg_rpc(MACH_PORT_BOOTSTRAP,
  BOOTSTRAP_OP_LOOKUP, "name")` and the reply contains a port
  descriptor whose translated `pd.name` is the SEND right under a
  name local to the caller's `port_space`.  Kernel-side services
  publish themselves with `bootstrap_register(name, kernel_name)`.

User-kernel split is in.  Ring-3 tasks are ELF64 programs built from
`user/*.c`, linked against libstyle9 (`lib/style9_*.c`), embedded into
the kernel image via `objcopy --rename-section .data=.rodata.<name>_elf`,
and registered in the program registry (`kern/progreg.c`).  Each task
gets its own pmap + vm_map at creation; the ELF loader walks PT_LOAD
segments, maps each user page with U=1 + R/W/X per p_flags, and `iretq`s
to `e_entry`.

Mach IPC is reachable from ring 3 at full power -- send / recv, bounded-
wait recv, one-shot RPC with autogenerated reply ports, port descriptors
(capability passing across spaces), OOL descriptors (bulk memory), and
the bootstrap + task_self special ports.  `hello.elf` walks the whole
surface end-to-end at every boot:

```
usermode: spawn 'hello' entry=0x40000000 (image=15608 bytes), stack=0x40010000
hello from hello.elf (libstyle9, ring 3)
  allocated port = 0x3
  self-send queued
  mach_msg round-trip via SYSCALL: OK
  recv_timed returned E_TIMEOUT after 50 ms: OK
  task_self GET_INFO ok: name='hello' tir_task_id=4
  bootstrap_lookup('kernel_task') -> name=0x4
  GET_INFO via bootstrap name: ok, tir_task_id=1
  OOL round-trip 512 bytes via echool: fnv1a=0x86a2b1c5 OK
hello.elf: all demos passed
[user thread exited, code=0]
```

The OOL line is the newest payoff: a ring-3 task constructs an
`mach_msg_ool_descriptor` pointing at its own user-VA buffer, ships it
through `mach_msg_rpc` to `svc/echool`, the kernel reads the bytes via
the sender's pmap (current under the special-port intercept), computes
FNV-1a, and the answer round-trips back in `msgh_id`.  Same wire format
the kernel-only `stress_ool` exercises.

The same kernel-side `mach_msg_send` / `mach_msg_recv_timed` that the
14 stress tests exercise is what userspace calls -- the syscall layer
just range-checks the user pointer and forwards.

Directories can now be made and removed -- `fs_apfs_mkdir` / `rmdir`,
`mkdir(2)` / `rmdir(2)` at 136 and 137, and a ring-3 binary that makes
one, puts a name in it, is refused the removal while that name is there,
and gets it once the name is gone.  A directory is not a file with the
mode changed: measured against the checker, its inode carries no data
stream at all (one that does is rejected by name), its entry's type and
its mode must agree, and it is counted in `apfs_num_directories` --
which, unlike `apfs_num_files`, is checked.  So both pairs are one
function with a question in it rather than two that agree today.

The terminal can now be TOLD something.  The console has echoed, edited a
line and turned Ctrl-C into a signal since the day it existed, but all of
it was fixed at compile time: `tcgetattr` answered ENOTTY on purpose,
because there was nothing behind it.  There is now -- a `struct termios`
the kernel keeps, `ioctl(2)` at 54 with `TIOCGETA` / `TIOCSETA` /
`TIOCGWINSZ`, and a line discipline that ASKS about each thing it does
instead of assuming it.  A program can turn ICANON and ECHO off and read
one keystroke with no Return behind it, which is the entire admission
price for full-screen software.  The layout is not guessed: Darwin
encodes the argument's size into the ioctl number, `TIOCGETA` is
`0x40487413`, and the `_Static_assert` that `struct termios` is 0x48
bytes is that arithmetic made by the compiler.

`ttyprobe` de-risks it from ring 3 the way `dirlist`, `pipefork` and
`filewrite` did their rungs -- a file and a pipe must answer ENOTTY, a
raw setting must READ BACK (or the kernel never kept it), `VMIN=0` must
turn a read into a poll, and one fed byte with no newline behind it must
arrive anyway.  Then **gstty**, GNU coreutils' `stty` and the tenth real
Apple binary, prints our terminal in a Mac's own words (`speed 38400
baud; rows 25; columns 80; ... isig icanon iexten echo echoe echok`) and
changes it back and forth with `-echo` and `sane`.  It needed no dylib
that was not already here.

The MODE WORD stopped being decoration.  `mkdir(2)` used to take a mode
and drop it -- documented as an honest edge, since there was no umask to
subtract and no chmod to correct it afterwards.  There is now a `chmod`
in the APFS writer (the cheapest edit it can make: sixteen bits inside a
record that does not move or change length, though the leaf still copies
because a block written in place is a block the live checkpoint names), a
real per-task `umask(2)`, and creates that write the mode they were
given.  What proves it is not this boot: the directory self-test leaves
its fixture wearing **0711**, which no create here produces, and the
boot after finds it still wearing it.

That is what **gmkdir** and **grmdir** -- coreutils' `mkdir` and `rmdir`,
the ELEVENTH and TWELFTH real Apple binaries -- needed.  Not directories:
those already worked.  They needed the mode word, an ownership family
honest enough to refuse (there are no users here, so anything but root
answers EPERM), the fd-relative calls, and one thing measured rather than
guessed: **a directory can be opened**.  GNU mkdir reported that the
directory it had just successfully created did not exist, because it
opens what it makes and `fs_open` answers about bytes.  A descriptor onto
a directory names a PLACE -- `read(2)` on one answers EISDIR -- and it is
what `openat`, `fdopendir`, `fchdir` and `fchmod` are all built on, all
four through one kernel call that answers "what path is this fd on".

A create now **splits and retries**, which was the last edge either making
call documented out loud and then refused at.  A name's records go into
two leaves at once -- the entry under the DIRECTORY's object id, the
inode under its OWN -- so a split moves both of them and everything above
them, and every address worked out beforehand is stale.  The writer
therefore does not resume: it makes the room and starts over from the
beginning, and it asks again after each split rather than once, because
either of the two leaves can be the full one.

What that cost was one latent bug it made reachable, and the shape of it
is worth keeping.  A file's inode record and the reference count of its
data stream are adjacent in key order and are written into the same node,
so an unlink looked for the second where the first was.  Adjacent means
the same node only until a SPLIT falls between them -- and a leaf filled
with twenty inodes and cut down the middle does exactly that.  The unlink
then answered success, took the inode and the entry, and left the stream
record behind, which apfsck calls "Data stream: has no references."  It
appeared on the fourth boot under the new fill-a-leaf self-test, which is
another way of saying a user would have found it; both cases are now
arranged on purpose rather than waited for.

A name can now **move**, within a directory or between two, taking a file
with its bytes or a directory with everything under it.  Nothing is made
and nothing destroyed, which is what makes a rename the one writer here
whose success moves no count at all -- and what makes the hard half of it
the INODE record rather than the entry.  That record carries a name and a
parent of its own; apfsck holds both to the entry that names them
("wrong name for only link", "bad parent for only link"), and a name of a
different length makes the record a different length, because the name
lives in an extended field with everything else packed after it.  So the
record is rebuilt beside the old one and put back, carrying every field
it had -- and the field that matters is the data stream, since a rename
that assembled a fresh record the way a create does would leave a
perfectly valid empty file where the caller's data used to be.

Both of those answers were **measured before the writer existed**.
`tools/apfspoke.py` pokes one field of one record on a copy of the
container and re-seals the block: the disagreement a forgetful rename
leaves behind is exactly the disagreement a poked record has, and
producing it that way costs no kernel at all.

And this rung, like the last one, made an older hole reachable.  The
longest key and value a tree has ever held are recorded in the footer of
its root node, and nothing here had ever written them, because every name
this system had made was shorter than one the image came with.  A rename
to a 28-character name was not, and apfsck said "Catalog: wrong maximum
key size in info footer."  The same tool settled what the field means: a
footer claiming MORE than any record needs is accepted and one claiming
less is refused, so the two are high-water marks that rise with an insert
and are never lowered -- a delete does not have to walk the tree to
tighten a bound no reader needs tight.

And then the writer stopped being the thing that was behind.  A file can
now **outlive its own name**, which is the oldest promise Unix makes about
`unlink(2)` and the one this kernel used to say out loud, in a comment, that
it did not keep: a file lives until its last name AND its last descriptor
are gone, and the second half needed the filesystem to know what was open.
Nothing told it.  The volume, meanwhile, had been ready the whole time --
every APFS container is formatted with a **private directory** that no path
reaches, for exactly this, and a checker looking in there knows it is
looking at orphans.

So what this rung mostly added is not a writer.  It is one row per open
file in `fs/fs.c` -- an object id and a count -- and the places a descriptor
is born, copied and dies wired to it: `open`, `dup`, `fork`, and the single
point where a slot is released.  `unlink` then asks the one question it
could never ask before, and if anything is holding the file, the name goes
and the bytes wait.

**mmap was the holder that nearly got away.**  A mapping copies the handle
into a VM object and pages through it long after the descriptor is closed,
which POSIX is explicit about, so the object takes a claim too.  That is
where the rung cost its panic: giving a file back can reach the volume and
sleep on the disk, `vm_map_remove` was freeing entries under the map's
spinlock, and a thread that blocks holding one in this kernel is never woken
again.  It now unlinks entries under the lock and frees them after, which is
what every kernel that has been here before does.  Found by `mmaptest` on
the first `munmap` of a mapped file -- the same lesson this file keeps
recording, that a comment explaining why an ordering is safe marks the place
it stopped being safe.

What an orphan has to look like was **measured with apfsck**, one refusal at
a time, before any of it was written: the entry is named `0x<oid>-dead`
("Orphan inode: wrong name"), the inode's parent must NOT be the private
directory ("Inode record: parent is private directory") and is the root
here, since a dangling parent reads as "free inode number in use" the moment
the old directory is removed, and the link count must be **zero** ("Orphan
inode: has a link count").  No flag is wanted, which was measured too.

The half no self-test can reach is the **crash**: a volume that comes up
with files in its private directory is one whose last boot ended between an
unlink and the close that would have finished it.  Such a volume is
perfectly valid -- apfsck accepts it, which is the whole reason the reap has
to be the kernel's job -- so one was fabricated with the host runner and
booted, and the mount said "1 file(s) were left waiting in the private
directory by an earlier boot and have been let go."

## A wake reaches the CPU

Three waits in the Darwin layer polled with `thread_yield` -- a pipe read, a
pipe write, and `wait4` -- and the rung that set out to park them found the
premise wrong.  `thread_yield` hands the CPU back at once, so a waiter
looping on it is not re-scheduled until the other side has used a whole
quantum: with the same counters compiled into both builds and the same work
in front of them, polling took **22** fruitless trips round `wait4`'s loop
and parking took **14**.  Nothing was eating the machine.

What was costing something sat underneath all three: **a wake did not
preempt**.  Making a thread READY asked for nothing further, so news waited
for whoever held the CPU to give it up or for the five-tick quantum to
expire.  Across one boot, **24074 wakes spent 154 seconds between ready and
running**, a mean of six milliseconds each; asking for a reschedule where a
thread is made ready brings that to **450 ms, a mean of zero** -- for every
blocking wait in the system, the twenty-odd thousand Mach receives included.

It is also what decides whether sleeping beats polling.  Parked without it
the three waits were **ten times slower** to notice anything (reap latency
5.6 ms polling, 52 ms parked), because a poller gets a fresh look every time
it is scheduled and a sleeper gets one and must be given the CPU to take it.
Parked with it: 3.9 ms.

The sleep queue belongs to the **scheduler**, not to the objects waited on:
`sched_wakeup(chan)` wakes whoever passed `chan` to `thread_block`, and the
list lives in `sched.c` on a link of its own.  An object holding a channel
cannot get its waiters wrong.  The console, which held a thread *pointer*,
could and did -- its slot had room for one, so a second reader overwrote the
first and the first was never woken again, and task teardown needed a hook
to stop a later keystroke waking freed memory.  Both are deleted rather than
fixed; they were the pointer.

A posted signal now reaches a sleeping thread, which stopped being optional
the moment these waits slept: `read(2)` on a pipe nobody was writing to had
become uninterruptible.  The wake is narrow on purpose -- a thread waiting on
a Mach port is linked into that port's list through the field the runqueue
uses, so waking one "just in case" is not a spurious wake but corruption.

Five instruments were wrong before one was right, and that is the part worth
keeping.  A throughput ratio *passed on the broken kernel*, because this
program's own parent is also a waiter and the theft was already in the
baseline.  97 ms of fork and reap got reported as stolen CPU.  A latency
figure was labelled for one wake and measured two.  A 10 ms bound passed on
a quiet boot and failed on a busy one with the kernel correct both times.
And the lost-wake check itself cried once in four boots, because a deadline
and the real news can land together.  **An absolute number under
uncontrolled load is a gauge, not an assertion**; the assertion here is the
load-independent one -- a `wait4` answered only because the deadline under it
fired, with a per-channel generation counter to prove nobody had spoken.

## ...and where a wake stands in the queue

The rung above left one figure unexplained and said so: a parent that read a
pipe and then reaped the child that wrote it took **103 ms**, stable to
within three, where two wakes of four milliseconds should not have cost that.
Asking for a reschedule had made the wake *arrive*; it had not decided where
the woken thread arrived *in the queue*.

It arrived at the back.  A thread that has just been woken is by construction
one that gave its slice up unused -- it asked for something, it was not
there, it slept -- and putting it behind the threads that have been running
means the news it was woken for waits out their slices.  So wake latency was
never one switch: it was *however many runnable threads are ahead of me*
times a quantum, and the quantum was five ticks.  Measured, with the delay
and its cause recorded at the same moment: **122 wakes over 20 ms in one
boot, every one of them with two to four threads queued ahead, delayed by a
whole quantum apiece** -- and a hundred milliseconds when two of the queue
wanted the CPU rather than one.  Both halves of that line matter; a duration
alone says a wake was late, and the queue depth beside it says why.

Wakes now go to the **front**.  The boost is worth exactly one slice and is
spent by taking it: the thread runs, and the moment it is preempted or yields
it rejoins the tail like everybody else, so nothing accumulates priority by
sleeping.  Sleeping buys the CPU for the first look, which is the entire
point of having been woken.

That took the pipe wake from a quantum to **89 microseconds** and the boot's
total wake delay from **5860 ms to 100**, worst case 100 ms to one PIT
period.  It also left the rest of the 103 ms standing, and correctly: what
remained was the *child* queueing for the CPU it needed to reach `_exit`,
which is not a wake at all and no wake fix could touch.  That is the quantum,
so the quantum was measured rather than argued about:

| slice | pipe-then-reap | reap alone | wake delay per boot |
| --- | --- | --- | --- |
| 5 ticks | 48.4 ms | 3.6 ms | 100 ms |
| **2 ticks** | **19.9 ms** | **5.1 ms** | **50 ms** |
| 1 tick | 18.5 ms | 8.9 ms | 40 ms |

Two ticks is the knee.  One buys nothing on the figure that motivated the
change and is the worst of the three at reaping, because a slice that short
cannot hold a task teardown: the dying child is preempted in the middle of it
and queues again to finish, so its parent waits two turns instead of one.  A
slice is always somebody else's waiting time, and the somebody is usually the
thread the others are waiting *for*.

The test that reported the 103 ms now reports its two legs apart -- the wake,
which the kernel owns end to end, and the queueing behind it, which it does
not -- because a single number there is what made a scheduling cost look like
a defect in the pipe.  End state: **24362 wakes reached the CPU in 30 ms, none
of them over 20**, and four boots of 89/90/90/90 with nothing failing.

## What a CPU knows about itself

Everything a CPU needed to know about itself was a plain global: which thread
is running, which stack a `SYSCALL` lands on, how deep it is inside critical
sections, whether it owes a reschedule, which thread to fall back on when
nothing is runnable, and its own GDT and TSS. Every one of those is a
*per-CPU* quantity that was correct as a global only because there is one
CPU, and each was cheap to move today and dearer tomorrow. So they moved
first, before anything can run on a second processor, where the right answer
is known in advance: **nothing about the system's behaviour may change, and
the whole boot has to say so.**

A CPU finds its own block through the **GS segment base** rather than by
indexing an array, because getting an index is the problem being solved --
reading the local APIC needs the APIC mapped, and a processor coming up needs
its stack and its current thread before it has mapped anything. The base is a
register, written once per CPU, after which `%gs:0` is the block with nothing
to look up. That is what the `SYSCALL` stub needs: on entry `%rsp` still
points into ring 3 and every register holds either an argument or something
the ABI promises to give back, so the kernel stack has to come out of memory
that can be found without spending a register to find it.

Two traps came with it, both worth naming:

* **Loading a segment register zeroes its base.** In long mode the base of
  `%fs`/`%gs` lives only in its MSR, and writing the register loads the
  *descriptor's* base -- zero, for every flat descriptor in our GDT. The GDT
  setup used to reload all five segment registers, which was free while
  nothing used `%gs` and would now be the single instruction that points a
  CPU's per-CPU block at physical address zero. It reloads three.
* **The boundary is the first spinlock, not the first CPU-flavoured call.**
  `spin_lock` counts preemption, the count is per-CPU, so per-CPU state has to
  work before any lock is taken -- which puts the setup at the top of `kmain`,
  before the console exists. The check that it worked therefore has to happen
  later, and does: the block is read back through the segment base and
  compared with the address the linker chose.

The preempt count was documented as kernel-wide *and deliberately so*,
because `sched_lock` is held across a context switch and the thread that
releases it is not the one that took it, so a per-thread count underflows.
Per-CPU keeps that property -- a switch hands over between two threads
standing on the same CPU -- while answering the question the PIT actually
has, which was never "is the kernel busy" but "may I take away the CPU I am
standing on". Same for the quantum: a tick is charged to the CPU that took
it. `cpu` in the shell prints one line per processor, and the end of every
console session prints the same line, so what each CPU was doing is on the
record next to what the wakes cost.

Ten boots, four of them the acceptance ladder from a pristine volume:
**89/90/90/90 pass, nothing failing, `apfsck` clean on every one** -- the same
tally, wake for wake, as the kernel before the move. What is still
single-CPU is everything above this: no APIC, no second processor started,
and `spin_lock` still spins without disabling interrupts, which is safe only
because no interrupt handler takes a lock the mainline can hold.

## The interrupt controller each CPU has of its own

The 8259 and the PIT are one chip each for the whole machine. The PIT can
tick one interrupt line, so it can debit one CPU's slice -- which makes the
per-CPU quantum above a quantum only the boot processor would ever spend.
Every CPU needs a timer of its own, and needs a way to be *told* something by
another CPU: a reschedule, a TLB invalidation, the startup sequence itself.
All of that is the local APIC, so it comes next.

**Software-enabling it can disconnect every legacy interrupt at once, and
that is the whole difficulty of the step.** On a PC the 8259's output does
not reach the CPU directly; it arrives at the local APIC's LINT0 pin and
passes through only if that pin's LVT entry says ExtINT and is unmasked.
Every LVT entry comes out of reset *masked*, and this kernel software-enables
the APIC itself -- so what LINT0 carries is this kernel's business, and
enabling the APIC without programming it would take the timer, the keyboard
and the disk away in one instruction. Whether the firmware had already set
"virtual wire mode" up is not known here and deliberately not relied on: the
two legacy pins are programmed in the same breath as the enable, which makes
the question moot. The failure mode if that is wrong is loud rather than
subtle: the boot stops at the first thing that waits.

(An earlier version of this section asserted there was *no* firmware in the
path, on the grounds that QEMU is loading the kernel directly. That was wrong,
and the MADT below is the thing that disproved it -- the ACPI tables it reads
are placed in low memory by a BIOS, and they identify themselves as `BOCHS`.
The conclusion did not change, only the reason it is right.)

The vectors above the 8259's 32..47 window used to have **no IDT gate at
all**, which is not the same as being ignored -- an interrupt delivered there
was a general-protection fault with an obscure error code. There are now
stubs and gates for all 256, generated rather than written out, and a second
handler table for the ones the APIC delivers, because the two kinds are
acknowledged to different chips and the table a handler came out of is how
the dispatcher knows which. A vector with nothing installed is ignored and
acknowledged to *nobody* -- which is what the spurious vector requires, since
there is no in-service bit behind it and an EOI would retire somebody else's
interrupt.

Proving the chip works is one measurement in two halves: the timer's counting
rate against the PIT, then the timer left running while its interrupts are
counted. The second half is the one that cannot be argued -- the mapping, the
enable, the LVT, the vector, the IDT gate, the dispatcher and the EOI all
have to be right for a single interrupt to be counted. Four boots:

	lapic: id=0 version=0x14, 6 LVT entries, regs at 0xfee00000
	       (uncached), legacy pins wired
	lapic: timer counts at 62085 kHz (input / 16), measured over 100 ms
	lapic: timer delivered 20 interrupt(s) at 100 Hz, want about 20

61.6 to 63.2 MHz at divide-16 across the four, so a ~1 GHz input clock, and
19 or 20 delivered every time. The count is reported as a **gauge, not an
assertion**, and the spread says why: the ruler is the PIT, and this host's
PIT delivery wanders by several percent. What *would* be a defect is zero,
and that alone is called out.

### What the APIC broke on the way in, and what that says

Mapping the APIC's registers cost a **page-table double-free**, and the bug
was older than the APIC. `pmap_create` copies the whole kernel PDPT-0 into
each task's private one -- deliberately, so kernel mappings stay reachable
while that pmap is loaded -- and its comment had even anticipated "a future
caller adding e.g. a high-MMIO mapping under PML4[0]". But `pmap_destroy`
freed everything from PDPT slot 1 upward on the theory that only slot 0 could
be shared, which was true only for as long as slot 0 was the kernel's only
mapping there. The APIC at `0xFEE00000` lands in slot 3; every task copied
that PD and PT, and the first task to die handed both back to the page
allocator while the kernel was still reading the APIC through them.

The second task to die is what made a noise -- pmm's double-free assertion --
and **the noise was luck**. Had a freed frame been handed out and written
before that second death, the symptom would have been an interrupt controller
quietly answering out of somebody else's memory. The rule is now one sentence
instead of two: an entry the kernel's own PDPT-0 still names is not ours to
free, whatever slot it is in.

## The slice changes hands

The APIC was worth turning on for this. Preemption now comes from the timer
belonging to the CPU whose slice is being spent, not from the one PIT the
machine has -- and the PIT keeps ticking, because it is still the clock:
timeouts, busy-sleeps and the uptime everything else is counted in.  It stops
debiting, not ticking.

**Exactly one timer may debit the slice**, so the hand-over relieves the PIT
*before* it arms the APIC. Getting that order backwards costs nothing
visible: two timers debiting means every quantum is spent twice as fast, which
raises no assertion and prints no line -- it just silently halves the number
the scheduler was tuned around. A gap costs one tick of one thread's slice; an
overlap costs the tuning.

For the same reason the new timer runs at **the rate the PIT was running**.
The quantum is a count of ticks, and that count was chosen off a measured
curve with a knee at two; a timer at some other rate would change the slice
without touching the constant that is supposed to express it.

The EOI is written *before* the schedule point, not after. An interrupt in
service blocks everything of its priority and below on that CPU until it is
acknowledged, and the timer sits at the top -- so a yield taken first would
carry the unacknowledged interrupt away with the outgoing thread's stack, and
the CPU would get no more timer ticks until that thread was scheduled again by
the ticks it is holding up. The 8259 path learned this the same way, as an
outright boot hang.

### The ruler was wrong, and the report is what said so

The first measurement of the new arrangement reported a slice of **18.1 ms**
where the constant says 20, on two boots out of four. Not noise: those two
boots had calibrated the APIC's counting rate at 56.4 MHz where the other two
read 61.8 and 62.3, and 62.3/56.4 is exactly the 10% by which their slices
were short. The calibration went straight into the timer's reload count, so a
rate measured 10% low is a timer that ticks 10% fast for ever.

The fault was in the ruler. Calibration waited for **ten PIT interrupts** and
called it 100 ms -- but a delivered interrupt can be late and can arrive in a
burst, and ten of them on this host sometimes land inside ninety milliseconds
of real time. The window was not the length it was assumed to be, and every
error in it is multiplied into the quantum.

The TSC is *read*, not delivered. Nothing can hurry a counter the CPU
increments itself, and its own calibration is checkable by eye: it prints
~3.53 GHz on a part sold as 3.6. So both halves of the probe are now timed by
the TSC, and the PIT is counted alongside instead of being trusted -- the gap
between them is precisely the PIT's delivery deficit, which is the thing that
was hiding in the middle of the measurement. Across four boots the calibration
now spans 61.3 to 62.3 MHz where it used to span 56.4 to 62.3, and the slice
comes out **20.1 ms**.

**Printing the two side by side turned out to be the useful part**, because it
separates the two ways this number can go wrong. One boot of the four reported
86 Hz and a slice of 23 ms -- and the PIT on that boot reported 86 Hz too, the
two counts within 0.2% of each other while the boot took 44 seconds where the
others took 25. Both chips falling behind together by the same amount is the
host stalling the guest, which is a thing to know and not a thing to fix here.
The earlier defect looked nothing like it: there the APIC ran 10% *ahead* of
the PIT, and a disagreement between the two clocks is what a mis-programmed
timer looks like. Agreement means the host; disagreement means us.

What makes this a check rather than a gauge is that the ladder already had a
test that cannot pass without it. `stress_preempt` spawns CPU-bound threads
that never yield and requires that every one of them made progress and that
preemptions were observed; with the PIT relieved, only the APIC timer can
satisfy it, and a hand-over that relieved one chip without arming the other
looks exactly like a cooperative scheduler.

## Counting the processors

A kernel that wants to start a second CPU has to be told there is one, and
CPUID will not tell it. CPUID describes the *part* -- how many logical
processors a package can have -- and says nothing about how many the firmware
brought out of reset or what APIC id each answers to. Only ACPI's MADT has
that list, so counting processors is an ACPI job on this architecture whether
one likes ACPI or not.

What is here is as much ACPI as that sentence requires and no more: a
signature scan of the EBDA and the BIOS region for the RSDP, a walk of the
XSDT (preferred, because its entries are 64 bits wide and a machine can put
its tables above four gigabytes) or the RSDT, and one table parsed. No AML,
no interpreter, no namespace.

Every step is checked, and the checks are the point. The RSDP's checksum,
because the eight bytes `RSD PTR ` appear in the middle of other things and
following one of those leads to a walk over nonsense. Each table's checksum,
because firmware has bugs too. Each entry's length before it is used to
advance, because a zero-length entry makes the walk stand still and a long one
makes it read past the table -- and a length nobody can trust means the
position of every following entry is a guess, so the walk stops there and says
where. Any failure at all ends the probe with a printed reason and a kernel
that keeps running on one processor: a machine that cannot describe itself is
not a machine to start extra CPUs on.

	acpi: rsdp at 0x00000000000f5260, revision 0, oem 'BOCHS '
	acpi: madt at 0x0000000007fe235c, 144 bytes, lapic regs 0xfee00000,
	      8259 present
	acpi:   lapic id 0 (acpi id 0) -- this processor
	acpi:   lapic id 1 (acpi id 1)
	acpi:   lapic id 2 (acpi id 2)
	acpi:   lapic id 3 (acpi id 3)
	acpi:   io apic id 0 at 0xfec00000, gsi base 0
	acpi: 4 processor(s) present, 1 running

**The address on the second line is a cross-check, not a printout.** It is
where firmware says this machine's local APIC lives; `0xfee00000` on the line
above it is where the MSR on this processor said it lives. Two independent
sources agreeing is evidence, where one source is a claim -- so a disagreement
gets a line of its own with both numbers in it.

Recognising *itself* in that list is the one thing the probe cannot get wrong
quietly, and it does it by APIC id rather than by position: the order is the
firmware's business and the boot processor is not obliged to be first. Which
means the boot CPU has to know its own id before ACPI is read, and it does --
from CPUID leaf 1, which answers with no APIC mapped, no MSR touched and no
page table in place. That is not a convenience; it is the position an
application processor wakes up in.

Two counts now exist and they are different questions. **Present** is how many
the firmware described and the kernel has blocks for; **online** is how many
are running kernel code. Equal on a machine where everyone started, and the
gap is the interesting number on a machine where one did not:

	cpu 0: lapic 0, running user-elf, idle id=2, preempt 0, quantum 0/2
	cpu 1: lapic 1, acpi id 1, NOT STARTED
	cpu 2: lapic 2, acpi id 2, NOT STARTED
	cpu 3: lapic 3, acpi id 3, NOT STARTED

The guest is booted with four processors from here on, because a guest with
one has no MADT worth reading and no application processor to start. Nothing
about the interrupt routing changed: the IO APIC's address is recorded because
the MADT is where it is written down, and not one interrupt goes through it
yet.

## Starting the others

An application processor comes out of reset in **real mode**, at a physical
address the startup message names, with no page tables, no stack, no segment
base and no idea which processor it is. Everything the rest of this kernel
takes for granted has to be built for it, in dependency order, by code that
runs in three addressing modes before it can call a C function.

The startup message carries a **page number in one byte**, which is why the
trampoline lives in the first megabyte -- there is no room in that field for an
address above `0xFF000`. It is copied to `0x8000`: conventional memory the
firmware is finished with, and a page `pmm` never offers because the whole
first megabyte is marked used at startup. The install checks the firmware
memory map anyway, since "never" is a property of today's allocator.

Nothing in the trampoline is patched at runtime. It is assembled knowing the
address it will run at, so the temporary GDT's base and the far-jump targets
are ordinary link-time constants -- the alternative is the same code plus a
table of offsets that has to agree with it, and the failure mode of a
disagreement is a processor jumping into the middle of nowhere with no way to
say so. Only the parameter block is written, and it is data: a CR3, a stack
top, and which `struct cpu` this one is.

Two far jumps, because a far jump is the only instruction that reloads CS and
therefore the only way to change what mode the next instruction is decoded in.
Real mode sets `CR0.PE` and jumps to 32-bit code; 32-bit code sets `CR4.PAE`,
`CR3`, `EFER.LME` and `CR0.PG` and jumps to **long mode at a kernel symbol** --
not one in the blob, because by then paging is on and the kernel's identity map
makes its own addresses mean what they say. So the copied part ends the moment
there is a page table, and the rest is ordinary kernel text a debugger can
find.

**`EFER.NXE` is not optional there, and finding that out the hard way would
have been silent.** This kernel's page tables set the no-execute bit, and while
`NXE` is clear, bit 63 of a PTE is not a permission but a *reserved bit
violation*: the first instruction fetched through those tables faults. On a
processor that has not loaded an IDT yet, a fault is not a panic — it is a
triple fault and a reset, with nothing printed and nothing to read.

What arrives in C does so with a stack and nothing else, and the order of the
first four things it does is the order of what depends on what:

1. **The GS base**, before anything at all. Every per-CPU reference in this
   kernel reads through it, and `spin_lock` is a per-CPU reference -- until
   this is installed, taking a lock writes to physical page zero.
2. **Its own GDT and TSS.** The TSS above all: it carries the stack a ring
   transition lands on, so two processors pointing at one would fault onto the
   same stack and overwrite each other's frame.
3. **The IDT register.** One table for the machine, but the register that
   points at it is per-processor and comes up holding zero.
4. **Its own APIC.** The registers are already mapped and the mapping is
   shared, but the enable bit, the task priority and every LVT entry are
   per-processor state that comes out of reset masked.

`LINT0` is where the 8259 arrives, and it is programmed as ExtINT **on the boot
processor only**. There is one 8259 wired to one CPU's pin; an AP that also
claimed it would be offering to answer legacy interrupts that were never routed
to it.

	mp: trampoline installed at 0x8000, 134 bytes, parameters at 0x8f00
	lapic: id=1 version=0x14, 6 LVT entries, regs at 0xfee00000 ...
	cpu 1: online, lapic 1, stack at 0x395000 -- parked, interrupts off
	lapic: id=2 version=0x14, ...
	cpu 2: online, lapic 2, stack at 0x399000 -- parked, interrupts off
	lapic: id=3 version=0x14, ...
	cpu 3: online, lapic 3, stack at 0x39d000 -- parked, interrupts off
	mp: 3 of 3 application processor(s) running kernel code, 4 cpu(s) online

Those `id=` numbers are each processor reading **its own** APIC id register and
printing it -- not the starter saying what it asked for. A CPU that came up
with the wrong identity, or on the wrong stack, or with the boot CPU's block,
would say so in that line.

And then each one **parks with interrupts off**, which is the honest end of
this rung rather than a shortcut. `spin_lock` spins without disabling
interrupts; that is safe on the boot processor only because no interrupt
handler there takes a lock the mainline can hold. On a second processor the
argument is not available -- it could take a lock, be interrupted, and have its
own handler wait for the lock it is holding. So the rung that makes locks
interrupt-safe is the rung that lets these three into the scheduler, and until
then they sit at `cli; hlt` where they can do no harm.

## A lock that survives an interrupt

The acquire has always been a real atomic exchange, so two processors could
never both hold a lock. What was missing was one processor against **itself**:
take a lock, be interrupted, and have the handler ask for the same lock. On a
single CPU that could not happen -- not because of anything about the lock, but
because no interrupt handler in this kernel took a lock the mainline could
hold. That is an argument about which handlers exist, and it stops being
available the moment a second processor runs kernel code with interrupts on.

So `spin_lock` now disables interrupts, and `spin_unlock` puts them back the
way it found them. **Saved and restored, not cleared and set**: nested critical
sections have to stay closed until the outermost one ends, and code that was
already running with interrupts off must not have them turned on underneath it.

**Where that saved state lives is the whole difficulty, and the answer is: in
the thread.** `sched_lock` is held *across* a context switch -- the outgoing
thread takes it and the incoming thread releases it -- so a per-CPU answer
would be restored by a thread that never saved it. Concretely: a thread that
yielded voluntarily with interrupts on can be resumed by one that entered the
scheduler from an interrupt with them off, and would then run kernel code with
interrupts disabled until something else happened to enable them. Not a crash.
Preemption quietly stopping on that CPU.

Per-thread balances because each thread performs exactly one acquire and one
release of its own; it is only the lock *object* that changes hands. With one
exception, and it is the exception that makes this look impossible until you
find it:

> **A brand-new thread releases a lock it never took.** The switch into it
> happens with `sched_lock` held by whoever switched, and the trampoline's
> first act is to drop it. A count starting at zero goes negative there.

So a created thread is initialised as though it already held one lock -- and
the state that release will restore is *interrupts on*, which is how a thread
has to start, since nothing else will enable them for it. The boot thread,
which is the context already running when it acquires a name, starts at zero.

No such deadlock has ever been observed here, and it is worth saying that
plainly rather than inventing a war story: every interrupt handler in this tree
is deliberately lock-free, and `sched_post_irq_wake` exists precisely so the
one thing a handler needs from the scheduler can be done by appending to a
lock-free list. This rung does not fix a bug that bit. It removes a
precondition -- "no handler takes a lock the mainline holds" -- that nobody
would be able to keep once four processors are running kernel code, and which
nothing checks.

Interrupts are now off for as long as any lock is held, which is a cost as well
as a fix: a long critical section is now a long stretch of deferred interrupts,
and the timer ticks that arrive during one are lost rather than late. The
timer's own report is where that would show, since it prints the rate actually
delivered against elapsed TSC time -- so the slice figure is a lock-hold-time
measurement now, as well as everything else it was.

## The console is held for a write, not for a character

The first four-processor boot printed

```
parkmp:ed with interrupt 3 s off
```

which is two processors' lines woven together **byte by byte**. Nothing was
lost and nothing was corrupted -- every character went through the lock. What
was lost is the only property a log has, which is that a line means one thing
said by one CPU.

The cause was not a missing lock but a lock at the wrong granularity.
`tty_putc` took `tty_lock` around each character, because `spin_lock` panics on
a same-CPU re-acquire and so a run of output could not hold it across the run.
On one processor that was invisible; it had been wrong the whole time and had
nothing to be wrong in front of.

So the unit of exclusion moves from the character to the **write** -- a span
this file already had, since `kprintf`, `tty_puts` and `tty_write` bracket
themselves so the hardware cursor is programmed once per write. That bracket
carries the lock now, and every `tty_putc` inside finds the console already
held by its own CPU. Which is why it is **not** a `struct spinlock`: a
recursive lock is not a spinlock with the check removed, it needs an owner and
a depth, and the check that remains is a different one -- a second CPU still
waits, and only the CPU that already holds it walks through.

The cost was measured rather than assumed, because the assumption was that
there would be one: interrupts are now off for a whole line instead of a
character, and the timer's own report says **98 Hz of 100 delivered and a
20.4 ms slice before, 98 Hz and 20.2 ms after** -- the same number twice, and
a difference smaller than the spread between two boots of the unchanged
kernel. What is *not* free is a real serial line: eighty characters at 115200
baud is seven milliseconds of polling the transmitter, free only because an
emulated one is always ready. The answer there is an output ring the UART
drains from its own interrupt, which is a rung of its own.

## An invalidation reaches the other processors

`invlpg` empties one entry out of the TLB of the processor that executes it
and says nothing to any other, and every other processor's TLB is a private
cache of the **same** page tables. So a mapping this CPU has just removed goes
on being used elsewhere, with no fault and no bound -- a store landing in a
page that now belongs to somebody else, discovered later and somewhere else.

There is no instruction that invalidates another processor's TLB. The only way
is to ask it to do so itself, which means an interrupt -- **and every spinlock
in this kernel now turns interrupts off**, which the rung before this one did
deliberately. That is a deadlock rather than a delay:

> CPU A takes a pmap's lock, changes a mapping, sends the request and waits
> for the acknowledgement. CPU B is spinning for that same lock with
> interrupts off. A waits for B to answer; B waits for A to let go. Neither
> is doing anything wrong.

So the request is not delivered only by interrupt. It is published as a serial
number, and every loop in this kernel that spins with interrupts off carries it
out from inside the spin -- `spin_lock`'s acquire above all. The interrupt is
then an optimisation for processors that are not spinning, and the correctness
lives in the poll. **The answer is a number stored, not a counter decremented**:
a processor may notice the same request twice, once each way, and storing the
same value twice is nothing where a second decrement would let the sender leave
while a CPU still held the stale entry.

The evidence is the acknowledgement, and a thousand of them run at boot: each
can only be written by the far processor and only after it has run our handler,
so the test covers the APIC, the IDT, the vector, the handler and the barriers
rather than this file's bookkeeping. **1000 invalidations, every one
acknowledged by all three others, 18 us each and 460 us at worst.**

Two numbers say what state this is really in, and both are worth more than the
test. **1002 shootdowns in a whole boot**, a thousand of which are the test --
this pmap only invalidates when it replaces or removes a live entry, and almost
every mapping made during a boot is a fresh one. And **3005 answered by the
interrupt, 0 from inside a spin loop**, which is exactly right while the other
processors are parked, since nothing there ever spins for a lock. That second
number staying zero for ever would mean the deadlock argument above is a story
about something that never happens, so it is printed rather than asserted.

## Four processors, one runqueue

A processor cannot switch **away** from a thread that does not exist, so it
cannot take the first one off the queue either; and a CPU with nothing runnable
still has to be executing something, on a stack it does not share. Both wants
are met by the same thread: the context an application processor is already
running -- the trampoline's stack -- is adopted as a thread, and that thread is
this CPU's idler. The boot processor does it the other way round, creating its
idler, because the context it is running has work left to do. Two shapes, one
invariant: every CPU owns an idle thread and no CPU shares one.

**What made one runqueue safe for four processors was already there**, and not
for this reason. `sched_lock` is held across every context switch -- taken by
the outgoing thread, released by the incoming one -- which was written so a
waker could never see a thread as both eligible and still executing. That same
span is exactly the window in which a yielding thread is on the queue and still
on its own stack, so a second processor spinning for the lock cannot see the
queue until the stack has been left.

Processors are **started early and released late**, and the gap is the point.
Early because the trampoline needs a page of conventional memory that later
boot-time allocation would be entitled to take. Late because `CR4.SMAP` is
turned on near the end of boot and the `SYSCALL` registers with it -- and a
control register is per CPU, so an application processor inherits *none* of
them. It inherits the page tables, the GDT layout, the IDT and the kernel's
opinions, which is why the omission never looks like one:

| missing | what it looks like instead |
| --- | --- |
| `CR0.WP` | ring 0 writes straight through a read-only page, so the copy-on-write fault the kernel arranged on purpose does not happen **on that CPU**, and two processes quietly share a page each believes is private |
| `CR4.OSFXSR` | `#UD` on the first `FXRSTOR`, which is this processor's first context switch |
| `CR4.SMAP` | kernel code dereferences user pointers without faulting, on that CPU only |
| `EFER.SCE` | `#UD` on the first system call a thread happens to make there |

A newly ready thread also has to reach a processor that is asleep: an idle CPU
in `hlt` would not notice until its own timer woke it a tick later, while the
queue grew in front of a busy one. So the CPU that queues the work tells one
that has none -- exactly one, only if it is genuinely idle, and only when more
than one thread is waiting, since for a single wake the waker is about to run
it itself and a message to anybody else is four APIC register accesses spent to
lose a race.

The evidence is a bit each thread sets out of its own CPU's block, which no
bookkeeping on the queueing side could fake: **eight threads ran on four of the
four processors that are up**, and the bit for processor three can only be set
by code executing on processor three.

### Counting yields is not waiting

Then the boot stopped finishing, in a way none of the above explains.

Every "wait for the other task to get somewhere" in this tree was a bounded run
of yields -- give somebody else a turn, look again, give up after N. That was a
way of **waiting**, because a yield with work queued behind it does not come
back until that work has had the CPU, so N turns bought N slices of real time.
With four processors it buys nothing: the task being waited for is not queued
behind this one, it is running beside it, so the yield finds an empty runqueue
and returns at once. Sixty-four turns are spent in microseconds, and the first
symptom was a test reporting a failure that was only a budget denominated in
the wrong unit:

```
loopchild.tport lookup failed after 64 yields
```

**And in the kernel the same shape was fatal rather than merely wrong.** The
boot thread waits for `hello.elf` the same way, and that one loop became
**350,000 context switches a second**, every one of them taking the scheduler's
global lock with interrupts disabled. The processor spent so much of its life
with interrupts off that it *missed most of its timer ticks* -- so the clock
stopped advancing, and once the clock stops every deadline in the system stops
expiring, so the timed waits everything else was parked on never returned. Four
processors at a hundred percent, a log frozen mid-line, and nothing wrong with
any lock.

Two instruments were built to find it and the second one did. A **spin
watchdog** reports a lock waited on for pathologically long, naming the lock,
the holder's CPU and both sites by symbol -- and it said nothing, which was
itself the finding: nobody was waiting for a lock. What answered was a
**census written straight at the UART**: no lock, no formatting, no console,
one byte at a time, because every other way this kernel has of saying what a
CPU is doing goes through the thing a wedge takes away.

```
[census] cpu0=boot/0040a947 cpu1=user-elf/001b983d cpu2=idle2/00124326 ...
[census] cpu0=boot/005576ee cpu1=user-elf/0024446c cpu2=idle2/00180df2 ...
```

Those are switch counts, and they answered the question in one line.

So a poll waits in **time**. `sched_nap_ms` in the kernel and `poll_turn` in
libstyle9 both ask for one millisecond and are given one timer tick, because
the tick is when deadlines are looked at -- asking for the smallest thing and
being handed the resolution keeps that number in the clock where it belongs. A
turn now means the same on any number of processors, which is what it meant on
one by accident. The same boot: **63,857 switches instead of 5.6 million**, and
`dash /bin/demo.sh` finishing in 38 turns where it had been abandoned after
8192.

`thread_yield` returns whether it actually switched, and the two callers that
are waiting for a *runnable* peer rather than for a device use it: a yield that
switched is still a wait and costs nothing extra, so the nap is the fallback
and not the rule.

### Not asleep is not the same as not listening

One boot in four still stopped, and always in the same place: a thread inside a
filesystem write that never came back, with the rest of the machine perfectly
healthy and nothing to look at. No panic, no failing test, no lock held.

`thread_wake` on a thread that is not `BLOCKED` returned doing nothing, and
that was right. A thread that is `READY` or `RUNNING` has not missed the news
-- it will look again on its own. But a thread can also be **between**: it has
decided to sleep and has not yet committed. On one processor that state had no
duration, because deciding and committing happen with nothing else able to run
on that CPU in between. On four it lasts as long as anybody likes.

The ATA driver is where it showed. A thread installs itself as the channel's
waiter and drops toward `THREAD_BLOCKED`; the disk's interrupt lands on
**another** processor and posts the wake; the wake finds a running thread and
evaporates. The thread then sleeps for ever.

So a wake that finds nobody asleep leaves a note, and the next attempt to sleep
reads it and declines to sleep. The caller re-tests its condition and finds
whatever the wake was about -- which is the contract every sleeper here already
keeps, since waking has always been a hint rather than a promise and everybody
parks inside `for (;;)`.

### An empty buffer said it held the superblock

The rung before this one left a defect written down rather than fixed: **one
four-processor boot in four stopped**, with nothing failing, nothing panicking,
and no lock held long enough for the watchdog to say so. It turned out to be
several things wearing one symptom, and the largest of them was not in the
scheduler at all.

**The first thing that had to be corrected was the description.** The machine
was never wedged. It reaches its shell prompt every time; what stops is a
spawned program, and `hello` gives up on it after its budget and carries on --
so the boot ends with fewer checks passed and no complaint. Reading "the log
stopped" as "the kernel stopped" cost a long time, and it was the hypervisor
that said otherwise: three processors halted in the idle loop, one in ring 3
running a service whose entire job is to spin, the clock ticking, the runqueue
empty. That is a picture of a machine asleep at a prompt, not a machine stuck.

**What the failing programs had in common was a file they could not open.**

```
darwin: open('/bin/demo.sh') -> apfs rv=-3
dash: 0: cannot open /bin/demo.sh: Input/output error
```

`bio_bufs` is a static array, so an untouched buffer says it holds **page 0 of
drive 0** -- and page 0 of an APFS container is the anchor superblock, the one
block this kernel rewrites at the end of every checkpoint. Everything that
looks a buffer up by `(drive, page)` therefore matched an empty buffer while
searching for the busiest block on the volume.

The read path survived that by accident: it keeps scanning past a match that is
not valid. The write path did not. It stopped at the *first* buffer claiming
the page, and stopping at an empty one means the write never reaches the buffer
that really holds it -- so the cache went on serving the previous superblock
for as long as it stayed resident. What comes out the far end is a checksum
that does not match, a lookup that answers "the disk or the tree lied", and a
program that cannot open a file that is plainly there.

One processor hid it because the order buffers get claimed in was the same
every boot. Four made that order a race. An empty buffer now says it holds a
page number no disk has, and the write loop no longer reads "nothing to patch
here" as "nowhere else to look".

### A thread that left the CPU without leaving the list

Three more were found on the way there, all of one family, and all of them real
whatever else was wrong.

**They were found by asking the hypervisor, not the kernel.** Every instrument
in this tree runs *inside* the machine being examined, and a wedge is exactly
the state where that is worth least. QEMU's monitor already knows every
register of every processor, so `info registers -a` on a stalled guest is a
census nothing in the guest has to still be working for:

```
CPU#0  RIP=00000000001187d3  CPL=0  HLT=1
CPU#1  RIP=0000000040001a00  CPL=3  HLT=0
CPU#2  RIP=00000000001187d3  CPL=0  HLT=1
CPU#3  RIP=00000000001187d3  CPL=0  HLT=1
```

**One field, four lists.** A thread here can be on a queue for several reasons
at once, and `struct thread` had one link field, `th_runq_link`, serving the
runqueue, the interrupt-deferred wake LIFO, the mutex waiter lists and the Mach
port waiter lists. The comment beside it said the four never overlap. They do,
and here is the one that mattered:

A thread doing a `mach_msg_recv_timed` is held on the port's waiter list by
`th_runq_link` for the whole of its park. Its deadline expires. The PIT posts
it, the drain wakes it, and the scheduler puts it on the runqueue -- **through
the very field the port is holding it by**. Now the two lists are one list. A
sender that pops the port's head writes NULL into the field and truncates the
*runqueue*; the next enqueue writes the runqueue's head into the *port's* list
and the message after that is handed to a thread that was never waiting for it.
Threads stop being on any list at all.

So an object's queue has a field of its own now (`th_wait_link`), and so does
the deferred-wake LIFO (`th_irq_link`), for the same reason one level up: the
thread sitting on that LIFO is `BLOCKED` and somebody else is entitled to wake
it before the drain arrives. Eight bytes each. The sleep queue had already been
given its own field for exactly this reason, and its comment had already
written the rule down; nobody applied it to the other three.

**A park that does not park still has to leave the list.** Same family, found
first. A thread puts itself on the port's list and calls
`thread_block_release`, and there are three ways back out of that call. A
sender that woke it took it off the list on the way past. A deadline that woke
it did not, and the caller knew that and tidied up. And a park that read the
note from the section above and **returned without ever blocking** did not
either -- and said nothing, so the loop went round and put the thread on the
list a *second* time, writing the tail's forward pointer to the tail itself: a
one-element cycle nothing can ever be extracted from. From then on every wake
the port hands out goes to that phantom instead of to whoever is actually
asleep.

The detach is now unconditional, which is also the only version of this that
does not have to be re-reasoned every time somebody invents a new way out of a
park. It costs a walk of a list that is nearly always empty.

**And the tail was not the head.** Taking a thread off one of these lists said
"if the link I just wrote is NULL then the list is empty", which is true only
when the thread being removed was also the head. Remove the *tail* of a
two-deep list and the head is still there, but the tail pointer said NULL -- so
the next thread to park took the "nobody is waiting" branch and wrote itself
over the head, and the thread already waiting there was never woken by anybody
again. One waiter is the common case, which is why this survived: it needs two
threads on one port and the second one to give up first.

Two things guard it now, and the first one is what proved the diagnosis rather
than argued for it. A **tripwire** at each of the three places a thread puts
itself on one of these lists asserts that it is not already on it; built
against the unfixed tree it fires on an ordinary boot, in `stress_ool`, naming
the branch:

```
*** kernel panic: KASSERT(self->th_runq_link == NULL && p->p_waiters_tail != self)
    at mach/port_msg.c:1720: recv: already on this port's waiter list
```

And a **selftest** (`port-wait`) arranges both scenes instead of waiting for
them. The race that produces the first one needs a wake to arrive against a
thread that has decided to sleep and not yet committed -- a window of a few
instructions on another processor -- but the note that wake leaves is a value a
test can simply set, so what was one boot in four is now deterministic on any
number of processors. The second scene parks a helper thread, times out behind
it twice, and then sends a message that the helper has to receive.

**And a thread that dies still owes the deadline list an answer.** A timed wait
is put on that list by its caller before the park and taken off by the same
caller after it -- and a thread killed *mid-park* never comes back to do the
second half, because `thread_block_release` retires it from inside the block,
above the layer that registered it. What is left behind is a pointer to a
thread about to be reaped, on the one list the timer walks on every tick. Like
everything else here it truncates rather than crashing: `sched_add_timed_waiter`
pushes at the head without asking whether the thread is already there, so once
the corpse's memory is handed to a new thread that registers a deadline of its
own, the push overwrites a forward pointer the list was still using, and every
waiter behind it stops having a deadline at all. `thread_exit` now takes itself
off, and the push asserts that it is not already on.

**⚠ And one of the same family is left**, named here rather than fixed. The same
kill-mid-park exit leaves the thread on the *port's* waiter list, because that
one cannot be undone from `thread_exit` without knowing which object is holding
it. The port's teardown drains that list later and wakes a thread that may
already have been reaped. Closing it means the waiting thread recording *what*
it is waiting on so the exit path can undo it, which is a rung of its own; what
is owed here is saying so.

### One more thing the hypervisor said

The same register dump answered a question nobody had asked. Read down the CR0
column: `80010013` on the boot processor and `e0010013` on the other three. The
difference is `CD` and `NW` -- **the caches, which come out of reset turned
off**. Every write to `CR0` in the trampoline was an `orl`, so the two bits
survived into long mode and every application processor had been running the
whole kernel uncached.

Nothing says so. The processor is correct, it boots, it passes; it is simply an
order of magnitude slower than the one that started it, and only on real
hardware, because an emulator ignores the bits entirely. It is the same shape
as the list in `cpu_state_init` -- a per-CPU register nobody inherits -- except
that it has to be fixed before there is anywhere to report it from, so it lives
in the trampoline beside `PG`.

### ⚠ Where this leaves it, measured rather than claimed

Six four-processor boots from a pristine volume: **five of them 92 or 93 checks
passing with nothing failing, and `apfsck` clean on all six**. Before these
fixes the same run put two boots in three at 70-76 checks with the tail of the
work missing.

The sixth still fails, and it fails the same way the block cache did:

```
apfs-ckpt: superblock at 54 unreadable
filewrite: FAIL nothing can be made inside a directory ring 3 just made
```

That is a block coming back with a checksum that does not match -- not a stale
block, which would be self-consistent, but a torn one. So there is a second
reader-writer overlap under the one that has been fixed, and the place to look
is the ATA channel: `ata_pio_xfer` holds `ch_lock` across a multi-sector
transfer, but the per-sector wait for the drive's interrupt *releases* it in
order to sleep, so the channel is not actually owned for the length of a
command. It is written down here rather than guessed at.

Next on the roadmap: **replacing an existing name** with a rename, which
POSIX requires and this refuses out loud; a **torn-write stand**, which
would make the checkpoint's promise that an interruption anywhere leaves the
old transaction a measurement rather than a claim; and the SMP work these
rungs make possible -- narrowing a shootdown to the processors that actually
have the pmap, a runqueue per CPU with work stealing, and an output ring so
the console stops holding interrupts off for a line.

Reading the tree stopped being O(volume) per question along the way.  The
same boot that read **54434 records over 6381 nodes** to answer its 1718
questions now answers the same 1718 with **2267 records over 4943**, and
what it looks up by name it finds through Apple's own directory hash
instead of by comparing every name on the volume.

What the tree still does NOT do is give a LEVEL back.  A root left with a
single child could be replaced by that child, and measured on the image a
one-child root is accepted in silence -- so this is a tree that can end
up taller than its contents need, which costs a lookup one hop and is not
a thing any checker objects to.

(SMAP user-pointer bracketing, the `vm_allocate` syscall, the whole XNU
ladder through S4 -- a clean-room dyld and libSystem, under which
unmodified Apple binaries run -- a filesystem on `dev/disk0`,
virtual-copy OOL semantics, files that can be created and removed, a
B-tree that grows a level, a file whose records need not share a leaf, a
real Apple shell redirecting into a file, nodes that leave the tree when
they empty, directories that can be made and removed, and a terminal a
program can put into raw mode were all on this list once and have since
landed.)

## License

BSD-2-Clause.  See SPDX headers in individual files.
