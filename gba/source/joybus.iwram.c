/*
 * JOY bus driver + master interrupt handler.
 *
 * Lives in IWRAM and is compiled as ARM so the ISR is fast enough to keep up
 * with back-to-back GameCube writes. Register usage mirrors afska's LinkCube (gba-link-connection).
 */
#include <tonc.h>
#include "joybus.h"

#define JOYCNT_RESET    0x0001
#define JOYCNT_RECV     0x0002
#define JOYCNT_SEND     0x0004
#define JOYCNT_IRQ      0x0040

#define RX_SIZE 256 /* power of two */

static volatile u32 rx_buf[RX_SIZE];
static volatile u32 rx_head, rx_tail;
static volatile u32 reply_word;

volatile u32 g_frame;
volatile u32 g_last_rx_frame;
volatile u32 g_reset_count;

IWRAM_CODE static void cl_isr(void)
{
    u16 flags = REG_IE & REG_IF;

    if (flags & IRQ_SERIAL) {
        u16 cnt = REG_JOYCNT;

        if (cnt & JOYCNT_RESET) {
            g_reset_count++;
            REG_JOY_TRANS = reply_word;
        }
        if (cnt & JOYCNT_RECV) {
            u32 head = rx_head;
            u32 next = (head + 1) & (RX_SIZE - 1);
            u32 w = REG_JOY_RECV;
            if (next != rx_tail) {
                rx_buf[head] = w;
                rx_head = next;
            }
            g_last_rx_frame = g_frame;
        }
        if (cnt & JOYCNT_SEND) {
            /* GC consumed our word; queue the latest reply again. */
            REG_JOY_TRANS = reply_word;
        }
        /* Acknowledge by writing 1s back to the flag bits (keep IRQ on). */
        REG_JOYCNT = (cnt & (JOYCNT_RESET | JOYCNT_RECV | JOYCNT_SEND)) | JOYCNT_IRQ;
    }

    if (flags & IRQ_VBLANK)
        g_frame++;

    REG_IF = flags;
    REG_IFBIOS |= flags; /* lets VBlankIntrWait() return */
}

void joybus_init(void)
{
    REG_IME = 0;
    rx_head = rx_tail = 0;
    reply_word = 0;

    REG_ISR_MAIN = cl_isr;

    REG_RCNT = 0xC000; /* JOY bus mode */
    REG_JOYCNT = JOYCNT_RESET | JOYCNT_RECV | JOYCNT_SEND | JOYCNT_IRQ;
    REG_JOY_TRANS = 0;

    REG_DISPSTAT |= DSTAT_VBL_IRQ;
    REG_IE = IRQ_VBLANK | IRQ_SERIAL;
    REG_IME = 1;
}

int joybus_pop(u32 *out)
{
    u32 tail = rx_tail;
    if (tail == rx_head)
        return 0;
    *out = rx_buf[tail];
    rx_tail = (tail + 1) & (RX_SIZE - 1);
    return 1;
}

void joybus_set_reply(u32 value)
{
    reply_word = value;
    /* Only refresh the register when the GC has already read the old one, so
       we never tear a word the GC is in the middle of reading. */
    if (!(REG_JOYSTAT & 0x0008))
        REG_JOY_TRANS = value;
}
