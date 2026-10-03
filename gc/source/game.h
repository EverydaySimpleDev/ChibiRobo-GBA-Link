/*
 * Chibi-Robo! Plug Into Adventure (GGTE01, USA rev 0) - game RAM addresses and
 * the Dolphin SDK functions we call inside main.dol.
 *
 * Function addresses come from the decomp's config/GGTE01/symbols.txt
 * RAM addresses come from the Chibi-Robo-Randomizer notes + Archipelago client. 
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

typedef int32_t s32;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t OSTime;
typedef int BOOL;

/* ---- Dolphin SDK (already linked into main.dol) ------------------------ */

typedef void (*SICallback)(s32 chan, u32 sr, void *context);
typedef void (*SITypeAndStatusCallback)(s32 chan, u32 type);

BOOL SITransfer(s32 chan, void *output, u32 outputBytes, void *input, u32 inputBytes,
                SICallback callback, OSTime delay);
u32 SIGetTypeAsync(s32 chan, SITypeAndStatusCallback callback);
void VIWaitForRetrace(void);
BOOL OSDisableInterrupts(void);
BOOL OSRestoreInterrupts(BOOL level);
void OSReport(const char *fmt, ...);

typedef struct
{
   u16 button;
   int8_t stickX, stickY, substickX, substickY;
   u8 triggerL, triggerR, analogA, analogB;
   int8_t err; /* 0 = PAD_ERR_NONE */
} PADStatus;   /* 12 bytes, 4 per read */

u32 PADRead(PADStatus *status);

// GBA Buttons
#define PAD_BUTTON_LEFT 0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN 0x0004
#define PAD_BUTTON_UP 0x0008
#define PAD_TRIGGER_Z 0x0010
#define PAD_TRIGGER_R 0x0020
#define PAD_TRIGGER_L 0x0040
#define PAD_BUTTON_A 0x0100
#define PAD_BUTTON_B 0x0200
#define PAD_BUTTON_X 0x0400
#define PAD_BUTTON_Y 0x0800
#define PAD_BUTTON_START 0x1000

#define OS_TIMER_CLOCK (162000000 / 4)
#define OSMicrosecondsToTicks(us) ((OSTime)(us) * (OS_TIMER_CLOCK / 125000) / 8)

#define SI_GBA 0x00040000u
#define SI_ERROR_NO_RESPONSE 0x0008u
#define SI_ERROR_BUSY 0x0080u
#define SI_ERROR_MASK 0x000Fu          /* under/over-run, collision, no response */
#define ADDR_SI_TYPE_CACHE 0x8022F8A8u /* SDK `Type[4]` (symbols.txt), last type per port */

/* ---- game RAM ---------------------------------------------------------- */

#define ADDR_GAME_ID 0x80000000u     /* "GGTE"                                  */
#define ADDR_BATTERY 0x8038F748u     /* float, current watts                    */
#define ADDR_BATTERY_MAX 0x8038F74Cu /* float, max watts                        */
#define ADDR_MOOLAH 0x8038F750u      /* u32 ("Munnie" in the Ghidra map)        */
#define ADDR_SCRAP 0x8038F754u       /* u32 ("Gear" in the Ghidra map; notes list \
                                        0x8038F756, the low half of this word)  */

/* Upper limits for the GBA TOOLS page (never add past these) */
#define CL_MAX_MOOLAH 999999u
#define CL_MAX_HAPPY 9999999u
#define CL_MAX_SCRAP 9999u
#define ADDR_HAPPY 0x8038F73Cu      /* u32, 0x8038F73E, \
#define ADDR_STAGE_ID 0x8025F847u   /* u8, see gba/source/rooms.c              */
#define ADDR_GAME_STATE 0x8025DF17u /* u8: 0x00 title, 0x01 play, 0x07 pause, \
                                       0x40 loading, 0x81 play (special area)  */

/* Optional values, 0 = not sent. Fill these in once verified in Dolphin. */
#define ADDR_NIGHT_FLAG 0u /* u8, candidate 0x80348D2B  */
/* Player: ChrObj (0x803860F0) - verified in Dolphin */
#define ADDR_POS_X 0x803860F4u  /* float, player world X                   */
#define ADDR_POS_Z 0x803860FCu  /* float, player world Z                   */
#define ADDR_FACING 0x80386108u /* float, radians, 0 = +Z, increasing toward +X */

/* Script variables: var(N) is the 32-bit word at 0x80367A2C + N*4 (value in
   the low half; matches var(1860) = 0x8036973E and var(1874) = 0x80369774 used
   by the Archipelago client). 1880/1881 are unused by the game + randomizer. */
#define ADDR_VAR(n) (0x80367A2Cu + (u32)(n) * 4u)
#define ADDR_MSG_TRIGGER ADDR_VAR(1880) /* script writes N: show message #N */
#define ADDR_MSG_DONE ADDR_VAR(1881)    /* we write N when #N is closed      */
#define ADDR_GBA_LINKED ADDR_VAR(1882)  /* we write 1 while a GBA is linked  */

/* Script flags: flag(N) = bit N%32 of the big-endian u32 at ADDR_SCRIPT_FLAGS + (N/32)*4 */
#define ADDR_SCRIPT_FLAGS 0x8036781Cu

/* ---- popup behaviour --------------------------------------------------- */

#define CL_DISMISS_PAD (PAD_BUTTON_A | PAD_BUTTON_B) /* GC buttons that close a popup */
#define CL_DISMISS_GBA_A 1                           /* the GBA's A button closes it too              */
#define CL_DISMISS_SWALLOW 1                         /* hide the closing press from the game, so A \
                                                        doesn't also talk/pick up/jump               */
#define CL_DISMISS_GRACE 30                          /* frames a popup must be up before it can close */
#define CL_AUTO_MESSAGES 1                           /* "GBA linked" + low battery popups             */

/* ---- patch sites (used by tools/chibi_link_patch.py) --------------------
 *   0x800156C8  bl VIWaitForRetrace in main()'s loop -> bl cl_frame_hook
 *   0x801CC118  bl PADRead (the game's only call)    -> bl cl_pad_read
 *   0x801615F4  lis/addi __ArenaLo (0x80672300) in OSInit -> end of our section
 */

#endif
