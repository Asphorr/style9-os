/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "cpu.h"
#include "cpuid.h"
#include "fpu.h"
#include "kprintf.h"
#include "msr.h"
#include "panic.h"
#include "sched.h"
#include "smap.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "uart.h"

/*
 * Block 0 is initialised in the image rather than by cpu_bsp_init, so
 * cp_self is already right when the GS base is first written and read.
 * An AP's block is filled by cpu_register from the MADT.
 */
struct cpu	cpus[MAXCPU] = {
	[0] = { .cp_self = &cpus[0], .cp_id = 0 },
};

/*
 * Both start at one: the boot CPU.  Others are counted present by
 * cpu_register and online by cpu_mark_online once they arrive.
 */
static unsigned int	ncpu_present = 1;	/* (c) blocks filled in */
static unsigned int	ncpu_online = 1;	/* (a) running kernel   */

void
cpu_bsp_init(void)
{
	uint32_t	eax;
	uint32_t	ebx;
	uint32_t	ecx;
	uint32_t	edx;

	wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)&cpus[0]);

	/*
	 * The APIC id, from CPUID because that needs no mapped APIC; the
	 * MADT walk recognises the running CPU by it.  lapic_init re-reads
	 * it from the ID register and checks the two agree.
	 */
	cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);
	cpus[0].cp_lapic_id = CPUID_1_EBX_APICID(ebx);
}

/*
 * Everything a processor must be told about itself before it can run a
 * thread.  Control registers and MSRs are per-CPU, so an AP inherits none
 * of the boot CPU's; it does share the page tables, GDT layout and IDT, so
 * an omission never looks like one.  It looks like:
 *
 *	no CR0.WP	-- ring 0 writes straight through read-only pages, so
 *			   the kernel's copy-on-write faults never happen on
 *			   that CPU and two processes share a "private" page.
 *	no CR4.OSFXSR	-- #UD on the first FXRSTOR, i.e. the first context
 *			   switch.
 *	no CR4.SMAP	-- kernel dereferences of user pointers do not fault,
 *			   on that CPU only.
 *	no EFER.SCE	-- #UD on the first system call made there.
 *
 * CR0.CD and CR0.NW (caches off after INIT) are cleared earlier, in
 * aptramp.S, in the write that turns paging on.
 *
 * The boot CPU does not call this: it gets CR0.WP from boot.S and the rest
 * from kmain.  fpu_init and syscall_init call the same per-CPU halves used
 * here, so each fact has one copy; smap_enable_runtime and smep_enable set
 * their CR4 bits on the boot CPU, and smap_init_cpu copies each one only
 * where the kernel turned it on.
 */
void
cpu_state_init(void)
{
	uint64_t	cr0;

	/*
	 * CR0.WP: ring 0 obeys the read-only bit.  The trampoline sets only
	 * what it must, so an AP arrives without it.
	 */
	__asm__ __volatile__ ("mov %%cr0, %0" : "=r" (cr0));
	cr0 |= ((uint64_t)1 << 16);
	__asm__ __volatile__ ("mov %0, %%cr0" : : "r" (cr0));

	fpu_init_cpu();
	smap_init_cpu();
	syscall_init_cpu();
}

void
cpu_print(void)
{
	struct cpu	*seen;

	/*
	 * The round trip is the test: `seen' came through the GS base,
	 * &cpus[0] is where C believes the block is.  If they differ, the
	 * first per-CPU write lands somewhere nobody is looking.
	 */
	seen = curcpu();
	if (seen != &cpus[0])
		panic("cpu: gs base reads %p, block 0 is at %p",
		    (void *)seen, (void *)&cpus[0]);
	if (seen->cp_id != 0)
		panic("cpu: block 0 claims id %u", (unsigned int)seen->cp_id);

	seen->cp_online = 1;	/* it is executing; that is the evidence */

	kprintf("cpu: boot cpu id=%u, per-cpu block at %p via gs base\n",
	    cpu_id(), (void *)seen);
}

int
cpu_register(uint32_t lapic_id, uint32_t acpi_id)
{
	struct cpu	*cp;
	unsigned int	i;

	/*
	 * The MADT lists the boot CPU, already in block 0, like any other,
	 * and not necessarily first; recognise it by APIC id.
	 */
	for (i = 0; i < ncpu_present; i++) {
		if (cpus[i].cp_lapic_id == lapic_id) {
			cpus[i].cp_acpi_id = acpi_id;
			return (-1);
		}
	}

	if (ncpu_present >= MAXCPU)
		return (-1);

	cp = &cpus[ncpu_present];
	cp->cp_self     = cp;
	cp->cp_id       = ncpu_present;
	cp->cp_lapic_id = lapic_id;
	cp->cp_acpi_id  = acpi_id;
	cp->cp_online   = 0;

	ncpu_present++;
	return ((int)cp->cp_id);
}

