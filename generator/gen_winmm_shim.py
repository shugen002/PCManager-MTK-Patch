#!/usr/bin/env python3
"""Generate the winmm pass-through shim.

HiConnectivityService.exe resolves winmm.dll through the normal search order, so
a winmm.dll dropped next to it wins over the system copy.  The replacement must
export exactly the same entries as the system winmm.dll, otherwise unrelated PC
Manager components that link against winmm fail to load.

MSVC's linker cannot emit forwarders to another DLL -- "Name=other.Name" in a
DEF is parsed as a local alias -- so real x64 thunks are generated instead:

  winmm.def         export list, names + exact ordinals (NONAME where unnamed)
  winmm_thunks.asm  one `jmp qword ptr [p_<name>]` stub per export
  winmm_ptrs.h      extern "C" UINT64 p_<name>;
  winmm_table.h     {name, ordinal, ptr} table consumed by src/proxy.cpp

The pointer table is filled at DLL load by loading the system winmm.dll and
calling GetProcAddress once per entry.

The export list is read from the system winmm.dll on the build machine, so the
DEF always matches the machine it is built on -- no snapshot to keep in sync:
the shim forwards to that machine's winmm and exports that machine's ABI.  Build
on (or CI against) the same Windows generation you intend to run on.

  build   gen_winmm_shim.py --out <dir> [--dll <winmm.dll>]
  check   gen_winmm_shim.py --check <built winmm.dll> [--dll <winmm.dll>]
"""
import argparse
import os
import struct
import sys


# --------------------------------------------------------------------------- PE
def _sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    sec = opt + optsz
    out = []
    for i in range(nsec):
        o = sec + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        out.append((vaddr, vsize, rawptr, rawsize))
    return ddir, out


def _rva_to_off(secs, rva):
    for vaddr, vsize, rawptr, rawsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None


def parse_exports(path):
    """Return (base, [(ordinal, name_or_empty), ...]) straight from the PE."""
    with open(path, "rb") as f:
        data = f.read()
    ddir, secs = _sections(data)
    export_rva, export_size = struct.unpack_from("<II", data, ddir)
    if not export_rva:
        raise SystemExit("no export directory in %s" % path)
    off = _rva_to_off(secs, export_rva)
    base = struct.unpack_from("<I", data, off + 16)[0]
    nfunc = struct.unpack_from("<I", data, off + 20)[0]
    nname = struct.unpack_from("<I", data, off + 24)[0]
    funcs = _rva_to_off(secs, struct.unpack_from("<I", data, off + 28)[0])
    names = _rva_to_off(secs, struct.unpack_from("<I", data, off + 32)[0])
    ords = _rva_to_off(secs, struct.unpack_from("<I", data, off + 36)[0])

    by_ordinal = {}
    for i in range(nname):
        rva = struct.unpack_from("<I", data, names + i * 4)[0]
        s = _rva_to_off(secs, rva)
        nm = data[s:data.index(b"\x00", s)].decode("latin1")
        # AddressOfNameOrdinals holds a 0-based index; the real ordinal adds Base.
        by_ordinal[base + struct.unpack_from("<H", data, ords + i * 2)[0]] = nm

    entries = []
    for i in range(nfunc):
        ordinal = base + i
        frva = struct.unpack_from("<I", data, funcs + i * 4)[0]
        if export_rva <= frva < export_rva + export_size:
            raise SystemExit("forwarder exports are not supported (ordinal %d)" % ordinal)
        entries.append((ordinal, by_ordinal.get(ordinal, "")))
    return base, entries


# --------------------------------------------------------------------- emitter
def emit(entries, out_dir):
    named = [(o, n) for o, n in entries if n]
    unnamed = [o for o, n in entries if not n]
    items = named + [(o, "winmm_ord_%d" % o) for o in unnamed]

    def path(name):
        return os.path.join(out_dir, name)

    with open(path("winmm.def"), "w", encoding="ascii", newline="\n") as f:
        f.write("LIBRARY winmm\nEXPORTS\n")
        for o, n in named:
            f.write("    %s @%d\n" % (n, o))
        for o in unnamed:
            f.write("    winmm_ord_%d @%d NONAME\n" % (o, o))

    with open(path("winmm_ptrs.h"), "w", encoding="ascii", newline="\n") as f:
        f.write("#pragma once\n#include <windows.h>\n")
        for _, n in items:
            f.write('extern "C" UINT64 p_%s;\n' % n)

    with open(path("winmm_table.h"), "w", encoding="ascii", newline="\n") as f:
        f.write("struct Ent { const char* name; unsigned ord; UINT64* p; };\n")
        f.write("static const Ent kEnts[] = {\n")
        for o, n in named:
            f.write('    { "%s", %d, &p_%s },\n' % (n, o, n))
        for o in unnamed:
            f.write("    { 0, %d, &p_winmm_ord_%d },\n" % (o, o))
        f.write("};\nstatic const int kEntCount = %d;\n" % len(items))

    with open(path("winmm_thunks.asm"), "w", encoding="ascii", newline="\n") as f:
        f.write("; auto-generated x64 pass-through thunks -- do not edit\n")
        f.write(".data\n")
        for _, n in items:
            f.write("PUBLIC p_%s\np_%s DQ 0\n" % (n, n))
        f.write("\n.code\n")
        for _, n in items:
            f.write("PUBLIC %s\n%s PROC\n    jmp QWORD PTR [p_%s]\n%s ENDP\n"
                    % (n, n, n, n))
        f.write("\nEND\n")
    return len(named), len(unnamed)


# ------------------------------------------------------------------------ main
def main():
    default_dll = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                               "System32", "winmm.dll")
    ap = argparse.ArgumentParser(description="generate the winmm pass-through shim")
    ap.add_argument("--dll", default=default_dll,
                    help="reference winmm.dll (default: %%SystemRoot%%\\System32\\winmm.dll)")
    ap.add_argument("--out", help="directory for the generated sources")
    ap.add_argument("--check", metavar="BUILT_DLL",
                    help="verify a built winmm.dll exports the same table as --dll")
    args = ap.parse_args()

    if args.check:
        base, actual = parse_exports(args.check)
        _, expected = parse_exports(args.dll)
        if actual == expected:
            print("winmm exports: %s matches %s (%d slots, base=%d)"
                  % (args.check, args.dll, len(actual), base))
            return 0
        print("winmm exports: MISMATCH in %s (reference %s)" % (args.check, args.dll),
              file=sys.stderr)
        for i in range(max(len(actual), len(expected))):
            a = actual[i] if i < len(actual) else None
            e = expected[i] if i < len(expected) else None
            if a != e:
                print("  slot %d: built=%r expected=%r" % (i, a, e), file=sys.stderr)
        return 1

    if not args.out:
        ap.error("--out is required when building")
    base, entries = parse_exports(args.dll)
    os.makedirs(args.out, exist_ok=True)
    n_named, n_unnamed = emit(entries, args.out)
    print("winmm shim: %s -> base=%d slots=%d named=%d unnamed=%d -> %s"
          % (args.dll, base, len(entries), n_named, n_unnamed, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
