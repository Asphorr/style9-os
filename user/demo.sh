#!/bin/sh
# demo.sh -- a real shell script, interpreted by a real Apple dash binary
# running on style9.  Registered in progreg and served through the
# synthetic /bin, so dash open(2)s it like any script file.
echo "[demo.sh] a shell script is running on style9"
gfactor 42
echo "[demo.sh] gfactor exit status: $?"
x=$(gfactor 8)
echo "[demo.sh] command substitution captured: $x"

# The working directory is the kernel's now, so `cd` moves and a relative
# path resolves against where we actually are.  Every line here would have
# printed the same thing when chdir(2) was a no-op that returned success --
# which is exactly why it is worth printing.
echo "[demo.sh] cwd starts at: $(pwd)"
cd /var/db
echo "[demo.sh] after cd /var/db: $(pwd)"
gls -l big.txt
# ".." inside an ARGUMENT: dash normalises its own `cd`, but it passes argv
# through untouched, so this one reaches the kernel with the dots still in it
# and only the kernel's resolver can make sense of it.
gls -l ../db/big.txt
cd ..
echo "[demo.sh] after cd ..: $(pwd)"
cd /
echo "[demo.sh] back at: $(pwd)"

# One open file behind several descriptors: 2>&1 makes a copy, `>&2` a copy
# of that, and the subshell is a child with copies of its own.  POSIX gives
# them all one offset, so each write lands after the one before it; with an
# offset per copy they would land on top of each other.
f=/etc/shared.txt
{ echo one; echo two >&2; (echo three); echo four; } > $f 2>&1
want='one
two
three
four'
if [ "$(gcat $f)" = "$want" ]; then
	echo "[demo.sh] PASS -- 2>&1, >&2 and a child share one offset"
else
	echo "[demo.sh] FAIL -- the shared file reads '$(gcat $f)'"
fi
echo "[demo.sh] done"
