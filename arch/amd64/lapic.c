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
#include "cpuid.h"
#include "intr.h"
#include "kprintf.h"
#include "lapic.h"
#include "msr.h"
#include "pit.h"
#include "pmap.h"
#include "sched.h"
#include "tsc.h"
#include "vm.h"

/*
 * Register offsets from the APIC's page base.  Every access is a single
 * aligned 32-bit read or write; anything wider is undefined.
 */
#define	LAPIC_ID		0x020	/* this CPU's hardware APIC id     */
#define	LAPIC_VERSION		0x030	/* version + max LVT entry         */
#define	LAPIC_TPR		0x080	/* task priority                   */
#define	LAPIC_EOI		0x0B0	/* end of interrupt (write only)   */
#define	LAPIC_SVR		0x0F0	/* spurious vector + enable bit    */
#define	LAPIC_LVT_TIMER		0x320
#define	LAPIC_LVT_LINT0		0x350
#define	LAPIC_LVT_LINT1		0x360
#define	LAPIC_LVT_ERROR		0x370
#define	LAPIC_ICR_LO		0x300	/* write sends the message         */
#define	LAPIC_ICR_HI		0x310	/* destination, in the top byte    */
#define	LAPIC_TIMER_ICR		0x380	/* initial count                   */
#define	LAPIC_TIMER_CCR		0x390	/* current count                   */
#define	LAPIC_TIMER_DCR		0x3E0	/* divide configuration            */

#define	LAPIC_SVR_ENABLE	(1u << 8)

/* LVT fields.  The vector occupies the low eight bits. */
#define	LVT_DELIVERY_FIXED	(0u << 8)
#define	LVT_DELIVERY_NMI	(4u << 8)
#define	LVT_DELIVERY_EXTINT	(7u << 8)
#define	LVT_MASKED		(1u << 16)
#define	LVT_TIMER_PERIODIC	(1u << 17)

/*
 * Interrupt-command fields.  DELIVERY_PENDING must be waited on: the ICR
 * is one register, and a second message written while the first is still
 * going out loses one of them.
 */
#define	ICR_DELIVERY_PENDING	(1u << 12)
#define	ICR_MODE_INIT		(5u << 8)
#define	ICR_MODE_STARTUP	(6u << 8)
#define	ICR_LEVEL_ASSERT	(1u << 14)

/*
 * How long to wait for a message to leave.  Delivery takes nanoseconds;
 * the bound only exists so a message that never goes out returns an
 * answer instead of hanging the boot.
 */
#define	LAPIC_ICR_WAIT_US	100

/*
 * Divide the APIC's input clock by 16.  Any divisor would do for a rate
 * measured against the TSC; 16 keeps a full 32-bit count from wrapping in
 * the calibration window at any plausible bus clock (undivided at 1 GHz it
 * would last four seconds; the window is a tenth of one).
 */
#define	LAPIC_DCR_DIV16		0x3
#define	LAPIC_DIVISOR		16

/*
 * How long to measure for, and how long to then let the timer run while
 * its interrupts are counted, in microseconds of TSC time.
 *
 * The ruler is the TSC, not PIT interrupts.  This host delivers PIT
 * interrupts in bursts, so ten of them can arrive in 90 ms and read the
 * counting rate 10% low -- straight into the reload count and every slice.
 * A counter the CPU reads cannot be hurried.  The PIT is still counted
 * alongside; the gap between the two is its delivery deficit.
 */
#define	LAPIC_CAL_US		100000	/* 100 ms                          */
#define	LAPIC_PROBE_US		200000	/* 200 ms                          */
#define	LAPIC_PROBE_HZ		100	/* rate the timer is asked to keep  */

static volatile uint8_t	*lapic_va;		/* (c) NULL until mapped   */
static uint64_t		 lapic_pa;		/* (c) 0 until mapped      */
static bool		 lapic_ok;		/* (c)                     */
static uint32_t		 lapic_hz;		/* (c) ticks/s at DIV16    */

/*
 * Set once, by lapic_timer_start, while the timer is still masked, so no
 * ordering against the ISR is needed.  False during lapic_timer_probe:
 * its twenty interrupts must not spend anybody's slice.
 */
static bool		 lapic_preempting;	/* (c) after timer_start   */
static unsigned int	 lapic_tick_hz;		/* (c) rate it now keeps   */
static uint64_t		 lapic_start_pit;	/* (c) PIT at the handover */

static bool	lapic_timer_arm(unsigned int hz);

