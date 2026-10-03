# Chibi-Robo! GBA Link

GameCube Game Boy Advance link-cable feature for **Chibi-Robo! Plug Into Adventure (GGTE01, USA)**.

When a GBA is plugged into controller port 2, 3 or 4 and powered on with **no cartridge**, the patched game uploads a small multiboot program to it. The GBA then shows:

- **HOME page:** a map of the Sanderson house that highlights the current room and shows a blip for Chibi-Robo, plus battery (bar and watts), moolah and happy points. Small boxes inside a room are separate stages entered from it: Chibi-House and Mother Spider's Room (Living Room), Drain and UFO (Backyard), Bedroom (Past) (Bedroom).
- **ROOM page:** a live top-down map of the current room, with Chibi-Robo's position and facing, the doors labelled by where they lead, an exits list, and item pins that disappear once the location is checked.
- **TOOLS page (hidden for now):** set `SHOW_TOOLS_PAGE` to 1 in `gba/source/main.c` to bring the tab back. Up/Down and A to add Moolah (+100/+1000), Happy Points (+100/+1000) or Scrap (+10/+100). The GBA sends a command (`CL_CMD_*`, `CL_REPLY_CMD` in the reply word), and the GameCube applies each command exactly once, only while playing, capped by `CL_MAX_*` in `gc/source/game.h`. It reports back in `CL_STATUS_CMD`.
- **SHOP page:** A buys a Battery Refill for 1,000 moolah (`CL_SHOP_BATTERY_COST`). The GBA refuses locally when you have less than 1,000 moolah or the battery is already full. Otherwise it sends `CL_CMD_SHOP_BATTERY`, and the GameCube re-checks the moolah (`CL_CMD_NO_FUNDS`), takes 1,000, and sets the battery to its max. It shares the command channel with TOOLS. Command ids 1-7 are now all used, so a new shop item needs a wider id field in `CL_REPLY_CMD`.
- **INFO page:** link details (game ID, packet and error counts) and the raw values.

Press **L/R** (or Left/Right) to cycle HOME -> ROOM -> INFO.

## Layout

