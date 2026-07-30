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
 * Interrupt-command fields.  DELIVERY_PENDING is the one that has to be
 * waited on: the ICR is a single register for the whole APIC, so writing a
 * second message while the first is still going out loses one of them.
 */
#define	ICR_DELIVERY_PENDING	(1u << 12)
#define	ICR_MODE_INIT		(5u << 8)
#define	ICR_MODE_STARTUP	(6u << 8)
#define	ICR_LEVEL_ASSERT	(1u << 14)

/*
 * How long to wait for a message to leave.  Generous by two orders of
 * magnitude -- delivery to a processor on the same die is measured in
 * nanoseconds -- because the point of the bound is to return with an answer
 * instead of hanging the boot if the message never goes out at all.
 */
#define	LAPIC_ICR_WAIT_US	100

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
 * interrupts are counted.  In microseconds of TSC time.
 *
 * ⚠ THE PIT WAS THE RULER HERE AND IT WAS THE WRONG ONE, which took a
 * measurement to notice.  Waiting for ten PIT INTERRUPTS is not the same as
 * waiting a tenth of a second: this host delivers them in bursts, so ten of
 * them can arrive in ninety milliseconds of real time, and the counting rate
 * derived from that window comes out ten percent low.  Two boots in four
 * calibrated 56.4 MHz where the other two read 61.8 and 62.3, and every one of
 * those numbers went straight into the timer's reload count -- a slice of
 * 18.1 ms wearing a constant that says 20.
 *
 * The TSC is READ, not delivered.  Nothing can compress a window measured by a
 * counter the CPU increments itself, and its own rate is anchored well enough
 * to check by eye: it prints ~3.58 GHz on a part sold as 3.6.  The PIT is
 * still counted alongside, because the difference between the two is exactly
 * the PIT's delivery deficit and that is worth being able to see.
 */
#define	LAPIC_CAL_US		100000	/* 100 ms                          */
#define	LAPIC_PROBE_US		200000	/* 200 ms                          */
#define	LAPIC_PROBE_HZ		100	/* rate the timer is asked to keep  */

static volatile uint8_t	*lapic_va;		/* (c) NULL until mapped   */
static uint64_t		 lapic_pa;		/* (c) 0 until mapped      */
static bool		 lapic_ok;		/* (c)                     */
static uint32_t		 lapic_hz;		/* (c) ticks/s at DIV16    */
/*
 * Ticks this timer has delivered.  One counter for the machine, which is
 * right while there is one CPU delivering into it and WRONG the moment there
 * are two: lapic_timer_report divides it by elapsed time to get one CPU's tick
 * rate, and N processors ticking would read as N times too fast.  It belongs
 * in struct cpu with the rest of what a CPU owns, and moves there in the rung
 * that starts a second one.
 */
static volatile uint64_t lapic_timer_count;	/* (a) ISR tally           */

/*
 * Set once, by lapic_timer_start, while the timer is still masked -- so the
 * ISR that reads it cannot be running yet and no ordering is needed.  False
 * during lapic_timer_probe deliberately: the probe delivers twenty
 * interrupts and none of them should spend anybody's slice.
 */
static bool		 lapic_preempting;	/* (c) after timer_start   */
static unsigned int	 lapic_tick_hz;		/* (c) rate it now keeps   */
static uint64_t		 lapic_start_pit;	/* (c) PIT at the handover */
static uint64_t		 lapic_start_tsc;	/* (c) TSC at the handover */

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
 * Wait for the last interrupt command to leave, with a bound.  Returns false
 * if it never did, which the caller reports rather than retries: a message
 * still pending after a hundred microseconds is not a message that is about
 * to be sent.
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
 * Send one interrupt command to one processor, named by its APIC id.  The
 * destination goes in the high half FIRST -- writing the low half is what
 * sends the message, so the order is not a preference.
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
	 * Assert only.  The de-assert half of the sequence is for the discrete
	 * 82489DX, which had no way to tell an INIT from the level it arrived
	 * at; every integrated APIC since treats INIT as edge-triggered and
	 * ignores the de-assert, and the machines this runs on have never seen
	 * that chip.
	 */
	return (lapic_ipi_send(apic_id, ICR_MODE_INIT | ICR_LEVEL_ASSERT));
}

bool
lapic_ipi_startup(uint32_t apic_id, uint64_t tramp_pa)
{
	uint32_t	vec;

	/*
	 * The startup message carries a PAGE NUMBER in its low byte, which is
	 * the whole reason the trampoline lives in the first megabyte: there is
	 * no room in this field for an address above 0xFF000.  A caller that
	 * hands over something else gets a refusal rather than a processor
	 * started at a rounded-down address.
	 */
	if ((tramp_pa & 0xFFF) != 0 || (tramp_pa >> 12) > 0xFF)
		return (false);
	vec = (uint32_t)(tramp_pa >> 12);

	return (lapic_ipi_send(apic_id,
	    ICR_MODE_STARTUP | ICR_LEVEL_ASSERT | vec));
}

