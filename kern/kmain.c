/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "bootstrap.h"
#include "clock.h"
#include "klog.h"
#include "services.h"
#include "vm.h"
#include "gdt.h"
#include "idt.h"
#include "intr.h"
#include "kbd.h"
#include "kbd_drv.h"
#include "kmem.h"
#include "kprintf.h"
#include "memmap.h"
#include "panic.h"
#include "pic.h"
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "sched.h"
#include "shell.h"
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

	uart_init();
	tty_init();
	kmain_banner(mb_magic, mb_info);

	tty_puts("\nbringing CPU tables online...\n");

	gdt_init();
	tty_puts("  [ok] gdt\n");

	idt_init();
	tty_puts("  [ok] idt\n");

	pic_init();
	tty_puts("  [ok] pic (8259 remapped to 0x20/0x28)\n");

	kmain_memory(mb_magic, mb_info);

	kbd_init();
	tty_puts("  [ok] kbd (IRQ1 unmasked)\n");

	intr_enable();
	tty_puts("  [ok] interrupts enabled\n");

	clock_init();
	tty_puts("  [ok] clock\n");

	kmain_memory_smoke();
	kmain_run_tests();

	kbd_drv_init();
	uart_drv_init();

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
	/*
	 * Just spawn hello.elf for now: the inline blob and the ELF
	 * both map at the same USER_CODE_VA/USER_STACK_VA, and with
	 * one shared PML4 a preempted blob would resume onto the
	 * ELF's freshly-overwritten pages.  Per-task PML4 lifts
	 * this; until then, one user thread at a time.
	 */
	usermode_run_hello_elf();

	shell_run();
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

	tty_puts("\n[1/13] stress mem 10000\n");
	rv_mem = stress_mem(10000);

	tty_puts("\n[2/13] stress mem boundary\n");
	rv_boundary = stress_mem_boundary();

	tty_puts("\n[3/13] stress timer 2s\n");
	rv_timer = stress_timer(2);

	tty_puts("\n[4/13] stress port 1000\n");
	int rv_port = stress_port(1000);

	tty_puts("\n[5/13] stress thread 200\n");
	int rv_thread = stress_thread(200);

	tty_puts("\n[6/13] stress preempt 4 workers, 1 s\n");
	int rv_preempt = stress_preempt(4, 1000);

	tty_puts("\n[7/13] stress sendonce 500\n");
	int rv_sendonce = stress_sendonce(500);

	tty_puts("\n[8/13] stress portset 4 members x 100\n");
	int rv_portset = stress_portset(4, 100);

	tty_puts("\n[9/13] stress intertask 200 (parent <-> worker task)\n");
	int rv_intertask = stress_intertask(200);

	tty_puts("\n[10/13] stress moverecv 200\n");
	int rv_moverecv = stress_moverecv(200);

	tty_puts("\n[11/13] stress nosenders 100\n");
	int rv_nosenders = stress_nosenders(100);

	tty_puts("\n[12/13] stress sendblock 2000\n");
	int rv_sendblock = stress_sendblock(2000);

	tty_puts("\n[13/13] stress rpc 200\n");
	int rv_rpc = stress_rpc(200);

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

