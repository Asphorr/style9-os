/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_CPU_H_
#define	_MACHINE_CPU_H_

/*
 * Per-CPU state, and how a CPU finds its own: through the GS segment base,
 * set once per CPU, which makes `%gs:0' its block with nothing to look up.
 * An array indexed by CPU id would need the id first, and reading it from
 * the local APIC needs the APIC mapped.  cp_self, at offset zero, holds the
 * block's own address because a segment-relative reference cannot produce
 * a pointer.
 *
 * Loading a segment register zeroes its base.  In long mode the %fs/%gs
 * base lives only in the MSR, and `movw %ax, %gs' loads the descriptor's
 * base, zero for every flat descriptor here -- so a stray %gs reload points
 * this CPU's block at physical page zero.  Nothing may reload %gs once the
 * base is set.
 *
 * Ring 3 shares the base, for now.  A Darwin binary reaches its TLS
 * through %gs, so a real Darwin kernel keeps the user base in the register
 * and the kernel's in IA32_KERNEL_GS_BASE, exchanging them with SWAPGS on
 * every entry and exit.  Nothing in ring 3 uses %gs yet, so no entry path
 * swaps.  The day a thread wants TLS, swapgs goes into syscall_entry,
 * isr_common and their return paths -- and into the NMI/#DF paths only
 * with the CS check that keeps a nested entry from swapping twice.  Until
 * then a ring-3 %gs reference faults.
 */

/*
 * Offsets the assembler needs: syscall_entry.S reaches the two rsp fields
 * through %gs, curcpu() reads CPU_SELF.  Checked against the struct below;
 * a drift would land a syscall frame on the wrong stack.
 */
#define	CPU_SELF		0
#define	CPU_KERNEL_RSP		8
#define	CPU_USER_RSP		16

/*
 * How many CPUs the kernel will bring up.  Raising it costs one
 * cache-line-aligned block each in .data.
 */
#define	MAXCPU			8

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

/* Cache-line aligned so CPUs updating their own counters share no line. */
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
	 * Set by this CPU, last of its bring-up, and spun on by whoever
	 * started it: a starter that sees it can trust the rest of the block.
	 */
	volatile int		 cp_online;	/* (a) it got here          */

	/*
	 * th_spin_depth / th_spin_saved_if for while this CPU has no thread
	 * to keep them in (early boot; an AP until it joins the scheduler).
	 * No context switch can happen then, so the CPU is the right owner.
	 */
	int			 cp_spin_depth;		/* (i)              */
	bool			 cp_spin_saved_if;	/* (i)              */

	/*
	 * Serial number of the last TLB-invalidation request this CPU has
	 * carried out; written only by it.  A serial rather than an ack count
	 * because a request can be noticed twice (the IPI and a spin loop's
	 * poll): re-storing a number is harmless, a second decrement is not.
	 * See pmap_tlb_poll.
	 */
	volatile uint64_t	 cp_tlb_gen;		/* (a)              */

	volatile int		 cp_preempt_count;	/* (i) see sched.h  */
	volatile int		 cp_need_resched;	/* (i)              */
	volatile unsigned int	 cp_quantum_used;	/* (i) timer ticks  */

	/*
	 * Ticks this CPU's APIC timer has delivered, and the TSC when it was
	 * armed.  Per-CPU: one machine-wide counter would run N times fast
	 * with N CPUs ticking into it.
	 */
	volatile uint64_t	 cp_timer_ticks;	/* (a)              */
	uint64_t		 cp_timer_start;	/* (c) TSC at arm   */

	/* Context switches performed here: where the work went. */
	volatile uint64_t	 cp_switches;		/* (i)              */

	/*
	 * Set by mp_release_aps once the per-CPU state this CPU will inherit
	 * is settled; the AP's parking loop waits for it.
	 */
	volatile int		 cp_release;		/* (a)              */
} __attribute__((aligned(64)));

_Static_assert(offsetof(struct cpu, cp_self) == CPU_SELF,
    "CPU_SELF disagrees with struct cpu");
_Static_assert(offsetof(struct cpu, cp_kernel_rsp) == CPU_KERNEL_RSP,
    "CPU_KERNEL_RSP disagrees with struct cpu");
_Static_assert(offsetof(struct cpu, cp_user_rsp) == CPU_USER_RSP,
    "CPU_USER_RSP disagrees with struct cpu");

extern struct cpu	cpus[MAXCPU];

/*
 * This CPU's block, read out of the GS base.  Volatile asm, so two calls
 * in one function both re-read: the answer holds only while this thread
 * cannot migrate, and the compiler cannot see where that window ends.
 *
 * current_thread may be held across a preemption point (after a
 * migration the new CPU's cp_curthread is the same thread); anything else
 * derived from curcpu(), cp_id above all, must be read inside the critical
 * section that uses it.
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

/* The thread running on this CPU; an lvalue (`current_thread = next'). */
#define	current_thread		(curcpu()->cp_curthread)

static inline unsigned int
cpu_id(void)
{

	return ((unsigned int)curcpu()->cp_id);
}

/*
 * Point this CPU's SYSCALL stack at `rsp'.  The stub uses it unchecked,
 * so it must be right before every return to ring 3; see
 * switch_user_kstack in kern/sched.c.
 */
static inline void
cpu_set_kernel_rsp(uint64_t rsp)
{

	curcpu()->cp_kernel_rsp = rsp;
}

/*
 * Point the boot CPU's GS base at block 0 and record its APIC id from
 * CPUID.  Must run before anything touches per-CPU state -- spin_lock
 * does, through the preempt count -- so it is the first statement of
 * kmain.  (An AP sets its own base first thing in ap_entry.)  Silent: there
 * is no console yet; cpu_print checks the result.
 */
void		cpu_bsp_init(void);

/*
 * The per-CPU control registers and MSRs this kernel depends on: CR0.WP,
 * the FPU's CR0/CR4 bits, CR4.SMAP and the four SYSCALL MSRs.  For an AP,
 * which inherits none of the boot CPU's; each missing one fails silently
 * and elsewhere (symptoms in cpu.c).
 */
void		cpu_state_init(void);

/*
 * Read this CPU's block back through the GS base, compare it with
 * &cpus[0], log it, and panic if they differ.
 */
void		cpu_print(void);

/*
 * One line per CPU: what runs on it, its idle thread, its slice and switch
 * count.  Backs the shell's `cpu'.
 */
void		cpu_dump(void);

/*
 * One line naming each CPU's thread, task and switch count, written byte
 * by byte at the UART with no lock or formatting, so it works when the
 * console or its lock is the problem.
 */
void		cpu_census_uart(void);

/*
 * Claim a block for a processor the MADT describes, with the ids it gives;
 * called once per usable entry.  Returns the dense index, or -1 if this is
 * the running processor (matched by APIC id) or there is no room.  Starts
 * nothing: cp_online stays zero until the CPU itself sets it.
 */
int		cpu_register(uint32_t lapic_id, uint32_t acpi_id);

/*
 * The calling processor is running kernel code: set the flag its starter
 * waits on and count it online.  No argument: a CPU announces only itself.
 */
void		cpu_mark_online(void);

/*
 * PRESENT: CPUs described by the firmware and given blocks.  ONLINE: CPUs
 * running kernel code.  The gap is the CPUs that failed to start.
 */
unsigned int	cpu_present_count(void);
unsigned int	cpu_online_count(void);

#endif /* !__ASSEMBLER__ */

#endif /* !_MACHINE_CPU_H_ */
