/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "ata_drv.h"
#include "apfs.h"
#include "bio.h"
#include "fat.h"
#include "fs.h"
#include "fs_txn.h"
#include "bootstrap.h"
#include "clock.h"
#include "darwin.h"
#include "host.h"
#include "klog.h"
#include "launchd.h"
#include "progreg.h"
#include "services.h"
#include "vm.h"
#include "acpi.h"
#include "cpu.h"
#include "fpu.h"
#include "gdt.h"
#include "idt.h"
#include "intr.h"
#include "kbd.h"
#include "lapic.h"
#include "kbd_drv.h"
#include "kmem.h"
#include "kprintf.h"
#include "memmap.h"
#include "mp.h"
#include "mutex.h"
#include "mouse.h"
#include "mouse_drv.h"
#include "panic.h"
#include "pic.h"
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "sched.h"
#include "shell.h"
#include "smap.h"
#include "stress.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "tty.h"
#include "uart.h"
#include "uart_drv.h"
#include "usermode.h"

#define	MULTIBOOT2_BOOTLOADER_MAGIC	0x36D76289U
#define	MULTIBOOT1_BOOTLOADER_MAGIC	0x2BADB002U

extern char	__kernel_start[];
extern char	__kernel_end[];

void	kmain(uint32_t, uint32_t);

static void	kmain_banner(uint32_t, uint32_t);
static void	kmain_memory(uint32_t, uint32_t);
static void	kmain_memory_smoke(void);
static void	kmain_run_tests(void);

/*
 * Top-of-kernel entry, called from boot.S with a stack set up and the
 * bootloader's magic and info pointer as arguments.
 *
 * Init order is deliberate: per-CPU base, then console so later stages
 * can log, then CPU tables (GDT before IDT so traps land in our handlers
 * using our selectors), the 8259 PIC (remapped before sti), memory, the
 * local APIC and MADT, the keyboard, and only then interrupts and the
 * clock.
 */
