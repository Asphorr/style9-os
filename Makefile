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
# would shadow generic, then kern, then dev, then test.  test/ is the
# boot-time stress harness; lives next to kern/ on the include path so
# its bodies can pull in any kernel header without indirection.
ARCH	= arch/amd64
SRCDIRS	= $(ARCH) kern dev test

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
VPATH	= $(ARCH):kern:dev:test

OBJS	= \
	$(OBJDIR)/boot.o	\
	$(OBJDIR)/gdt.o		\
	$(OBJDIR)/idt.o		\
	$(OBJDIR)/pic.o		\
	$(OBJDIR)/isr.o		\
	$(OBJDIR)/intr.o	\
	$(OBJDIR)/pit.o		\
	$(OBJDIR)/pmap.o	\
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
	$(OBJDIR)/memmap.o	\
	$(OBJDIR)/pmm.o		\
	$(OBJDIR)/kmem.o	\
	$(OBJDIR)/port.o	\
	$(OBJDIR)/bootstrap.o	\
	$(OBJDIR)/services.o	\
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
	$(OBJDIR)/dev_subsystem.o \
	$(OBJDIR)/elf.o		\
	$(OBJDIR)/hello_elf.o	\
	$(OBJDIR)/ksym.o

all: kernel.elf

# ---- ring-3 user-mode programs ------------------------------------------
# Built as standalone freestanding ELF64s and then wrapped into kernel-side
# .o files via objcopy so the kernel image contains the bytes inline.  No
# filesystem yet, so embedding the ELF directly is the simplest delivery.
USER_DIR     = user
USER_CFLAGS  = -m64 -std=c11 -ffreestanding -nostdlib			\
	       -fno-pic -fno-pie -fno-stack-protector			\
	       -fno-asynchronous-unwind-tables				\
	       -mno-red-zone -mno-mmx -mno-sse -mno-sse2		\
	       -O2 -Wall -Wextra
USER_LDFLAGS = -m elf_x86_64 -nostdlib -T $(USER_DIR)/user.ld		\
	       -z noexecstack -z max-page-size=0x1000 -static

$(OBJDIR)/hello.user.o: $(USER_DIR)/hello.c | $(OBJDIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJDIR)/hello.elf: $(OBJDIR)/hello.user.o $(USER_DIR)/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(OBJDIR)/hello.user.o

# Wrap the user ELF as a kernel-linkable object so the kernel sees
# _binary_obj_hello_elf_start / _end symbols.  --rename-section parks
# the bytes in .rodata so they are read-only at runtime.
$(OBJDIR)/hello_elf.o: $(OBJDIR)/hello.elf
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.hello_elf			\
	    hello.elf hello_elf.o

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

$(KSYMS_STAGE1): $(OBJS) $(OBJDIR)/ksyms_stub.o $(ARCH)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(OBJDIR)/ksyms_stub.o

$(KSYMS_REAL_S): $(KSYMS_STAGE1) tools/gen_ksyms.sh
	tools/gen_ksyms.sh $(KSYMS_STAGE1) > $@

$(OBJDIR)/ksyms_real.o: $(KSYMS_REAL_S)
	$(CC) $(ASFLAGS) -c $< -o $@

kernel.elf: $(OBJS) $(OBJDIR)/ksyms_real.o $(ARCH)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(OBJDIR)/ksyms_real.o

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S | $(OBJDIR)
	$(CC) $(ASFLAGS) $(DEPFLAGS) -c $< -o $@

# Pull in the auto-generated .d files.  The leading '-' so a missing
# file (first build, or post-clean) is silent rather than fatal.
-include $(OBJS:.o=.d)

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
