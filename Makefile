# Hobby OS build (x86_64, long mode).
#
# Tree layout:
#   arch/amd64/		boot stub, GDT/IDT/PIC, ISR asm, trap dispatcher
#   kern/		machine-independent kernel core
#   vm/			physical + virtual memory (pmm, vm_map, vm_object)
#   fs/			block cache, transactions, and the neutral file layer
#   fs/apfs/		the APFS reader and writer, and its self-tests
#   fs/fat/		the FAT reader
#   mach/		Mach IPC (ports, bootstrap, services)
#   dev/		drivers (tty, kbd, dbgcon)
#   obj/		build artefacts (created on demand)
#   kernel.elf		final linked image, at the project root
#
# vm/ and fs/ sit BESIDE kern/ rather than inside it, which is the split
# 4.4BSD draws and FreeBSD still keeps: sys/kern stays flat and large, while
# sys/vm and sys/fs are siblings.  The claim being made is that memory and
# file systems are subsystems the kernel core uses, not parts of it -- the
# same claim mach/ and dev/ already make here.  Adding a directory costs one
# word in SRCDIRS (which becomes its -I) and one in VPATH; OBJS names objects
# without a directory, so it does not change, and no #include does either.
#
# A file system gets a directory of its own, which is the same split again one
# level down: fs/ holds what every file system uses (the block cache, the
# transaction layer, the neutral layer that dispatches to whichever volume
# mounted) and each format holds itself.  There are two of them and one is
# thirty times the size of the other, so the flat fs/ that carried both had
# stopped saying which files belonged to what.
#
# Targets:
#   make		build kernel.elf
#   make run		boot it in QEMU
#   make clean		remove obj/ and kernel.elf

CC	= gcc
LD	= ld
OBJCOPY	= objcopy
QEMU	= /mnt/c/Program\ Files/qemu/qemu-system-x86_64.exe
# Emulated CPU.  Ring-3 Darwin binaries -- our clang/ld64 dyld and the real
# Apple ones -- reach past the QEMU default (qemu64), where an XMM instruction
# simply #UDs.  This was Penryn (SSE4.1) on the theory that SSE4.1 is the macOS
# x86_64 baseline; gls disproved it by faulting on PCMPGTQ, so the answer is
# now MEASURED rather than assumed.  Disassembling every vendored binary and
# counting instructions by extension:
#
#	gls.macho         20 SSE4.2 sites (pcmpgtq), 12 SSE4.1
#	libgmp.10.dylib   38 SSE4.2 sites, 1 SSE4.1
#	everything else    0 SSE4.2
#	NOTHING, anywhere, uses a VEX-encoded (AVX) instruction
#
# libgmp is the uncomfortable one: gfactor has been linking against SSE4.2 code
# since it was added and only survives because GMP picks its routines from
# CPUID at startup and found none.  It was one dispatch decision away from the
# fault gls actually hit.
#
# Nehalem is the smallest model that covers what is there: it adds SSE4.2 and
# POPCNT to Penryn and nothing else -- no AES, no XSAVE, no AVX -- so the
# FXSAVE-only FPU context switch stays correct, which an AVX-capable model
# would quietly invalidate (YMM state FXSAVE does not save).
QEMU_CPU = Nehalem

# Four processors, because a guest with one has no MADT worth reading and no
# application processor to start.  The extras sit in the firmware's halt loop
# until the kernel starts them, so they cost the host nothing while it is still
# only the boot processor that runs kernel code.  tools/run.ps1 keeps the same
# default; override with `make run QEMU_SMP=1' to boot as a uniprocessor.
QEMU_SMP ?= 4

# Directories that contain sources and their public headers.  Listed in
# include-search order: arch first so e.g. machine/io.h-style headers
# would shadow generic, then kern, vm, fs, dev, mach, test.  mach/ holds
# the Mach IPC subsystem (port/bootstrap/services/klog); test/ holds the
# boot-time stress harness.  All of them sit on the include path so their
# bodies can pull in any kernel header without indirection -- which is why
# moving a file between these directories does not touch a single #include.
ARCH	= arch/amd64
SRCDIRS	= $(ARCH) kern vm fs fs/apfs fs/fat dev mach test

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
VPATH	= $(ARCH):kern:vm:fs:fs/apfs:fs/fat:dev:mach:test

