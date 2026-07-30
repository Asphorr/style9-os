/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_CPU_H_
#define	_MACHINE_CPU_H_

/*
 * Per-CPU state, and how a CPU finds its own.
 *
 * WHAT THIS REPLACES.  Everything a CPU needs to know about itself was a
 * plain global: which thread is running (current_thread), which stack a
 * SYSCALL lands on (syscall_kernel_rsp), how deep this CPU is inside
 * critical sections (preempt_count), whether it owes a reschedule
 * (need_resched), which thread to fall back on when nothing is runnable
 * (idle_thread).  Every one of those is a per-CPU quantity that happened to
 * be correct as a global because there was one CPU, and every new line of
 * kernel code written against them made the eventual move more expensive.
 * So the move comes first, on one CPU, where the answer is known: nothing
 * about the system's behaviour may change, and the whole boot must say so.
 *
 * HOW A CPU FINDS ITS BLOCK: the GS segment base.  Not an array indexed by
 * an id, because getting the id is the problem -- reading the local APIC
 * needs the APIC mapped, and a CPU coming up needs its stack and its
 * curthread before it has mapped anything.  The GS base is a register, set
 * once per CPU, and `%gs:0' is then one memory reference away from the
 * block with nothing to look up.  That is also why cp_self sits at offset
 * zero and holds the block's own linear address: a segment-relative
 * reference cannot produce a pointer, so the block stores one.
 *
 * ⚠ LOADING A SEGMENT REGISTER ZEROES ITS BASE.  In long mode the base of
 * %fs/%gs lives only in the MSR; writing the register from a descriptor
 * (movw %ax, %gs) loads that descriptor's base, which is zero for every
 * flat descriptor in our GDT.  So a stray %gs reload silently points this
 * CPU's per-CPU block at physical address zero, and the first thing to
 * notice is a corrupted low page.  gdt_init_cpu used to reload %gs along
 * with the other segment registers and no longer does; nothing else in the
 * kernel may either.
 *
 * ⚠ RING 3 SHARES THE BASE, deliberately, for now.  A Darwin binary reaches
 * its thread-local storage through %gs, so a real Darwin kernel keeps the
 * user value in the register and the kernel value in
 * IA32_KERNEL_GS_BASE, exchanging them with SWAPGS at every entry and
 * exit.  We have no ring-3 %gs user yet -- ring 3 ran with a zero base and
 * therefore never touched %gs at all -- so the base stays the kernel's in
 * both rings and no entry path needs swapgs.  That is a debt with a known
 * shape and a known price: the day a thread wants TLS, swapgs goes into
 * syscall_entry, isr_common and their two return paths, and NOT into the
 * NMI/#DF paths without the CS-check that keeps a nested entry from
 * swapping twice.  Until then a ring-3 %gs reference faults, which is what
 * it did before.
 */

/*
 * Offsets the assembler needs.  syscall_entry.S reaches three fields
 * through %gs and cannot ask the compiler where they are, so they are
 * spelled out here and checked against the struct below -- a drift between
 * the two would land a syscall frame on the wrong stack, which is the kind
 * of bug that reads as random memory corruption several subsystems away.
 */
#define	CPU_SELF		0
#define	CPU_KERNEL_RSP		8
#define	CPU_USER_RSP		16

/*
 * How many CPUs the kernel is willing to bring up.  Sized for the machines
 * this runs on rather than for the architecture; raising it costs one
 * cache-line-aligned block each in .data.
 */
#define	MAXCPU			8

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

/*
 * Cache-line aligned so two CPUs updating their own counters do not fight
 * over one line.  On a single CPU that costs padding and buys nothing; it
 * is here because the cost of remembering later is a performance mystery
 * rather than a compile error.
 */
struct cpu {
	struct cpu		*cp_self;	/* (c) this block's address */
	uint64_t		 cp_kernel_rsp;	/* (i) SYSCALL lands here   */
	uint64_t		 cp_user_rsp;	/* (i) stashed across entry */

	struct thread		*cp_curthread;	/* (i) running here now     */
	struct thread		*cp_idle_thread;/* (c) this CPU's idler     */

	uint32_t		 cp_id;		/* (c) dense kernel index   */
	uint32_t		 cp_lapic_id;	/* (c) hardware id          */
	uint32_t		 cp_acpi_id;	/* (c) as the MADT names it */

	/*
	 * Set by this CPU itself once it is running kernel code, and read by
	 * whoever started it to find out whether it arrived.  Volatile
	 * because that reader spins on it, and written last of everything the
	 * arriving CPU sets up, so a starter that sees it can trust the rest
	 * of the block.
	 */
	volatile int		 cp_online;	/* (a) it got here          */

