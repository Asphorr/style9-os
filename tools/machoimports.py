#!/usr/bin/env python3
"""What a Mach-O asks the outside world for -- and what we do not yet answer.

Before a real Apple binary can run here, exactly one question matters: which
of the symbols it imports does our clean-room libSystem not export?  This
answers it from the two FILES rather than from reading source, by walking the
LC_DYLD_CHAINED_FIXUPS import table (where a modern Apple-toolchain binary
keeps its imports) and the export TRIE of the dylibs meant to satisfy them.

It is how the terminal rung was scoped: coreutils' stty turned out to be ten
symbols away, of which five were the terminal and five were printf internals,
and it needed no dylib that was not already here.  Guessing would have cost a
build and a boot per guess.

Usage: machoimports.py FILE [FILE...]        -- dylibs + imports, one per file
       machoimports.py -d KNOWN NEW          -- what NEW imports that KNOWN does not
       machoimports.py -g DYLIB[,DYLIB] BIN... -- what BIN wants that no DYLIB exports

Example:
       tools/machoimports.py -g obj/libSystem.B.dylib,obj/libedit.3.dylib \\
           extern/dash.macho
"""
import struct
import sys

LC_SYMTAB = 0x02
LC_DYSYMTAB = 0x0B
LC_LOAD_DYLIB = 0x0C
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_MAIN = 0x80000028
LC_DYLD_CHAINED_FIXUPS = 0x80000034
LC_DYLD_INFO_ONLY = 0x80000022
LC_LOAD_DYLINKER = 0x0E

FAT_MAGIC = 0xCAFEBABE
MH_MAGIC_64 = 0xFEEDFACF


def slice64(b):
    """Return the x86-64 slice of a possibly-fat file."""
    magic, = struct.unpack_from(">I", b, 0)
    if magic in (FAT_MAGIC, 0xCAFEBABF):
        n, = struct.unpack_from(">I", b, 4)
        for i in range(n):
            cpu, _sub, off, size, _al = struct.unpack_from(">5I", b, 8 + i * 20)
            if cpu == 0x01000007:            # CPU_TYPE_X86_64
                return b[off:off + size]
        raise SystemExit("no x86-64 slice")
    return b


def cstr(b, off):
    end = b.index(b"\0", off)
    return b[off:end].decode("utf-8", "replace")


def read(path):
    b = slice64(open(path, "rb").read())
    magic, cpu, _sub, ftype, ncmds, _sz, _fl, _r = struct.unpack_from("<8I", b, 0)
    if magic != MH_MAGIC_64:
        raise SystemExit("%s: not a 64-bit Mach-O (magic %#x)" % (path, magic))

    dylibs, imports, linker = [], [], None
    off, kind = 32, "none"
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<2I", b, off)
        if cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB):
            noff, = struct.unpack_from("<I", b, off + 8)
            dylibs.append(cstr(b, off + noff))
        elif cmd == LC_LOAD_DYLINKER:
            noff, = struct.unpack_from("<I", b, off + 8)
            linker = cstr(b, off + noff)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            doff, dsize = struct.unpack_from("<2I", b, off + 8)
            imports += chained(b[doff:doff + dsize])
            kind = "chained fixups"
        elif cmd == LC_SYMTAB and not imports:
            symoff, nsyms, stroff, _strsz = struct.unpack_from("<4I", b, off + 8)
            for i in range(nsyms):
                soff, stype, _sect, _desc, _val = struct.unpack_from(
                    "<IBBHQ", b, symoff + i * 16)
                if (stype & 0x0E) == 0x00:          # N_UNDF
                    imports.append((cstr(b, stroff + soff), 0))
            if imports:
                kind = "symtab undefineds"
        off += size
    return dylibs, sorted(set(imports)), kind, linker