OBJS	= \
	$(OBJDIR)/boot.o	\
	$(OBJDIR)/acpi.o	\
	$(OBJDIR)/aptramp.o	\
	$(OBJDIR)/cpu.o		\
	$(OBJDIR)/mp.o		\
	$(OBJDIR)/gdt.o		\
	$(OBJDIR)/idt.o		\
	$(OBJDIR)/lapic.o	\
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
	$(OBJDIR)/mutex.o	\
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
	$(OBJDIR)/host.o	\
	$(OBJDIR)/vm.o		\
	$(OBJDIR)/vm_object.o	\
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
	$(OBJDIR)/mouse.o	\
	$(OBJDIR)/mouse_drv.o	\
	$(OBJDIR)/uart_drv.o	\
	$(OBJDIR)/ata_drv.o	\
	$(OBJDIR)/rtc.o		\
	$(OBJDIR)/dev_subsystem.o \
	$(OBJDIR)/elf.o		\
	$(OBJDIR)/macho.o	\
	$(OBJDIR)/darwin.o	\
	$(OBJDIR)/bio.o		\
	$(OBJDIR)/fat.o		\
	$(OBJDIR)/apfs.o	\
	$(OBJDIR)/apfs_test.o	\
	$(OBJDIR)/fs_txn.o	\
	$(OBJDIR)/fs.o		\
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
	$(OBJDIR)/oolchild_elf.o \
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
	$(OBJDIR)/dirlist_macho.o \
	$(OBJDIR)/timeprobe_macho.o \
	$(OBJDIR)/mmaptest_macho.o \
	$(OBJDIR)/filewrite_macho.o \
	$(OBJDIR)/ttyprobe_macho.o \
	$(OBJDIR)/gstty_macho.o \
	$(OBJDIR)/gmkdir_macho.o \
	$(OBJDIR)/grmdir_macho.o \
	$(OBJDIR)/tree_macho.o \
	$(OBJDIR)/guname_macho.o \
	$(OBJDIR)/gcat_macho.o \
	$(OBJDIR)/gls_macho.o \
	$(OBJDIR)/gfactor_macho.o \
	$(OBJDIR)/genv_macho.o \
	$(OBJDIR)/gtimeout_macho.o \
	$(OBJDIR)/pipefork_macho.o \
	$(OBJDIR)/dash_macho.o \
	$(OBJDIR)/demo_sh_macho.o \
	$(OBJDIR)/libSystem_dylib.o \
	$(OBJDIR)/libgmp_dylib.o \
	$(OBJDIR)/libedit_dylib.o \
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
USER_PROGRAMS = hello clock tasks sh excchild excchild_ud excchild_thr excchild_resume lsmp vmmap echod launchctl loopchild oolchild selfkill top heartbeatd argecho crasher

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
	    --set-section-alignment .data=4096				\
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

# tree: a SECOND real Apple x86-64 macOS CLI binary (Homebrew bottle, tree
# 2.3.2, vendored in extern/tree.macho).  Depends only on libSystem; it walks a
# directory hierarchy via opendir/readdir/lstat -- the binary that motivated the
# FAT VFS gaining real subdirectories.  Embedded like figlet for the loader.
$(OBJDIR)/tree.macho: extern/tree.macho | $(OBJDIR)
	cp $< $@

# guname: a THIRD real Apple x86-64 macOS CLI binary (GNU coreutils 9.11's
# uname, a Homebrew bottle vendored in extern/guname.macho).  Depends only on
# libSystem.  It exercises the machine-identity trick: it calls uname(2) and
# prints what it is told, so our kernel's fabricated Darwin identity card makes
# a genuine Apple binary report a Mac that does not exist.  Embedded like figlet.
$(OBJDIR)/guname.macho: extern/guname.macho | $(OBJDIR)
	cp $< $@

