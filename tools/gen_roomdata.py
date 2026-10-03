#!/usr/bin/env python3
"""
Generates the ROOM page data from the game's stage data and the Archipelago
location table:

    py tools/gen_roomdata.py <stage json dir> <script dir> <locations.py>

  <stage json dir>  stageNN.json from `unplug stage export --iso <iso> stageNN -o stageNN.json`
  <script dir>      stageNN.us   from `unplug script disassemble-all --iso <iso> -o <dir>`
  <locations.py>    worlds/chibi_robo/locations.py of the apworld

Writes (relative to this repo):
  gba/source/roomdata.c      room bounds, loading zones (+ where they warp) and
                             outlets per room
  shared/chibi_link_pins.h   one item pin per Archipelago location (position +
                             pickup bit) and the Chibi-Doors (position + the
                             script flag set when opened), compiled into both the
                             GBA and GC code

World units = stage JSON units = the player position at ChrObj+0x04/+0x0C.
"""
import ast
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_ROOMDATA = os.path.join(HERE, "..", "gba", "source", "roomdata.c")
OUT_PINS = os.path.join(HERE, "..", "shared", "chibi_link_pins.h")

ROOMS = [1, 2, 3, 4, 5, 6, 7, 9, 11, 14, 16, 18, 22]
MARGIN = 25

# Rooms whose stage data has too few placed objects for good bounds. Mother Spider's arena is
# mostly spawned by script (stage22.us places the player at (-44, 91) and (0, 50) - script
# coordinates are 100x the JSON's). The GBA also grows a room's bounds live when the player
# walks outside them, so these only need to be a sensible start.
# (min_x, max_x, min_z, max_z) in world units, per stage id.
BOUNDS_OVERRIDE = {
    5: (-30, 30, -20, 40),       # Chibi-House: floor is x -16..16, z -8..29; the stage also parks
                                 # objects at z 400-667 (house parts, Tonpy letters) that aren't walkable
    22: (-160, 160, -110, 210),  # Mother Spider's Room
}

# Locations whose object the script positions at runtime (like killing the spider to spawn them), 
# so the stage data only has a placeholder position: pin them at the arena centre instead.
PIN_POS_OVERRIDE = {
    "Mother Spider - Frog Ring": (-12, 50),
    "Mother Spider - Left Leg":  (12, 50),
}

# locations.py uses the AP client's room numbering (client.py stage_hex_to_id)
AP_TO_STAGE = {1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 9, 10: 11, 11: 14, 12: 16, 13: 18, 14: 22}

PIN_ITEM, PIN_COIN = 0, 1       # pin kinds (CL_PIN_* in chibi_link_pins.h)

MARKERS = {                      # object name -> marker kind (MK_* in roomdata.h)
    "socket": 1,                  # outlet
}

# Chibi-Doors are the house_r_chibi_h_door_1 objects (the _door_2 objects only mark the
# radar-hidden ones; flags 1030-1048 track revealing those). Each room's script has a
# dispatcher over var(881) - one `case` per door - that sets the door's "opened" flag and
# then `attach <object id>`es it (e.g. Living Room: flag 703 -> object 94, "left of the
# Foyer door"; Kitchen: flag 718 -> "in front of the Living Room door").
CHIBI_DOOR = "house_r_chibi_h_door_1"


def chibi_door_flags(script):
    """object id -> the script flag its dispatcher case sets when the door is opened."""
    out = {}
    blocks = re.split(r"^\s*case\s+eq\(var\(881\.d\),\s*\d+\.d\)", script, flags=re.M)
    for blk in blocks[1:]:
        blk = re.split(r"^\s*break\s", blk, maxsplit=1, flags=re.M)[0]
        flags = [int(f) for f in re.findall(r"^\s*set\s+flag\((\d+)\.d\),\s*1\.w", blk, re.M) if int(f) != 1013]
        att = re.search(r"^\s*attach\s+(\d+)\.d", blk, re.M)
        if flags and att:
            out[int(att.group(1))] = flags[0]
    return out


# Objects the game parks out of sight sit in this corner of every stage; ignore them.
def parked(x, z):
    return (x < -660 and z > 460) or (abs(x) < 1 and abs(z) < 1)


def bounds(objs):
    pts = [(o["position"]["x"], o["position"]["z"]) for o in objs]
    pts = [(x, z) for x, z in pts if not parked(x, z) and abs(x) < 1500 and abs(z) < 1500]
    if len(pts) < 4:
        return None
    xs = sorted(p[0] for p in pts)
    zs = sorted(p[1] for p in pts)
    q = lambda a, f: a[int(f * (len(a) - 1))]
    return (int(q(xs, .02)) - MARGIN, int(q(xs, .98)) + MARGIN,
            int(q(zs, .02)) - MARGIN, int(q(zs, .98)) + MARGIN)