static inline uint32_t
lapic_read(unsigned int reg)
{

	return (*(volatile uint32_t *)(lapic_va + reg));
}

static inline void
lapic_write(unsigned int reg, uint32_t val)
{

	*(volatile uint32_t *)(lapic_va + reg) = val;
}

void
lapic_eoi(void)
{

	if (!lapic_ok)
		return;
	lapic_write(LAPIC_EOI, 0);
}

bool
lapic_present(void)
{

	return (lapic_ok);
}

uint32_t
lapic_id(void)
{

	if (!lapic_ok)
		return (0);
	return (lapic_read(LAPIC_ID) >> 24);
}

uint32_t
lapic_timer_hz(void)
{

	return (lapic_hz);
}

uint64_t
lapic_base_pa(void)
{

	return (lapic_pa);
}

/*
 * Wait, bounded, for the last interrupt command to leave.  False if it
 * never did; callers report that rather than retry.
 */
static bool
lapic_icr_idle(void)
{
	uint64_t	t0;

	if ((lapic_read(LAPIC_ICR_LO) & ICR_DELIVERY_PENDING) == 0)
		return (true);

	t0 = tsc_read();
	while ((lapic_read(LAPIC_ICR_LO) & ICR_DELIVERY_PENDING) != 0) {
		if (tsc_to_us(tsc_read() - t0) > LAPIC_ICR_WAIT_US)
			return (false);
		__asm__ __volatile__ ("pause");
	}
	return (true);
}

/*
 * Send one interrupt command to the processor with this APIC id.  The
 * destination goes in the high half first: writing the low half sends.
 */
static bool
lapic_ipi_send(uint32_t apic_id, uint32_t cmd)
{

	if (!lapic_ok)
		return (false);
	if (!lapic_icr_idle())
		return (false);

	lapic_write(LAPIC_ICR_HI, apic_id << 24);
	lapic_write(LAPIC_ICR_LO, cmd);

	return (lapic_icr_idle());
}

bool
lapic_ipi_init(uint32_t apic_id)
{

	/*
	 * Assert only.  The INIT de-assert is for the discrete 82489DX;
	 * integrated APICs ignore it.
	 */
	return (lapic_ipi_send(apic_id, ICR_MODE_INIT | ICR_LEVEL_ASSERT));
}

bool
lapic_ipi_startup(uint32_t apic_id, uint64_t tramp_pa)
{
	uint32_t	vec;

	/*
	 * STARTUP carries a page number in its low byte, so the trampoline
	 * must be page-aligned and below 0x100000.  Anything else is refused
	 * rather than started at a rounded-down address.
	 */
	if ((tramp_pa & 0xFFF) != 0 || (tramp_pa >> 12) > 0xFF)
		return (false);
	vec = (uint32_t)(tramp_pa >> 12);

	return (lapic_ipi_send(apic_id,
	    ICR_MODE_STARTUP | ICR_LEVEL_ASSERT | vec));
}

bool
lapic_ipi_vector(uint32_t apic_id, uint8_t vec)
{

	/*
	 * Fixed delivery, physical destination, edge triggered: every field
	 * zero but the vector and the assert bit.
	 */
	if (vec < INTR_LOCAL_BASE)
		return (false);

	return (lapic_ipi_send(apic_id, ICR_LEVEL_ASSERT | (uint32_t)vec));
}

static void
lapic_timer_isr(struct trapframe *tf)
{

	(void)tf;

	/* Per-CPU, so the report's per-CPU tick rate is honest. */
	__atomic_add_fetch(&curcpu()->cp_timer_ticks, 1, __ATOMIC_RELAXED);

	/*
	 * Debit the slice of whatever runs on this CPU, once this timer owns
	 * the job; the one PIT could only ever charge one CPU.
	 *
	 * Not gated on preempt_is_enabled: the gate belongs at the schedule
	 * point, so a critical section that ends later still owes the
	 * reschedule earned here.
	 */
	if (lapic_preempting && preempt_quantum_tick())
		preempt_resched_request();
}