# gcat: GNU coreutils 9.11's cat, a Homebrew bottle vendored in
# extern/gcat.macho.  It is the smallest binary that reads a FILE rather than
# metadata: open, fstat, a page-aligned read loop, write.  Pointed at the APFS
# volume it prints bytes that came off real extents, through a real Apple
# binary, with nothing in it aware of what filesystem answered.  Cost four new
# libSystem symbols.  Embedded like figlet.
$(OBJDIR)/gcat.macho: extern/gcat.macho | $(OBJDIR)
	cp $< $@

# gls: GNU coreutils 9.11's ls, from the same bottle.  The first binary that
# asks what the filesystem REMEMBERS -- mode, owner, link count, timestamps --
# rather than what it holds, so it is the one that made the kernel stop
# throwing the inode's fixed part away.  It also walks a directory the *at
# way: opendir once, then fstatat(dirfd(dirp), name) per entry.  Cost 23 new
# libSystem symbols, the largest gap any binary here has needed closed, of
# which the load-bearing ones are the calendar (gmtime_r) and strmode.
# Embedded like figlet.
$(OBJDIR)/gls.macho: extern/gls.macho | $(OBJDIR)
	cp $< $@

# gfactor: a FOURTH real Apple x86-64 macOS CLI binary (GNU coreutils 9.11's
# factor, a Homebrew bottle vendored in extern/gfactor.macho).  Unlike the
# libSystem-only binaries above, it also depends on libgmp -- the first binary
# to drive a SECOND dependency, so our dyld must map the whole closure and bind
# each import against the dylib its lib_ordinal names.  Embedded like figlet.
$(OBJDIR)/gfactor.macho: extern/gfactor.macho | $(OBJDIR)
	cp $< $@

# libgmp: the real Homebrew x86_64 libgmp.10.dylib (GMP 6.3.0), gfactor's
# second dependency.  Not a program -- a dylib to bind against -- so it gets the
# dylib wrap rule below rather than a %_macho.o.  The kernel registers it under
# its literal install name (kern/darwin.c) and the dyld backchannel maps it.
$(OBJDIR)/libgmp.10.dylib: extern/libgmp.10.dylib | $(OBJDIR)
	cp $< $@

# dirlist (directory-enumeration probe): a self-authored Darwin-ABI binary that
# imports opendir$INODE64 / readdir$INODE64 / stat$INODE64 from our libSystem to
# walk the FAT volume.  Same real toolchain + low relink as dyldhello -- it
# de-risks the readdir syscalls before a genuine binary (tree) depends on them.
$(OBJDIR)/dirlist.dwn.o: $(USER_DIR)/dirlist.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/dirlist.macho: $(OBJDIR)/dirlist.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry \
	    -pagezero_size $(DYLDHELLO_BASE)

# timeprobe (wall-clock probe): the same kind of self-authored Darwin-ABI
# binary, for the clock.  It checks that the time is plausible, that it
# advances, and that it never runs backwards -- de-risking gettimeofday and
# clock_gettime before gdate or a timestamped listing depends on them.
$(OBJDIR)/timeprobe.dwn.o: $(USER_DIR)/timeprobe.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/timeprobe.macho: $(OBJDIR)/timeprobe.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry \
	    -pagezero_size $(DYLDHELLO_BASE)

# mmaptest (demand-paging probe): the same shape again, for mmap(2).  Checks
# that a mapping bigger than the machine's memory succeeds, that a page whose
# first writer is the KERNEL (read(2) into an untouched mapping) faults in
# correctly, and that a file mapping's bytes are the file's -- read(2) at the
# same offsets being the oracle.
$(OBJDIR)/mmaptest.dwn.o: $(USER_DIR)/mmaptest.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/mmaptest.macho: $(OBJDIR)/mmaptest.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry \
	    -pagezero_size $(DYLDHELLO_BASE)