def chained(d):
    """dyld_chained_fixups_header -> the import table."""
    (_ver, _starts, imports_off, symbols_off, imports_count,
     imports_format, _symbols_format) = struct.unpack_from("<7I", d, 0)
    out = []
    for i in range(imports_count):
        if imports_format == 1:                     # DYLD_CHAINED_IMPORT
            v, = struct.unpack_from("<I", d, imports_off + i * 4)
            lib, weak, noff = v & 0xFF, (v >> 8) & 1, v >> 9
        elif imports_format == 2:                   # ..._ADDEND
            v, _a = struct.unpack_from("<Iq", d, imports_off + i * 12)
            lib, weak, noff = v & 0xFF, (v >> 8) & 1, v >> 9
        else:                                       # ..._ADDEND64
            v, = struct.unpack_from("<Q", d, imports_off + i * 16)
            lib, weak, noff = v & 0xFFFF, (v >> 16) & 1, v >> 32
        out.append((cstr(d, symbols_off + noff), lib | (weak << 8)))
    return out


LC_DYLD_EXPORTS_TRIE = 0x80000033


def uleb(b, off):
    v, shift = 0, 0
    while True:
        c = b[off]
        off += 1
        v |= (c & 0x7F) << shift
        if not (c & 0x80):
            return v, off
        shift += 7


def exports(path):
    """Every symbol a dylib offers, walked out of its export trie."""
    b = slice64(open(path, "rb").read())
    ncmds, = struct.unpack_from("<I", b, 16)
    off, trie = 32, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<2I", b, off)
        if cmd == LC_DYLD_EXPORTS_TRIE:
            toff, tsize = struct.unpack_from("<2I", b, off + 8)
            trie = b[toff:toff + tsize]
        elif cmd == LC_DYLD_INFO_ONLY and trie is None:
            _rb, _rs, _bb, _bs, _wb, _ws, _lb, _ls, eoff, esize = \
                struct.unpack_from("<10I", b, off + 8)
            trie = b[eoff:eoff + esize]
        off += size
    if trie is None:
        raise SystemExit("%s: no export trie" % path)

    out, stack = [], [(0, "")]
    while stack:
        node, prefix = stack.pop()
        tsize, p = uleb(trie, node)
        if tsize:
            out.append(prefix)
        p += tsize
        nkids = trie[p]
        p += 1
        for _ in range(nkids):
            end = trie.index(b"\0", p)
            edge = trie[p:end].decode()
            child, p = uleb(trie, end + 1)
            stack.append((child, prefix + edge))
    return set(out)


def gap(dylibs_arg, *bins):
    """What these binaries want that the named dylibs do not offer.

    Several dylibs, comma-separated: a binary is bound against its whole
    closure, so measuring it against one of them reports symbols as missing
    that another already provides.
    """
    libs = dylibs_arg.split(",")
    have = set()
    for lib in libs:
        have |= exports(lib)
    shown = ",".join(l.rsplit("/", 1)[-1] for l in libs)
    for path in bins:
        dylibs, imports, _k, _l = read(path)
        missing = [n for n, _f in imports if n not in have]
        print("== %s: %d of %d imports are NOT in %s" %
              (path.rsplit("/", 1)[-1], len(missing), len(imports), shown))
        for d in dylibs:
            print("   needs dylib: %s" % d)
        for n in missing:
            print("   MISSING %s" % n)
        print()


def show(path):
    dylibs, imports, kind, linker = read(path)
    print("== %s" % path)
    print("   linker : %s" % (linker or "(static)"))
    for i, d in enumerate(dylibs, 1):
        print("   dylib %d: %s" % (i, d))
    print("   %d imports (%s)" % (len(imports), kind))
    for name, flags in imports:
        print("      %-40s lib#%d%s" % (name, flags & 0xFF,
                                        "  WEAK" if flags & 0x100 else ""))


def diff(known, new):
    kn = {n for n, _ in read(known)[1]}
    dylibs, imports, _kind, _l = read(new)
    extra = [n for n, _f in imports if n not in kn]
    print("== %s wants %d symbol(s) that %s does not" %
          (new, len(extra), known))
    for d in dylibs:
        print("   dylib: %s" % d)
    for n in extra:
        print("   %s" % n)


if __name__ == "__main__":
    if sys.argv[1] == "-d":
        diff(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "-g":
        gap(sys.argv[2], *sys.argv[3:])
    else:
        for p in sys.argv[1:]:
            show(p)