void
kmain(uint32_t mb_magic, uint32_t mb_info)
{

	/*
	 * First of all.  Per-CPU state is reached through the GS base, which
	 * is zero until this runs, so any earlier access would hit physical
	 * page zero -- and spin_lock is one, via the preempt count.  There is
	 * no console yet; cpu_print reports the result once there is.
	 */
	cpu_bsp_init();

	uart_init();
	tty_init();
	/*
	 * Before the banner: the tests scribble on the screen and clear it,
	 * and the first thing on screen should be the boot log.
	 */
	tty_selftest();
	tty_wrap_selftest();
	tty_region_selftest();
	tty_colour_selftest();
	kmain_banner(mb_magic, mb_info);

	tty_puts("\nbringing CPU tables online...\n");

	cpu_print();
	tty_puts("  [ok] per-cpu block (gs base)\n");

	gdt_init_cpu();
	tty_puts("  [ok] gdt + tss (this cpu's own)\n");

	idt_init();
	tty_puts("  [ok] idt\n");

	pic_init();
	tty_puts("  [ok] pic (8259 remapped to 0x20/0x28)\n");

	kmain_memory(mb_magic, mb_info);

	/*
	 * After the memory system, because the APIC's registers must be
	 * mapped, and with interrupts still off, because enabling the APIC
	 * routes the 8259 through LINT0, and until that pin is programmed the
	 * legacy controller reaches nobody.
	 */
	if (lapic_init())
		tty_puts("  [ok] local apic (legacy pins wired through)\n");
	else
		tty_puts("  [--] local apic absent -- 8259 alone\n");

	/*
	 * After the APIC, so the boot CPU knows its own APIC id and can find
	 * itself in the table, and the table's APIC address can be checked
	 * against the MSR's.
	 */
	if (acpi_madt_probe())
		tty_puts("  [ok] acpi madt (processors counted)\n");
	else
		tty_puts("  [--] no acpi madt -- one processor\n");

	kbd_init();
	tty_puts("  [ok] kbd (IRQ1 unmasked)\n");

	intr_enable();
	tty_puts("  [ok] interrupts enabled\n");

	clock_init();
	tty_puts("  [ok] clock\n");

	/*
	 * Needs the PIT ticking to measure against and interrupts on to catch
	 * its own, so it cannot happen beside lapic_init.
	 */
	lapic_timer_probe();

	/*
	 * Hand the slice from the PIT to this CPU's APIC timer, at the PIT's
	 * rate so the quantum keeps its length.  If this declines (no APIC,
	 * nothing measured) the PIT keeps the job.
	 */
	if (lapic_timer_start())
		tty_puts("  [ok] apic timer debits the slice\n");
	else
		tty_puts("  [--] preemption stays on the PIT\n");

	/*
	 * The other processors, last of the machine bring-up: starting one
	 * needs the APIC (INIT and startup IPIs), the page allocator (a stack
	 * each) and the TSC (the sequence has real microsecond delays).  They
	 * arrive and park until mp_release_aps below -- see ap_entry.
	 */
	if (mp_start_aps() != 0)
		tty_puts("  [ok] application processors up (parked)\n");

	/*
	 * Right after the APs arrive: every mapping change from now on relies
	 * on TLB shootdown reaching them, so a failure should show here, not
	 * as corruption later.
	 */
	pmap_tlb_selftest();

	kmain_memory_smoke();
	kmain_run_tests();

	kbd_drv_init();
	/*
	 * The keyboard's second consumer.  It declines every key until a
	 * Darwin task claims the console; registered at boot so the driver
	 * thread never has to ask whether it exists.
	 */
	kbd_drv_set_sink(darwin_cons_sink);
	/* The mouse has no early consumer, so it comes up with the drivers. */
	mouse_init();
	mouse_drv_init();
	uart_drv_init();
	ata_drv_init();
	bio_init();		/* before any filesystem reads a block */
	fs_fat_init();
	fs_apfs_init();
	/*
	 * Finish anything an interrupted boot left unnamed, now, while nothing
	 * holds a file.  Zero on every clean boot.
	 */
	(void)fs_reap_orphans();
	/*
	 * Mounting is the block cache's worst honest workload (the same
	 * metadata blocks, over and over); report what it cost.
	 */
	bio_stats();
	ata_irq_stats();

	/*
	 * The filesystem self-tests.  The order is load-bearing where noted.
	 *
	 * The write test goes after the mount numbers so its I/O does not muddy
	 * them (fs/fs.h says what it claims).  The allocation test takes free
	 * blocks, checks the disk agrees and gives them back (fs/apfs/apfs.h).
	 * The checkpoint test moves the container to a new transaction id, so
	 * it follows those two.
	 */
	fs_write_selftest();
	if (fs_apfs_ready())
		fs_apfs_alloc_selftest();
	fs_ckpt_selftest();

	/* A write whose checkpoint describes the bytes it actually wrote. */
	fs_data_selftest();

	/*
	 * Truncate before grow: the file truncate finds is the one the last
	 * boot grew (so its tail proves growth survived), and the length it
	 * leaves gives the grow test work on every boot.
	 */
	fs_trunc_selftest();
	fs_grow_selftest();

	/*
	 * A file, then a directory with a name inside it, made from nothing and
	 * left for the next boot to remove -- so the volume ends every boot
	 * after the first as it began.
	 */
	fs_make_selftest();
	fs_dirs_selftest();

	/*
	 * Checks a file a real Apple shell redirected into during the previous
	 * boot.  Must run before ring 3 starts, or it would see this boot's
	 * write instead of what survived power-off.
	 */
	fs_shell_selftest();

	/*
	 * B-tree operations no ordinary path reaches any more: a split, a node
	 * whose first key changes, a node emptied out of the tree.  The last
	 * two are arranged rather than waited for, since where a delete lands
	 * depends on where the splits fell.
	 */
	fs_split_selftest();
	fs_index_selftest();
	fs_drop_selftest();

	/* A file whose two records a split put either side of a boundary. */
	fs_stream_selftest();

	/*
	 * The extent-reference tree (the one that counts the volume's runs)
	 * outgrowing its single-node root.  Early on purpose: from then on it
	 * has an index level for good, so every later test, this boot and
	 * next, exercises the two-level walk.
	 */
	fs_extref_selftest();

	/*
	 * The same shapes reached by an ordinary caller: names go into a
	 * directory until the create that finds a leaf full has to split it.
	 * After the tests above, so it fills the deep tree they leave.
	 */
	fs_room_selftest();

	/*
	 * A rename: the one writer whose success changes no count on the
	 * volume.  After the fill, so it moves records between fuller leaves.
	 */
	fs_move_selftest();

	/*
	 * A file that outlives its name.  After the move test: the same
	 * machinery, pointed at the private directory.
	 */
	fs_orphan_selftest();

	/*
	 * A rename onto a taken name -- move and orphaning as one edit: the
	 * name answers with the newcomer while the replaced file keeps
	 * answering whoever holds it.  After both, since it composes them.
	 */
	fs_clobber_selftest();

	/*
	 * Past checkpoints read back through /.xid as written, refuse writes,
	 * and age out on the free queue's schedule.  After every writer,
	 * since it reads what they published.
	 */
	fs_view_selftest();

	/*
	 * Every file the tests opened was given back.  After the last test
	 * that opens files and before ring 3 opens any.
	 */
	fs_open_check();

	/*
	 * A lookup by key answers what a full walk does.  Last, against the
	 * three-level tree the split, index and drop tests leave behind: on a
	 * pristine volume the descent would never have to choose.
	 */
	fs_seek_selftest();

	/*
	 * Mutations batch, and the checkpoint is owed until something collects
	 * it (policy in fs/fs.c).  This sync is the boot's collection point:
	 * every cross-boot claim the tests make is durable from here, not from
	 * the call that made it.  Then the syncer publishes on a clock.
	 */
	if (fs_ready() && fs_sync() < 0)
		kprintf("kmain: the closing sync failed -- what the "
		    "self-tests wrote is not yet on the platter\n");
	fs_syncer_start();

	/*
	 * A demo service for ring 3 to look up: kernel_task's task_self port
	 * (MACH_PORT_TASK_SELF in kernel_space), published as "kernel_task".
	 * A lookup gets a fresh SEND right in the caller's space, served by
	 * the synchronous task_self dispatcher.
	 */
	if (bootstrap_register("kernel_task",
	    MACH_PORT_TASK_SELF) != MACH_MSG_OK)
		panic("kmain: bootstrap_register(kernel_task)");

	/*
	 * The kernel-side Mach services (clock, stats, tasks, echool, man,
	 * progreg) and launchd's subsystem.  Each is a PORT_SPECIAL_SERVICE
	 * port with a synchronous dispatcher, registered by name with the
	 * bootstrap port so any task finds it via bootstrap_lookup.
	 */
	services_init();

	/*
	 * The host port (machine identity, page size), on the same registry:
	 * bootstrap_lookup("host") for native tasks, the mach_host_self() trap
	 * for Darwin binaries.  After services_init, whose kernel_space
	 * install path it shares.
	 */
	host_init();

	/*
	 * Publish the bootstrap port's own kernel_space SEND for
	 * task_get_special_port(TASK_SPECIAL_BOOTSTRAP).  Not in
	 * bootstrap_init: task_subsystem_init must first claim kernel_space's
	 * well-known low names (TASK_SELF=1, BOOTSTRAP=2).
	 */
	bootstrap_publish();

	/*
	 * The structured kernel log.  It mirrors to tty, which already copies
	 * to COM1 and debugcon, so every klog line reaches all three.
	 */
	klog_service_init();

	/* Boot markers, so `log tail' has something to show. */
	klog(KLOG_LEVEL_INFO,  "boot", "stress pass complete");
	klog(KLOG_LEVEL_INFO,  "boot", "drivers + services up");
	klog(KLOG_LEVEL_DEBUG, "boot", "entering shell");

	syscall_init();
	smap_init();
	(void)smap_enable_runtime();

	/*
	 * Now release the parked processors: CR4.SMAP and the SYSCALL
	 * registers, set just above, are the last per-CPU state a thread
	 * depends on, and a CPU released earlier would run user threads
	 * without them.
	 */
	if (mp_release_aps() != 0)
		sched_smp_selftest();

	/*
	 * With the APs running, where this list gets corrupted -- though the
	 * test arranges the corruption rather than racing for it.
	 */
	port_wait_selftest();

	/*
	 * The kill-vs-lock scenes: both spawn a task and kill it, one in the
	 * middle of disk I/O, so they need the volume mounted and the write
	 * path proven by the fs tests above.
	 */
	mutex_kill_selftest();
	fs_kill_selftest();

	progreg_init();

	/*
	 * launchd's catalog names programs by string, and progreg_init has
	 * just filled the registry that resolves them.  See
	 * launchd_load_catalog in mach/launchd.c.
	 */
	launchd_load_catalog();

	/*
	 * Run hello.elf once before sh.elf gets the console: it exercises the
	 * userspace surface end to end (port self-send, task_self RPC,
	 * bootstrap_lookup, OOL round trip via svc/echool) and exits 0 on
	 * success -- a ring-3 smoke test for a headless boot.
	 */
	{
		long	hello_id;

		hello_id = progreg_spawn("hello");
		if (hello_id > 0) {
			/*
			 * Nap, do not yield: with APs running, hello.elf runs
			 * beside this thread, not behind it, and a yield loop
			 * spins (see sched_nap_ms).
			 */
			while (task_is_alive((uint64_t)hello_id))
				sched_nap_ms(1);
			sched_reap_zombies();
		} else {
			kprintf("kmain: spawn(hello) failed rv=%ld\n",
			    hello_id);
		}
		/*
		 * Page and frame counts after the demos, not at mount time:
		 * the loader populates its mappings eagerly, so only ring-3
		 * programs fault, and the frame-sharing counters stay zero
		 * until something forks.
		 */
		vm_fault_stats();
		pmm_stats();
		vm_image_stats();
		vm_pages_stats();
		vm_map_stats();
		darwin_cons_stats();
		tty_stats();
		if (fs_apfs_ready())
			fs_apfs_stats();
		fs_txn_stats();
		fs_handle_stats();
		mutex_stats();
		bio_stats();
		ata_irq_stats();
		kmem_stats();
	}

	/*
	 * The ring-3 shell takes over as the user-facing surface.
	 *
	 * sh.elf's dev_open_stream("kbd") moves the single RECV right on
	 * kbd_input_port out of kernel_space, so shell_run() must not be
	 * called alongside it: it would panic (port_set_insert on a port we
	 * no longer own) or race sh.elf for characters.  kern/shell.c stays
	 * only as the fallback when sh.elf fails to spawn.
	 */
	if (progreg_spawn("sh") < 0) {
		kprintf("kmain: spawn(sh) failed -- falling back to kernel shell\n");
		shell_run();
		/* NOTREACHED */
	}

	/*
	 * The boot thread's job is done: exit, rather than linger on the
	 * runqueue, and be reaped like any zombie.  kernel_task is unaffected
	 * -- its driver, service and idle threads still hold it.
	 */
	thread_exit();
	/* NOTREACHED */
}

