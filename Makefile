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
# Emulated CPU.  The macOS x86_64 baseline is Penryn (SSE4.1, no AVX): ring-3
# Darwin binaries -- our clang/ld64 dyld + real Apple binaries -- assume SSE4.1,
# which the QEMU default (qemu64) lacks, so an XMM insn #UDs there.  Penryn has
# no AVX, which keeps the FXSAVE-only FPU context switch (no XSAVE/YMM) correct.
QEMU_CPU = Penryn

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
	$(OBJDIR)/fpu.o		\
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
	$(OBJDIR)/macho.o	\
	$(OBJDIR)/darwin.o	\
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
	$(OBJDIR)/loopchild_elf.o \
	$(OBJDIR)/selfkill_elf.o \
	$(OBJDIR)/top_elf.o \
	$(OBJDIR)/heartbeatd_elf.o \
	$(OBJDIR)/argecho_elf.o \
	$(OBJDIR)/crasher_elf.o \
	$(OBJDIR)/machotest_macho.o \
	$(OBJDIR)/machotest_fat_macho.o \
	$(OBJDIR)/darwinhello_macho.o \
	$(OBJDIR)/darwinmsg_macho.o \
	$(OBJDIR)/dyld_macho.o \
	$(OBJDIR)/dyldhello_macho.o \
	$(OBJDIR)/dyldbig_macho.o \
	$(OBJDIR)/figlet_macho.o \
	$(OBJDIR)/libSystem_dylib.o \
	$(OBJDIR)/ksym.o

all: kernel.elf

# ---- ring-3 user-mode programs ------------------------------------------
# Built as standalone freestanding ELF64s and then wrapped into kernel-side
# .o files via objcopy so the kernel image contains the bytes inline.  No
# filesystem yet, so embedding the ELF directly is the simplest delivery.
#
# Every user program links against libstyle9 (lib/style9*.{c,h} + crt0.S).
# crt0 provides _start: it reads the argc/argv frame the kernel launcher
# lays down at entry %rsp, calls main(argc, argv), then exit(rv).
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
USER_PROGRAMS = hello clock tasks sh excchild excchild_ud excchild_thr excchild_resume lsmp vmmap echod launchctl loopchild selfkill top heartbeatd argecho crasher

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

# ---- ring-3 programs delivered as Mach-O (S1: container loader) ----------
# Apple ships ring-3 binaries as Mach-O.  To exercise the kernel's Mach-O
# loader (kern/macho.c) we take an ordinary style9 user ELF and rewrap it
# as a Mach-O with the host tool tools/elf2macho -- this build host has no
# Darwin cross toolchain, and a converter yields a deterministic, spec-shaped
# container without one.  The program ABI inside is unchanged (libstyle9
# crt0 + SYS_* numbers); only the container format differs, which is exactly
# what S1 sets out to prove.  Matching Apple's syscall/Mach-trap ABI is a
# separate, later step.
#
# Each name is built from user/<name>.c via the generic %.user.o + %.elf
# rules (the .macho targets pull the .elf in), then converted to BOTH a thin
# x86-64 Mach-O and a one-slice fat/universal archive, and objcopy-wrapped
# into _binary_<name>_macho.o / _binary_<name>_fat_macho.o for progreg.c.
MACHO_PROGRAMS = machotest

# Host build of the converter: a plain hosted compile (NOT the freestanding
# kernel flags).  -Ikern lets it share the exact wire structs in kern/macho.h
# and kern/elf.h, so the converter and the kernel can never drift apart.
# -U_FORTIFY_SOURCE disables glibc's _FORTIFY_SOURCE memcpy checking, whose
# object-size analysis throws a false positive on our calloc'd-buffer +
# computed-offset writes at -O2; runtime hardening is irrelevant for a
# deterministic build tool, and plain -Warray-bounds stays on for real bugs.
HOST_CC     = $(CC)
HOST_CFLAGS = -O2 -Wall -Wextra -std=c11 -Ikern -U_FORTIFY_SOURCE
ELF2MACHO   = $(OBJDIR)/elf2macho

