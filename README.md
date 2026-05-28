# style9-os

A BSD `style(9)` hobby kernel for x86_64.  Boots via PVH (QEMU `-kernel`
for ELF64) or Multiboot1/2, runs preemptive multitasking with
Mach-style ports as the IPC primitive.

Built from scratch — no upstream tree, no glue from another OS.

## What's in it

| layer | files | what |
|---|---|---|
| boot | `arch/amd64/boot.S`, `linker.ld` | MB2 header + PVH ELF note, 32→64 mode transition, identity-map low 1 GiB with 2 MiB huge pages |
| gdt | `arch/amd64/gdt.c` | 8-entry GDT laid out for `SYSCALL`/`SYSRET` MSR arithmetic; 104-byte TSS with `rsp0` for the ring-3 → ring-0 stack switch on IRQs/exceptions |
| syscall | `arch/amd64/syscall_entry.S`, `kern/syscall.c` | `SYSCALL` entry stub: stashes user RSP, switches to per-thread kernel stack via `syscall_kernel_rsp`, builds `struct syscall_frame`, calls the C dispatcher, `SYSRETQ` back; MSRs (EFER.SCE / STAR / LSTAR / FMASK) wired in `syscall_init`.  Caller-save registers are restored from the frame on the way out so user code matches the Linux x86_64 ABI |
| syscalls list | `kern/syscall.c` | `SYS_PRINT`, `SYS_EXIT`, `SYS_YIELD`, `SYS_PORT_ALLOC`, `SYS_PORT_DEALLOC`, `SYS_MSG_SEND`, `SYS_MSG_RECV`, `SYS_MSG_RECV_TIMED`, `SYS_MSG_RPC`, `SYS_SPAWN`, `SYS_TASK_ALIVE` -- Mach IPC surface fully exposed plus minimal task lifecycle |
| usermode | `arch/amd64/usermode.c` | per-task PML4 staged at task creation; ELF loader maps PT_LOAD segments at U=1 + the requested prot bits; `iretq` lands at `e_entry` with a fresh user stack |
| elf loader | `kern/elf.c` | static ELF64 parser.  Walks PT_LOAD program headers, allocates 4 KiB user pages and maps them via the task's pmap with R/W/X taken from p_flags, copies file data via `pmm_kva_from_pa` of the freshly-allocated frame |
| progreg | `kern/progreg.c` | "program registry" -- four user ELFs (`hello`, `clock`, `tasks`, `sh`) embedded in the kernel image via objcopy.  `progreg_spawn(name)` creates a task and loads the matching ELF into it; `SYS_SPAWN` is the userspace door |
| traps | `arch/amd64/idt.c`, `intr.c`, `isr.S` | 48-vector IDT, trap-frame dispatcher, symbolicated autopsy on exception |
| irqs | `arch/amd64/pic.c`, `pit.c` | 8259 remap to 0x20/0x28, PIT @ 100 Hz with quantum tracking |
| clock | `kern/tsc.c`, `clock.c` | rdtsc + PIT-anchored calibration, `uptime_ms`, busy-sleep |
| memory map | `kern/memmap.c` | parses MB1 / MB2 / PVH boot info into one sorted table |
| pmm | `kern/pmm.c` | bitmap page allocator, first-fit, capped at boot identity-map |
| pmap | `arch/amd64/pmap.c` | per-task 4-level page tables (kernel half shared, user half private); `pmap_kenter / kremove / kextract` for kernel mappings, `pmap_enter / remove / extract` for per-task user mappings |
| vm map | `kern/vm.c` | per-task `vm_map` records anon user-VA ranges; `vm_map_find_space` picks free holes, `vm_map_release_anon` walks `VME_F_ANON` entries at task teardown and `pmm_free_page`s each leaf before `pmap_destroy` rips the page-table tree |
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
| ata drv | `dev/ata_drv.c` | LBA28+LBA48 ATA PIO driver, exposed as `dev/disk0` -- block device handle, no filesystem yet |
| tty | `dev/tty.c` | VT-style ANSI CSI state machine over the VGA console; sh.elf drives the manpage TUI through it |
| user shell | `user/sh.c` | sh.elf, the ring-3 shell.  Apple/BSD-flavoured manpage TUI: NAME/SYSTEM/SEE ALSO sections, gray-on-black with bold-white labels, horizontal rule, fixed status bar with uptime.  Builtins: `help / echo / clear / about / ool`.  Spawnable: any registered user program |
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
wrapper for QEMU launching is in `tools/runlog.ps1`.

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
sh.elf -- a ring-3 shell in an Apple/BSD-flavoured manpage TUI:

```
  NAME           style9-os -- BSD-flavoured x86_64 kernel with Mach IPC

  SYSTEM         arch     x86_64
                 ram      1704 / 130944 KiB
                 tasks    2 live
                 shell    sh.elf

  SEE ALSO       style(9), help(1)
 ──────────────────────────────────────────────────────────────────────────────
 style9-os(9)                                                          00:00:04
 ──────────────────────────────────────────────────────────────────────────────
$
```

Builtins:

```
help                    list commands + spawnable programs
echo                    print arguments
clear                   erase + repaint splash
about                   version banner + live counters
ool                     OOL Mach IPC round-trip via svc/echool
```

Anything else is a `SYS_SPAWN` -- registered programs `hello`, `clock`,
`tasks` are reachable by name.  `clock` and `tasks` themselves are ring-3
consumers of `svc/clock` and `svc/tasks` (registered kernel services
reachable via `bootstrap_lookup`).

The legacy `kern/shell.c` stays in the tree as a fallback for the case
where sh.elf fails to spawn -- it has the full ddb-style introspection
surface (`mem`, `memmap`, `pmap`, `task`, `thread`, `sched`, `port list`,
`stress <subcommand>`, `crash <variant>`, `panic`).  The same surface is
reachable interactively via `ddb` once that's invoked from a panic path.

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
  descriptors carry capabilities; OOL descriptors carry bulk memory
  copied into freshly-allocated frames in the receiver's pmap (physical
  copy in v1; virtual copy / COW deferred).
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

Next on the roadmap: SMAP so user-pointer derefs are bracketed by
`stac/clac`; a `vm_allocate` syscall so userspace can ask the kernel for
fresh user-VA ranges (unblocks the OOL `deallocate` flag and gets us a
real heap); a filesystem on `dev/disk0` so ELFs can live on disk instead
of being embedded; virtual-copy (COW) OOL semantics; real SMP.

## License

BSD-2-Clause.  See SPDX headers in individual files.