/*
 * Boot-time test pass: every stress harness in turn, so a headless
 * `make log' boot exercises the whole stack.  A failure does not skip the
 * tests after it -- one boot gives the full picture.
 */
static void
kmain_run_tests(void)
{
	int	rv_mem, rv_boundary, rv_timer;

	tty_set_attr(TTY_ATTR(TTY_YELLOW, TTY_BLACK));
	tty_puts("\n--- boot-time stress pass ---\n");
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));

	tty_puts("\n[1/15] stress mem 10000\n");
	rv_mem = stress_mem(10000);

	tty_puts("\n[2/15] stress mem boundary\n");
	rv_boundary = stress_mem_boundary();

	tty_puts("\n[3/15] stress timer 2s\n");
	rv_timer = stress_timer(2);

	tty_puts("\n[4/15] stress port 1000\n");
	int rv_port = stress_port(1000);

	tty_puts("\n[5/15] stress thread 200\n");
	int rv_thread = stress_thread(200);

	tty_puts("\n[6/15] stress preempt 4 workers, 1 s\n");
	int rv_preempt = stress_preempt(4, 1000);

	tty_puts("\n[7/15] stress sendonce 500\n");
	int rv_sendonce = stress_sendonce(500);

	tty_puts("\n[8/15] stress portset 4 members x 100\n");
	int rv_portset = stress_portset(4, 100);

	tty_puts("\n[9/15] stress intertask 200 (parent <-> worker task)\n");
	int rv_intertask = stress_intertask(200);

	tty_puts("\n[10/15] stress moverecv 200\n");
	int rv_moverecv = stress_moverecv(200);

	tty_puts("\n[11/15] stress nosenders 100\n");
	int rv_nosenders = stress_nosenders(100);

	tty_puts("\n[12/15] stress sendblock 2000\n");
	int rv_sendblock = stress_sendblock(2000);

	tty_puts("\n[13/15] stress rpc 200\n");
	int rv_rpc = stress_rpc(200);

	tty_puts("\n[14/15] stress ool 4 (parent <-> worker OOL transfer)\n");
	int rv_ool = stress_ool(4);

	tty_puts("\n[15/15] stress mutex 4 workers x 200 (sleeping lock)\n");
	int rv_mutex = stress_mutex(4, 200);

	tty_set_attr(TTY_ATTR(TTY_YELLOW, TTY_BLACK));
	tty_puts("\n--- stress pass summary ---\n");
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));
	kprintf("  stress mem 10000     : %s (rv=%d)\n",
	    rv_mem == 0 ? "PASS" : "FAIL", rv_mem);
	kprintf("  stress mem boundary  : %s (rv=%d)\n",
	    rv_boundary == 0 ? "PASS" : "FAIL", rv_boundary);
	kprintf("  stress timer 2s      : %s (rv=%d)\n",
	    rv_timer == 0 ? "PASS" : "FAIL", rv_timer);
	kprintf("  stress port 1000     : %s (rv=%d)\n",
	    rv_port == 0 ? "PASS" : "FAIL", rv_port);
	kprintf("  stress thread 200    : %s (rv=%d)\n",
	    rv_thread == 0 ? "PASS" : "FAIL", rv_thread);
	kprintf("  stress preempt 4/1s  : %s (rv=%d)\n",
	    rv_preempt == 0 ? "PASS" : "FAIL", rv_preempt);
	kprintf("  stress sendonce 500  : %s (rv=%d)\n",
	    rv_sendonce == 0 ? "PASS" : "FAIL", rv_sendonce);
	kprintf("  stress portset 4x100 : %s (rv=%d)\n",
	    rv_portset == 0 ? "PASS" : "FAIL", rv_portset);
	kprintf("  stress intertask 200 : %s (rv=%d)\n",
	    rv_intertask == 0 ? "PASS" : "FAIL", rv_intertask);
	kprintf("  stress moverecv 200  : %s (rv=%d)\n",
	    rv_moverecv == 0 ? "PASS" : "FAIL", rv_moverecv);
	kprintf("  stress nosenders 100 : %s (rv=%d)\n",
	    rv_nosenders == 0 ? "PASS" : "FAIL", rv_nosenders);
	kprintf("  stress sendblock 2000: %s (rv=%d)\n",
	    rv_sendblock == 0 ? "PASS" : "FAIL", rv_sendblock);
	kprintf("  stress rpc 200       : %s (rv=%d)\n",
	    rv_rpc == 0 ? "PASS" : "FAIL", rv_rpc);
	kprintf("  stress ool 4         : %s (rv=%d)\n",
	    rv_ool == 0 ? "PASS" : "FAIL", rv_ool);
	kprintf("  stress mutex 4x200   : %s (rv=%d)\n",
	    rv_mutex == 0 ? "PASS" : "FAIL", rv_mutex);

	int rv_so_notify = stress_sendonce_notify();
	kprintf("  sendonce-notify      : %s (rv=%d)\n",
	    rv_so_notify == 0 ? "PASS" : "FAIL", rv_so_notify);

	int rv_deadname = stress_deadname_multi();
	kprintf("  deadname-multi       : %s (rv=%d)\n",
	    rv_deadname == 0 ? "PASS" : "FAIL", rv_deadname);
}

