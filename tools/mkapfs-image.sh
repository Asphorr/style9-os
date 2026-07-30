#!/bin/sh
#
# Build the APFS test container that fixtures/style9.apfs.gz holds.
#
# This cannot run as part of `make`, and not on this project's usual WSL box
# either: writing APFS needs the out-of-tree linux-apfs-rw kernel module, and
# a DKMS module needs headers for the running kernel, which WSL2 does not
# ship.  So this is run by hand on a real Linux host (a VM is fine) whenever
# the test image's contents need to change, and its gzipped output is checked
# in -- the build itself only gunzips.
#
# Prerequisites on that host:
#	sudo apt-get install apfsprogs apfs-dkms
# or, for a module and formatter from the same upstream revision:
#	git clone https://github.com/eafer/linux-apfs-rw.git && make -C linux-apfs-rw
#	git clone https://github.com/eafer/apfsprogs.git && make -C apfsprogs/mkapfs
#
# THE ONE NON-OBVIOUS THING: the module mounts READ-ONLY by default, on
# purpose -- its write support is experimental and can corrupt a container.
# `-o rw` does NOT override that and gives no diagnostic; the option that
# enables writing is spelled `readwrite`.  Without it every mkdir below fails
# with EROFS and the image comes out empty.
#
# The figlet fonts are copied in from a directory given as $FONTS (default
# /tmp/fonts); they are the same bytes the FAT test image carries, so the two
# volumes differ only in what they can express -- see below.
#
# Usage: tools/mkapfs-image.sh [size-MiB] [output.gz]
set -eu

SIZE=${1:-512}		# under ~128 MiB mkapfs refuses: "small containers"
OUT=${2:-style9.apfs.gz}
FONTS=${FONTS:-/tmp/fonts}
IMG=$(mktemp -u /tmp/style9-XXXXXX.apfs)
MNT=$(mktemp -d)

trap 'sudo umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT

truncate -s "${SIZE}M" "$IMG"
mkapfs -L style9 "$IMG"

sudo mount -o loop,readwrite -t apfs "$IMG" "$MNT"
case "$(mount | grep -F "$MNT")" in
*"(ro,"*)	echo "mounted read-only -- is this module too old for 'readwrite'?" >&2
		exit 1 ;;
esac

# Contents the kernel-side tests key on.  Keep hello.txt tiny (one extent) and
# big.txt large enough to need several, so the reader is exercised on both
# shapes.  /bin/echo is a real ELF and lives here on purpose: /bin is where the
# Darwin layer overlays its built-in program registry, so a file there proves
# the two are merged rather than one hiding the other.
FIGDIR=usr/local/Cellar/figlet/2.2.5/share/figlet/fonts
sudo mkdir -p "$MNT/bin" "$MNT/etc" "$MNT/var/db" "$MNT/$FIGDIR"
printf 'style9-os reads a real APFS volume.\n' | sudo tee "$MNT/etc/hello.txt" >/dev/null
sudo sh -c 'i=0; while [ $i -lt 4096 ]; do
	printf "%08d style9 apfs extent test line\n" $i; i=$((i+1)); done' \
	> /tmp/style9-big.txt
sudo cp /tmp/style9-big.txt "$MNT/var/db/big.txt"
sudo cp /bin/echo "$MNT/bin/echo"

# figlet's fonts, at the path figlet actually opens.  On the FAT image these
# had to sit in the root and be found by a basename fallback, because 8.3
# names cannot spell /usr/local/Cellar/figlet/2.2.5/share/figlet/fonts -- on
# APFS the real path simply exists, and no fallback is involved.
sudo cp "$FONTS"/*.flf "$MNT/$FIGDIR/"
sync

echo "--- contents ---"
sudo ls -lR "$MNT"
md5sum "$MNT/etc/hello.txt" "$MNT/var/db/big.txt" "$MNT/bin/echo" \
    "$MNT/$FIGDIR/standard.flf"

sudo umount "$MNT"
apfsck "$IMG"			# must be silent: our writes left it valid
gzip -9c "$IMG" > "$OUT"
rm -f "$IMG" /tmp/style9-big.txt
echo "wrote $OUT"
