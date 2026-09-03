#!/bin/sh
# makedemo.sh -- GNU make, a real Apple binary, driving a real build on the
# APFS volume.  Every command below is dash, gmake, or a coreutils bottle;
# nothing in any of them knows what kernel is answering.
#
# The sources are rewritten on every boot so their timestamps are newer than
# whatever an earlier boot built: the first make below always has work to
# do, and the second never does, which is the whole of what make is FOR --
# and both halves are checked, since a make that rebuilds everything every
# time passes the first and would not be make.
P=/src/hello
gmkdir -p /tmp $P/parts
printf 'hello, ' > $P/parts/a.txt
printf 'style9\n' > $P/parts/b.txt
gcat > $P/Makefile <<'EOF'
# A Makefile on style9.  Recipes run under /bin/sh, which is dash here.
WORD := $(shell gcat parts/b.txt)

all: greeting.txt

greeting.txt: parts/a.txt parts/b.txt
	gcat parts/a.txt parts/b.txt > $@

# Three independent targets under two job slots: make starts two, and for
# the third it has to WAIT for a token to come back over its jobserver
# pipe -- in pselect, with SIGCHLD unblocked for the wait alone, which is
# the readiness call this rung exists for and the one wait a child's
# death has to end.
trio: left.txt middle.txt right.txt

left.txt: parts/a.txt
	gcat parts/a.txt > $@

middle.txt: parts/a.txt parts/b.txt
	gcat parts/a.txt parts/b.txt > $@

right.txt: parts/b.txt
	gcat parts/b.txt > $@

info:
	@echo "the shell function said: $(WORD)"

broken:
	@echo "about to exit 3 on purpose"
	@exit 3
EOF

echo "[makedemo] first build -- there is work to do:"
gmake -C $P
if [ "$(gcat $P/greeting.txt)" = "hello, style9" ]; then
	echo "[makedemo] PASS -- greeting.txt holds what the recipe made"
else
	echo "[makedemo] FAIL -- greeting.txt holds '$(gcat $P/greeting.txt)'"
fi

echo "[makedemo] second build -- nothing should happen:"
out=$(gmake -C $P 2>&1)
echo "$out"
case "$out" in
*"Nothing to be done"*)
	echo "[makedemo] PASS -- make compared the timestamps and did nothing" ;;
*)
	echo "[makedemo] FAIL -- make rebuilt an up-to-date target" ;;
esac

echo "[makedemo] the shell function, from a variable make expanded:"
gmake -C $P info

echo "[makedemo] three targets under two job slots, through a jobserver pipe:"
gmake -C $P -j2 --jobserver-style=pipe trio
if [ "$(gcat $P/left.txt)$(gcat $P/right.txt)" = "hello, style9" ] &&
    [ "$(gcat $P/middle.txt)" = "hello, style9" ]; then
	echo "[makedemo] PASS -- all three targets of the parallel build landed"
else
	echo "[makedemo] FAIL -- the parallel build lost a target"
fi

# The wording avoids the word the acceptance tally greps for.
echo "[makedemo] a recipe that exits 3 on purpose:"
gmake -C $P broken
rc=$?
if [ "$rc" = "2" ]; then
	echo "[makedemo] PASS -- make reported the error and exited 2"
else
	echo "[makedemo] FAIL -- make exited $rc for a recipe that exited 3"
fi
echo "[makedemo] done"
