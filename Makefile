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
QEMU	= /mnt/c/Program\ Files/qemu/qemu-system-x86_64.exe

# Directories that contain sources and their public headers.  Listed in
# include-search order: arch first so e.g. machine/io.h-style headers
# would shadow generic, then kern, then dev.
ARCH	= arch/amd64
SRCDIRS	= $(ARCH) kern dev

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
VPATH	= $(ARCH):kern:dev

OBJS	= \
	$(OBJDIR)/boot.o	\
	$(OBJDIR)/gdt.o		\
	$(OBJDIR)/idt.o		\
	$(OBJDIR)/pic.o		\
	$(OBJDIR)/isr.o		\
	$(OBJDIR)/intr.o	\
	$(OBJDIR)/pit.o		\
	$(OBJDIR)/pmap.o	\
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
	$(OBJDIR)/kbd.o

all: kernel.elf

kernel.elf: $(OBJS) $(ARCH)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

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
