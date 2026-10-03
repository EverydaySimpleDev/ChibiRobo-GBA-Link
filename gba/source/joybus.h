#ifndef JOYBUS_H
#define JOYBUS_H

#include <tonc_types.h>

/* Puts the link port in JOY bus mode and installs the IRQ handler. */
void joybus_init(void);

/* Returns 1 and stores the next word received from the GameCube, 0 if none. */
int joybus_pop(u32 *out);

/* Value the GameCube gets back the next time it issues a JOY bus read. */
void joybus_set_reply(u32 value);

/* Frames-since-boot counter bumped by the VBlank IRQ. */
extern volatile u32 g_frame;

/* Frame number of the last word received (for connection timeouts). */
extern volatile u32 g_last_rx_frame;

/* Set when the GameCube sends a JOY bus reset (0xFF). */
extern volatile u32 g_reset_count;

#endif
