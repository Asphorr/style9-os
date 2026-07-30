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
 * Top-of-kernel entry called from boot.S after the multiboot1 stub has
 * set up a stack and pushed the bootloader's magic and info pointer.
 *
 * Init order is deliberate: console first so subsequent stages can log,
 * then CPU tables (GDT before IDT so traps land in our handlers using
 * our selectors), then the 8259 PIC (must be remapped before sti), then
 * the keyboard (depends on IRQ1 being routable).  Only at the very end
 * do we enable interrupts globally.
 */
void
kmain(uint32_t mb_magic, uint32_t mb_info)
{

	/*
	 * FIRST, before anything at all.  Per-CPU state is reached through
	 * the GS base and the base is zero until this runs, so any earlier
	 * code that touched it would read and write physical page zero
	 * instead -- and spin_lock touches it, by way of the preempt count,
	 * which puts the boundary before the first lock rather than before
	 * the first obviously CPU-flavoured call.  There is no console yet;
	 * cpu_print checks the result out loud once there is one.
	 */
	cpu_bsp_init();

	uart_init();
	tty_init();
	/*
	 * Before the banner, because it scribbles across the screen and
	 * clears it: the first thing a reader should see is a boot log, not
	 * the wreckage of the test that made the console trustworthy enough
	 * to print one.  It runs at all because everything above this line
	 * -- and every shell above that -- has been drawing text under a
	 * cursor that was not following it.
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
	 * After the memory system, because the APIC's registers have to be
	 * MAPPED, and while interrupts are still off, because the interrupt
	 * path is what is being rewired: software-enabling the APIC routes
	 * the 8259's output through its LINT0 pin, and until that pin is
	 * programmed the legacy controller reaches nobody at all.
	 */
	if (lapic_init())
		tty_puts("  [ok] local apic (legacy pins wired through)\n");
	else
		tty_puts("  [--] local apic absent -- 8259 alone\n");

	kbd_init();
	tty_puts("  [ok] kbd (IRQ1 unmasked)\n");

	intr_enable();
	tty_puts("  [ok] interrupts enabled\n");

	clock_init();
	tty_puts("  [ok] clock\n");

	/*
	 * Needs the PIT ticking to be measured against, and interrupts on to
	 * be caught arriving, so it cannot happen beside lapic_init.
	 */
	lapic_timer_probe();

	kmain_memory_smoke();
	kmain_run_tests();

	kbd_drv_init();
	/*
	 * Give the keyboard its second consumer.  Until a Darwin task claims
	 * the console this sink declines every key and the driver behaves
	 * exactly as it did; registering it at boot rather than at first
	 * claim keeps the driver thread free of a "has anyone registered
	 * yet" question it would have to ask on every keystroke.
	 */
	kbd_drv_set_sink(darwin_cons_sink);
	/*
	 * Mouse comes up here, not beside kbd_init: lighting IRQ12 before
	 * clock_init would let a pending aux byte fire an IRQ whose
	 * intr_dispatch -> sched_check_timeouts -> clock_uptime_ms path
	 * divides by the still-zero pit_hz().  The keyboard is unmasked
	 * early because the boot shell needs it; the mouse has no
	 * early-boot consumer, so deferring it to here costs nothing.
	 */
	mouse_init();
	mouse_drv_init();
	uart_drv_init();
	ata_drv_init();
	bio_init();		/* before any filesystem reads a block */
	fs_fat_init();
	fs_apfs_init();
	/*
	 * Anything an interrupted boot left with no name is finished now, while
	 * nothing in the system holds a file and the answer is therefore known
	 * without asking.  Zero on every clean boot; the log says so when not.
	 */
	(void)fs_reap_orphans();
	/*
	 * Mounting is the block cache's worst honest workload: probing two
	 * filesystems and walking a volume's tree asks for the same metadata
	 * blocks over and over.  Reporting here says what that cost.
	 */
	bio_stats();
	ata_irq_stats();

	/*
	 * After the mount numbers are reported, so the cost of mounting stays
	 * comparable across boots and the write test's own I/O does not muddy
	 * it.  See fs/fs.h for what the test claims and why a read-back alone
	 * would not have been evidence.
	 */
	fs_write_selftest();
	/*
	 * And the other half of writing: the space accounting.  Takes a run of
	 * free blocks, checks the disk agrees, and gives it back -- see
	 * fs/apfs/apfs.h for why it cannot honestly keep them.
	 */
	if (fs_apfs_ready())
		fs_apfs_alloc_selftest();
	/*
	 * And the mechanism that will one day make both of those durable as a
	 * unit: a checkpoint.  Runs last of the three because it moves the
	 * container to a new transaction id, and the two tests above are
	 * easier to read against the one it booted in.
	 */
	fs_ckpt_selftest();

	/*
	 * And what all of it is for: a write that leaves the checkpoint behind
	 * it describing the bytes it actually had.  Last, because it is the
	 * only one of these that moves a file's contents.
	 */
	fs_data_selftest();

	/*
	 * And the shape of the tree changing.  Shorter FIRST: the file it finds
	 * is the one the boot before grew, so the tail it checks on the way in
	 * is the proof that growth outlived the machine, and the length it
	 * leaves behind is what gives the growth test something to do on every
	 * boot instead of once in the life of an image.
	 */
	fs_trunc_selftest();
	fs_grow_selftest();

	/*
	 * And a file that was not there at all.  Every test above works on
	 * something the image already carried; this one makes its own, writes
	 * into it, and leaves it for the next boot to find and take away again
	 * -- so the volume ends every boot after the first exactly as it began.
	 */
	fs_make_selftest();

	/*
	 * And a DIRECTORY that was not there at all, which is the same claim
	 * about a different record -- and one more besides: a name is made
	 * inside it, so a directory this kernel invented an instant ago is one
	 * the reader can descend into and the writer can key an entry under.
	 * It is emptied again and left, for the same reason the file above is.
	 */
	fs_dirs_selftest();

	/*
	 * And the one test in here the kernel cannot satisfy: a file a REAL
	 * Apple shell redirected into, during the boot before this one.  It has
	 * to run here, before ring 3 starts, or it would be checking what was
	 * written moments ago instead of what survived the power going off.
	 */
	fs_shell_selftest();

	/*
	 * Last of all, the three operations nothing else reaches any more.  A
	 * node that runs out of room: appending stopped filling one once runs
	 * that touch began to be merged, so this asks for a split outright.
	 * A node that stops starting where the index above it says it does,
	 * which needs a delete to land on a node's first record.  And a node
	 * that has nothing left in it at all, which has to leave the tree.
	 * The last two are arranged rather than waited for, because where a
	 * delete lands is a property of where the splits fell.
	 */
	fs_split_selftest();
	fs_index_selftest();
	fs_drop_selftest();

	/*
	 * And a file whose two records a split has put on either side of a
	 * node boundary, which is the case an unlink used to answer success to
	 * while leaving half of it on the volume.
	 */
	fs_stream_selftest();

	/*
	 * And the same three shapes again, this time asked for by an ordinary
	 * caller rather than by a test: names go into a directory until a leaf
	 * has no room, and the create that finds it full is the one that has to
	 * split it.  Runs after the three above so that the tree it fills is
	 * the deep one they leave behind.
	 */
	fs_room_selftest();

	/*
	 * And a name moved rather than made or destroyed, which is the one
	 * writer whose success changes no count on the volume.  It runs after
	 * the fill above for the same reason that one runs after the splits:
	 * the leaves it moves records between are fuller here than they would
	 * be against a pristine volume.
	 */
	fs_move_selftest();

	/*
	 * And a file that outlives the name it was reached by, which is the
	 * half of unlink(2) this kernel used to say out loud it did not keep.
	 * After the move test because it is the same machinery pointed at the
	 * private directory, and a failure there is worth reading first.
	 */
	fs_orphan_selftest();

	/*
	 * And that every file those tests opened was given back.  Last of the
	 * filesystem tests on purpose: it is an assertion about all of them,
	 * and the only moment it can be made is after the last one and before
	 * ring 3 opens anything of its own.
	 */
	fs_open_check();

	/*
	 * And then, against the tree those three leave behind -- three levels
	 * deep, with split halves and a node's worth of gaps in it -- that
	 * looking a record up by its key answers what reading every record
	 * answers.  It goes last on purpose: run against the pristine volume it
	 * would be checking a descent through two levels that never had to
	 * choose.
	 */
	fs_seek_selftest();

	/*
	 * Register a demo service under the bootstrap port so ring-3
	 * code has something to look up.  MACH_PORT_TASK_SELF=1 in
	 * kernel_space resolves to kernel_task's task_self port; we
	 * publish it as "kernel_task" so a lookup returns a fresh SEND
	 * right under a new name in the caller's space, which then
	 * routes through the synchronous task_self dispatcher when used.
	 */
	if (bootstrap_register("kernel_task",
	    MACH_PORT_TASK_SELF) != MACH_MSG_OK)
		panic("kmain: bootstrap_register(kernel_task)");

	/*
	 * Bring up the kernel-side Mach services (clock, stats, tasks).
	 * Each is a PORT_SPECIAL_SERVICE port with a synchronous
	 * dispatcher, registered under its string name in the bootstrap
	 * port so any task -- kernel or future ring-3 -- finds them via
	 * the standard bootstrap_lookup path.
	 */
	services_init();

	/*
	 * Publish the host port (machine-identity + page-size service) on the
	 * same bootstrap registry.  Native tasks reach it via
	 * bootstrap_lookup("host"); genuine Darwin binaries via the
	 * mach_host_self() trap.  Runs after services_init since it shares the
	 * bootstrap registry and the kernel_space install path.
	 */
	host_init();

	/*
	 * Publish the bootstrap port's own kernel_space SEND so a task can
	 * fetch it via task_get_special_port(TASK_SPECIAL_BOOTSTRAP).  Runs
	 * here (Phase 2), not in bootstrap_init: kernel_space's well-known low
	 * names (TASK_SELF=1, BOOTSTRAP=2) must be claimed by
	 * task_subsystem_init first.
	 */
	bootstrap_publish();

	/*
	 * Bring up the structured kernel log on the same machinery.
	 * Writes here mirror to tty, which already pipes through to
	 * COM1 + debugcon, so every klog line lands on three sinks at
	 * once -- visible on the VGA console, captured by
	 * `qemu -serial file:...`, and dumped by `qemu -debugcon stdio`.
	 */
	klog_service_init();

	/* A couple of boot-time markers, mostly so `log tail` after
	   the shell comes up has something to show. */
	klog(KLOG_LEVEL_INFO,  "boot", "stress pass complete");
	klog(KLOG_LEVEL_INFO,  "boot", "drivers + services up");
	klog(KLOG_LEVEL_DEBUG, "boot", "entering shell");

	syscall_init();
	smap_init();
	(void)smap_enable_runtime();

	progreg_init();

	/*
	 * Only now can launchd resolve its boot catalog: every job in it names
	 * a program by string, and the registry that turns a string into an
	 * image is the one progreg_init just filled.  Ordering it here rather
	 * than leaving launchd's worker thread to guess is the whole point --
	 * see launchd_load_catalog in mach/launchd.c.
	 */
	launchd_load_catalog();

	/*
	 * Run hello.elf once before handing the console to sh.elf.  Its
	 * main() exercises the userspace surface end-to-end (port self-send,
	 * task_self RPC, bootstrap_lookup chain, OOL round-trip via
	 * svc/echool) and exits with rv==0 on success.  Doing it here gives
	 * a headless boot a deterministic ring-3 smoke test without needing
	 * a way to drive sh.elf's stdin.
	 */
	{
		long	hello_id;

		hello_id = progreg_spawn("hello");
		if (hello_id > 0) {
			while (task_is_alive((uint64_t)hello_id))
				thread_yield();
			sched_reap_zombies();
		} else {
			kprintf("kmain: spawn(hello) failed rv=%ld\n",
			    hello_id);
		}
		/*
		 * What the demos actually cost in pages.  Reported here rather
		 * than beside the block-cache line above because nothing has
		 * faulted yet at mount time -- every mapping the loader makes
		 * is populated eagerly, and only the ring-3 programs above ask
		 * for memory they have not touched.
		 */
		vm_fault_stats();
		/*
		 * Frames, after the demos rather than at mount time.  The
		 * sharing counters are the ones worth reading here: at boot
		 * they are necessarily zero because nothing has forked yet,
		 * so the line printed during pmm_init can only ever say the
		 * mechanism is idle.
		 */
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
	 * Phase 2: ring-3 shell takes over as the user-facing surface.
	 *
	 * sh.elf calls dev_open_stream("kbd"), which MOVE_RECEIVEs the
	 * single RECV right on kbd_input_port out of kernel_space and into
	 * sh.elf's port_space.  After that the in-kernel kern/shell.c can
	 * no longer recv on kbd_input_port, so we must NOT call shell_run()
	 * here -- it would either panic (port_set_insert on a port we no
	 * longer own) or steal characters in a race with sh.elf.  The file
	 * stays in the tree as a fallback / reference; phase 3 deletes it
	 * once the userspace surface has full parity (disk + dev listing
	 * subcommands).
	 *
	 * If sh.elf ever fails to spawn we drop straight into the legacy
	 * kernel shell so the system stays interactive.
	 */
	if (progreg_spawn("sh") < 0) {
		kprintf("kmain: spawn(sh) failed -- falling back to kernel shell\n");
		shell_run();
		/* NOTREACHED */
	}

	/*
	 * sh.elf is now the interactive surface.  This thread (the boot
	 * thread / kernel_task's id=1) has done its job; exit so it falls
	 * off the runq cleanly.
	 *
	 * Why not yield-spin: a yield-loop keeps the boot thread perpetually
	 * READY, and pick_next_locked only returns idle_thread when the
	 * runq is empty.  idle_loop is the only caller of
	 * sched_reap_zombies, so a boot thread that lingers on the runq
	 * starves the reaper -- exited user tasks stay in task_list, the
	 * shell's yield-spin on SYS_TASK_ALIVE never sees them go away,
	 * and the prompt never comes back after a child returns.
	 *
	 * thread_exit() turns this thread into a zombie and hands the CPU
	 * to whatever pick_next finds next; idle is now reachable and
	 * reaps both this boot thread and any later user-task zombies.
	 * kernel_task's t_refs stay high because kbd_drv_thread,
	 * uart_drv_thread, idle_thread, and the service threads are all
	 * still attached to it, so the task itself is unaffected.
	 */
	thread_exit();
	/* NOTREACHED */
}

/*
 * Boot-time test pass.  Runs every stress harness in turn so a headless
 * `make log` boot exercises the whole stack without needing keyboard
 * input.  Once it returns the shell takes over for interactive use.
 *
 * The tests are independently fatal-tolerant: a failure in stress_mem
 * does not skip the boundary or timer test, because we want the full
 * picture in one boot rather than peeling failures back one at a time.
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
 *	pmap	probe the live CR3 (the boot identity map) and prep the
 *		machine-dependent VM API.  Does not touch any mappings;
 *		it just records where the root table lives so later
 *		callers can extend the tree.
 *
 *	kmem	initialise empty buckets; first kmalloc() will pull a
 *		page from pmm on demand.
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
	 * Enable SSE/x87 for ring 3 and capture the clean FXSAVE template
	 * BEFORE any thread exists: thread_subsystem_init's boot thread and
	 * every thread_create seed th_fpu from it, and the first context
	 * switch FXRSTORs it.
	 */
	fpu_init();
	thread_subsystem_init();
	sched_init();

	tty_puts("  [ok] memory subsystem ready\n");
}

/*
 * Touch every layer with a representative workload, so a regression
 * in any of pmm / pmap / kmem shows up as a visible panic / wrong
 * stat in the boot log rather than a latent bug surfacing weeks later
 * from some unrelated code path.
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

