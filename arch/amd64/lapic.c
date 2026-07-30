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
#include "intr.h"
#include "kprintf.h"
#include "lapic.h"
#include "msr.h"
#include "pit.h"
#include "pmap.h"
#include "sched.h"
#include "vm.h"

/*
 * Register offsets from the APIC's page base.  Every access is a single
 * aligned 32-bit read or write; the architecture does not define anything
 * else, and a wider access is not a slow access but an undefined one.
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
 * Divide the APIC's input clock by 16 before it reaches the counter.  Any
 * divisor would do for a ratio measured against the PIT; 16 is chosen so
 * that a full 32-bit count cannot wrap during the calibration window on any
 * plausible bus frequency -- at 1 GHz undivided that would be four seconds,
 * and we measure for a tenth of one.
 */
#define	LAPIC_DCR_DIV16		0x3
#define	LAPIC_DIVISOR		16

/*
 * How long to measure for, and how long to then let the timer run while its
 * interrupts are counted.  Both in PIT ticks, because the PIT is the clock
 * being measured against and counting its interrupts is the only reading
 * here that does not depend on the thing under test.
 */
#define	LAPIC_CAL_TICKS		10	/* 100 ms at 100 Hz                */
#define	LAPIC_PROBE_TICKS	20	/* 200 ms                          */
#define	LAPIC_PROBE_HZ		100	/* rate the timer is asked to keep  */

static volatile uint8_t	*lapic_va;		/* (c) NULL until mapped   */
static bool		 lapic_ok;		/* (c)                     */
static uint32_t		 lapic_hz;		/* (c) ticks/s at DIV16    */
static volatile uint64_t lapic_timer_count;	/* (a) ISR tally           */

