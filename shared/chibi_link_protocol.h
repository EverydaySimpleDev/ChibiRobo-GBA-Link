/*
 * Chibi-Robo! GBA Link - wire protocol shared by the GameCube patch and the GBA multiboot program.
 *
 * Transport: Nintendo JOY bus (GC <-> GBA link cable).
 *   - GC "write" command (0x15) delivers one 32-bit word into the GBA's
 *     REG_JOY_RECV and raises the GBA serial IRQ.
 *   - GC "read" command (0x14) fetches the 32-bit word the GBA left in
 *     REG_JOY_TRANS.
 *
 * GC -> GBA packet (sent as consecutive 0x15 writes):
 *   word 0         CL_HEADER(type, payload_words)
 *   word 1..N      payload
 *   word N+1       CL_Checksum(header, payload, N)
 *
 * GBA -> GC: the GBA always keeps CL_REPLY(keys, flags) in REG_JOY_TRANS so
 * the GC can read controller input / page requests whenever it wants.
 *
 * Both CPUs are 32-bit; the GC side sends words with the byte order already
 * swapped (see gc/source/gbalink.c) so the values below arrive as native
 * little-endian u32 on the GBA.
 */
#ifndef CHIBI_LINK_PROTOCOL_H
#define CHIBI_LINK_PROTOCOL_H

#include <stdint.h>

#define CL_PROTOCOL_VERSION 1u

#define CL_MAGIC 0xCB10u
#define CL_HEADER(type, len) ((CL_MAGIC << 16) | (((len) & 0xFFu) << 8) | ((type) & 0xFFu))
#define CL_IS_HEADER(w) (((w) >> 16) == CL_MAGIC)
#define CL_HEADER_TYPE(w) ((w) & 0xFFu)
#define CL_HEADER_LEN(w) (((w) >> 8) & 0xFFu)

#define CL_MAX_PAYLOAD 32u

/* ---- packet types ------------------------------------------------------ */

#define CL_PKT_HELLO 0x01u   /* payload: [0]=protocol version, [1]=game id (ASCII, e.g. 'GGTE') */
#define CL_PKT_STATUS 0x02u  /* payload: see CL_STATUS_* word indices below */
#define CL_PKT_BYE 0x03u     /* no payload: game is shutting the link down */
#define CL_PKT_MESSAGE 0x04u /* popup, see CL_MSG_* below */

/* CL_PKT_STATUS payload layout (word index) */
#define CL_STATUS_BATTERY 0 /* lo16 = current watts, hi16 = max watts        */
#define CL_STATUS_MOOLAH 1  /* u32 moolah                                     */
#define CL_STATUS_HAPPY 2   /* u32 happy points                               */
#define CL_STATUS_ROOM 3    /* lo16 = stage/room id, hi16 = CL_FLAG_* bits    */
#define CL_STATUS_POS 4     /* lo16 = s16 world X, hi16 = s16 world Z (units) */
#define CL_STATUS_TIME 5    /* lo16 = day number, hi16 = minutes since 00:00  */
#define CL_STATUS_MSG 6     /* lo8 = seq of the popup that should be on screen \
                               (0 = none); the GBA closes anything else     */
#define CL_STATUS_FACING 7  /* lo16 = facing, 65536 = one turn (0 = +Z, toward +X) */
#define CL_STATUS_PINS 8    /* bit k = the current room's k-th pin in CL_PINS is collected */
#define CL_STATUS_PINS_HI 9 /* same for pins 32..63 of the room                          */
#define CL_STATUS_CDOORS 10 /* bit k = the current room's k-th Chibi-Door (CL_CDOORS) is open */
#define CL_STATUS_SCRAP 11  /* u32 scrap                                              */
#define CL_STATUS_CMD 12    /* last GBA command handled: CL_CMD_ACK_* bits below       */
#define CL_STATUS_WORDS 13

/* ---- GBA -> GC commands (TOOLS page, later a shop) ----------------------
 * The GBA puts a command in its reply word (CL_REPLY_CMD) and keeps sending it;
 * the GC applies each new sequence number once and echoes it in CL_STATUS_CMD. */
