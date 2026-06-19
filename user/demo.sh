#!/bin/sh
# demo.sh -- a real shell script, interpreted by a real Apple dash binary
# running on style9.  Registered in progreg and served through the
# synthetic /bin, so dash open(2)s it like any script file.
echo "[demo.sh] a shell script is running on style9"
gfactor 42
echo "[demo.sh] gfactor exit status: $?"
x=$(gfactor 8)
echo "[demo.sh] command substitution captured: $x"
echo "[demo.sh] done"