/*
 * Bring up the memory subsystem in the same order BSD machdep does:
 *
 *	memmap	parse the firmware-supplied map (mb1 / mb2 / PVH);
 *		nothing allocates here.  Failure is fatal.
 *
 *	pmm	build the page-frame bitmap on top of the memmap, reserve
 *		low 1 MiB, the kernel image, and the bitmap's own pages.
 *		After this, pmm_alloc_page works.
 *
 *	pmap	record the live CR3 (the boot identity map) as
 *		kernel_pmap's root; no mappings are touched.
 *
 *	kmem	initialise empty buckets; first kmalloc() will pull a
 *		page from pmm on demand.
 *
 * then the VM, IPC, task, thread and scheduler layers that sit on them.
 */
static void
kmain_memory(uint32_t mb_magic, uint32_t mb_info)
{

	tty_puts("\nbringing memory subsystem online...\n");

	memmap_init(mb_magic, mb_info);
	memmap_print();

	pmm_init();
	pmap_bootstrap();
	kmem_init();
	vm_init();
	port_subsystem_init();
	bootstrap_init();
	task_subsystem_init();
	/*
	 * Enable SSE/x87 and capture the clean FXSAVE template before any
	 * thread exists: the boot thread and every thread_create seed th_fpu
	 * from it.
	 */
	fpu_init();
	thread_subsystem_init();
	sched_init();

	tty_puts("  [ok] memory subsystem ready\n");
}