def warp_targets(script):
    """object id -> stage id, from `.interact N.d, *label` + the first `warp S.d` in that event."""
    labels = {}
    for m in re.finditer(r"^\s*\.interact\s+(\d+)\.d,\s*\*(\w+)", script, re.M):
        labels[int(m.group(1))] = m.group(2)
    out = {}
    for obj, label in labels.items():
        # the event runs until the next top-level block (local loc_ labels are part of it)
        m = re.search(r"^%s:\s*$(.*?)(?=^\s*\.interact|^(?:sub|evt|lib)_\w+:|\Z)" % re.escape(label),
                      script, re.M | re.S)
        if not m:
            continue
        w = re.search(r"^\s*warp\s+(\d+)\.d", m.group(1), re.M)
        if w:
            out[obj] = int(w.group(1))
    return out


def read_coin_codes(src):
    """Location codes of the coin_locations option (COIN_LOCATION_CODES in locations.py)."""
    m = re.search(r"COIN_LOCATION_CODES[^=]*=\s*frozenset\(\[(.*?)\]\)", src, re.S)
    if not m:
        return set()
    return set(eval("[" + re.sub(r"#.*", "", m.group(1)) + "]", {"range": range}))


def read_locations(path):
    """name -> (ap stage, bit, address, object id, is coin) from LOCATION_TABLE in locations.py."""
    src = open(path, encoding="utf-8").read()
    coin_codes = read_coin_codes(src)
    out = {}
    for m in re.finditer(r'^\s*"([^"]+)":\s*ChibiRoboLocationData\((.*)\),?\s*$', src, re.M):
        args = [a.strip() for a in m.group(2).split(",")]
        try:
            vals = [ast.literal_eval(a) if a not in ("None",) else None for a in args[:6]]
        except (ValueError, SyntaxError):
            continue
        vals += [None] * (6 - len(vals))
        code, region, stage, bit, addr, obj = vals
        out[m.group(1)] = (stage, bit, addr, obj, code in coin_codes)
    return out


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main(json_dir, us_dir, locations_py):
    stages = {}
    for sid in ROOMS:
        jp = os.path.join(json_dir, "stage%02d.json" % sid)
        if os.path.exists(jp):
            stages[sid] = json.load(open(jp, encoding="utf-8"))["objects"]

    rooms, doors, markers, cdoors = [], [], [], []
    for sid in ROOMS:
        objs = stages.get(sid)
        b = BOUNDS_OVERRIDE.get(sid) or (objs and bounds(objs))
        if not b:
            continue
        up = os.path.join(us_dir, "stage%02d.us" % sid)
        script = open(up, encoding="utf-8", errors="replace").read() if os.path.exists(up) else ""
        targets = warp_targets(script)
        cflags = chibi_door_flags(script)
        first_door, first_marker = len(doors), len(markers)
        for o in objs:
            x, z = o["position"]["x"], o["position"]["z"]
            if parked(x, z):
                continue
            if o["object"] == "map_jump_box":
                dest = targets.get(o["id"], -1)
                if dest in ROOMS and dest != sid:  # else: cutscene/NPC trigger or a dev stage
                    doors.append((int(round(x)), int(round(z)), dest, o["id"]))
            elif o["object"] == CHIBI_DOOR and o["id"] in cflags:
                cdoors.append((sid, int(round(x)), int(round(z)), cflags[o["id"]], o["id"]))
            elif o["object"] in MARKERS:
                markers.append((int(round(x)), int(round(z)), MARKERS[o["object"]], o["object"], o["id"]))
        rooms.append((sid,) + b + (first_door, len(doors) - first_door, first_marker, len(markers) - first_marker))

    # item pins: one per location with a pickup bit and an object we can place
    pins, skipped = [], []
    for name, (ap_stage, bit, addr, obj, coin) in read_locations(locations_py).items():
        sid = AP_TO_STAGE.get(ap_stage)
        o = None
        if sid in stages and obj is not None:
            o = next((x for x in stages[sid] if x["id"] == obj), None)
        if name in PIN_POS_OVERRIDE and addr is not None and sid in stages:
            x, z = PIN_POS_OVERRIDE[name]
        elif addr is None or o is None or parked(o["position"]["x"], o["position"]["z"]):
            skipped.append(name)
            continue
        else:
            x, z = int(round(o["position"]["x"])), int(round(o["position"]["z"]))
        short = name.split(" - ", 1)[-1]
        pins.append((sid, x, z, addr, bit, PIN_COIN if coin else PIN_ITEM, short, name))
    pins.sort(key=lambda p: p[0])  # grouped by room: bit k of the room mask = k-th pin of that room
    per_room = {}
    for p in pins:
        per_room[p[0]] = per_room.get(p[0], 0) + 1
    if max(per_room.values()) > 64:
        sys.exit("more than 64 pins in one room - widen CL_STATUS_PINS")

    with open(OUT_ROOMDATA, "w", newline="\n") as f:
        w = f.write
        w("/* Generated by tools/gen_roomdata.py from the GGTE01 stage data - do not edit. */\n")
        w('#include "roomdata.h"\n\n')
        w("const MapDoor g_map_doors[] = {\n")
        for x, z, dest, oid in doors:
            w("    { %5d, %5d, %3d }, /* map_jump_box %d */\n" % (x, z, dest, oid))
        w("};\n\n")
        w("const MapMarker g_map_markers[] = {\n")
        for x, z, kind, oname, oid in markers:
            w("    { %5d, %5d, %d }, /* %s %d */\n" % (x, z, kind, oname, oid))
        w("};\n\n")
        w("const MapRoom g_map_rooms[] = {\n")
        w("    /* stage, min_x, max_x, min_z, max_z, first door, doors, first marker, markers */\n")
        for r in rooms:
            w("    { %2d, %5d, %5d, %5d, %5d, %3d, %2d, %3d, %2d },\n" % r)
        w("};\n\n")
        w("const int g_map_room_count = %d;\n" % len(rooms))

    with open(OUT_PINS, "w", newline="\n") as f:
        w = f.write
        w("/*\n * Chibi-Robo! GBA Link - item pins for the GBA ROOM page.\n"
          " * Generated by tools/gen_roomdata.py from the apworld's locations.py - do not edit.\n *\n"
          " * Both sides expand CL_PINS(X): the GBA uses stage/x/z/name to draw a pin, the\n"
          " * GameCube uses addr/bit to report which of the current room's pins are\n"
          " * collected (CL_STATUS_PINS/_HI: bit k = the k-th pin of that room, in this\n"
          " * order). addr/bit are the location's pickup bit (bit N of the little-endian\n"
          " * u32 at addr, like client.py check_location), so a pin disappears as soon\n"
          " * as the check happens.\n *\n"
          " * kind: CL_PIN_ITEM, or CL_PIN_COIN for the coin_locations option's coins. Those\n"
          " * only exist when the seed has that option, which the randomizer tells the GC\n"
          " * code through cl_coin_pins. Without it the GC reports them as collected and\n"
          " * clears CL_FLAG_COIN_PINS, so the GBA leaves them off the map and the counts.\n *\n"
          " * X(stage, world x, world z, bitfield addr, bit, kind, name)\n */\n")
        w("#ifndef CHIBI_LINK_PINS_H\n#define CHIBI_LINK_PINS_H\n\n")
        w("#define CL_PIN_ITEM %d\n#define CL_PIN_COIN %d\n\n" % (PIN_ITEM, PIN_COIN))
        w("#define CL_PIN_COUNT %d\n\n" % len(pins))
        w("#define CL_PINS(X) \\\n")
        for sid, x, z, addr, bit, kind, short, full in pins:
            w("    X(%2d, %5d, %5d, 0x%08Xu, %2d, %d, %s) \\\n" % (sid, x, z, addr, bit, kind, c_str(short)))
        w("\n/* Chibi-Doors: X(stage, world x, world z, script flag set when opened).\n"
          "   flag(N) = bit N%32 of the big-endian u32 at 0x8036781C + (N/32)*4.\n"
          "   CL_STATUS_CDOORS bit k = the current room's k-th door (in this order) is open. */\n")
        w("#define CL_CDOOR_COUNT %d\n\n" % len(cdoors))
        w("#define CL_CDOORS(X) \\\n")
        for sid, x, z, flag, oid in cdoors:
            w("    X(%2d, %5d, %5d, %4d) /* %s %d */ \\\n" % (sid, x, z, flag, CHIBI_DOOR, oid))
        w("\n#endif\n")

    print("rooms %d, doors %d, outlets %d, chibi-doors %d %s, pins %d (skipped %d: %s)" % (
        len(rooms), len(doors), len(markers), len(cdoors), [(c[0], c[3]) for c in cdoors],
        len(pins), len(skipped), ", ".join(skipped[:6]) + (" ..." if len(skipped) > 6 else "")))
    print("pins per room:", dict(sorted(per_room.items())))
    print("coin pins:", sum(1 for p in pins if p[5] == PIN_COIN))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3])
