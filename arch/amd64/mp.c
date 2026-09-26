/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "intr.h"
#include "kprintf.h"
#include "lapic.h"
#include "memmap.h"
#include "mp.h"
#include "msr.h"
#include "pmap.h"
#include "pmm.h"
#include "sched.h"
#include "thread.h"
#include "uart.h"
#include "tsc.h"

/*
 * Startup-protocol waits: after INIT, and after the first STARTUP IPI
 * before sending the second (the protocol sends two, since the first can
 * be lost while the processor is still leaving INIT).  ARRIVAL bounds how
 * long a started processor gets to walk the trampoline and check in; it
 * needs a tiny fraction of that, so one still absent after it is absent.
 */
#define	AP_INIT_WAIT_US		10000
#define	AP_SIPI_WAIT_US		200
#define	AP_ARRIVAL_WAIT_US	100000

/* How long a released processor gets to reach its idle thread. */
#define	AP_RELEASE_WAIT_US	100000

/*
 * The stack an AP arrives on.  sched_cpu_attach later adopts it as the
 * CPU's idle thread, so it lives for good; sized like a thread's kstack
 * (THREAD_DEFAULT_KSTACK).
 */
#define	AP_STACK_PAGES		4

void		ap_entry(struct cpu *cp);

extern char	ap_tramp_start[];
extern char	ap_tramp_end[];

static void	mp_wait_us(uint64_t us);
static bool	mp_install_trampoline(void);
static bool	mp_page_is_ram(uint64_t pa);
static bool	mp_start_one(struct cpu *cp);
static void	mp_resched_ipi(struct trapframe *tf);
static void	mp_where_ipi(struct trapframe *tf);

/*
 * Real-time wait, measured on the TSC rather than counted in loop
 * iterations: the protocol's waits are in real microseconds.
 */
static void
mp_wait_us(uint64_t us)
{
	uint64_t	t0;

	t0 = tsc_read();
	while (tsc_to_us(tsc_read() - t0) < us)
		__asm__ __volatile__ ("pause");
}

/*
 * Does the firmware's memory map call this page ordinary RAM?  pmm never
 * hands out the first megabyte, but copying the trampoline over something
 * the firmware still owns would fail with no message, and asking is cheap.
 */
static bool
mp_page_is_ram(uint64_t pa)
{
	size_t	i;

	for (i = 0; i < memmap_nentries; i++) {
		if (memmap_entries[i].me_type != MEMMAP_FREE)
			continue;
		if (pa < memmap_entries[i].me_base)
			continue;
		if (pa + PAGE_SIZE >
		    memmap_entries[i].me_base + memmap_entries[i].me_length)
			continue;
		return (true);
	}
	return (false);
}

static bool
mp_install_trampoline(void)
{
	volatile uint8_t	*dst;
	const uint8_t		*src;
	size_t			 len;
	size_t			 i;

	len = (size_t)(ap_tramp_end - ap_tramp_start);
	if (len == 0 || len > AP_PARAM_OFF) {
		kprintf("mp: trampoline is %u bytes and the room before its "
		    "parameter block is %u -- not installed\n",
		    (unsigned int)len, (unsigned int)AP_PARAM_OFF);
		return (false);
	}

	if (!mp_page_is_ram(AP_TRAMP_PA)) {
		kprintf("mp: the memory map does not call 0x%x usable RAM -- "
		    "no trampoline, no application processors\n",
		    (unsigned int)AP_TRAMP_PA);
		return (false);
	}

	/*
	 * A byte loop (no memcpy here), volatile because the stores are code
	 * another processor will fetch, which the compiler cannot see being
	 * read.
	 */
	src = (const uint8_t *)ap_tramp_start;
	dst = (volatile uint8_t *)pmm_kva_from_pa(AP_TRAMP_PA);
	for (i = 0; i < len; i++)
		dst[i] = src[i];

	kprintf("mp: trampoline installed at 0x%x, %u bytes, parameters at "
	    "0x%x\n", (unsigned int)AP_TRAMP_PA, (unsigned int)len,
	    (unsigned int)AP_PARAM_PA);
	return (true);
}

/*
 * Start one processor and wait for it to check in.  Serialised: there is
 * one trampoline page and one parameter block.
 */
