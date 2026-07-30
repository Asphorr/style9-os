/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "cpu.h"
#include "kprintf.h"
#include "msr.h"
#include "panic.h"
#include "sched.h"
#include "thread.h"

/*
 * The blocks themselves.
 *
 * Block 0 is initialised HERE rather than by cpu_bsp_init, so that the
 * boot CPU's block is already coherent in the image: cp_self must be right
 * before the first read through the segment base, and the first read
 * happens in the same breath as the write that makes it possible.  An
 * application processor gets its block filled by the code that starts it,
 * which knows the id it is handing out.
 */
struct cpu	cpus[MAXCPU] = {
	[0] = { .cp_self = &cpus[0], .cp_id = 0 },
};

static unsigned int	ncpu = 1;	/* (c) brought up, not present  */

void
cpu_bsp_init(void)
{

	/*
	 * The whole per-CPU mechanism is this one register: write a block's
	 * address into the GS base and `%gs:0' is that block, with nothing
	 * to index and nothing to look up.
	 */
	wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)&cpus[0]);
}

void
cpu_print(void)
{
	struct cpu	*seen;

	/*
	 * The round trip is the test.  `seen' came out of the segment base
	 * the hardware will use for every per-CPU reference from here on;
	 * &cpus[0] is where the C side believes the block is.  If those
	 * differ, everything works until the first per-CPU write, which
	 * then lands somewhere nobody is looking -- so the check earns its
	 * panic, and the panic names which of the two it distrusted.
	 */
	seen = curcpu();
	if (seen != &cpus[0])
		panic("cpu: gs base reads %p, block 0 is at %p",
		    (void *)seen, (void *)&cpus[0]);
	if (seen->cp_id != 0)
		panic("cpu: block 0 claims id %u", (unsigned int)seen->cp_id);

	kprintf("cpu: %u cpu, boot cpu id=%u, per-cpu block at %p "
	    "via gs base\n", ncpu, cpu_id(), (void *)seen);
}

unsigned int
cpu_count(void)
{

	return (ncpu);
}

/*
 * One line per CPU that has been brought up.
 *
 * Reads the blocks by address rather than through the segment base, which
 * is the only way to see a CPU that is not this one -- and the reason the
 * fields are worth a command at all: with one CPU this says what the
 * scheduler already says, and with two it is the difference between "the
 * kernel is busy" and "that one is idle while this one queues".
 *
 * Deliberately unlocked.  Everything here is a single word being read while
 * its owner may be writing it, so a line can be a moment out of date; a
 * lock would buy consistency between fields nobody compares and cost the
 * ability to run this from a wedged CPU, which is when it is wanted most.
 */
void
cpu_dump(void)
{
	struct cpu	*cp;
	struct thread	*th;
	unsigned int	i;

	for (i = 0; i < ncpu; i++) {
		cp = &cpus[i];
		th = cp->cp_curthread;
		kprintf("cpu %u: lapic %u, running %s, idle id=%llu, "
		    "preempt %d%s, quantum %u/%u, kstack 0x%llx\n",
		    (unsigned int)cp->cp_id, (unsigned int)cp->cp_lapic_id,
		    th != NULL && th->th_name != NULL ? th->th_name : "-",
		    cp->cp_idle_thread != NULL ?
		    (unsigned long long)cp->cp_idle_thread->th_id : 0ULL,
		    cp->cp_preempt_count,
		    cp->cp_need_resched != 0 ? " (resched owed)" : "",
		    cp->cp_quantum_used, (unsigned int)PREEMPT_QUANTUM_TICKS,
		    (unsigned long long)cp->cp_kernel_rsp);
	}
}