/*
 * Touch every layer with a representative workload, so a regression in
 * pmm / pmap / kmem shows up in the boot log as a panic or a wrong stat.
 */
static void
kmain_memory_smoke(void)
{
	void		*small, *medium, *large, *huge;
	uint8_t		*p;
	uint64_t	 pa;
	size_t		 i;

	tty_puts("\nmemory subsystem smoke test...\n");

	small  = kmalloc(16);
	medium = kmalloc(256);
	large  = kmalloc(2000);
	huge   = kmalloc(8192);

	kprintf("  kmalloc(16)   = %p\n", small);
	kprintf("  kmalloc(256)  = %p\n", medium);
	kprintf("  kmalloc(2000) = %p\n", large);
	kprintf("  kmalloc(8192) = %p\n", huge);

	if (small == NULL || medium == NULL || large == NULL || huge == NULL)
		panic("smoke: kmalloc returned NULL");

	/* Scribble each buffer end-to-end to confirm the full extent is mapped. */
	for (p = small, i = 0; i < 16; i++)
		p[i] = (uint8_t)(0xAA ^ i);
	for (p = medium, i = 0; i < 256; i++)
		p[i] = (uint8_t)(0x55 ^ i);
	for (p = large, i = 0; i < 2000; i++)
		p[i] = (uint8_t)i;
	for (p = huge, i = 0; i < 8192; i++)
		p[i] = (uint8_t)(i >> 3);

	kfree(small);
	kfree(medium);
	kfree(large);
	kfree(huge);

	/* Direct pmm round-trip. */
	pa = pmm_alloc_page();
	if (pa == PA_INVALID)
		panic("smoke: pmm_alloc_page returned PA_INVALID");
	kprintf("  pmm_alloc_page = 0x%llx\n", (unsigned long long)pa);
	pmm_free_page(pa);

	pmm_stats();
	pmap_stats();
	kmem_stats();

	tty_puts("  [ok] smoke test passed\n");
}

static void
kmain_banner(uint32_t mb_magic, uint32_t mb_info)
{

	tty_set_attr(TTY_ATTR(TTY_LIGHT_GREEN, TTY_BLACK));
	tty_puts("style9-os: kernel up\n");

	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));
	tty_puts(
	    "----------------------------------------\n");

	kprintf("  boot magic      : 0x%08x", mb_magic);
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GREEN, TTY_BLACK));
	if (mb_magic == MULTIBOOT2_BOOTLOADER_MAGIC)
		tty_puts("  (multiboot2)\n");
	else if (mb_magic == MULTIBOOT1_BOOTLOADER_MAGIC)
		tty_puts("  (multiboot1)\n");
	else if (mb_magic == 0)
		tty_puts("  (PVH)\n");
	else {
		tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
		tty_puts("  (unknown protocol)\n");
	}
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));

	kprintf("  boot info ptr   : 0x%08x\n", mb_info);
	kprintf("  kernel range    : %p .. %p\n",
	    (void *)__kernel_start, (void *)__kernel_end);
	kprintf("  kernel size     : %u bytes\n",
	    (unsigned int)(__kernel_end - __kernel_start));
}

