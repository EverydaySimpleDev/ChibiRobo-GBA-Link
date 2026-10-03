#!/usr/bin/env python3
"""
Chibi-Robo! GBA Link patcher.

Adds the GBA link code (gc/build/chibi_link.bin) to Chibi-Robo! Plug Into
Adventure (GGTE01, USA) as a new DOL text section, then:
  * redirects main()'s per-frame `bl VIWaitForRetrace` (0x800156C8) to
    cl_frame_hook,
  * redirects the game's `bl PADRead` (0x801CC118) to cl_pad_read, so
    popups can be closed with a GameCube button,
  * raises OSInit's __ArenaLo constant so the game heap starts after our
    section.

Usage:
  py chibi_link_patch.py dol <main.dol> <out.dol>
  py chibi_link_patch.py iso <Chibi-Robo.iso> <out.iso>

The ISO mode never touches the input file: it copies it, writes the patched
DOL in place and moves the FST (which directly follows main.dol on this disc)
to just after the larger DOL.
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BIN = os.path.join(HERE, "..", "gc", "build", "chibi_link.bin")
DEFAULT_SYM = os.path.join(HERE, "..", "gc", "build", "chibi_link.sym")

GAME_ID = b"GGTE01"
SECTION_ADDR = 0x80672300          # retail __ArenaLo, see gc/link.ld
HOOK_SITE = 0x800156C8             # bl VIWaitForRetrace in main()
HOOK_ORIG = 0x4815BF69
PAD_SITE = 0x801CC118              # the game's only bl PADRead
PAD_ORIG = 0x4BFA7881
MAILBOX_ADDR = 0x80672300          # cl_mailbox, see gc/source/messages.h
ARENA_SITES = [
    # (lis addr, addi addr, original lis, original addi)
    (0x801615F4, 0x801615F8, 0x3C608067, 0x38632300),  # ArenaLo = __ArenaLo
    (0x8016162C, 0x80161630, 0x3C608067, 0x386302F0),  # debugger path: ArenaLo = stack end
]


def align(v, a):
    return (v + a - 1) & ~(a - 1)


class Dol:
    def __init__(self, data):
        self.data = bytearray(data)
        h = self.data
        self.text_off = list(struct.unpack_from(">7I", h, 0x00))
        self.data_off = list(struct.unpack_from(">11I", h, 0x1C))
        self.text_addr = list(struct.unpack_from(">7I", h, 0x48))
        self.data_addr = list(struct.unpack_from(">11I", h, 0x64))
        self.text_size = list(struct.unpack_from(">7I", h, 0x90))
        self.data_size = list(struct.unpack_from(">11I", h, 0xAC))
        self.bss_addr, self.bss_size, self.entry = struct.unpack_from(">3I", h, 0xD8)

    def sections(self):
        for o, a, s in zip(self.text_off + self.data_off,
                           self.text_addr + self.data_addr,
                           self.text_size + self.data_size):
            if s:
                yield o, a, s

    def file_end(self):
        return max(o + s for o, _, s in self.sections())

    def addr_to_off(self, addr):
        for o, a, s in self.sections():
            if a <= addr < a + s:
                return o + addr - a
        raise ValueError("address %08X is not in any DOL section" % addr)

    def read32(self, addr):
        return struct.unpack_from(">I", self.data, self.addr_to_off(addr))[0]

    def write32(self, addr, value):
        struct.pack_into(">I", self.data, self.addr_to_off(addr), value)

    def add_text_section(self, addr, blob):
        slot = next((i for i in range(7) if self.text_size[i] == 0), None)
        if slot is None:
            raise RuntimeError("no free text section slot in the DOL")
        for _, a, s in self.sections():
            if a < addr + len(blob) and addr < a + s:
                raise RuntimeError("new section overlaps an existing one at %08X" % a)
        if self.bss_addr < addr + len(blob) and addr < self.bss_addr + self.bss_size:
            raise RuntimeError("new section overlaps .bss")
        off = align(len(self.data), 0x20)
        self.data += b"\0" * (off - len(self.data))
        self.data += blob
        self.data += b"\0" * (align(len(self.data), 0x20) - len(self.data))
        self.text_off[slot] = off
        self.text_addr[slot] = addr
        self.text_size[slot] = align(len(blob), 0x20)
        struct.pack_into(">7I", self.data, 0x00, *self.text_off)
        struct.pack_into(">7I", self.data, 0x48, *self.text_addr)
        struct.pack_into(">7I", self.data, 0x90, *self.text_size)
        return slot


def read_symbols(path):
    syms = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 3:
                syms[parts[2]] = int(parts[0], 16)
    return syms


def branch_link(site, target):
    rel = target - site
    if not -0x2000000 <= rel < 0x2000000:
        raise RuntimeError("branch target out of range")
    return 0x48000001 | (rel & 0x03FFFFFC)


def patch_dol_bytes(dol_bytes, blob, syms):
    dol = Dol(dol_bytes)

    hook = dol.read32(HOOK_SITE)
    if hook != HOOK_ORIG:
        raise RuntimeError("main.dol is already patched or is not GGTE01 rev 0 "
                           "(found %08X at %08X, expected %08X)" % (hook, HOOK_SITE, HOOK_ORIG))
    if dol.read32(PAD_SITE) != PAD_ORIG:
        raise RuntimeError("unexpected code at the PADRead call site %08X" % PAD_SITE)
    for lis, addi, olis, oaddi in ARENA_SITES:
        if dol.read32(lis) != olis or dol.read32(addi) != oaddi:
            raise RuntimeError("unexpected OSInit code at %08X" % lis)

    if syms["__cl_start"] != SECTION_ADDR:
        raise RuntimeError("chibi_link.bin was not linked at %08X" % SECTION_ADDR)
    if syms.get("cl_mailbox") != MAILBOX_ADDR:
        raise RuntimeError("cl_mailbox moved (expected %08X)" % MAILBOX_ADDR)
    end = align(syms["__cl_end"], 0x20)
    if SECTION_ADDR + len(blob) > end:
        raise RuntimeError("blob is larger than __cl_end says")

    slot = dol.add_text_section(SECTION_ADDR, blob)
    dol.write32(HOOK_SITE, branch_link(HOOK_SITE, syms["cl_frame_hook"]))
    dol.write32(PAD_SITE, branch_link(PAD_SITE, syms["cl_pad_read"]))

    hi, lo = (end + 0x8000) >> 16, end & 0xFFFF
    for lis, addi, _, _ in ARENA_SITES:
        dol.write32(lis, 0x3C600000 | hi)     # lis  r3, hi
        dol.write32(addi, 0x38630000 | lo)    # addi r3, r3, lo

    print("  section T%d: %08X-%08X (%d bytes)" % (slot, SECTION_ADDR, end, len(blob)))
    print("  hook:       %08X -> bl %08X (cl_frame_hook)" % (HOOK_SITE, syms["cl_frame_hook"]))
    print("  pad hook:   %08X -> bl %08X (cl_pad_read)" % (PAD_SITE, syms["cl_pad_read"]))
    print("  mailbox:    %08X" % MAILBOX_ADDR)
    print("  ArenaLo:    80672300 -> %08X" % end)
    return bytes(dol.data)


def load_build(bin_path, sym_path):
    for p in (bin_path, sym_path):
        if not os.path.exists(p):
            sys.exit("missing %s - build gc/ first (see README)" % p)
    with open(bin_path, "rb") as f:
        blob = f.read()
    return blob, read_symbols(sym_path)


def cmd_dol(args):
    blob, syms = load_build(args.bin, args.sym)
    with open(args.input, "rb") as f:
        data = f.read()
    out = patch_dol_bytes(data, blob, syms)
    with open(args.output, "wb") as f:
        f.write(out)
    print("wrote", args.output)


def cmd_iso(args):
    blob, syms = load_build(args.bin, args.sym)
    if os.path.abspath(args.input) == os.path.abspath(args.output):
        sys.exit("output must be a different file than the input ISO")

    with open(args.input, "rb") as f:
        header = f.read(0x440)
        if header[:6] != GAME_ID:
            sys.exit("not a %s ISO (game id %r)" % (GAME_ID.decode(), header[:6]))
        dol_off, fst_off, fst_size, fst_max = struct.unpack_from(">4I", header, 0x420)
        f.seek(dol_off)
        dol_hdr = Dol(f.read(0x100))
        f.seek(dol_off)
        dol_data = f.read(dol_hdr.file_end())
        f.seek(fst_off)
        fst = f.read(fst_size)

    new_dol = patch_dol_bytes(dol_data, blob, syms)
    layout = plan_iso_layout(dol_off, len(dol_data), fst_off, fst_size, fst, len(new_dol),
                             os.path.getsize(args.input))

    print("copying ISO -> %s" % args.output)
    shutil.copyfile(args.input, args.output)
    with open(args.output, "r+b") as f:
        write_iso_layout(f, layout, dol_off, fst_off, fst_size, new_dol, fst)
    print("wrote", args.output)


def plan_iso_layout(dol_off, dol_size, fst_off, fst_size, fst, new_dol_size, disc_size):
    """
    Decide where the (larger) patched DOL goes.

    "inplace": the FST directly follows main.dol and there's room to grow - write the DOL where
               it was and move the FST up behind it (vanilla ISOs).
    "move":    no room (e.g. after the randomizer, unplug writes qp.bin right after the FST) -
               write the DOL into the first free gap on the disc and point the header's DOL
               offset (0x420) at it. The FST and every file stay where they are.
    """
    count = struct.unpack_from(">I", fst, 8)[0]
    files = sorted((struct.unpack_from(">I", fst, i * 12 + 4)[0], struct.unpack_from(">I", fst, i * 12 + 8)[0])
                   for i in range(1, count) if fst[i * 12] == 0)
    used = sorted([(0, dol_off), (dol_off, dol_size), (fst_off, fst_size)] + files)

    first_after_fst = min((o for o, _ in files if o >= fst_off + fst_size), default=disc_size)
    new_fst_off = align(dol_off + new_dol_size, 0x100)
    if fst_off >= dol_off + dol_size and new_fst_off + fst_size <= first_after_fst:
        return ("inplace", new_fst_off)

    end = 0
    for start, size in used + [(disc_size, 0)]:
        gap = align(end, 0x8000)
        if gap + new_dol_size <= start:
            return ("move", gap)
        end = max(end, start + size)
    sys.exit("no free space on this disc for the patched main.dol")


def write_iso_layout(f, layout, dol_off, fst_off, fst_size, new_dol, fst):
    mode, where = layout
    if mode == "inplace":
        # clear the old DOL + FST area, then write the new DOL and FST
        f.seek(dol_off)
        f.write(b"\0" * (where + fst_size - dol_off))
        f.seek(dol_off)
        f.write(new_dol)
        f.seek(where)
        f.write(fst)
        f.seek(0x424)
        f.write(struct.pack(">I", where))
        print("  FST moved:  %08X -> %08X" % (fst_off, where))
    else:
        f.seek(where)
        f.write(new_dol)
        f.seek(0x420)
        f.write(struct.pack(">I", where))
        print("  DOL moved:  %08X -> %08X (no room to grow in place)" % (dol_off, where))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=DEFAULT_BIN, help="injected code blob (default: gc/build/chibi_link.bin)")
    ap.add_argument("--sym", default=DEFAULT_SYM, help="nm listing of the blob (default: gc/build/chibi_link.sym)")
    sub = ap.add_subparsers(dest="mode", required=True)
    for name, fn in (("dol", cmd_dol), ("iso", cmd_iso)):
        p = sub.add_parser(name)
        p.add_argument("input")
        p.add_argument("output")
        p.set_defaults(func=fn)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