# filewrite (volume-writing probe): the same shape again, for the rung where
# ring 3 can change the disk.  Checks that O_CREAT makes a file, that the bytes
# come back through a DIFFERENT descriptor, that an overwrite in the middle
# leaves the length alone, that O_APPEND ignores the cursor, that O_TRUNC
# empties at open time, that unlink removes the name, and that a built-in is
# still refused -- de-risking open/write/unlink before dash's `>` depends on
# them.
$(OBJDIR)/filewrite.dwn.o: $(USER_DIR)/filewrite.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/filewrite.macho: $(OBJDIR)/filewrite.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry \
	    -pagezero_size $(DYLDHELLO_BASE)

# ttyprobe (terminal probe): the same shape again, for the rung where the
# terminal can be TOLD something.  Checks that a fresh terminal is canonical
# with echo, that a file and a pipe both answer ENOTTY, that a raw setting
# READS BACK (so the kernel kept it rather than libSystem pretending), that
# VMIN=0 turns a read into a poll, and that one fed byte with no newline
# behind it arrives anyway -- de-risking termios before stty depends on it.
$(OBJDIR)/ttyprobe.dwn.o: $(USER_DIR)/ttyprobe.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/ttyprobe.macho: $(OBJDIR)/ttyprobe.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry \
	    -pagezero_size $(DYLDHELLO_BASE)

# gstty: the TENTH real Apple x86-64 macOS CLI binary (GNU coreutils 9.11's
# stty, from the same Homebrew bottle as gls/gcat/genv, vendored in extern/).
# The others read files, walk directories or compute; this one does nothing but
# interrogate and reconfigure the TERMINAL -- tcgetattr, tcsetattr and
# TIOCGWINSZ are its entire trade -- so it is the exact oracle for the rung it
# arrives on, and it needs no dylib we did not already have.
$(OBJDIR)/gstty.macho: extern/gstty.macho | $(OBJDIR)
	cp $< $@

# gmkdir / grmdir: the ELEVENTH and TWELFTH real Apple binaries (GNU coreutils
# 9.11's mkdir and rmdir, from the bottle gls and gstty came out of).  They are
# what makes the directory rung usable by hand rather than only by our own
# self-tests, and what they needed was not directories at all -- both were
# already there -- but the MODE WORD: chmod, the ownership calls that cannot be
# honoured here and say so, and the fd-relative family.
$(OBJDIR)/gmkdir.macho: extern/gmkdir.macho | $(OBJDIR)
	cp $< $@

$(OBJDIR)/grmdir.macho: extern/grmdir.macho | $(OBJDIR)
	cp $< $@

# pipefork (multi-process probe): a self-authored Darwin-ABI binary that
# exercises fork/execve/wait4/pipe/dup2 through our libSystem -- the probe that
# de-risks the process syscalls before genuine Apple binaries (genv, gtimeout)
# depend on them.  Same toolchain + low relink as dirlist.
$(OBJDIR)/pipefork.dwn.o: $(USER_DIR)/pipefork.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/pipefork.macho: $(OBJDIR)/pipefork.dwn.o $(OBJDIR)/libSystem.B.dylib
	$(DARWIN_LD) $(DARWIN_LDF) -o $@ $< -L$(OBJDIR) -lSystem.B -e _entry \
	    -pagezero_size $(DYLDHELLO_BASE)

# genv / gtimeout: the FIFTH and SIXTH real Apple x86-64 macOS CLI binaries
# (GNU coreutils 9.11's env and timeout, from the same Homebrew bottle as
# guname/gfactor, vendored in extern/).  They are the multi-process oracles:
# env exec(2)s its command in place, timeout fork(2)s it and wait4(2)s --
# genuine Apple binaries driving the Darwin process-lifecycle syscalls.
$(OBJDIR)/genv.macho: extern/genv.macho | $(OBJDIR)
	cp $< $@

$(OBJDIR)/gtimeout.macho: extern/gtimeout.macho | $(OBJDIR)
	cp $< $@