static inline void
cpuid_count(uint32_t leaf, uint32_t subleaf,
    uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{

	__asm__ __volatile__ ("cpuid"
	    : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
	    : "0" (leaf), "2" (subleaf));
}

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

static void
lapic_timer_isr(struct trapframe *tf)
{

	(void)tf;
	__atomic_add_fetch(&lapic_timer_count, 1, __ATOMIC_RELAXED);
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
	 * x2APIC reaches the same registers through MSRs, and in that mode
	 * the MMIO window is not there to be read.  Nobody has put us in it
	 * -- we would have to ask -- but a machine that arrives already in
	 * x2APIC mode would answer every register read below with a fault,
	 * so the possibility is checked rather than assumed away.
	 */
	if ((base & APIC_BASE_EXTD) != 0) {
		kprintf("lapic: found in x2APIC mode, which this driver does "
		    "not speak -- staying on the 8259 alone\n");
		return (false);
	}

	/*
	 * The global enable bit is set out of reset, but say so explicitly:
	 * a kernel that relies on the state firmware left is a kernel that
	 * works until the firmware changes.
	 */
	if ((base & APIC_BASE_EN) == 0) {
		base |= APIC_BASE_EN;
		wrmsr(MSR_APIC_BASE, base);
	}

	pa = base & APIC_BASE_ADDR_MASK;

	/*
	 * Mapped identity and UNCACHEABLE.  The identity VA is free: the
	 * kernel's own map covers the low gigabyte and ring 3 lives in the
	 * one above it, while the APIC sits near the top of the 32-bit
	 * range.  Uncacheable is not an optimisation choice -- these are
	 * device registers whose value changes underneath the CPU, and a
	 * cached read of the timer's current count would answer with
	 * whatever it said the first time.
	 */
	if (!pmap_kenter(pa, pa,
	    VM_PROT_READ | VM_PROT_WRITE | PMAP_NOCACHE)) {
		kprintf("lapic: could not map registers at 0x%llx\n",
		    (unsigned long long)pa);
		return (false);
	}
	lapic_va = (volatile uint8_t *)(uintptr_t)pa;
	lapic_ok = true;

	ver = lapic_read(LAPIC_VERSION);

	/*
	 * Accept interrupts of every priority.  The task-priority register
	 * comes out of reset holding zero on the boot CPU, but an AP's does
	 * not have to, and a non-zero TPR silently drops everything below
	 * it -- a symptom that looks exactly like a missing interrupt
	 * source.
	 */
	lapic_write(LAPIC_TPR, 0);

	/*
	 * THE TWO LEGACY PINS, BEFORE THE ENABLE TAKES EFFECT ANYWHERE.
	 * LINT0 carries the 8259's output and LINT1 the NMI line, and both
	 * LVT entries are masked out of reset; software-enabling the APIC
	 * with them still masked disconnects the PIT, the keyboard and the
	 * disk in one instruction.  ExtINT means "the vector comes from the
	 * 8259, go and ask it", which is what makes the existing interrupt
	 * path keep working unchanged.
	 */
	lapic_write(LAPIC_LVT_LINT0, LVT_DELIVERY_EXTINT);
	lapic_write(LAPIC_LVT_LINT1, LVT_DELIVERY_NMI);

	/* Nothing to run yet: the timer stays masked until it is measured. */
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);

	/*
	 * Software-enable, and name the spurious vector.  A spurious
	 * interrupt is what the CPU takes when an interrupt it was about to
	 * accept is withdrawn between the arbitration and the acknowledge;
	 * it needs a gate to land on and DELIBERATELY has no handler, so the
	 * dispatcher's "nothing installed" path ignores it -- which is
	 * correct, because there is no in-service bit behind it and writing
	 * EOI would retire a real interrupt belonging to somebody else.
	 */
	lapic_write(LAPIC_SVR, LAPIC_VEC_SPURIOUS | LAPIC_SVR_ENABLE);

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
	uint64_t	t0;
	uint64_t	t1;
	uint64_t	cal_ms;
	uint64_t	fired;
	uint32_t	remaining;
	uint32_t	elapsed;
	uint32_t	want;
	unsigned int	hz;

	if (!lapic_ok)
		return;

	hz = pit_hz();
	if (hz == 0) {
		kprintf("lapic: timer unmeasured -- the PIT is not running\n");
		return;
	}

	/*
	 * Preemption off across both halves.  Not for correctness of the
	 * ratio -- both clocks run whoever is on the CPU, so being
	 * descheduled would not skew it -- but because the window is a
	 * tenth of a second, which is five of this kernel's quanta, and a
	 * measurement that reports what it measured is easier to trust than
	 * one that reports what it asked for.  Interrupts stay ON: the PIT
	 * ticks are the ruler.
	 */
	preempt_disable();

	/*
	 * Calibration.  Count down from the top with the timer MASKED, so
	 * nothing is delivered and nothing is disturbed, and read the
	 * counter and the PIT together at the end.  The PIT delta is used as
	 * measured rather than as requested, since the loop can only
	 * overshoot.
	 */
	lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFFu);

	t0 = pit_ticks();
	while (pit_ticks() - t0 < LAPIC_CAL_TICKS)
		__asm__ __volatile__ ("pause");
	remaining = lapic_read(LAPIC_TIMER_CCR);
	t1 = pit_ticks();

	lapic_write(LAPIC_TIMER_ICR, 0);	/* stop counting */

	elapsed = 0xFFFFFFFFu - remaining;
	if (t1 == t0) {
		preempt_enable();
		kprintf("lapic: timer unmeasured -- the PIT did not move\n");
		return;
	}
	lapic_hz = (uint32_t)(((uint64_t)elapsed * hz) / (t1 - t0));
	cal_ms   = (t1 - t0) * 1000 / hz;

	/*
	 * Delivery.  Ask for LAPIC_PROBE_HZ periodic and count what arrives
	 * over a window the PIT measures.  This is the half that cannot be
	 * argued: the mapping, the enable, the LVT, the vector, the IDT gate
	 * and the dispatcher all have to be right for a single one of these
	 * to be counted.
	 */
	__atomic_store_n(&lapic_timer_count, 0, __ATOMIC_RELAXED);
	intr_install_local(LAPIC_VEC_TIMER, lapic_timer_isr);

	lapic_write(LAPIC_TIMER_ICR, lapic_hz / LAPIC_PROBE_HZ);
	lapic_write(LAPIC_LVT_TIMER,
	    LAPIC_VEC_TIMER | LVT_DELIVERY_FIXED | LVT_TIMER_PERIODIC);

	t0 = pit_ticks();
	while (pit_ticks() - t0 < LAPIC_PROBE_TICKS)
		__asm__ __volatile__ ("pause");
	t1 = pit_ticks();

	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_TIMER_ICR, 0);

	preempt_enable();

	fired = __atomic_load_n(&lapic_timer_count, __ATOMIC_RELAXED);
	want  = (uint32_t)(((t1 - t0) * LAPIC_PROBE_HZ) / hz);

	kprintf("lapic: timer counts at %u kHz (input / %u), "
	    "measured against the PIT over %llu ms\n",
	    (unsigned int)(lapic_hz / 1000), (unsigned int)LAPIC_DIVISOR,
	    (unsigned long long)cal_ms);

	/*
	 * A tally rather than an assertion, and for the reason this kernel
	 * has learned twice: an absolute count over a window nothing
	 * controls is a gauge.  What WOULD be a defect is zero -- that says
	 * the chip is not delivering at all, which no amount of load
	 * explains -- so that alone is called out.
	 */
	kprintf("lapic: timer delivered %llu interrupt(s) at %u Hz, "
	    "want about %u%s\n",
	    (unsigned long long)fired, (unsigned int)LAPIC_PROBE_HZ, want,
	    fired == 0 ? "  *** NOTHING ARRIVED ***" : "");
}