static bool
mp_start_one(struct cpu *cp)
{
	volatile uint64_t	*param;
	uint64_t		 stack;
	uint64_t		 t0;

	stack = pmm_alloc_pages(AP_STACK_PAGES);
	if (stack == PA_INVALID) {
		kprintf("mp: no memory for cpu %u's stack\n",
		    (unsigned int)cp->cp_id);
		return (false);
	}

	/*
	 * cp_kernel_rsp starts as the bootstrap stack.  No SYSCALL lands on
	 * it before this CPU runs a user thread, and switching to one
	 * replaces it; until then it shows `cpu' where a parked CPU stands.
	 */
	param = (volatile uint64_t *)pmm_kva_from_pa(AP_PARAM_PA);
	param[AP_P_CR3 / 8] = pmap_kernel_root_pa();
	param[AP_P_RSP / 8] = (uint64_t)(uintptr_t)pmm_kva_from_pa(stack) +
	    AP_STACK_PAGES * PAGE_SIZE;
	param[AP_P_CPU / 8] = (uint64_t)(uintptr_t)cp;

	cp->cp_kernel_rsp = param[AP_P_RSP / 8];

	if (!lapic_ipi_init(cp->cp_lapic_id)) {
		kprintf("mp: cpu %u (lapic %u): the INIT message would not "
		    "leave the APIC\n", (unsigned int)cp->cp_id,
		    (unsigned int)cp->cp_lapic_id);
		return (false);
	}
	mp_wait_us(AP_INIT_WAIT_US);

	if (!lapic_ipi_startup(cp->cp_lapic_id, AP_TRAMP_PA)) {
		kprintf("mp: cpu %u (lapic %u): the startup message would not "
		    "leave the APIC\n", (unsigned int)cp->cp_id,
		    (unsigned int)cp->cp_lapic_id);
		return (false);
	}
	mp_wait_us(AP_SIPI_WAIT_US);

	if (cp->cp_online == 0)
		(void)lapic_ipi_startup(cp->cp_lapic_id, AP_TRAMP_PA);

	/*
	 * Spin holding nothing: the arriving processor prints on its way in
	 * and so takes the console lock.
	 */
	t0 = tsc_read();
	while (cp->cp_online == 0) {
		if (tsc_to_us(tsc_read() - t0) > AP_ARRIVAL_WAIT_US) {
			kprintf("mp: cpu %u (lapic %u) never arrived -- it was "
			    "asked twice and given %u us\n",
			    (unsigned int)cp->cp_id,
			    (unsigned int)cp->cp_lapic_id,
			    (unsigned int)AP_ARRIVAL_WAIT_US);
			return (false);
		}
		__asm__ __volatile__ ("pause");
	}

	return (true);
}

/*
 * "Look at the runqueue."  The handler only sets need_resched, which the
 * tail of intr_dispatch already acts on.  At release time the same IPI
 * wakes a parked processor out of its hlt; there the interrupt itself is
 * the message.
 */
static void
mp_resched_ipi(struct trapframe *tf)
{

	(void)tf;
	preempt_resched_request();
}

void
mp_resched(struct cpu *cp)
{

	if (cp == NULL || cp->cp_online == 0)
		return;
	(void)lapic_ipi_vector(cp->cp_lapic_id, INTR_VEC_RESCHED);
}

/*
 * "Where are you?"  A CPU's program counter cannot be read from another
 * CPU, and everything else here reports the scheduler's view of it, which
 * says nothing about a CPU looping outside the scheduler.  So the far CPU
 * takes an interrupt and reports the trapframe it lands in: ring (cs & 3,
 * kernel or user spin) and RIP.
 *
 * Written straight at the UART with no lock, like cpu_census_uart, since
 * it is wanted when the console is not answering.
 */
void
mp_where(struct trapframe *tf)
{
	static const char	hex[] = "0123456789abcdef";
	int			sh;

	uart_puts("\r\n[where] cpu");
	uart_putc((char)('0' + (cpu_id() % 10)));
	uart_puts((tf->tf_cs & 3) == 3 ? " ring3 rip=" : " ring0 rip=");
	for (sh = 60; sh >= 0; sh -= 4)
		uart_putc(hex[(tf->tf_rip >> sh) & 0xF]);
	uart_puts("\r\n");
}

static void
mp_where_ipi(struct trapframe *tf)
{

	mp_where(tf);
}

void
mp_where_all(void)
{
	struct cpu	*me;
	unsigned int	 i;

	me = curcpu();
	for (i = 0; i < cpu_present_count(); i++) {
		if (&cpus[i] == me || cpus[i].cp_online == 0)
			continue;
		(void)lapic_ipi_vector(cpus[i].cp_lapic_id, INTR_VEC_WHERE);
	}
}

unsigned int
mp_start_aps(void)
{
	unsigned int	present;
	unsigned int	started;
	unsigned int	i;

	present = cpu_present_count();
	if (present <= 1)
		return (0);

	/*
	 * Before any AP starts: RESCHED is what later releases it from its
	 * parking loop.
	 */
	intr_install_local(INTR_VEC_RESCHED, mp_resched_ipi);
	intr_install_local(INTR_VEC_WHERE, mp_where_ipi);

	if (!lapic_present()) {
		kprintf("mp: %u processor(s) described and no local APIC to "
		    "start them with\n", present);
		return (0);
	}

	if (!mp_install_trampoline())
		return (0);

	started = 0;
	for (i = 1; i < present; i++) {
		if (mp_start_one(&cpus[i]))
			started++;
	}

	kprintf("mp: %u of %u application processor(s) running kernel code, "
	    "%u cpu(s) online\n", started, present - 1, cpu_online_count());

	return (started);
}

