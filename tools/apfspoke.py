#!/usr/bin/env python3
"""Read and deliberately damage one field of a style9 APFS image, so that
apfsck can be asked what it thinks of the result.

    python3 tools/apfspoke.py IMAGE list                 -- names, entry vs record
    python3 tools/apfspoke.py IMAGE name   NAME LETTER   -- the inode's NAME field
    python3 tools/apfspoke.py IMAGE parent NAME OID      -- the inode's ai_parent_id
    python3 tools/apfspoke.py IMAGE footer [KEY VAL]     -- the root's btree_info

This is a MEASURING INSTRUMENT, not a repair tool.  A writer that forgets an
obligation leaves the volume disagreeing with itself in some particular way,
and the cheapest way to learn what the checker calls that disagreement is to
produce it directly -- one field, on a copy of the container, before the writer
that would produce it honestly has been written at all.  Everything the rename
ladder in fs/apfs/apfs.c states was measured with this.

Only equal-length edits are made, so no record moves and no length, free-space
total or key count changes: the block's Fletcher-64 is the whole of what has to
be recomputed.  Work on a COPY.
"""
import struct
import sys

BS = 4096
ROOT, LEAF, FIXED = 1, 2, 4
T_INODE, T_DREC = 3, 9
XF_NAME = 4
INFO = 40			# btree_info, at the end of a root node


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


class Img:
    def __init__(self, path):
        self.f = open(path, "r+b")

    def block(self, n):
        self.f.seek(n * BS)
        return bytearray(self.f.read(BS))

    def write(self, n, b):
        assert len(b) == BS
        self.f.seek(n * BS)
        self.f.write(b)


def fletcher(b):
    """Fletcher-64 mod 2^32-1 over block+8; the answer goes in the first 8."""
    lo = hi = 0
    for off in range(8, BS, 4):
        lo = (lo + u32(b, off)) % 0xFFFFFFFF
        hi = (hi + lo) % 0xFFFFFFFF
    c1 = (0xFFFFFFFF - ((lo + hi) % 0xFFFFFFFF)) % 0xFFFFFFFF
    c2 = (0xFFFFFFFF - ((lo + c1) % 0xFFFFFFFF)) % 0xFFFFFFFF
    return c1 | (c2 << 32)


def reseal(img, bno, b):
    struct.pack_into("<Q", b, 0, fletcher(b))
    img.write(bno, b)


def live_nxsb(img):
    """The newest valid container superblock in the descriptor ring."""
    anchor = img.block(0)
    assert anchor[32:36] == b"NXSB", "block 0 is not a container superblock"
    base = u64(anchor, 112)
    blocks = u32(anchor, 104) & 0x7FFFFFFF
    best, best_xid = None, -1
    for i in range(blocks):
        b = img.block(base + i)
        if b[32:36] != b"NXSB" or u64(b, 0) != fletcher(b):
            continue
        xid = u64(b, 16)
        if xid > best_xid:
            best, best_xid = b, xid
    assert best is not None, "no valid superblock in the descriptor ring"
    return best


def node_records(b):
    """Every (key, value, the value's absolute offset) in one node."""
    flags, nkeys = u16(b, 32), u32(b, 36)
    toff, tlen = u16(b, 40), u16(b, 42)
    toc = 56 + toff
    keys = toc + tlen
    vals = BS - (INFO if flags & ROOT else 0)
    out = []
    for i in range(nkeys):
        if flags & FIXED:
            ko, vo = u16(b, toc + i * 4), u16(b, toc + i * 4 + 2)
            k, v, va = b[keys + ko:], b[vals - vo:], vals - vo
        else:
            ko, kl = u16(b, toc + i * 8), u16(b, toc + i * 8 + 2)
            vo, vl = u16(b, toc + i * 8 + 4), u16(b, toc + i * 8 + 6)
            k = b[keys + ko:keys + ko + kl]
            v, va = b[vals - vo:vals - vo + vl], vals - vo
        out.append((bytes(k), bytes(v), va))
    return flags, out


def omap_walk(img, bno, out):
    flags, recs = node_records(img.block(bno))
    for k, v, _ in recs:
        if flags & LEAF:
            out[u64(k, 0)] = u64(v[8:16], 0)
        else:
            omap_walk(img, u64(v, 0), out)


def fs_walk(img, bno, omap, out, depth=0):
    assert depth < 16, "the tree is deeper than any tree here should be"
    flags, recs = node_records(img.block(bno))
    for k, v, va in recs:
        if flags & LEAF:
            out.append((k, v, va, bno))
        else:
            fs_walk(img, omap[u64(v, 0)], omap, out, depth + 1)