#define CL_CMD_NONE 0u
#define CL_CMD_MOOLAH_100 1u
#define CL_CMD_MOOLAH_1000 2u
#define CL_CMD_HAPPY_100 3u
#define CL_CMD_HAPPY_1000 4u
#define CL_CMD_SCRAP_10 5u
#define CL_CMD_SCRAP_100 6u
#define CL_CMD_SHOP_BATTERY 7u /* shop: 1000 moolah -> battery refilled to max. */
#define CL_SHOP_BATTERY_COST 1000u

#define CL_CMD_ACK_SEQ(w) ((w) & 7u)           /* sequence of the last handled command */
#define CL_CMD_ACK_RESULT(w) (((w) >> 8) & 3u) /* CL_CMD_OK / CL_CMD_REFUSED           */
#define CL_CMD_ACK_VALID 0x8000u               /* set once any command was handled     */
#define CL_CMD_OK 1u
#define CL_CMD_REFUSED 2u  /* e.g. not in gameplay right now       */
#define CL_CMD_NO_FUNDS 3u /* shop: not enough moolah              */

#define CL_FLAG_NIGHT 0x0001u       /* game is in a night cycle              */
#define CL_FLAG_POS_VALID 0x0002u   /* CL_STATUS_POS holds real coordinates  */
#define CL_FLAG_TIME_VALID 0x0004u  /* CL_STATUS_TIME holds real values      */
#define CL_FLAG_MAX_VALID 0x0008u   /* battery hi16 holds the real max       */
#define CL_FLAG_PLAYING 0x0010u     /* in gameplay (not title/loading)       */
#define CL_FLAG_PAUSED 0x0020u      /* game is paused                        */
#define CL_FLAG_LOADING 0x0040u     /* loading a save or area                */
#define CL_FLAG_NIGHT_VALID 0x0080u /* CL_FLAG_NIGHT is meaningful           */
#define CL_FLAG_COIN_PINS 0x0100u   /* seed has coin_locations: show CL_PIN_COIN pins */

/* CL_PKT_MESSAGE payload:
 *   word 0      seq (bits 0-7, never 0) | icon (8-15) | dismiss hint (16-23)
 *   words 1-4   title, 16 bytes
 *   words 5..   text, up to CL_MSG_TEXT_MAX bytes, line breaks allowed
 * Strings are NUL padded and packed 4 bytes per word, first byte in bits 0-7.
 * The GC resends the active message periodically; the GBA ignores repeats. */
#define CL_MSG_TITLE_MAX 16u
#define CL_MSG_TEXT_MAX 96u
#define CL_MSG_WORDS(textlen) (5u + ((textlen) + 4u) / 4u)

/* icons (match the GBA's 8x8 icon set) */
#define CL_ICON_BATTERY 0u
#define CL_ICON_COIN 1u
#define CL_ICON_HEART 2u
#define CL_ICON_HOUSE 3u
#define CL_ICON_CLOCK 4u
#define CL_ICON_PLUG 5u

/* dismiss hint bits: which buttons close the popup (for the on-screen hint) */
#define CL_HINT_GC_A 0x01u
#define CL_HINT_GC_B 0x02u
#define CL_HINT_GC_START 0x04u
#define CL_HINT_GC_Z 0x08u
#define CL_HINT_GBA_A 0x10u
#define CL_HINT_AUTO 0x20u /* closes by itself after a while */

/* ---- GBA -> GC reply word ---------------------------------------------- */

#define CL_REPLY_MAGIC 0xB0u
#define CL_REPLY(keys, flags) ((CL_REPLY_MAGIC << 24) | (((flags) & 0xFFu) << 16) | ((keys) & 0x3FFu))
#define CL_REPLY_READY 0x01u /* GBA program is running and parsing packets */

/* bits 10-12 = command sequence, bits 13-15 = command id (CL_CMD_*) */
#define CL_REPLY_CMD(seq, id) ((((seq) & 7u) << 10) | (((id) & 7u) << 13))
#define CL_REPLY_CMD_SEQ(w) (((w) >> 10) & 7u)
#define CL_REPLY_CMD_ID(w) (((w) >> 13) & 7u)

static inline uint32_t CL_Checksum(uint32_t header, const uint32_t *payload, uint32_t len)
{
    uint32_t sum = header ^ 0xC41B0B0Fu;
    uint32_t i;
    for (i = 0; i < len; i++)
        sum = ((sum << 5) | (sum >> 27)) + payload[i];
    return sum;
}

#endif
