#!/usr/bin/env python3
"""
Appends the GBA popup subroutines to Chibi-Robo stage scripts (.us).

    py add_gba_subs.py <stage.us> [<stage.us> ...]

Usage from inside that stage's script:

    pushbp
    setsp   3.d
    run     *sub_gba_msg        ; show message #3 on the GBA and carry on
    popbp

    pushbp
    setsp   3.d
    run     *sub_gba_msg_wait   ; show #3, pause this script until the player closes it
    popbp                       ; (no wait when no GBA is linked; gives up after 60 s)

Messages live in chibi-mini-gba gc/source/messages.c. Vars used:
    var(1880) trigger (game script -> GC), var(1881) closed id (GC -> script),
    var(1882) 1 while a GBA is linked (GC -> script), var(1883) wait timer.

Files are edited as raw bytes (they contain Shift-JIS text) and keep their
line endings. Files that already have the subs are skipped.
"""
import sys

SUBS = [
    "",
    "",
    "; RANDOMIZER: GBA link cable popups (chibi-mini-gba gc/source/messages.c)",
    ";   pushbp / setsp <id>.d / run *sub_gba_msg / popbp       show message #id on the GBA",
    ";   pushbp / setsp <id>.d / run *sub_gba_msg_wait / popbp  same, then wait until it is closed",
    ";   (the wait is skipped when no GBA is linked and gives up after 60 s)",
    "sub_gba_msg:",
    "\tset\tvar(1880.d), sp(0.b)",
    "\treturn",
    "",
    "sub_gba_msg_wait:",
    "\tset\tvar(1881.d), 0.w",
    "\tset\tvar(1883.d), 0.w",
    "\tset\tvar(1880.d), sp(0.b)",
    "loc_gba_msg_check:",
    "\tif\teq(var(1882.d), 1.w), else *loc_gba_msg_done ; no GBA linked: nothing to wait for",
    "\tif\teq(var(1881.d), sp(0.b)), else *loc_gba_msg_sleep",
    "\tgoto\t*loc_gba_msg_done ; closed by the player",
    "loc_gba_msg_sleep:",
    "\tset\tadda(var(1883.d), 1.w)",
    "\tif\tlt(var(1883.d), 3600.w), else *loc_gba_msg_done ; 60 s safety timeout",
    "\twait\t@time, 1.w",
    "\tgoto\t*loc_gba_msg_check",
    "loc_gba_msg_done:",
    "\treturn",
    "",
]


def add_subs(path):
    data = open(path, "rb").read()
    if b"sub_gba_msg:" in data:
        print("skip (already has the subs):", path)
        return
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    data = data.rstrip(b"\r\n") + nl + nl.join(line.encode("ascii") for line in SUBS)
    open(path, "wb").write(data)
    print("added:", path)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        add_subs(p)