| Path                           | What it is                                                                                                                                                                                                                            |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `gba/`                         | GBA multiboot program (devkitARM and libtonc). Builds `chibi_link_mb.gba`, about 20 KB, well under the 256 KB multiboot limit.                                                                                                        |
| `gba/art/`                     | Sprite art. `chibi_head.png` is the 16x16 title-bar head and `chibi_0..3.webp` are the walk frames. `tools/gen_sprites.py` converts them into `gba/source/chibi_head.c` and `chibi_walk.c`, and `build.bat` runs it before compiling. |
| `gba/source/roomdata.c`        | Room bounds and doors for the ROOM page, **generated** by `tools/gen_roomdata.py`.                                                                                                                                                    |
| `gc/`                          | GameCube code injected into `main.dol` (devkitPPC). It probes the ports, uploads the multiboot image, and streams stats.                                                                                                              |
| `shared/chibi_link_protocol.h` | Packet format used by both sides.                                                                                                                                                                                                     |
| `shared/chibi_link_pins.h`     | Item pins shown on the ROOM page (one list, compiled into both sides).                                                                                                                                                                |
| `tools/chibi_link_patch.py`    | Adds the code to `main.dol` or to a copy of the ISO.                                                                                                                                                                                  |
| `src/`                         | The original "Hello Chibi Robo!" Test :D (Sorry I didnt' have the heart to delete it yet)                                                                                                                                             |

## Build

You need devkitPro with `gamecube-dev` and `gba-dev` (`C:\devkitPro\msys2\usr\bin\pacman -S gba-dev`), plus Python 3 (`py`).

```bat
build.bat                                   :: builds gba\ and gc\
build.bat "D:\path\to\Chibi-Robo (USA).iso" :: also writes out\ChibiRobo_GBALink.iso
```

The input ISO is never modified. You can also patch just a DOL:

```bat
py tools\chibi_link_patch.py dol main.dol main_patched.dol
```

`gba\chibi_link_demo_mb.gba` is a preview build that fakes the GameCube, so you can open it straight in mGBA.

## Randomizer / Archipelago integration

The Chibi-Robo randomizer can apply this patch for you:

- **Randomizer option:** Options -> **GBA link cable (port 2-4)**. With an Archipelago `.apcr` loaded, the value comes from the `gba_link` option instead.
- **How it's applied:** after all unplug edits, `GbaLinkPatcher.cs` patches the finished ISO in place. It uses `Resources\chibi_link.bin` and `Resources\chibi_link.sym`. `Resources\chibi_link_mb.gba` is kept alongside for reference.
- **Script vars:** `Resources\stage05.us` `sub_576` resets `var(1880)` and `var(1881)` along with the other randomizer vars.

After changing anything, run `build.bat` and patch into the ISO

## Testing in Dolphin

1. Set Controllers on Port 2 to **GBA (Integrated)**.
2. Set the GBA BIOS under Config -> GameCube.
3. Leave the GBA ROM slot empty and boot `out\ChibiRobo_GBALink.iso`.

The GBA window shows the Chibi-Robo! Link screen a few seconds after the game starts. If OSReport logging is enabled, the GC side logs `[GBALink] ...` messages.

On real hardware, use a GC–GBA link cable and turn the GBA on with no cartridge inserted.

## How the patch works

- **New code section:** the code and the GBA ROM are linked at `0x80672300`, which is the retail `__ArenaLo`, and added to `main.dol` as a new text section (T2).
- **Heap start:** OSInit's ArenaLo constant (`0x801615F4`) is raised to the end of that section, so the heap starts after it. This costs about 25 KB of heap.
- **Pad hook:** the game's only `bl PADRead` (`0x801CC118`) now calls `cl_pad_read`, which sees and can hide popup-closing presses.
- **Frame hook:** `main()`'s per-frame `bl VIWaitForRetrace` (`0x800156C8`) now calls `cl_frame_hook`, which waits for retrace and then runs `cl_tick()`.
- **Link logic:** all SI traffic is chained through the game's own `SITransfer` and `SIGetTypeAsync` callbacks, so the game never blocks.
  - **Multiboot:** the uploader follows the GBA BIOS JOY-boot protocol, ported from FIX94's gc-gba-link-cable-demo.
  - **Status stream:** once linked, the GC sends a status packet every 4 frames and reads back the GBA's key state (`cl_gba_reply`).
    - **Probing:** Dolphin's integrated GBA only answers some type probes, so besides probing ports 2-4 itself, the GC watches the SDK's per-port `Type[]` cache (`0x8022F8A8`) every frame and starts the link the moment any port reports a GBA.
- **Reconnects:** if the GBA is already running the program (for example after a game reset), the upload is skipped. If it stops answering, the GC goes back to probing.

## Data sources and what still needs verifying

RAM addresses are in `gc/source/game.h`, and room IDs and map rectangles are in `gba/source/rooms.c`.

| Value           | Address                                                                | Status                                                                          |
| --------------- | ---------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| Battery / max   | `0x8038F748` / `0x8038F74C` (float)                                    | Verified in Dolphin: shows 80/80 at the start of the game.                      |
| Moolah          | `0x8038F750` (u32, `Munnie`)                                           | From the Ghidra map.                                                            |
| Happy points    | `0x8038F73C` (u32)                                                     | **Unverified.** The notes list `…73E`, the low half of this word.               |
| Stage ID        | `0x8025F847` (u8)                                                      | Verified: shows Living Room (Party) during the intro.                           |
| Game state      | `0x8025DF17` (u8)                                                      | Used by the AP client.                                                          |
| Player position | X `0x803860F4`, Y `0x803860F8`, Z `0x803860FC` (float, `ChrObj`+4/8/C) | Verified in Dolphin by walking Chibi-Robo around. Same units as the stage JSON. |
| Facing          | `0x80386108` (float radians, `ChrObj`+0x18)                            | Verified: 0 = +Z, increasing toward +X.                                         |
| Day/night       | off (`0`)                                                              | Fill in `ADDR_NIGHT_FLAG` once found; the GBA uses it automatically.            |

The HOME map still puts the blip in the middle of the room, because `rooms.c` has no world extents. The ROOM page uses the real position.

## ROOM page data

- **Room map:** `gba/source/roomdata.c` holds each room's floor bounds and its loading zones (`map_jump_box` objects that can get toggled with the keys) with the stage each one warps to. Regenerate it after exporting the stages:

  ```
  unplug stage export --iso <iso> stage07 -o <dir>\stage07.json        (for every stage)
  unplug script disassemble-all --iso <iso> -o <dir2>
  py tools\gen_roomdata.py <dir> <dir2> > gba\source\roomdata.c
  ```

- **Map orientation:** screen right = +X, screen down = +Z.
- **Chibi-Doors:** these are the 60 `house_r_chibi_h_door_1` objects. The `_door_2` objects only mark the radar-hidden ones, and flags 1030-1048 track revealing those. Each room's script has a `var(881)` dispatcher, one `case` per door, which sets that door's "opened" flag and attaches the object. The generator reads the object/flag pairs from there into `CL_CDOORS`.
  - Script flag N is bit `N % 32` of the big-endian u32 at `0x8036781C + (N/32)*4`.
  - The GBA hides opened doors and shows "C-Doors left/total".
- **Outlets:** these are the `socket` objects, drawn in green.
- **Item pins:** add pins to `CL_PINS` in `shared/chibi_link_pins.h` as `X(stage, x, z, addr, bit, name)`:
  - `x`/`z` come from the object's position in the stage JSON.
  - `addr`/`bit` come from the Archipelago location's `address`/`bit` in `locations.py`.
  - The game reports the pin collected once that current-stage pickup bit is set, the same check that the Ap World makes.

## Popup messages (GameCube -> GBA)

The GameCube can show a popup on the GBA: an icon, a title of up to 16 characters, and up to 96 characters of word-wrapped text. It plays a chime when it opens. The player closes it with **A or B on the GameCube controller** or **A on the GBA**.

**From game scripts (randomizer / unplug `.us` files):**

```
set var(1880.d), 3.d                        ; show message #3 from gc/source/messages.c
if eq(var(1881.d), 3.d), else *loc_waiting  ; var(1881) = 3 once the player closed it
```

Add or edit messages in `gc/source/messages.c`.

Every `Resources\stageXX.us` also ends with two helper subroutines (added by `tools/add_gba_subs.py`):

```
pushbp
setsp   3.d
run     *sub_gba_msg        ; show #3 and carry on
popbp

pushbp
setsp   3.d
run     *sub_gba_msg_wait   ; show #3, pause this script until the player closes it
popbp                       ; (no wait when no GBA is linked, via var(1882); gives up after 60 s)
```

A new global `lib` isn't possible: the lib table is fixed at 376 entries (0-375), and unplug crashes when assembling `.lib 376`. Reusing an existing index risks one the engine calls itself.

**From outside the game (Archipelago client, Memory editing):** use **`0x80672300`**. Its layout is in `gc/source/messages.h`, and all fields are big-endian.

```python
MB = 0x80672300
ack = read_u32(MB + 0x0C)
write_bytes(MB + 0x10, bytes([icon, 0, 0, 0]))           # icon, reserved, duration (frames, 0 = until closed)
write_bytes(MB + 0x14, b"Archipelago".ljust(16, b"\0"))  # title
write_bytes(MB + 0x24, b"Received Chibi-Blaster!".ljust(96, b"\0"))
write_u32(MB + 0x08, ack + 1)                            # post
# read-only: +0x84 link state (2 = linked), +0x88 last closed seq, +0x8C GBA buttons
```

**Automatic popups** (turn off with `CL_AUTO_MESSAGES` in `game.h`): "GBA Linked!", which closes itself after 5 seconds, and a low-battery warning below 20% (This may be annoying to some users?). The dismiss buttons, the GBA A option, button swallowing and the grace period are also set in `game.h`.

The GameCube re-sends the active popup every 1.5 seconds and puts its sequence number in every status packet. A lost packet or a GBA reset therefore can't leave a popup stuck open or closed.

All of this was tested end to end in Dolphin with the integrated GBA: the script trigger, GameCube A to close with var(1881) set, the mailbox post, and GBA A to close.

## Adding features

1. Add a packet type or status word in `shared/chibi_link_protocol.h`.
2. Fill it in `build_status()` in `gc/source/gbalink.c`.
3. Draw it in `gba/source/main.c`.

GBA -> GC input already flows back: `cl_gba_reply` holds the GBA's buttons and current page. That makes GBA-driven features possible, such as a Chibi-Radar or remote actions.

# AI Disclaimer

AI was used for research and debugging but all the code was hand added by me and 'almost?' fully understood.

There are some sections that were copied and pasted from other random documentations or how to videos like the gba connector / and the tools to convert the images into GBA friendly files.

Examples: https://github.com/FIX94/gc-gba-link-cable-demo was a great help with getting the gba connection working but not fully understood yet