bool
lapic_init(void)
{
	uint64_t	base;
	uint64_t	pa;
	uint32_t	eax;
	uint32_t	ebx;
	uint32_t	ecx;
	uint32_t	edx;
	uint32_t	ver;

	cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);
	if ((edx & (1u << 9)) == 0) {
		kprintf("lapic: this CPU reports no local APIC -- "
		    "staying on the 8259 alone\n");
		return (false);
	}

	base = rdmsr(MSR_APIC_BASE);

	/*
	 * In x2APIC mode the registers are MSRs and the MMIO window is gone;
	 * a machine that arrives in it would fault on every read below.
	 */
	if ((base & APIC_BASE_EXTD) != 0) {
		kprintf("lapic: found in x2APIC mode, which this driver does "
		    "not speak -- staying on the 8259 alone\n");
		return (false);
	}

	/* Globally enabled out of reset; set anyway, not trusting firmware. */
	if ((base & APIC_BASE_EN) == 0) {
		base |= APIC_BASE_EN;
		wrmsr(MSR_APIC_BASE, base);
	}

	pa = base & APIC_BASE_ADDR_MASK;

	/*
	 * Mapped identity and uncacheable.  The identity VA is free: the
	 * kernel map covers the low gigabyte and ring 3 the one above, while
	 * the APIC sits near the top of the 32-bit range.  Uncacheable because
	 * these are device registers; a cached read of the timer's current
	 * count would repeat the first answer.
	 */
	if (!pmap_kenter(pa, pa,
	    VM_PROT_READ | VM_PROT_WRITE | PMAP_NOCACHE)) {
		kprintf("lapic: could not map registers at 0x%llx\n",
		    (unsigned long long)pa);
		return (false);
	}
	lapic_va = (volatile uint8_t *)(uintptr_t)pa;
	lapic_pa = pa;
	lapic_ok = true;

	ver = lapic_read(LAPIC_VERSION);

	/*
	 * Accept every priority.  TPR is zero out of reset on the boot CPU,
	 * but an AP's need not be, and a non-zero TPR silently holds off
	 * everything below it.
	 */
	lapic_write(LAPIC_TPR, 0);

	/*
	 * The two legacy pins, before the enable.  LINT0 carries the 8259's
	 * output and LINT1 the NMI line; both are masked out of reset, and
	 * enabling the APIC with them masked cuts off the PIT, keyboard and
	 * disk at once.  ExtINT means "ask the 8259 for the vector", which
	 * keeps the legacy interrupt path working unchanged.  Written
	 * unconditionally, not trusting what firmware left.
	 *
	 * ExtINT on the boot CPU only: the one 8259 is routed to one LINT0,
	 * and an AP claiming it too could take the same IRQ.  Every CPU
	 * wants its own NMI pin.
	 */
	if (cpu_id() == 0)
		lapic_write(LAPIC_LVT_LINT0, LVT_DELIVERY_EXTINT);
	else
		lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
	lapic_write(LAPIC_LVT_LINT1, LVT_DELIVERY_NMI);

	/* Nothing to run yet: the timer stays masked until it is measured. */
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);

	/*
	 * Software-enable, and name the spurious vector: what the CPU takes
	 * when an interrupt is withdrawn between arbitration and acknowledge.
	 * It deliberately has no handler, so the dispatcher ignores it
	 * without an EOI -- there is no in-service bit behind it.
	 */
	lapic_write(LAPIC_SVR, LAPIC_VEC_SPURIOUS | LAPIC_SVR_ENABLE);

	/*
	 * The id from the register replaces the one recorded earlier (CPUID
	 * in cpu_bsp_init on the boot CPU, the MADT via cpu_register on an
	 * AP).  A disagreement means the APICs were renumbered; say so.
	 */
	if (curcpu()->cp_lapic_id != (lapic_read(LAPIC_ID) >> 24))
		kprintf("lapic: *** cpuid called this processor %u, the APIC's "
		    "own register says %u ***\n",
		    (unsigned int)curcpu()->cp_lapic_id,
		    (unsigned int)(lapic_read(LAPIC_ID) >> 24));
	curcpu()->cp_lapic_id = lapic_read(LAPIC_ID) >> 24;

	kprintf("lapic: id=%u version=0x%02x, %u LVT entries, "
	    "regs at 0x%llx (uncached), legacy pins wired\n",
	    (unsigned int)lapic_id(), (unsigned int)(ver & 0xFF),
	    (unsigned int)(((ver >> 16) & 0xFF) + 1),
	    (unsigned long long)pa);

	return (true);
}

