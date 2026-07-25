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
echo "[demo.sh] done"