$(ELF2MACHO): tools/elf2macho.c kern/macho.h kern/elf.h | $(OBJDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tools/elf2macho.c

# machotest ships as two containers from one ELF: a thin slice and a
# single-arch fat/universal archive, so the loader's thin path AND its
# fat slice-picker both get boot-exercised.
$(OBJDIR)/machotest.macho: $(OBJDIR)/machotest.elf $(ELF2MACHO)
	$(ELF2MACHO) $< $@

$(OBJDIR)/machotest_fat.macho: $(OBJDIR)/machotest.elf $(ELF2MACHO)
	$(ELF2MACHO) fat $< $@

# darwinhello (S2): a freestanding Darwin-ABI stub -- raw class-encoded
# syscalls, NO libstyle9 and NO crt0, so it links from its own object alone
# (NOT through the libstyle9 %.elf rule).  elf2macho's `macos` mode stamps it
# LC_BUILD_VERSION PLATFORM_MACOS, so macho_load flips the task to the Darwin
# syscall personality and its write/getpid/exit + Mach traps exercise
# darwin_dispatch (kern/darwin.c).  The generic %_macho.o rule below wraps the
# .macho into _binary_darwinhello_macho_{start,end} for progreg.c.
$(OBJDIR)/darwinhello.o: $(USER_DIR)/darwinhello.S | $(OBJDIR)
	$(CC) $(USER_ASFLAGS) -c $< -o $@

$(OBJDIR)/darwinhello.elf: $(OBJDIR)/darwinhello.o $(USER_DIR)/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(OBJDIR)/darwinhello.o

$(OBJDIR)/darwinhello.macho: $(OBJDIR)/darwinhello.elf $(ELF2MACHO)
	$(ELF2MACHO) macos $< $@

# darwinmsg (S3): a freestanding Darwin program that does a real mach_msg()
# round-trip.  Freestanding C this time (no libstyle9, no crt0): -fno-builtin
# keeps the compiler from lowering its struct stores into a memcpy/memset call
# the no-libc link could not resolve.  Same macos wrapping as darwinhello.
$(OBJDIR)/darwinmsg.o: $(USER_DIR)/darwinmsg.c | $(OBJDIR)
	$(CC) $(USER_CFLAGS) -fno-builtin -c $< -o $@

$(OBJDIR)/darwinmsg.elf: $(OBJDIR)/darwinmsg.o $(USER_DIR)/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(OBJDIR)/darwinmsg.o

$(OBJDIR)/darwinmsg.macho: $(OBJDIR)/darwinmsg.elf $(ELF2MACHO)
	$(ELF2MACHO) macos $< $@

# ---- ring-3 programs delivered as DYNAMIC Mach-O (S4: real Darwin toolchain) -
# S1-S3 rewrap a style9 ELF with elf2macho because the host had no Darwin
# toolchain.  S4 brings the real one: clang emits Mach-O objects for the
# x86_64-apple-macos target and ld64.lld links genuine DYNAMIC Mach-Os
# (LC_LOAD_DYLINKER + LC_LOAD_DYLIB + LC_DYLD_CHAINED_FIXUPS) -- exactly what an
# Apple binary carries.  They are bound at runtime by our own clean-room dyld
# (user/dyld.c) against our own libSystem (user/libsystem.c); no Apple bits.
#
# Everything is relinked LOW into the style9 user-VA window [0x40000000,
# 0x80000000) (kern/vm.h): the main exe and the linker via -pagezero_size (the
# linker is an MH_EXECUTE so the existing loader maps it), each clear of the
# others and of the user stack at 0x4000F000.  -fixup_chains gives the dyld a
# single fixup format with no lazy-binding / stub-helper path.
DARWIN_CC     = clang-18
DARWIN_LD     = ld64.lld-18
DARWIN_TARGET = x86_64-apple-macos11
# SSE is intentionally LEFT ON here (unlike the style9 USER_CFLAGS): the kernel
# now enables it for ring 3 (fpu_init -> CR4.OSFXSR|OSXMMEXCPT) and saves and
# restores XMM/x87 per-thread (thread_switch_asm FXSAVE/FXRSTOR), so clang may
# vectorise freely -- which is also exactly why a real Apple binary's baseline
# SSE2 will run.  Removing -mno-sse here re-introduces the XMM insns that #UD'd
# before the FPU rung; their now-clean execution is the proof SSE works.
DARWIN_CFLAGS = -target $(DARWIN_TARGET) -O2 -fno-builtin -Wall -Wextra
DARWIN_LDF    = -arch x86_64 -platform_version macos 11.0 11.0 -fixup_chains

DYLD_BASE      = 0x60000000
DYLDHELLO_BASE = 0x50000000

# clean-room libSystem.B.dylib -- the link-time + runtime dependency of every
# dynamic program; its -install_name is the path the dyld resolves at runtime.
$(OBJDIR)/libsystem.dwn.o: $(USER_DIR)/libsystem.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/libSystem.B.dylib: $(OBJDIR)/libsystem.dwn.o
	$(DARWIN_LD) -dylib $(DARWIN_LDF) -o $@ $< \
	    -install_name /usr/lib/libSystem.B.dylib -undefined dynamic_lookup

# our dynamic linker: a freestanding MH_EXECUTE at a fixed base, entry _dyld_start.
$(OBJDIR)/dyld.dwn.o: $(USER_DIR)/dyld.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/dyld.macho: $(OBJDIR)/dyld.dwn.o
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -e _dyld_start \
	    -pagezero_size $(DYLD_BASE)

# dyldhello: the dynamic test program -- imports write/exit from libSystem and
# names /usr/lib/dyld as its LC_LOAD_DYLINKER; entry _entry.
$(OBJDIR)/dyldhello.dwn.o: $(USER_DIR)/dyldhello.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/dyldhello.macho: $(OBJDIR)/dyldhello.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry \
	    -pagezero_size $(DYLDHELLO_BASE)

# dyldbig: dyldhello relinked WITHOUT -pagezero_size, so ld64 gives it the
# default 4 GiB __PAGEZERO and __TEXT at 0x100000000 -- the exact addressing of
# a real Apple binary.  It exercises macho_load's load-bias path (relocate-low
# into the user window), the structural prerequisite for loading an arbitrary
# Apple binary; everything else is identical to dyldhello.
$(OBJDIR)/dyldbig.macho: $(OBJDIR)/dyldhello.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry

# figlet: a REAL Apple x86-64 macOS CLI binary -- a Homebrew bottle, vendored in
# extern/figlet.macho (NOT built from source).  This is the S5 north-star test:
# a genuine Apple-toolchain dynamic Mach-O (chained fixups, __TEXT @ 0x100000000,
# depends only on /usr/lib/libSystem.B.dylib) loaded by macho_load's relocate-low
# path, bound by our clean-room dyld against our clean-room libSystem, and run.
# Copied into the build dir so the generic %_macho.o rule embeds it like the rest.
$(OBJDIR)/figlet.macho: extern/figlet.macho | $(OBJDIR)
	cp $< $@

# libSystem is embedded so the dyld backchannel (kern/darwin.c) can map it by
# path -- it is a dependency to bind against, not a program to run, so it gets
# its own wrap rule (a .dylib, not a .macho).  objcopy derives the symbols
# _binary_libSystem_B_dylib_{start,end} from the input file name.
$(OBJDIR)/libSystem_dylib.o: $(OBJDIR)/libSystem.B.dylib
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.libSystem_dylib		\
	    libSystem.B.dylib libSystem_dylib.o

# Wrap a .macho into a kernel-linkable object exposing
# _binary_<name>_macho_start / _end.  Mirror of the %_elf.o rule.
$(OBJDIR)/%_macho.o: $(OBJDIR)/%.macho
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.$*_macho			\
	    $*.macho $*_macho.o

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
-include $(addsuffix .d,$(addprefix $(OBJDIR)/,$(MACHO_PROGRAMS:=.user)))

$(OBJDIR):
	mkdir -p $(OBJDIR)

run: kernel.elf
	$(QEMU) -cpu $(QEMU_CPU) -kernel kernel.elf -no-reboot

# `make log` boots the kernel headlessly with debugcon routed to QEMU's
# stdout, captures it to obj/boot.log via tee, kills QEMU after a short
# delay, and prints the log.  Replaces the screendump dance for any
# change that only needs to verify "what did the kernel print this run".
LOGFILE	= $(OBJDIR)/boot.log
LOGSEC	?= 2

log: kernel.elf
	@mkdir -p $(OBJDIR)
	@rm -f $(LOGFILE)
	@($(QEMU) -cpu $(QEMU_CPU) -kernel kernel.elf -no-reboot -display none	\
	    -serial file:$$PWD/$(LOGFILE) 2>/dev/null &);			\
	  sleep $(LOGSEC);							\
	  taskkill.exe /F /IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
	@printf '\n--- %s ---\n' $(LOGFILE)
	@cat $(LOGFILE)
	@printf '\n--- end ---\n'

clean:
	rm -rf $(OBJDIR) kernel.elf

.PHONY: all run log clean