def volume(img):
    """The volume superblock and its object map, from the live checkpoint."""
    sb = live_nxsb(img)
    comap = {}
    omap_walk(img, u64(img.block(u64(sb, 160)), 48), comap)
    vol = img.block(comap[u64(sb, 184)])
    assert vol[32:36] == b"APSB", "that is not a volume superblock"
    vomap = {}
    omap_walk(img, u64(img.block(u64(vol, 128)), 48), vomap)
    return vol, vomap


def fs_root(img):
    vol, vomap = volume(img)
    return vomap[u64(vol, 136)]


def tree(img):
    vol, vomap = volume(img)
    recs = []
    fs_walk(img, vomap[u64(vol, 136)], vomap, recs)
    return recs


def inode_name(v):
    """Where the NAME extended field's datum sits inside an inode value."""
    nexts = u16(v, 92)
    xf = 96
    data = xf + nexts * 4
    for i in range(nexts):
        typ, size = v[xf + i * 4], u16(v, xf + i * 4 + 2)
        if typ == XF_NAME:
            return data, size
        data += (size + 7) & ~7
    return None, 0


def by_name(recs, want):
    """Every entry with this name, and every inode record, by object id."""
    ents, inodes = [], {}
    for k, v, va, bno in recs:
        typ = u64(k, 0) >> 60
        if typ == T_INODE:
            inodes[u64(k, 0) & 0x0FFFFFFFFFFFFFFF] = (v, va, bno)
        elif typ == T_DREC:
            nm = k[12:].split(b"\0")[0].decode("utf-8", "replace")
            if want is None or nm == want:
                ents.append((nm, u64(k, 0) & 0x0FFFFFFFFFFFFFFF,
                             u64(v, 0), bno))
    return ents, inodes


def show_footer(img, set_key=None, set_val=None):
    """The root's trailing btree_info: flags, node size, key size, value size,
    LONGEST KEY, LONGEST VALUE, key count, node count."""
    bno = fs_root(img)
    b = img.block(bno)
    at = BS - INFO
    print("root node %d: longest key %d, longest value %d, %d keys, %d nodes"
          % (bno, u32(b, at + 16), u32(b, at + 20), u64(b, at + 24),
             u64(b, at + 32)))
    if set_key is None and set_val is None:
        return
    if set_key is not None:
        struct.pack_into("<I", b, at + 16, set_key)
    if set_val is not None:
        struct.pack_into("<I", b, at + 20, set_val)
    print("          -> longest key %d, longest value %d"
          % (u32(b, at + 16), u32(b, at + 20)))
    reseal(img, bno, b)


def longest(img):
    """What the records actually are, which is what the footer should say."""
    k = v = 0
    for key, val, _va, _bno in tree(img):
        k = max(k, len(key))
        v = max(v, len(val))
    print("records:   longest key %d, longest value %d" % (k, v))


def main():
    img = Img(sys.argv[1])
    what = sys.argv[2]

    if what == "footer":
        if len(sys.argv) > 4:
            show_footer(img, int(sys.argv[3]), int(sys.argv[4]))
        else:
            show_footer(img)
        longest(img)
        return

    recs = tree(img)
    if what == "list":
        ents, inodes = by_name(recs, None)
        for nm, parent, child, _bno in sorted(ents, key=lambda e: e[1]):
            rec = inodes.get(child)
            if rec is None:
                print("%-20s parent %-4d -> inode %-4d  (NO RECORD)" %
                      (nm, parent, child))
                continue
            off, size = inode_name(rec[0])
            said = rec[0][off:off + size].split(b"\0")[0].decode() if off \
                else "?"
            print("%-20s parent %-4d -> inode %-4d  record says parent %d, "
                  'name "%s", node %d' %
                  (nm, parent, child, u64(rec[0], 0), said, rec[2]))
        return

    name = sys.argv[3]
    ents, inodes = by_name(recs, name)
    assert len(ents) == 1, "%d entries called %s" % (len(ents), name)
    child = ents[0][2]
    v, va, bno = inodes[child]
    b = img.block(bno)

    if what == "name":
        off, size = inode_name(v)
        assert off, "that inode has no name field"
        at = va + off
        was = bytes(b[at:at + size]).split(b"\0")[0].decode()
        b[at] = ord(sys.argv[4][0])
        now = bytes(b[at:at + size]).split(b"\0")[0].decode()
        print('inode %d in node %d: name "%s" -> "%s" (the entry still says '
              '"%s")' % (child, bno, was, now, name))
    elif what == "parent":
        at = va			# ai_parent_id is the first thing in the value
        was = u64(b, at)
        struct.pack_into("<Q", b, at, int(sys.argv[4]))
        print("inode %d in node %d: parent %d -> %s (the entry is still in "
              "%d)" % (child, bno, was, sys.argv[4], was))
    else:
        raise SystemExit("no such poke: %s" % what)
    reseal(img, bno, b)


if __name__ == "__main__":
    main()
