# Hobby OS build (x86_64, long mode).
#
# Tree layout:
#   arch/amd64/		boot stub, GDT/IDT/PIC, ISR asm, trap dispatcher
#   kern/		machine-independent kernel core
#   dev/		drivers (tty, kbd, dbgcon)
#   obj/		build artefacts (created on demand)
#   kernel.elf		final linked image, at the project root
#
# Targets:
#   make		build kernel.elf
#   make run		boot it in QEMU
#   make clean		remove obj/ and kernel.elf

CC	= gcc
LD	= ld
OBJCOPY	= objcopy
QEMU	= /mnt/c/Program\ Files/qemu/qemu-system-x86_64.exe

# Directories that contain sources and their public headers.  Listed in
# include-search order: arch first so e.g. machine/io.h-style headers
# would shadow generic, then kern, dev, mach, test.  mach/ holds the
# Mach IPC subsystem (port/bootstrap/services/klog); test/ holds the
# boot-time stress harness.  Both sit on the include path so their
# bodies can pull in any kernel header without indirection.
ARCH	= arch/amd64
SRCDIRS	= $(ARCH) kern dev mach test

OBJDIR	= obj

INCLUDES = $(addprefix -I,$(SRCDIRS))

# -MMD writes a .d alongside each .o listing every header the source
# pulled in (excluding system headers).  -MP adds a stub rule for every
# such header so the build does NOT explode when a header is deleted;
# instead the .o is just rebuilt.  Together this means edits to .h
# files trigger the right .o recompiles without us re-listing deps by
# hand.
DEPFLAGS = -MMD -MP -MF $(OBJDIR)/$*.d

CFLAGS	= -m64 -std=c11 -ffreestanding -nostdlib		\
	  -fno-pic -fno-pie -fno-stack-protector		\
	  -fno-asynchronous-unwind-tables			\
	  -fno-omit-frame-pointer				\
	  -mno-red-zone -mno-mmx -mno-sse -mno-sse2		\
	  -mcmodel=kernel					\
	  -O2 -Wall -Wextra -Wpedantic				\
	  $(INCLUDES)

ASFLAGS	= -m64 -fno-pic -fno-pie $(INCLUDES)

LDFLAGS	= -m elf_x86_64 -T $(ARCH)/linker.ld			\
	  -nostdlib -z noexecstack -z max-page-size=0x1000

# VPATH lets the pattern rules below find sources without the recipe
# spelling out the source directory explicitly.
VPATH	= $(ARCH):kern:dev:mach:test

OBJS	= \
	$(OBJDIR)/boot.o	\
	$(OBJDIR)/gdt.o		\
	$(OBJDIR)/idt.o		\
	$(OBJDIR)/pic.o		\
	$(OBJDIR)/isr.o		\
	$(OBJDIR)/intr.o	\
	$(OBJDIR)/pit.o		\
	$(OBJDIR)/pmap.o	\
	$(OBJDIR)/smap.o	\
	$(OBJDIR)/syscall_entry.o \
	$(OBJDIR)/syscall.o	\
	$(OBJDIR)/usermode.o	\
	$(OBJDIR)/user_blob.o	\
	$(OBJDIR)/switch.o	\
	$(OBJDIR)/kmain.o	\
	$(OBJDIR)/kprintf.o	\
	$(OBJDIR)/panic.o	\
	$(OBJDIR)/ddb.o		\
	$(OBJDIR)/spinlock.o	\
	$(OBJDIR)/witness.o	\
	$(OBJDIR)/memmap.o	\
	$(OBJDIR)/pmm.o		\
	$(OBJDIR)/kmem.o	\
	$(OBJDIR)/port_object.o	\
	$(OBJDIR)/port_space.o	\
	$(OBJDIR)/port_msg.o	\
	$(OBJDIR)/bootstrap.o	\
	$(OBJDIR)/services.o	\
	$(OBJDIR)/launchd.o	\
	$(OBJDIR)/klog.o	\
	$(OBJDIR)/vm.o		\
	$(OBJDIR)/task.o	\
	$(OBJDIR)/thread.o	\
	$(OBJDIR)/sched.o	\
	$(OBJDIR)/tsc.o		\
	$(OBJDIR)/clock.o	\
	$(OBJDIR)/shell.o	\
	$(OBJDIR)/cmds.o	\
	$(OBJDIR)/stress.o	\
	$(OBJDIR)/tty.o		\
	$(OBJDIR)/dbgcon.o	\
	$(OBJDIR)/uart.o	\
	$(OBJDIR)/kbd.o		\
	$(OBJDIR)/kbd_drv.o	\
	$(OBJDIR)/uart_drv.o	\
	$(OBJDIR)/ata_drv.o	\
	$(OBJDIR)/dev_subsystem.o \
	$(OBJDIR)/elf.o		\
	$(OBJDIR)/progreg.o	\
	$(OBJDIR)/hello_elf.o	\
	$(OBJDIR)/clock_elf.o	\
	$(OBJDIR)/tasks_elf.o	\
	$(OBJDIR)/sh_elf.o	\
	$(OBJDIR)/excchild_elf.o \
	$(OBJDIR)/excchild_ud_elf.o \
	$(OBJDIR)/excchild_thr_elf.o \
	$(OBJDIR)/excchild_resume_elf.o \
	$(OBJDIR)/lsmp_elf.o \
	$(OBJDIR)/vmmap_elf.o \
	$(OBJDIR)/echod_elf.o \
	$(OBJDIR)/launchctl_elf.o \
	$(OBJDIR)/ksym.o

