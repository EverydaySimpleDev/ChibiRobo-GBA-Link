#ifndef MESSAGES_H
#define MESSAGES_H

#include "game.h"

typedef struct {
    u8 icon;            /* CL_ICON_* */
    const char *title;  /* <= 16 chars */
    const char *text;   /* <= 96 chars */
} CLScriptMessage;

extern const CLScriptMessage cl_script_messages[];
extern const int cl_script_message_count;

/*
 * Mailbox for tools outside the game (Archipelago client, Gecko codes, etc...). It always lives at the very start of the injected
 * section, CL_MAILBOX_ADDR. All fields are big-endian (GameCube native).
 *
 * To post a message:
 *   1. read `ack`
 *   2. write icon / duration / title / text (NUL terminated)
 *   3. write post = ack + 1
 * The game copies it into its queue and sets ack = post. The read-only
 * fields at the end report what happened.
 */
#define CL_MAILBOX_ADDR   0x80672300u
#define CL_MAILBOX_MAGIC  0x434C4D42u /* 'CLMB' */

typedef struct {
    u32 magic;              /* 0x00 'CLMB'                                  */
    u16 version;            /* 0x04 1                                       */
    u16 size;               /* 0x06 sizeof(CLMailbox)                       */
    u32 post;               /* 0x08 writer: bump after filling the fields   */
    u32 ack;                /* 0x0C game: = post once queued                */
    u8  icon;               /* 0x10 CL_ICON_*                               */
    u8  reserved;           /* 0x11                                         */
    u16 duration;           /* 0x12 frames until auto-close, 0 = until closed */
    char title[16];         /* 0x14                                         */
    char text[96];          /* 0x24                                         */
    u32 link_state;         /* 0x84 game: 0 no GBA, 1 connecting, 2 linked  */
    u32 last_dismissed;     /* 0x88 game: seq of the last closed popup      */
    u32 gba_keys;           /* 0x8C game: GBA buttons (bit 0 = A ...)       */
} CLMailbox;                /* 0x90 bytes */

extern volatile CLMailbox cl_mailbox;

#endif
