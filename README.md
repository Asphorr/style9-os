# style9-os

A BSD `style(9)` hobby kernel for x86_64.  Boots via PVH (QEMU `-kernel`
for ELF64) or Multiboot1/2, runs preemptive multitasking with
Mach-style ports as the IPC primitive.

Built from scratch — no upstream tree, no glue from another OS.

## What's in it

| layer | files | what |
|---|---|---|
| boot | `arch/amd64/boot.S`, `linker.ld` | MB2 header + PVH ELF note, 32→64 mode transition, identity-map low 1 GiB with 2 MiB huge pages |
| traps | `arch/amd64/idt.c`, `intr.c`, `isr.S` | 48-vector IDT, trap-frame dispatcher, autopsy print on exception |
| irqs | `arch/amd64/pic.c`, `pit.c` | 8259 remap to 0x20/0x28, PIT @ 100 Hz with quantum tracking |
| clock | `kern/tsc.c`, `clock.c` | rdtsc + PIT-anchored calibration, `uptime_ms`, busy-sleep |
| memory map | `kern/memmap.c` | parses MB1 / MB2 / PVH boot info into one sorted table |
| pmm | `kern/pmm.c` | bitmap page allocator, first-fit, capped at boot identity-map |
| pmap | `arch/amd64/pmap.c` | 4-level page-table walker, `pmap_kenter / kremove / kextract` |
| kmem | `kern/kmem.c` | power-of-two bucketed allocator, header-magic redzones |
| ddb | `kern/ddb.c`, `panic.c`, `kprintf.c` | KASSERT, RBP-chain backtrace, in-kernel debugger |
| spinlock | `kern/spinlock.c` | holder tracking, preempt-counter integration |
| ports | `kern/port.c` | Mach `mach_msg_header_t` wire format, SEND / RECEIVE / SEND_ONCE rights, port descriptors carrying capabilities across spaces, blocking send on full queue |
| port sets | `kern/port.c` | `port_set_allocate / insert / remove`, recv-on-many |
| notif | `kern/port.c` | `MOVE_RECEIVE` rebinding, no-senders detection wakes recv with `MACH_E_DEAD` |
| tasks | `kern/task.c` | resource container, owns one `port_space` |
| threads | `kern/thread.c`, `arch/amd64/switch.S` | callee-saved context switch, kstack-backed |
| sched | `kern/sched.c` | cooperative + preemptive round-robin, idle thread reaps zombies |
| kbd drv | `kern/kbd_drv.c` | bridges the PS/2 IRQ ring to `kbd_input_port` |
| uart drv | `kern/uart_drv.c` | enables COM1 RX IRQ, bridges to `uart_input_port` (same wire format as the keyboard, single character per message) |
| shell | `kern/shell.c`, `cmds.c` | recv's on a port set that has both `kbd_input_port` and `uart_input_port` as members -- either input source drives the same line editor |
| drivers | `dev/*.c` | 16550 UART (TX polled, RX IRQ-driven), VGA, PS/2 keyboard, QEMU debugcon |

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

The kernel runs a 12-test stress pass at boot, end-to-end exercising
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
```

Sample boot output:

```
[10/12] stress moverecv 200
stress_moverecv: 200 rounds (transfer RECV right in flight)
stress_moverecv: names 0 -> 0, conserved 293 -> 293
stress_moverecv: PASS

[11/12] stress nosenders 100
stress_nosenders: 100 rounds (last sender drops, recv must wake with DEAD)
stress_nosenders: names 0 -> 0, conserved 293 -> 293
stress_nosenders: PASS

[12/12] stress sendblock 2000
stress_sendblock: 2000 rounds (producer blocks on full queue, slow consumer)
stress_sendblock: names 0 -> 0, conserved 293 -> 293
stress_sendblock: PASS
```

## Shell

Once the boot tests pass, you land in a shell:

```
help                    list commands
mem                     pmm + kmem stats
memmap                  firmware-supplied physical map
pmap                    kernel pmap state
uptime                  kernel uptime in s / ms / ticks
task                    list live tasks
thread                  list threads + scheduler state
sched                   context-switch and preempt counters
yield                   give up the CPU once
port new                allocate a port, print its name
port list               dump the kernel port_space
port pingpong N         single-thread Mach RPC demo
stress <subcommand>     run any of the stress tests interactively
crash <variant>         deliberately trip a kernel check
                        (dfree | wild | assert | unmapped | nonc)
panic                   user-requested panic, exercises the autopsy path
clear, echo
```

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
- The wire format matches real Mach (`mach_msg_header_t`, 24 bytes;
  `mach_msg_port_descriptor`, 8 bytes; `MACH_MSGH_BITS_COMPLEX`), so
  any future userspace ABI is forward-compatible.
- Port descriptors in messages translate names between the sender's
  `port_space` and the receiver's, transferring capabilities.  The
  same code path serves single-space (kernel ↔ kernel) and multi-space
  (kernel ↔ worker-task) IPC — it's just two different `port_space`
  arguments.

Deferred for now: out-of-line memory descriptors, real SMP,
user/kernel mode split (next on the roadmap: ring-3 task, `syscall`/
`sysret` plumbing, copy-in/out for `mach_msg`, ELF64 loader).  Each
will land when a stress test or a userspace target surfaces a need
for it.

## License

BSD-2-Clause.  See SPDX headers in individual files.
