#!/bin/sh
# sqldemo.sh -- sqlite3, a real Apple binary, keeping a database on the
# APFS volume.  One process builds and fills it; others open what the
# first left behind and read it back, so what is checked is what reached
# the volume, not what one process remembers.
D=/tmp/style9.db
gmkdir -p /tmp

echo "[sqldemo] sqlite3 -version:"
sqlite3 -version

echo "[sqldemo] building $D in one process:"
sqlite3 $D <<'EOF'
DROP TABLE IF EXISTS rung;
DROP TABLE IF EXISTS big;
CREATE TABLE rung (n INTEGER PRIMARY KEY, bin TEXT NOT NULL, links TEXT);
BEGIN;
INSERT INTO rung (bin, links) VALUES ('figlet', 'libSystem');
INSERT INTO rung (bin, links) VALUES ('gfactor', 'libSystem libgmp');
INSERT INTO rung (bin, links) VALUES ('dash', 'libSystem libedit');
INSERT INTO rung (bin, links) VALUES ('gmake', 'libSystem');
INSERT INTO rung (bin, links) VALUES ('sqlite3', 'libSystem libz readline');
COMMIT;
BEGIN;
INSERT INTO rung (bin, links) VALUES ('rolled back', 'nothing');
ROLLBACK;
CREATE TABLE big (x INTEGER, s TEXT);
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 5000)
INSERT INTO big SELECT x, printf('row-%05d', x) FROM c;
CREATE INDEX big_s ON big(s);
EOF

echo "[sqldemo] reading it back in a second process:"
got=$(sqlite3 $D "SELECT count(*), group_concat(bin, ',') FROM rung;")
if [ "$got" = "5|figlet,gfactor,dash,gmake,sqlite3" ]; then
	echo "[sqldemo] PASS -- five rows committed, the rolled-back one absent"
else
	echo "[sqldemo] FAIL -- rung reads back as '$got'"
fi

got=$(sqlite3 $D "SELECT count(*), sum(x), max(s) FROM big;")
if [ "$got" = "5000|12502500|row-05000" ]; then
	echo "[sqldemo] PASS -- 5000 rows over many pages, and their index"
else
	echo "[sqldemo] FAIL -- big reads back as '$got'"
fi

got=$(sqlite3 $D "SELECT x FROM big WHERE s = 'row-04321';")
if [ "$got" = "4321" ]; then
	echo "[sqldemo] PASS -- a lookup through the index"
else
	echo "[sqldemo] FAIL -- the index finds '$got'"
fi

got=$(sqlite3 $D "PRAGMA integrity_check;")
if [ "$got" = "ok" ]; then
	echo "[sqldemo] PASS -- integrity_check says ok"
else
	echo "[sqldemo] FAIL -- integrity_check says '$got'"
fi

echo "[sqldemo] SQL's math functions, over libm:"
got=$(sqlite3 :memory: "SELECT round(sqrt(2) * 1e6), pow(2, 10),
    round(exp(1) * 1e6), round(sin(pi() / 2) * 1e6),
    round(ln(1000) * 1e6), round(atan2(1, 1) * 4e6);")
if [ "$got" = "1414214.0|1024.0|2718282.0|1000000.0|6907755.0|3141593.0" ]; then
	echo "[sqldemo] PASS -- sqrt, pow, exp, sin, ln, atan2"
else
	echo "[sqldemo] FAIL -- the math reads '$got'"
fi