void
lapic_timer_probe(void)
{
	uint64_t	c0;
	uint64_t	cal_us;
	uint64_t	probe_us;
	uint64_t	pit0;
	uint64_t	fired;
	uint32_t	remaining;
	uint32_t	elapsed;
	uint32_t	want;

	if (!lapic_ok)
		return;

	if (tsc_hz() == 0) {
		kprintf("lapic: timer unmeasured -- the TSC is not "
		    "calibrated\n");
		return;
	}

	/*
	 * Preemption off across both halves.  Not needed for the ratio --
	 * being descheduled would not skew it -- but the windows span several
	 * quanta, and the loops should measure what they asked for.
	 * Interrupts stay on: the second half counts them arriving.
	 */
	preempt_disable();

	/*
	 * Calibration: count down from the top with the timer masked and
	 * read the counter against elapsed TSC time -- as measured, not as
	 * requested, since the loop can only overshoot.
	 */
	lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFFu);

	c0 = tsc_read();
	while (tsc_to_us(tsc_read() - c0) < LAPIC_CAL_US)
		__asm__ __volatile__ ("pause");
	remaining = lapic_read(LAPIC_TIMER_CCR);
	cal_us = tsc_to_us(tsc_read() - c0);

	lapic_write(LAPIC_TIMER_ICR, 0);	/* stop counting */

	elapsed = 0xFFFFFFFFu - remaining;
	if (cal_us == 0) {
		preempt_enable();
		kprintf("lapic: timer unmeasured -- no time passed\n");
		return;
	}
	lapic_hz = (uint32_t)(((uint64_t)elapsed * 1000000) / cal_us);

	/*
	 * Delivery: run periodic at LAPIC_PROBE_HZ and count what arrives.
	 * A single count needs the mapping, enable, LVT, vector, IDT gate and
	 * dispatcher all to be right.
	 */
	__atomic_store_n(&curcpu()->cp_timer_ticks, 0, __ATOMIC_RELAXED);
	intr_install_local(LAPIC_VEC_TIMER, lapic_timer_isr);

	lapic_write(LAPIC_TIMER_ICR, lapic_hz / LAPIC_PROBE_HZ);
	lapic_write(LAPIC_LVT_TIMER,
	    LAPIC_VEC_TIMER | LVT_DELIVERY_FIXED | LVT_TIMER_PERIODIC);

	c0 = tsc_read();
	pit0 = pit_ticks();
	while (tsc_to_us(tsc_read() - c0) < LAPIC_PROBE_US)
		__asm__ __volatile__ ("pause");
	probe_us = tsc_to_us(tsc_read() - c0);
	pit0 = pit_ticks() - pit0;

	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_TIMER_ICR, 0);

	preempt_enable();

	fired = __atomic_load_n(&curcpu()->cp_timer_ticks, __ATOMIC_RELAXED);
	want  = (uint32_t)((probe_us * LAPIC_PROBE_HZ) / 1000000);

	kprintf("lapic: timer counts at %u kHz (input / %u), "
	    "measured against the TSC over %llu us\n",
	    (unsigned int)(lapic_hz / 1000), (unsigned int)LAPIC_DIVISOR,
	    (unsigned long long)cal_us);

	/*
	 * A tally, not an assertion: a count over an uncontrolled window is a
	 * gauge.  Only zero, which no load explains, is called out.
	 */
	kprintf("lapic: timer delivered %llu interrupt(s) at %u Hz, "
	    "want about %u (the PIT delivered %llu)%s\n",
	    (unsigned long long)fired, (unsigned int)LAPIC_PROBE_HZ, want,
	    (unsigned long long)pit0,
	    fired == 0 ? "  *** NOTHING ARRIVED ***" : "");
}

/*
 * Arm this CPU's timer periodic at `hz' and start its clock for the
 * report.  Divisor, LVT, then count: writing the count starts the timer,
 * so everything it is delivered as must be set first.
 */
static bool
lapic_timer_arm(unsigned int hz)
{
	struct cpu	*cp;
	uint32_t	 icr;

	if (!lapic_ok || lapic_hz == 0 || hz == 0)
		return (false);

	icr = lapic_hz / hz;
	if (icr == 0) {
		kprintf("lapic: %u Hz is faster than this timer can count "
		    "(%u ticks/s)\n", hz, (unsigned int)lapic_hz);
		return (false);
	}

	cp = curcpu();
	__atomic_store_n(&cp->cp_timer_ticks, 0, __ATOMIC_RELAXED);
	cp->cp_timer_start = tsc_read();

	intr_install_local(LAPIC_VEC_TIMER, lapic_timer_isr);

	lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);
	lapic_write(LAPIC_LVT_TIMER,
	    LAPIC_VEC_TIMER | LVT_DELIVERY_FIXED | LVT_TIMER_PERIODIC);
	lapic_write(LAPIC_TIMER_ICR, icr);

	return (true);
}