# dash: the SEVENTH real Apple x86-64 macOS CLI binary, and the first SHELL
# (Homebrew dash-shell 0.5.13.4, vendored in extern/dash.macho).  It binds
# libSystem AND libedit -- the latter answered by our clean-room stub dylib
# below -- and drives the whole process substrate at once: PATH search via
# stat64 against the synthetic /bin, fork/execve/wait3 for commands, pipe +
# dup2 for pipelines, fcntl(F_DUPFD) for saved fds, setjmp/longjmp for its
# error unwinding.  Embedded like figlet.
$(OBJDIR)/dash.macho: extern/dash.macho | $(OBJDIR)
	cp $< $@

# demo.sh: not a binary -- a plain-text shell script carried in progreg so
# the synthetic /bin can serve it to dash (open/stat work on it; execve
# refuses it by magic).  The .macho name is only to ride the generic embed
# rule; nothing ever parses it as Mach-O.
$(OBJDIR)/demo_sh.macho: $(USER_DIR)/demo.sh | $(OBJDIR)
	cp $< $@

# clean-room libedit.3.dylib -- dash's interactive line-editor dependency,
# stubbed (el_init returns NULL -> dash falls back to raw reads; it only
# consults libedit when stdin is a tty anyway).  Zero imports, so the dyld
# closure terminates at it; registered under /usr/lib/libedit.3.dylib in
# kern/darwin.c exactly like libSystem.
$(OBJDIR)/libedit.dwn.o: $(USER_DIR)/libedit_stub.c | $(OBJDIR)
	$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@

$(OBJDIR)/libedit.3.dylib: $(OBJDIR)/libedit.dwn.o
	$(DARWIN_LD) -dylib $(DARWIN_LDF) -o $@ $< \
	    -install_name /usr/lib/libedit.3.dylib

# libSystem is embedded so the dyld backchannel (kern/darwin.c) can map it by
# path -- it is a dependency to bind against, not a program to run, so it gets
# its own wrap rule (a .dylib, not a .macho).  objcopy derives the symbols
# _binary_libSystem_B_dylib_{start,end} from the input file name.
$(OBJDIR)/libSystem_dylib.o: $(OBJDIR)/libSystem.B.dylib
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.libSystem_dylib		\
	    --set-section-alignment .data=4096	\
	    libSystem.B.dylib libSystem_dylib.o

# libgmp embedded the same way: objcopy derives the kernel symbols
# _binary_libgmp_10_dylib_{start,end} from the file name "libgmp.10.dylib".
$(OBJDIR)/libgmp_dylib.o: $(OBJDIR)/libgmp.10.dylib
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.libgmp_dylib			\
	    --set-section-alignment .data=4096		\
	    libgmp.10.dylib libgmp_dylib.o

# libedit embedded the same way: objcopy derives the kernel symbols
# _binary_libedit_3_dylib_{start,end} from the file name "libedit.3.dylib".
$(OBJDIR)/libedit_dylib.o: $(OBJDIR)/libedit.3.dylib
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.libedit_dylib		\
	    --set-section-alignment .data=4096		\
	    libedit.3.dylib libedit_dylib.o