void
cpu_mark_online(void)
{
	struct cpu	*cp;

	cp = curcpu();

	/* Release the flag the starter spins on, then count this CPU once. */
	__atomic_store_n(&cp->cp_online, 1, __ATOMIC_RELEASE);
	__atomic_add_fetch(&ncpu_online, 1, __ATOMIC_ACQ_REL);
}

unsigned int
cpu_present_count(void)
{

	return (ncpu_present);
}

unsigned int
cpu_online_count(void)
{

	return (__atomic_load_n(&ncpu_online, __ATOMIC_ACQUIRE));
}

/*
 * Who is on each processor, said without the console.  kprintf takes the
 * console lock every CPU shares, so it cannot report on a machine wedged
 * on it; this writes at the UART directly, with no lock and no formatting
 * (hence hex a nibble at a time).
 */
void
cpu_census_uart(void)
{
	static const char	 hex[] = "0123456789abcdef";
	struct cpu		*cp;
	struct thread		*th;
	unsigned int		 i;
	int			 sh;

	uart_puts("\r\n[census]");
	for (i = 0; i < ncpu_present; i++) {
		cp = &cpus[i];
		th = cp->cp_curthread;

		uart_puts(" cpu");
		uart_putc((char)('0' + (i % 10)));
		uart_putc('=');
		uart_puts(th != NULL && th->th_name != NULL ? th->th_name :
		    (cp->cp_online != 0 ? "(parked)" : "(down)"));
		/*
		 * The task id too: ring-3 threads share generic names
		 * ("user-elf", "fork"), and which program is spinning is the
		 * question.
		 */
		uart_putc('.');
		for (sh = 12; sh >= 0; sh -= 4)
			uart_putc(hex[((th != NULL && th->th_task != NULL ?
			    th->th_task->t_id : 0) >> sh) & 0xF]);
		uart_putc('/');
		for (sh = 28; sh >= 0; sh -= 4)
			uart_putc(hex[(cp->cp_switches >> sh) & 0xF]);
	}
	uart_puts("\r\n");
}

/*
 * One line per CPU.  Reads the blocks by address, the only way to see
 * another CPU's, and unlocked: each field is one word its owner may be
 * writing, so a line can be a moment stale, and no lock is needed to ask.
 */
void
cpu_dump(void)
{
	struct cpu	*cp;
	struct thread	*th;
	unsigned int	i;

	for (i = 0; i < ncpu_present; i++) {
		cp = &cpus[i];
		th = cp->cp_curthread;
		if (cp->cp_online == 0) {
			kprintf("cpu %u: lapic %u, acpi id %u, NOT STARTED\n",
			    (unsigned int)cp->cp_id,
			    (unsigned int)cp->cp_lapic_id,
			    (unsigned int)cp->cp_acpi_id);
			continue;
		}
		/*
		 * Online with no thread and no idle thread is parked, not
		 * idle: it has never been in the scheduler.
		 */
		if (th == NULL && cp->cp_idle_thread == NULL) {
			kprintf("cpu %u: lapic %u, online and PARKED, "
			    "stack 0x%llx\n", (unsigned int)cp->cp_id,
			    (unsigned int)cp->cp_lapic_id,
			    (unsigned long long)cp->cp_kernel_rsp);
			continue;
		}
		/*
		 * `running' is a snapshot; the switch count says where the
		 * work actually went.
		 */
		kprintf("cpu %u: lapic %u, running %s, idle id=%llu, "
		    "preempt %d%s, quantum %u/%u, %llu switches, "
		    "kstack 0x%llx\n",
		    (unsigned int)cp->cp_id, (unsigned int)cp->cp_lapic_id,
		    th != NULL && th->th_name != NULL ? th->th_name : "-",
		    cp->cp_idle_thread != NULL ?
		    (unsigned long long)cp->cp_idle_thread->th_id : 0ULL,
		    cp->cp_preempt_count,
		    cp->cp_need_resched != 0 ? " (resched owed)" : "",
		    cp->cp_quantum_used, (unsigned int)PREEMPT_QUANTUM_TICKS,
		    (unsigned long long)cp->cp_switches,
		    (unsigned long long)cp->cp_kernel_rsp);
	}
}