/*
 * The same timer on an AP, after the hand-over, at the counting rate
 * measured on the boot CPU.  That assumes every APIC timer counts off the
 * same bus clock; lapic_timer_report checks it, printing each CPU's
 * delivered rate.
 */
bool
lapic_timer_start_ap(void)
{

	if (!lapic_preempting)
		return (false);
	return (lapic_timer_arm(lapic_tick_hz));
}

bool
lapic_timer_start(void)
{
	unsigned int	hz;

	if (!lapic_ok || lapic_hz == 0)
		return (false);

	/*
	 * The PIT's rate.  The slice is PREEMPT_QUANTUM_TICKS ticks, tuned
	 * against a 100 Hz tick; another rate here would silently change the
	 * quantum without touching the constant that expresses it.
	 */
	hz = pit_hz();
	if (hz == 0)
		return (false);

	/* Checked first: the hand-over below is one-way. */
	if (lapic_hz / hz == 0) {
		kprintf("lapic: %u Hz is faster than this timer can count "
		    "(%u ticks/s) -- preemption stays with the PIT\n",
		    hz, (unsigned int)lapic_hz);
		return (false);
	}

	/*
	 * The hand-over.  Exactly one timer may debit the slice; two would
	 * silently halve the quantum.  So the PIT is relieved before this
	 * timer is unmasked: the gap costs at most one tick of one slice.
	 *
	 * The PIT keeps ticking: timeouts and busy-sleeps are counted in its
	 * ticks.  It stops debiting, not ticking.
	 */
	pit_release_preempt();

	lapic_tick_hz = hz;
	lapic_preempting = true;
	lapic_start_pit = pit_ticks();

	if (!lapic_timer_arm(hz)) {
		lapic_preempting = false;
		kprintf("lapic: could not arm the timer -- preemption stays "
		    "with the PIT\n");
		return (false);
	}

	kprintf("lapic: timer periodic at %u Hz (count %u per tick), "
	    "this CPU debits its own slice now -- %u tick(s), %u ms\n",
	    hz, (unsigned int)(lapic_hz / hz),
	    (unsigned int)PREEMPT_QUANTUM_TICKS,
	    (unsigned int)(PREEMPT_QUANTUM_TICKS * 1000 / hz));

	return (true);
}

bool
lapic_timer_preempting(void)
{

	return (lapic_preempting);
}

void
lapic_timer_report(void)
{
	struct cpu	*cp;
	uint64_t	 now;
	uint64_t	 ticks;
	uint64_t	 pit;
	uint64_t	 us;
	uint64_t	 rate;
	uint64_t	 slice_us;
	unsigned int	 i;

	if (!lapic_preempting)
		return;

	now = tsc_read();
	pit = pit_ticks() - lapic_start_pit;

	/*
	 * One line per CPU: the slice is a per-CPU fact, and a timer that
	 * never started, runs at the wrong rate or stopped being delivered
	 * shows up here and nowhere else.
	 */
	for (i = 0; i < cpu_present_count(); i++) {
		cp = &cpus[i];
		if (cp->cp_timer_start == 0)
			continue;
		ticks = __atomic_load_n(&cp->cp_timer_ticks,
		    __ATOMIC_RELAXED);
		us = tsc_to_us(now - cp->cp_timer_start);
		if (us == 0)
			continue;
		rate     = ticks * 1000000 / us;
		slice_us = ticks == 0 ? 0 :
		    (uint64_t)PREEMPT_QUANTUM_TICKS * us / ticks;
		kprintf("lapic: cpu %u ticked %llu in %llu ms -- %llu Hz "
		    "of %u asked, slice %llu us%s\n", i,
		    (unsigned long long)ticks,
		    (unsigned long long)(us / 1000),
		    (unsigned long long)rate, lapic_tick_hz,
		    (unsigned long long)slice_us,
		    ticks == 0 ? "  *** THE SLICE IS UNCHARGED ***" : "");
	}

	/*
	 * And the PIT beside them, over the same TSC time: it is delivered
	 * and can fall behind (see LAPIC_CAL_US), and the gap is its deficit.
	 */
	us = tsc_to_us(now - cpus[0].cp_timer_start);
	if (us == 0)
		return;
	kprintf("lapic: the PIT ticked %llu over the same %llu ms -- %llu Hz "
	    "of the %u it keeps\n", (unsigned long long)pit,
	    (unsigned long long)(us / 1000),
	    (unsigned long long)(pit * 1000000 / us), lapic_tick_hz);
}