all: kernel.elf

# ---- ring-3 user-mode programs ------------------------------------------
# Built as standalone freestanding ELF64s and then wrapped into kernel-side
# .o files via objcopy so the kernel image contains the bytes inline.  No
# filesystem yet, so embedding the ELF directly is the simplest delivery.
#
# Every user program links against libstyle9 (lib/style9*.{c,h} + crt0.S).
# crt0 provides _start, calls main(argc=0, argv=NULL), then exit(rv).
USER_DIR     = user
LIB_DIR      = lib
USER_CFLAGS  = -m64 -std=c11 -ffreestanding -nostdlib			\
	       -fno-pic -fno-pie -fno-stack-protector			\
	       -fno-asynchronous-unwind-tables				\
	       -mno-red-zone -mno-mmx -mno-sse -mno-sse2		\
	       -O2 -Wall -Wextra					\
	       -I$(LIB_DIR)
USER_ASFLAGS = -m64 -fno-pic -fno-pie -I$(LIB_DIR)
USER_LDFLAGS = -m elf_x86_64 -nostdlib -T $(USER_DIR)/user.ld		\
	       -z noexecstack -z max-page-size=0x1000 -static

# libstyle9 objects.  crt0 MUST come first on the link line so its
# _start lands at the .text._start section the linker script anchors
# at e_entry; the LD script orders sections via *(.text._start) first,
# but listing crt0 ahead of the rest is the canonical libc pattern
# and avoids surprises if the script ever changes.
LIB_OBJS = \
	$(OBJDIR)/crt0.o	\
	$(OBJDIR)/style9_sys.o	\
	$(OBJDIR)/style9_str.o	\
	$(OBJDIR)/style9_mem.o	\
	$(OBJDIR)/style9_io.o	\
	$(OBJDIR)/style9_mach.o	\
	$(OBJDIR)/style9_dev.o	\
	$(OBJDIR)/style9_man.o

$(OBJDIR)/crt0.o: $(LIB_DIR)/crt0.S | $(OBJDIR)
	$(CC) $(USER_ASFLAGS) -c $< -o $@

# -MMD generates a .d sidecar so edits to lib/style9.h trigger user-
# library rebuilds; without it the lib/*.o land in obj/ with no header
# dependency, and changing a struct layout in style9.h leaves the kernel
# rebuilt but the user libs stale -- yielding mismatched wire format on
# every IPC the kernel and ring-3 exchange.
$(OBJDIR)/style9_%.o: $(LIB_DIR)/style9_%.c | $(OBJDIR)
	$(CC) $(USER_CFLAGS) $(DEPFLAGS) -c $< -o $@

# List the user programs in the registry.  Each must have a matching
# user/<name>.c file; everything else is wired up via the pattern rules
# below.  To add one, drop user/<name>.c on disk, append the name here,
# and register the matching _binary_<name>_elf_start/_end pair in
# kern/progreg.c.
USER_PROGRAMS = hello clock tasks sh excchild excchild_ud excchild_thr excchild_resume lsmp vmmap echod launchctl

$(OBJDIR)/%.user.o: $(USER_DIR)/%.c | $(OBJDIR)
	$(CC) $(USER_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJDIR)/%.elf: $(OBJDIR)/%.user.o $(LIB_OBJS) $(USER_DIR)/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(LIB_OBJS) $(OBJDIR)/$*.user.o

# Wrap each user ELF as a kernel-linkable object so the kernel sees
# _binary_<name>_elf_start / _end symbols.  --rename-section parks the
# bytes in .rodata so they are read-only at runtime.  cd $(OBJDIR) so
# objcopy doesn't bake the obj/ prefix into the symbol names.
$(OBJDIR)/%_elf.o: $(OBJDIR)/%.elf
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.$*_elf			\
	    $*.elf $*_elf.o

USER_ELFS = $(foreach p,$(USER_PROGRAMS),$(OBJDIR)/$(p).elf)