# Wrap a .macho into a kernel-linkable object exposing
# _binary_<name>_macho_start / _end.  Mirror of the %_elf.o rule.
#
# --set-section-alignment is what lets the loader stop copying these bytes.
# A Mach-O segment is page-aligned in the address space it asks for and
# page-aligned in the file, so if the blob itself starts on a page boundary
# in the kernel image, every whole page of a read-only segment lands exactly
# on a frame -- and kern/macho.c maps that frame into the task instead of
# allocating a new one and copying into it, once per task.  Drop the flag and
# nothing breaks: the alignment test fails and the loader copies as it used
# to, which the "borrowed / copied" counters at boot will report.
$(OBJDIR)/%_macho.o: $(OBJDIR)/%.macho
	cd $(OBJDIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386	\
	    --rename-section .data=.rodata.$*_macho			\
	    --set-section-alignment .data=4096		\
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

# Test disk: an APFS container carrying a couple of known files, which the
# kernel's APFS probe mounts off drive 0.  Checked in gzipped -- 512 MiB
# apparent but only a few hundred KiB of real content -- because BUILDING one
# needs a Linux host running the out-of-tree APFS module (see
# tools/mkapfs-image.sh), and no ordinary build should require that.
#
# Attaching it is the boot script's job, not make's: `make run` and `make log`
# below shell out to the WINDOWS qemu binary, which only works where WSL
# interop is enabled.  Where it is not, build here and boot with
# tools/run.ps1 from PowerShell -- that path always works and takes -Disk.
DISKFIX	= fixtures/style9.apfs.gz
DISKIMG	= $(OBJDIR)/style9.apfs

disk: $(DISKIMG)

$(DISKIMG): $(DISKFIX) | $(OBJDIR)
	gunzip -c $< > $@
	@printf 'disk: %s\n' $@

run: kernel.elf $(DISKIMG)
	$(QEMU) -cpu $(QEMU_CPU) -smp $(QEMU_SMP) -kernel kernel.elf	\
	    -no-reboot							\
	    -drive file=$(DISKIMG),format=raw,if=ide,index=0,media=disk

# `make log` boots the kernel headlessly with debugcon routed to QEMU's
# stdout, captures it to obj/boot.log via tee, kills QEMU after a short
# delay, and prints the log.  Replaces the screendump dance for any
# change that only needs to verify "what did the kernel print this run".
LOGFILE	= $(OBJDIR)/boot.log
LOGSEC	?= 2

log: kernel.elf $(DISKIMG)
	@mkdir -p $(OBJDIR)
	@rm -f $(LOGFILE)
	@($(QEMU) -cpu $(QEMU_CPU) -smp $(QEMU_SMP) -kernel kernel.elf		\
	    -no-reboot -display none						\
	    -drive file=$(DISKIMG),format=raw,if=ide,index=0,media=disk		\
	    -serial file:$$PWD/$(LOGFILE) 2>/dev/null &);			\
	  sleep $(LOGSEC);							\
	  taskkill.exe /F /IM qemu-system-x86_64.exe >/dev/null 2>&1 || true
	@printf '\n--- %s ---\n' $(LOGFILE)
	@cat $(LOGFILE)
	@printf '\n--- end ---\n'

# THE APFS WRITER, BUILT FOR THE HOST
#
# fs/apfs reaches outside itself for five symbols and no assembler, so it
# compiles and runs as an ordinary program against an image FILE -- see the
# header of tools/hostapfs.c for what that buys and what it does not.  Built
# with the host's own flags rather than CFLAGS: none of -ffreestanding,
# -mcmodel=kernel or -mno-sse describes a userland program, and the point is
# to run the same C somewhere a debugger and a second compiler can reach it.
#
# -fno-strict-aliasing is not a workaround for this harness.  The writer reads
# on-disk structures through pointers of several types, which is exactly what
# the kernel build's -O2 does too; saying so here keeps the two builds honest
# about compiling the same language.
HOSTCC	 ?= gcc
HOSTAPFS  = $(OBJDIR)/hostapfs
HOSTSRC	  = tools/hostapfs.c fs/apfs/apfs.c fs/apfs/apfs_test.c
HOSTFLAGS = -std=c11 -O1 -g -Wall -Wextra -fno-strict-aliasing $(INCLUDES)

hostapfs: $(HOSTAPFS)

$(HOSTAPFS): $(HOSTSRC) | $(OBJDIR)
	$(HOSTCC) $(HOSTFLAGS) -o $@ $(HOSTSRC)
	@printf 'hostapfs: %s\n' $@

# One run of the whole ladder against a FRESH image, and apfsck on what it
# leaves.  The image is removed first because $(DISKIMG) is a timestamp rule
# over the fixture: once it exists it is newer for ever, and a target that is
# silently up to date is a test that silently starts from the last run's
# damage.  That cost a whole acceptance pass once.
hostcheck: $(HOSTAPFS)
	@rm -f $(DISKIMG)
	@$(MAKE) --no-print-directory $(DISKIMG) >/dev/null
	./$(HOSTAPFS) $(DISKIMG) $(T)
	apfsck -c $(DISKIMG) && printf 'hostcheck: apfsck is happy\n'

clean:
	rm -rf $(OBJDIR) kernel.elf

.PHONY: all run log disk clean hostapfs hostcheck