static void
lapic_timer_isr(struct trapframe *tf)
{

	(void)tf;
	__atomic_add_fetch(&lapic_timer_count, 1, __ATOMIC_RELAXED);

	/*
	 * The slice, from the moment this timer owns it.  Charged to whatever
	 * is running on THIS CPU, which is the CPU that took the interrupt,
	 * which is the CPU whose slice is being spent -- and that is the whole
	 * reason the debit moved here from the PIT, which could only ever
	 * charge one of them.
	 *
	 * Not gated on preempt_is_enabled: the gate belongs at the schedule
	 * point and not at the flag-set point, so that a critical section
	 * which ends later still owes the reschedule it earned here.
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
	lapic_pa = pa;
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
	 *
	 * Written unconditionally rather than after checking what the
	 * firmware left, because the state a previous owner of this register
	 * left it in is exactly the thing that is not worth depending on.
	 * (There IS a firmware in the path, whatever an earlier version of
	 * this comment claimed: the ACPI tables the MADT probe reads are put
	 * in low memory by it, and they say 'BOCHS'.)
	 */
	/*
	 * And only on the BOOT processor.  There is one 8259 and its output is
	 * wired to one CPU's LINT0; an application processor that also claimed
	 * ExtINT would be offering to answer legacy interrupts that were never
	 * routed to it, and the day interrupts are enabled over there the same
	 * IRQ0 could be taken by either -- which is not a race this kernel
	 * needs to have an opinion about yet.  The NMI pin is per-processor and
	 * every CPU wants it.
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
	 * Software-enable, and name the spurious vector.  A spurious
	 * interrupt is what the CPU takes when an interrupt it was about to
	 * accept is withdrawn between the arbitration and the acknowledge;
	 * it needs a gate to land on and DELIBERATELY has no handler, so the
	 * dispatcher's "nothing installed" path ignores it -- which is
	 * correct, because there is no in-service bit behind it and writing
	 * EOI would retire a real interrupt belonging to somebody else.
	 */
	lapic_write(LAPIC_SVR, LAPIC_VEC_SPURIOUS | LAPIC_SVR_ENABLE);

	/*
	 * The id, now from the register, over the one CPUID gave cpu_bsp_init
	 * before any of this existed.  They are the same field read two ways
	 * and a machine where they differ has renumbered its APICs behind us,
	 * so the disagreement is worth a line of its own rather than a silent
	 * overwrite.
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
	 * Preemption off across both halves.  Not for correctness of the
	 * ratio -- every clock in sight runs whoever is on the CPU, so being
	 * descheduled would not skew it -- but because the window is a
	 * tenth of a second, which is five of this kernel's quanta, and a
	 * measurement that reports what it measured is easier to trust than
	 * one that reports what it asked for.  Interrupts stay ON: the second
	 * half is about interrupts arriving.
	 */
	preempt_disable();

	/*
	 * Calibration.  Count down from the top with the timer MASKED, so
	 * nothing is delivered and nothing is disturbed, and read the counter
	 * against elapsed TSC time.  The elapsed time is used as MEASURED
	 * rather than as requested, since the loop can only overshoot.
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
	 * Delivery.  Ask for LAPIC_PROBE_HZ periodic and count what arrives.
	 * This is the half that cannot be argued: the mapping, the enable, the
	 * LVT, the vector, the IDT gate and the dispatcher all have to be
	 * right for a single one of these to be counted.
	 */
	__atomic_store_n(&lapic_timer_count, 0, __ATOMIC_RELAXED);
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

	fired = __atomic_load_n(&lapic_timer_count, __ATOMIC_RELAXED);
	want  = (uint32_t)((probe_us * LAPIC_PROBE_HZ) / 1000000);

	kprintf("lapic: timer counts at %u kHz (input / %u), "
	    "measured against the TSC over %llu us\n",
	    (unsigned int)(lapic_hz / 1000), (unsigned int)LAPIC_DIVISOR,
	    (unsigned long long)cal_us);

	/*
	 * A tally rather than an assertion, and for the reason this kernel
	 * has learned twice: an absolute count over a window nothing
	 * controls is a gauge.  What WOULD be a defect is zero -- that says
	 * the chip is not delivering at all, which no amount of load
	 * explains -- so that alone is called out.
	 */
	kprintf("lapic: timer delivered %llu interrupt(s) at %u Hz, "
	    "want about %u (the PIT delivered %llu)%s\n",
	    (unsigned long long)fired, (unsigned int)LAPIC_PROBE_HZ, want,
	    (unsigned long long)pit0,
	    fired == 0 ? "  *** NOTHING ARRIVED ***" : "");
}

