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
# Usage: tools/mkapfs-image.sh [size-MiB] [output.gz]
set -eu

SIZE=${1:-512}		# under ~128 MiB mkapfs refuses: "small containers"
OUT=${2:-style9.apfs.gz}
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

# Contents the kernel-side tests key on.  Keep hello.txt tiny (one extent)
# and big.txt large enough to need several, so the reader is exercised on
# both shapes.
sudo mkdir -p "$MNT/bin" "$MNT/etc"
printf 'style9-os reads a real APFS volume.\n' | sudo tee "$MNT/etc/hello.txt" >/dev/null
sudo sh -c 'i=0; while [ $i -lt 4096 ]; do
	printf "%08d style9 apfs extent test line\n" $i; i=$((i+1)); done' \
	> /tmp/style9-big.txt
sudo cp /tmp/style9-big.txt "$MNT/bin/big.txt"
sync

echo "--- contents ---"
ls -lR "$MNT"
md5sum "$MNT/etc/hello.txt" "$MNT/bin/big.txt"

sudo umount "$MNT"
apfsck "$IMG"			# must be silent: our writes left it valid
gzip -9c "$IMG" > "$OUT"
rm -f "$IMG" /tmp/style9-big.txt
echo "wrote $OUT"