# ---- man pages -----------------------------------------------------------
# Render each docs/man/<name>.9 mdoc source to plain ASCII via mandoc, then
# embed the bytes in the kernel image via objcopy.  `col -b` strips the
# backspace-overstrike sequences mandoc uses for bold so the embedded text
# is clean ASCII the in-kernel renderer does not have to decode.  The
# resulting kernel symbols are _binary_<name>_9_txt_start and _end, used
# by mach/services.c's "man" service to ship pages to ring-3 callers.
#
# Auto-detected: every *.9 under docs/man/ contributes one .o.
MAN_SOURCES := $(wildcard docs/man/*.9)
MAN_TXTS    := $(MAN_SOURCES:docs/man/%.9=$(OBJDIR)/%.9.txt)
MAN_OBJS    := $(MAN_SOURCES:docs/man/%.9=$(OBJDIR)/%_man.o)

$(OBJDIR)/%.9.txt: docs/man/%.9 | $(OBJDIR)
	mandoc -Tutf8 $< | col -b > $@

$(OBJDIR)/%_man.o: $(OBJDIR)/%.9.txt
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.$*_man			\
	    $*.9.txt $*_man.o

#
# Two-pass link to embed a kernel-symbol table in .ksymtab.
#
# Pass 1: link with a tiny stub ksyms object so the kernel ELF has all
# real addresses resolved.  tools/gen_ksyms.sh then reads `nm -n` on
# that pass-1 image and emits ksyms_real.S, an assembly blob holding
# (addr, name_ptr) tuples + a string pool, all anchored under
# __ksymtab_start / __ksymtab_end.
#
# Pass 2: re-link with ksyms_real.o.  The linker script places
# .ksymtab between .data and .bss, so growth of the symbol section
# between pass 1 and pass 2 only shifts .bss -- text/rodata/data
# addresses stay put, and the addresses gen_ksyms.sh baked in remain
# correct in the final binary.
#
KSYMS_STUB_S  = $(OBJDIR)/ksyms_stub.S
KSYMS_REAL_S  = $(OBJDIR)/ksyms_real.S
KSYMS_STAGE1  = $(OBJDIR)/kernel.stage1

$(KSYMS_STUB_S): | $(OBJDIR)
	@printf '.section .ksymtab,"a",@progbits\n.global __ksymtab_start\n.global __ksymtab_end\n__ksymtab_start:\n.quad 0\n.quad 0\n__ksymtab_end:\n' > $@

$(OBJDIR)/ksyms_stub.o: $(KSYMS_STUB_S)
	$(CC) $(ASFLAGS) -c $< -o $@

$(KSYMS_STAGE1): $(OBJS) $(MAN_OBJS) $(OBJDIR)/ksyms_stub.o $(ARCH)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(MAN_OBJS) $(OBJDIR)/ksyms_stub.o

$(KSYMS_REAL_S): $(KSYMS_STAGE1) tools/gen_ksyms.sh
	tools/gen_ksyms.sh $(KSYMS_STAGE1) > $@

$(OBJDIR)/ksyms_real.o: $(KSYMS_REAL_S)
	$(CC) $(ASFLAGS) -c $< -o $@

kernel.elf: $(OBJS) $(MAN_OBJS) $(OBJDIR)/ksyms_real.o $(ARCH)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(MAN_OBJS) $(OBJDIR)/ksyms_real.o

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S | $(OBJDIR)
	$(CC) $(ASFLAGS) $(DEPFLAGS) -c $< -o $@

# Pull in the auto-generated .d files.  The leading '-' so a missing
# file (first build, or post-clean) is silent rather than fatal.  Cover
# kernel objects, libstyle9 objects, and per-program .user.o objects so
# header edits trigger the matching rebuilds everywhere.
-include $(OBJS:.o=.d)
-include $(LIB_OBJS:.o=.d)
-include $(addsuffix .d,$(addprefix $(OBJDIR)/,$(USER_PROGRAMS:=.user)))

$(OBJDIR):
	mkdir -p $(OBJDIR)

run: kernel.elf
	$(QEMU) -kernel kernel.elf -no-reboot

# `make log` boots the kernel headlessly with debugcon routed to QEMU's
# stdout, captures it to obj/boot.log via tee, kills QEMU after a short
# delay, and prints the log.  Replaces the screendump dance for any
# change that only needs to verify "what did the kernel print this run".
LOGFILE	= $(OBJDIR)/boot.log
LOGSEC	?= 2

log: kernel.elf
	@mkdir -p $(OBJDIR)
	@rm -f $(LOGFILE)
	@($(QEMU) -kernel kernel.elf -no-reboot -display none			\
	    -serial file:$$PWD/$(LOGFILE) 2>/dev/null &);			\
	  sleep $(LOGSEC);							\
	  taskkill.exe /F /IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
	@printf '\n--- %s ---\n' $(LOGFILE)
	@cat $(LOGFILE)
	@printf '\n--- end ---\n'

clean:
	rm -rf $(OBJDIR) kernel.elf

.PHONY: all run log clean