unsigned int
mp_release_aps(void)
{
	struct cpu	*me;
	struct cpu	*cp;
	uint64_t	 t0;
	unsigned int	 present;
	unsigned int	 joined;
	unsigned int	 i;

	me      = curcpu();
	present = cpu_present_count();
	joined  = 0;

	for (i = 0; i < present; i++) {
		cp = &cpus[i];
		if (cp == me || cp->cp_online == 0)
			continue;

		/*
		 * Flag first, interrupt second: the interrupt only ends the
		 * hlt.  Sent before the flag, it could wake a processor that
		 * sees nothing and sleeps again, with no second IPI coming.
		 */
		__atomic_store_n(&cp->cp_release, 1, __ATOMIC_RELEASE);
		mp_resched(cp);

		/*
		 * One at a time: each allocates its idle thread, and letting
		 * them all into kmalloc at once would make boot the first
		 * test of that lock under contention.
		 */
		t0 = tsc_read();
		while (cp->cp_curthread == NULL) {
			if (tsc_to_us(tsc_read() - t0) > AP_RELEASE_WAIT_US) {
				kprintf("mp: cpu %u was released and did not "
				    "reach the scheduler in %u us\n",
				    (unsigned int)cp->cp_id,
				    (unsigned int)AP_RELEASE_WAIT_US);
				break;
			}
			__asm__ __volatile__ ("pause");
		}
		if (cp->cp_curthread != NULL)
			joined++;
	}

	kprintf("mp: %u application processor(s) in the scheduler, %u cpu(s) "
	    "serving one runqueue\n", joined, joined + 1);

	return (joined);
}

/*
 * An application processor becomes one of this kernel's CPUs.
 *
 * Called from the trampoline with a stack and nothing else: no GS base, no
 * GDT, IDT or APIC of its own.  The order below is dependency order, and
 * the GS base comes first: until it is set every per-CPU reference, and so
 * every spin_lock, reads physical page zero.
 *
 * It is started early and parked, and released late (mp_release_aps, near
 * the end of kmain): CR4.SMAP and the SYSCALL MSRs are set only then, and
 * a CPU already running threads would run them without either.  Parked,
 * its timer and LINT0 are masked and no runqueue knows it; it answers only
 * IPIs -- TLB invalidation, `where', and the RESCHED that releases it.
 *
 * Released, it loads its per-CPU control state, adopts its stack as its
 * idle thread, arms its own timer and joins the runqueue, after which it
 * is an ordinary CPU.
 */
void
ap_entry(struct cpu *cp)
{

	wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)cp);

	/*
	 * Its own GDT and above all its own TSS, which holds the stack a ring
	 * transition lands on.  gdt_init_cpu finds this CPU through the GS
	 * base set above.
	 */
	gdt_init_cpu();

	/*
	 * The IDT is shared but IDTR is per-CPU and still zero.  Before
	 * anything that can fault: a fault with no IDT is a triple fault.
	 */
	idt_load();

	/*
	 * The registers are already mapped (shared mapping), but the enable
	 * bit, TPR and LVT entries are per-CPU state.
	 */
	(void)lapic_init();

	/*
	 * Announce before cpu_mark_online, so this line is out before the
	 * starting CPU, spinning on the flag, prints its next one.
	 */
	kprintf("cpu %u: online, lapic %u, stack at 0x%llx -- parked, "
	    "answering invalidations\n", cpu_id(),
	    (unsigned int)cp->cp_lapic_id,
	    (unsigned long long)cp->cp_kernel_rsp);

	/*
	 * A release store: everything above is visible before the flag the
	 * starting CPU spins on.
	 */
	cpu_mark_online();

	/*
	 * Parked until mp_release_aps.  `sti; hlt' back to back: sti takes
	 * effect only after the next instruction, so no interrupt can slip in
	 * between and be slept through, and hlt with interrupts off would
	 * never wake.  cli on the way out, so the flag is read and everything
	 * after it runs with interrupts off.
	 */
	while (__atomic_load_n(&cp->cp_release, __ATOMIC_ACQUIRE) == 0)
		__asm__ __volatile__ ("sti; hlt; cli");

	/*
	 * The per-CPU control registers and MSRs.  Before the scheduler: its
	 * first switch here does an FXRSTOR (needs CR4.OSFXSR), and it may
	 * then run a user thread (needs CR0.WP, CR4.SMAP, EFER.SCE).
	 */
	cpu_state_init();

	/*
	 * A thread (which is also the idle thread), then the timer: the
	 * timer's interrupt debits a slice, and a slice belongs to a thread.
	 */
	sched_cpu_attach();

	if (!lapic_timer_start_ap())
		kprintf("cpu %u: *** no timer of its own -- nothing will "
		    "preempt what runs here ***\n", cpu_id());

	kprintf("cpu %u: in the scheduler, idle thread id=%llu, its own "
	    "timer debiting its own slice\n", cpu_id(),
	    (unsigned long long)cp->cp_idle_thread->th_id);

	sched_cpu_idle();
	/* NOTREACHED */
}