bool
lapic_timer_start(void)
{
	unsigned int	hz;
	uint32_t	icr;

	if (!lapic_ok || lapic_hz == 0)
		return (false);

	/*
	 * THE RATE IS THE PIT'S, and not a number of this file's choosing.
	 * The slice is counted in TICKS -- PREEMPT_QUANTUM_TICKS of them --
	 * and that count was measured against a 100 Hz tick, on a curve with
	 * a knee at two ticks.  Keeping the old rate is what makes the old
	 * measurement still describe the new timer; changing the rate here
	 * would quietly change the quantum without touching the constant that
	 * is supposed to express it.
	 */
	hz = pit_hz();
	if (hz == 0)
		return (false);

	icr = lapic_hz / hz;
	if (icr == 0) {
		kprintf("lapic: %u Hz is faster than this timer can count "
		    "(%u ticks/s) -- preemption stays with the PIT\n",
		    hz, (unsigned int)lapic_hz);
		return (false);
	}

	/*
	 * The hand-over, and it is one-way.  EXACTLY ONE timer may debit the
	 * slice: two would spend it twice as fast, and the symptom of that is
	 * not an error message but a quantum silently halved -- the kind of
	 * change this kernel spent a whole rung measuring.  So the PIT is
	 * relieved BEFORE this timer is unmasked.  The gap costs at most one
	 * tick of one thread's slice; the overlap would cost the number the
	 * scheduler is tuned around.
	 *
	 * The PIT keeps ticking, and must: it is the machine's clock, the
	 * ruler this timer was calibrated against, the thing timeouts and the
	 * busy-sleeps are counted in.  It stops debiting, not ticking.
	 */
	pit_release_preempt();

	lapic_tick_hz = hz;
	lapic_preempting = true;
	__atomic_store_n(&lapic_timer_count, 0, __ATOMIC_RELAXED);
	lapic_start_pit = pit_ticks();
	lapic_start_tsc = tsc_read();

	intr_install_local(LAPIC_VEC_TIMER, lapic_timer_isr);

	/*
	 * Divisor, then the LVT, then the count -- in that order, because
	 * writing the initial count is what starts the timer, and everything
	 * it will be delivered as has to be true before it can fire.
	 */
	lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);
	lapic_write(LAPIC_LVT_TIMER,
	    LAPIC_VEC_TIMER | LVT_DELIVERY_FIXED | LVT_TIMER_PERIODIC);
	lapic_write(LAPIC_TIMER_ICR, icr);

	kprintf("lapic: timer periodic at %u Hz (count %u per tick), "
	    "this CPU debits its own slice now -- %u tick(s), %u ms\n",
	    hz, (unsigned int)icr, (unsigned int)PREEMPT_QUANTUM_TICKS,
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
	uint64_t	ticks;
	uint64_t	pit;
	uint64_t	us;
	uint64_t	rate;
	uint64_t	slice_us;

	if (!lapic_preempting)
		return;

	ticks = __atomic_load_n(&lapic_timer_count, __ATOMIC_RELAXED);
	pit   = pit_ticks() - lapic_start_pit;
	us    = tsc_to_us(tsc_read() - lapic_start_tsc);
	if (us == 0)
		return;

	/*
	 * The oracle that runs for the whole session rather than a window,
	 * and the one that has to be right: the SLICE, in microseconds, taken
	 * from the rate this timer has actually delivered over however long
	 * the machine has been up.  PREEMPT_QUANTUM_TICKS is a count of ticks
	 * and only means 20 ms while the ticks arrive at the rate they were
	 * asked for, so this is the number that says the hand-over kept the
	 * quantum it promised to keep.
	 *
	 * Against the TSC, and not against the PIT, for the reason written up
	 * at LAPIC_CAL_US: the PIT is delivered and can fall behind, so a
	 * disagreement between the two would leave us unable to say which of
	 * them was wrong.  The PIT's own count is printed beside it because
	 * the gap IS that deficit, and seeing it is how the wrong ruler was
	 * found in the first place.
	 */
	rate     = ticks * 1000000 / us;
	slice_us = ticks == 0 ? 0 :
	    (uint64_t)PREEMPT_QUANTUM_TICKS * us / ticks;

	kprintf("lapic: timer ticked %llu in %llu ms -- %llu Hz of %u asked, "
	    "slice %llu us (the PIT ticked %llu)%s\n",
	    (unsigned long long)ticks, (unsigned long long)(us / 1000),
	    (unsigned long long)rate, lapic_tick_hz,
	    (unsigned long long)slice_us, (unsigned long long)pit,
	    ticks == 0 ? "  *** THE SLICE IS UNCHARGED ***" : "");
}