	volatile int		 cp_preempt_count;	/* (i) see sched.h  */
	volatile int		 cp_need_resched;	/* (i)              */
	volatile unsigned int	 cp_quantum_used;	/* (i) timer ticks  */
} __attribute__((aligned(64)));

_Static_assert(offsetof(struct cpu, cp_self) == CPU_SELF,
    "CPU_SELF disagrees with struct cpu");
_Static_assert(offsetof(struct cpu, cp_kernel_rsp) == CPU_KERNEL_RSP,
    "CPU_KERNEL_RSP disagrees with struct cpu");
_Static_assert(offsetof(struct cpu, cp_user_rsp) == CPU_USER_RSP,
    "CPU_USER_RSP disagrees with struct cpu");

extern struct cpu	cpus[MAXCPU];

/*
 * The one primitive: this CPU's block, read out of the GS base.
 *
 * Volatile asm rather than a cached load, so two calls in one function
 * re-read.  That matters the moment a thread can move between CPUs: the
 * answer is only valid for as long as this thread cannot be migrated, and
 * the compiler has no way to know where that window ends.
 *
 * ⚠ WHAT MAY OUTLIVE THE WINDOW.  current_thread is safe to hold across a
 * preemption point -- if the thread migrates, the new CPU's cp_curthread
 * is still this same thread -- but anything else derived from curcpu()
 * (cp_id above all) is stale the instant this thread can be moved, so read
 * it inside the critical section that uses it.
 */
static inline struct cpu *
curcpu(void)
{
	struct cpu	*cp;

	__asm__ __volatile__ ("movq %%gs:%c1, %0"
	    : "=r" (cp)
	    : "i" (CPU_SELF));
	return (cp);
}

/*
 * The thread running on this CPU.  An lvalue, as the global it replaces
 * was, so that the move to per-CPU state is the whole of the change and
 * not a hundred rewritten uses -- the scheduler still says
 * `current_thread = next'.
 */
#define	current_thread		(curcpu()->cp_curthread)

static inline unsigned int
cpu_id(void)
{

	return ((unsigned int)curcpu()->cp_id);
}

/*
 * Point this CPU's ring-transition stack at `rsp'.  The SYSCALL stub reads
 * it out of the per-CPU block with no register to spare and no chance to
 * check it, so the scheduler owes it a correct value before every return
 * to ring 3; see switch_user_kstack in kern/sched.c.
 */
static inline void
cpu_set_kernel_rsp(uint64_t rsp)
{

	curcpu()->cp_kernel_rsp = rsp;
}

/*
 * Install the GS base for the CPU executing this call and claim block
 * `id'.  Must run before ANY code that touches per-CPU state, which since
 * spin_lock counts preemption means before the first lock: it is the first
 * statement of kmain on the boot CPU, and will be the first statement an
 * application processor executes in C.
 *
 * Deliberately silent -- it runs before there is a console to print to.
 * cpu_print is the half that checks the result out loud.
 */
void		cpu_bsp_init(void);

/*
 * Prove the mechanism and say so: read this CPU's block back through the
 * segment base and compare it with the address the C side knows.  A boot
 * where the two disagree has a working kernel right up until the first
 * per-CPU write goes somewhere else, so it is worth one line of log and a
 * panic that names the fault.
 */
void		cpu_print(void);

/*
 * One line per CPU the kernel knows about: what is running on it, what it
 * falls back on, and how much of its slice is gone.  Backs the shell's `cpu'.
 */
void		cpu_dump(void);

/*
 * Claim a block for a processor the firmware described but nobody has started.
 * Called once per usable entry in the MADT, with the ids that table gives.
 * Fills the block far enough that the code which eventually starts this CPU
 * has somewhere to point its GS base -- cp_self above all, since a block whose
 * self-pointer is wrong is a CPU that cannot find itself.
 *
 * Returns the dense index claimed, or -1 if this is the processor already
 * running (matched by APIC id) or there is no room left.  It does NOT start
 * anything: cp_online stays zero until the CPU itself says otherwise.
 */
int		cpu_register(uint32_t lapic_id, uint32_t acpi_id);

/*
 * Say that the CALLING processor is now running kernel code: set the flag its
 * starter is waiting on and add it to the online count.  Takes no argument on
 * purpose -- a CPU announces itself and cannot be announced by somebody else.
 */
void		cpu_mark_online(void);

/*
 * Two different questions.  PRESENT is how many the firmware described and the
 * kernel has blocks for; ONLINE is how many are running kernel code.  They are
 * equal on a machine where every processor started, and the gap between them
 * is the interesting number on a machine where one did not.
 */
unsigned int	cpu_present_count(void);
unsigned int	cpu_online_count(void);

#endif /* !__ASSEMBLER__ */

#endif /* !_MACHINE_CPU_H_ */
