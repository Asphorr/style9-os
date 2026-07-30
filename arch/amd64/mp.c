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
#include "kprintf.h"
#include "lapic.h"
#include "memmap.h"
#include "mp.h"
#include "msr.h"
#include "pmap.h"
#include "pmm.h"
#include "tsc.h"

/*
 * The startup sequence's waits, from the multiprocessor protocol: hold after
 * INIT while the far processor resets its state, then a short pause after the
 * first startup message before repeating it.  The repeat is not superstition --
 * the protocol says to send two, because on some implementations the first can
 * be dropped if it arrives while the processor is still coming out of INIT.
 *
 * ARRIVAL is how long we are willing to wait for the processor to say it got
 * there.  It has an entire trampoline to walk, in three addressing modes, plus
 * whatever a printf costs it, and on an emulated machine under a loaded host
 * that is not instant -- but a tenth of a second is thousands of times what it
 * takes, so a processor still absent at the end of it is absent.
 */
#define	AP_INIT_WAIT_US		10000
#define	AP_SIPI_WAIT_US		200
#define	AP_ARRIVAL_WAIT_US	100000

/*
 * A stack per processor, in the same size the scheduler gives a thread, and
 * for the same reason: what runs on it is kernel code with an interrupt frame
 * possibly on top.  This one is only the BOOTSTRAP stack -- what an AP stands
 * on between long mode and having threads of its own.
 */
#define	AP_STACK_PAGES		4

void		ap_entry(struct cpu *cp);

extern char	ap_tramp_start[];
extern char	ap_tramp_end[];

static void	mp_wait_us(uint64_t us);
static bool	mp_install_trampoline(void);
static bool	mp_page_is_ram(uint64_t pa);
static bool	mp_start_one(struct cpu *cp);

/*
 * Real-time wait, measured rather than counted.
 *
 * A loop of a fixed number of pauses is not a wait, it is a guess that stops
 * being true when the code is built differently or the host is busy -- and
 * every wait in this file is part of a protocol with real microseconds in it.
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
 * Does the firmware's memory map call this page ordinary RAM?
 *
 * The trampoline's address is a constant chosen to be in conventional memory,
 * and pmm marks the whole first megabyte used so it will never hand it out --
 * but both of those are today's arrangements, and copying code over something
 * the firmware still owns would be a fault with no message attached.  Cheap to
 * ask, so it is asked.
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
	 * A byte loop because this kernel has no memcpy, and volatile on the
	 * destination because what is being written is code another processor
	 * will fetch: the compiler has no reason to believe these stores are
	 * ever read, and every reason to think a page of them can be merged
	 * away.
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
 * Ask one processor to start, and wait for it to say it did.
 *
 * Serialised on purpose: the parameter block is one, and so is the trampoline
 * page.  Starting them in parallel would need one page each and buy a few
 * milliseconds once per boot.
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
	 * The bootstrap stack is also this CPU's SYSCALL stack for now, which
	 * is what cp_kernel_rsp means -- nothing lands on it until this CPU
	 * runs a user thread, and by then the scheduler will have replaced it
	 * with that thread's own.  Recording it makes `cpu' able to show where
	 * a parked processor is standing.
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
	 * Spin, holding nothing.  The arriving processor prints a line of its
	 * own on the way in, which means it takes the console lock -- so this
	 * wait must not be holding anything that processor could want, and it
	 * is not.
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

unsigned int
mp_start_aps(void)
{
	unsigned int	present;
	unsigned int	started;
	unsigned int	i;

	present = cpu_present_count();
	if (present <= 1)
		return (0);

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

/*
 * WHERE AN APPLICATION PROCESSOR BECOMES ONE OF THIS KERNEL'S CPUS.
 *
 * Called from the trampoline with a stack and nothing else: no per-CPU base,
 * no GDT of its own, no IDT register, no APIC.  The order below is the order
 * of what depends on what, and the first line is the one that everything else
 * is written on top of -- until the GS base is installed, every per-CPU
 * reference in this kernel reads physical page zero, and spin_lock is a
 * per-CPU reference.
 *
 * ⚠ AND THEN IT PARKS, WITH INTERRUPTS OFF, which is the honest end of this
 * step rather than a shortcut.  Three things are missing before this processor
 * can be allowed to run anything:
 *
 *	- Locks that close interrupts.  spin_lock spins without disabling
 *	  them, which is safe on the boot processor only because no handler
 *	  there takes a lock the mainline can hold -- an argument about which
 *	  handlers exist, and not one that survives a second CPU.
 *	- An idle thread of its own, and a scheduler that knows a runqueue can
 *	  be served by more than one CPU.
 *	- TLB shootdown.  This processor would cache translations that the
 *	  boot processor changes, and nothing tells it to forget them.
 *
 * Until then it sits here, where it can do no harm, and says so in `cpu'.
 */
void
ap_entry(struct cpu *cp)
{

	wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)cp);

	/*
	 * Its own GDT and TSS -- the TSS above all, because that is the one
	 * thing that cannot be shared: it carries the stack a ring transition
	 * lands on, and two processors pointing at one would fault onto the
	 * same stack.  gdt_init_cpu reads which CPU it is from the block the
	 * line above installed.
	 */
	gdt_init_cpu();

	/*
	 * The IDT is one table for the machine but the register that points at
	 * it is per-processor, and this one holds zero.  Before anything that
	 * could fault, because a fault with no IDT is a triple fault and a
	 * silent reset.
	 */
	idt_load();

	/*
	 * Its own APIC: the registers are already mapped (the boot processor
	 * did that, and the mapping is shared), but the enable bit, the task
	 * priority and every LVT entry are per-processor state that comes up
	 * out of reset masked.
	 */
	(void)lapic_init();

	/*
	 * Announce BEFORE claiming to be online, which is the wrong way round
	 * for readability and the right way round for the console.
	 *
	 * ⚠ The tty takes its lock per CHARACTER, deliberately -- spin_lock
	 * panics on a same-CPU re-acquire, so a run of output cannot hold it
	 * across the run.  On one processor that was invisible; with two, two
	 * kprintfs interleave BYTE BY BYTE, and the first four-processor boot
	 * printed "parkmp:ed with interrupt 3 s off".  Setting the flag last
	 * means the processor that is spinning on it cannot print until this
	 * line is finished, which is enough while the only concurrent output
	 * is a bring-up message.  It is not a substitute for serialising the
	 * console, which is owed the day these processors run anything.
	 */
	kprintf("cpu %u: online, lapic %u, stack at 0x%llx -- parked with "
	    "interrupts off\n", cpu_id(), (unsigned int)cp->cp_lapic_id,
	    (unsigned long long)cp->cp_kernel_rsp);

	/*
	 * Released with an ordering barrier: the processor that started this
	 * one is spinning on it, and everything above must be visible before
	 * the flag that says "everything above is done".
	 */
	cpu_mark_online();

	for (;;) {
		__asm__ __volatile__ ("cli; hlt");
	}
}
