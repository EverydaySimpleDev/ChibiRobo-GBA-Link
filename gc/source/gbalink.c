/*
 * Chibi-Robo! GBA Link - GameCube side.
 *
 * Injected into main.dol as a new section. Every frame main() calls
 * cl_frame_hook() in place of VIWaitForRetrace(). From there we:
 *   1. probe controller ports 2-4 for a Game Boy Advance,
 *   2. upload gba/chibi_link_mb.gba with the GBA BIOS JOY bus multiboot based on https://github.com/FIX94/gc-gba-link-cable-demo,
 *   3. stream CL_PKT_STATUS packets (battery, moolah, happy points, room...)
 *      and read back the GBA's key state.
 *
 * All SI work happens in SITransfer completion callbacks (interrupt context);
 * cl_tick() only schedules work and reads game RAM, so the FPU is only ever
 * used on the main thread.
 */
#include "game.h"
#include "messages.h"
#include "chibi_link_protocol.h"
#include "chibi_link_pins.h"

#define CHAN_FIRST          1      /* controller port 2 */
#define CHAN_LAST           3      /* controller port 4 */
#define PROBE_INTERVAL      10     /* frames between port probes          */
#define ERROR_COOLDOWN      30     /* frames to wait after a failure      */
#define BOOT_SETTLE         90     /* frames between upload and first read */
#define BOOT_START_TIMEOUT  600    /* frames to wait for our GBA program  */
#define LINK_TIMEOUT        240    /* frames without a valid reply = gone */
#define STATUS_INTERVAL     4      /* frames between status packets       */
#define HELLO_INTERVAL      120
#define BOOT_MAX_POLLS      4000   /* reset/status polls before giving up */
#define XFER_DELAY          OSMicrosecondsToTicks(60)

#define CMD_STATUS  0x00
#define CMD_READ    0x14   /* GC reads GBA's JOY_TRANS  */
#define CMD_WRITE   0x15   /* GC writes GBA's JOY_RECV  */
#define CMD_RESET   0xFF

extern const u8 cl_gba_rom[];
extern const u8 cl_gba_rom_end[];

enum {
    ST_IDLE,        /* waiting to probe the next port          */
    ST_PROBING,     /* SIGetTypeAsync in flight                 */
    ST_CHECK,       /* read in flight: is our program running?  */
    ST_BOOT,        /* multiboot upload running                 */
    ST_BOOT_WAIT,   /* upload done, GBA is starting the program */
    ST_LINK,        /* streaming packets                        */
    ST_ERROR,       /* cooling down after a failure             */
};

enum { B_RESET, B_STATUS, B_READKEY, B_SEND, B_READCRC };

static volatile int  g_state = ST_IDLE;
static volatile int  g_chan = CHAN_FIRST;
static volatile int  g_busy;          /* a transfer chain is in flight        */
static volatile int  g_retry;         /* SITransfer refused; reissue from tick */
static volatile u32  g_frame;
static volatile u32  g_state_frame;   /* frame the current state began        */
static volatile u32  g_last_reply;    /* frame of the last valid GBA reply     */
static volatile int  g_alive;         /* our GBA program answered             */

/* Most recent GBA reply: keys in bits 0-9, CL_REPLY flags in 16-23. */
volatile u32 cl_gba_reply;

static u8 out_buf[8] __attribute__((aligned(32)));
static u8 in_buf[8]  __attribute__((aligned(32)));

static struct {
    u32 out_len, in_len;
    SICallback cb;
    OSTime delay;
} xfer;

static struct {
    int step;
    u32 polls;
    u32 i, size;
    u32 key, crc;
    int crc_sent;
} boot;

static struct {
    u32 words[64];
    u32 count, pos;
    int read_done;
} tx;

static void link_cb(s32 chan, u32 sr, void *ctx);

/* ---- popup messages ----------------------------------------------------- */

__attribute__((section(".mailbox"), used))
volatile CLMailbox cl_mailbox = { CL_MAILBOX_MAGIC, 1, sizeof(CLMailbox) };

#define MSG_QUEUE 6

typedef struct {
    u8 seq, icon;
    u16 duration;           /* 0 = until dismissed */
    u16 script_id;          /* nonzero: came from var(1880), report in var(1881) */
    char title[CL_MSG_TITLE_MAX];
    char text[CL_MSG_TEXT_MAX];
} Msg;

static Msg msg_q[MSG_QUEUE];
static int msg_head, msg_count;
static u8 msg_next_seq;
static int msg_sent;                  /* active message went out at least once */
static u32 msg_first_send, msg_last_send;
static volatile int msg_dismiss;      /* set by cl_pad_read / GBA A press      */
static u16 pad_prev, pad_swallow;
static u32 gba_keys_prev;

/* GC buttons (PAD_BUTTON_*) as of the game's last PADRead. */
volatile u16 cl_pad_buttons;

static void copy_str(char *dst, const volatile char *src, int max)
{
    int i = 0;
    for (; i < max && src && src[i]; i++)
        dst[i] = src[i];
    for (; i < max; i++)
        dst[i] = 0;
}

/* Queue a popup for the GBA. Returns 0 if the queue is full. */
int cl_post_message(u8 icon, const char *title, const char *text, u16 duration, u16 script_id)
{
    BOOL lvl = OSDisableInterrupts();
    if (msg_count >= MSG_QUEUE) {
        OSRestoreInterrupts(lvl);
        return 0;
    }
    Msg *m = &msg_q[(msg_head + msg_count) % MSG_QUEUE];
    m->seq = 0;
    m->icon = icon;
    m->duration = duration;
    m->script_id = script_id;
    copy_str(m->title, title, CL_MSG_TITLE_MAX);
    copy_str(m->text, text, CL_MSG_TEXT_MAX);
    msg_count++;
    OSRestoreInterrupts(lvl);
    return 1;
}

static Msg *msg_active(void)
{
    return msg_count ? &msg_q[msg_head] : 0;
}

static int msg_showing(void)
{
    return msg_count && msg_sent && g_state == ST_LINK;
}

static u32 msg_hint(const Msg *m)
{
    u32 h = 0;
    if (CL_DISMISS_PAD & PAD_BUTTON_A)     h |= CL_HINT_GC_A;
    if (CL_DISMISS_PAD & PAD_BUTTON_B)     h |= CL_HINT_GC_B;
    if (CL_DISMISS_PAD & PAD_BUTTON_START) h |= CL_HINT_GC_START;
    if (CL_DISMISS_PAD & PAD_TRIGGER_Z)    h |= CL_HINT_GC_Z;
    if (CL_DISMISS_GBA_A)                  h |= CL_HINT_GBA_A;
    if (m->duration)                       h |= CL_HINT_AUTO;
    return h;
}

static void msg_close_active(void)
{
    Msg *m = msg_active();
    if (!m)
        return;
    cl_mailbox.last_dismissed = m->seq;
    if (m->script_id)
        *(volatile u32 *)ADDR_MSG_DONE = m->script_id;
    OSReport("[GBALink] popup %d closed\n", m->seq);
    msg_head = (msg_head + 1) % MSG_QUEUE;
    msg_count--;
    msg_sent = 0;
}

/*
 * Replaces the game's only `bl PADRead` (0x801CC118). Sees the buttons before
 * the game does, so a press that closes a popup can be hidden from it.
 */
u32 cl_pad_read(PADStatus *status)
{
    u32 ret = PADRead(status);
    if (status[0].err != 0)
        return ret;

    u16 held = status[0].button;
    u16 pressed = held & ~pad_prev;
    pad_prev = held;
    cl_pad_buttons = held;

    if ((pressed & CL_DISMISS_PAD) && msg_showing() &&
        g_frame - msg_first_send >= CL_DISMISS_GRACE) {
        msg_dismiss = 1;
        if (CL_DISMISS_SWALLOW)
            pad_swallow |= pressed & CL_DISMISS_PAD;
    }
    pad_swallow &= held;              /* released: stop hiding it */
    status[0].button = held & ~pad_swallow;
    return ret;
}

/* Collects new messages and handles closing; runs once per frame. */
static void msg_update(u32 now)
{
    /* 1. external mailbox */
    if (cl_mailbox.post != cl_mailbox.ack) {
        char title[CL_MSG_TITLE_MAX + 1], text[CL_MSG_TEXT_MAX + 1];
        copy_str(title, cl_mailbox.title, CL_MSG_TITLE_MAX);
        copy_str(text, cl_mailbox.text, CL_MSG_TEXT_MAX);
        title[CL_MSG_TITLE_MAX] = text[CL_MSG_TEXT_MAX] = 0;
        if (cl_post_message(cl_mailbox.icon, title, text, cl_mailbox.duration, 0))
            cl_mailbox.ack = cl_mailbox.post;
    }

    /* 2. game scripts: set var(1880) = N */
    volatile u32 *trig = (volatile u32 *)ADDR_MSG_TRIGGER;
    u32 v = *trig;
    if (v) {
        u32 id = (v & 0xFFFF) ? (v & 0xFFFF) : (v >> 16);
        if (id < (u32)cl_script_message_count && cl_script_messages[id].text) {
            const CLScriptMessage *sm = &cl_script_messages[id];
            if (cl_post_message(sm->icon, sm->title, sm->text, 0, (u16)id))
                *trig = 0;
        } else {
            *trig = 0;                  /* unknown id: drop it, and report it closed so */
            *(volatile u32 *)ADDR_MSG_DONE = id; /* sub_gba_msg_wait doesn't hang */
        }
    }

    /* 3. activate / close the head of the queue */
    Msg *m = msg_active();
    if (m && m->seq == 0) {
        msg_next_seq = msg_next_seq % 255 + 1;
        m->seq = msg_next_seq;
        msg_sent = 0;
    }

    u32 gkeys = (g_state == ST_LINK) ? (cl_gba_reply & 0x3FF) : 0;
    if (msg_showing()) {
        if (CL_DISMISS_GBA_A && (gkeys & ~gba_keys_prev & 0x0001))
            msg_dismiss = 1;
        if (m->duration && now - msg_first_send >= m->duration)
            msg_dismiss = 1;
    }
    gba_keys_prev = gkeys;

    if (msg_dismiss) {
        msg_dismiss = 0;
        if (msg_showing())
            msg_close_active();
    }

    cl_mailbox.gba_keys = gkeys;
    /* lets scripts skip waiting on a popup nobody can see (sub_gba_msg_wait) */
    *(volatile u32 *)ADDR_GBA_LINKED = g_state == ST_LINK;
    cl_mailbox.link_state = g_state == ST_LINK ? 2 : (g_state >= ST_CHECK && g_state <= ST_BOOT_WAIT ? 1 : 0);
}

static void tx_message(const Msg *m)
{
    u32 p[CL_MSG_WORDS(CL_MSG_TEXT_MAX)];
    u32 textlen = 0;
    while (textlen < CL_MSG_TEXT_MAX && m->text[textlen])
        textlen++;
    u32 len = CL_MSG_WORDS(textlen);
    if (len > CL_MSG_WORDS(CL_MSG_TEXT_MAX))
        len = CL_MSG_WORDS(CL_MSG_TEXT_MAX);

    p[0] = m->seq | ((u32)m->icon << 8) | (msg_hint(m) << 16);
    for (u32 i = 1; i < len; i++)
        p[i] = 0;
    for (u32 i = 0; i < CL_MSG_TITLE_MAX; i++)
        p[1 + i / 4] |= (u32)(u8)m->title[i] << ((i & 3) * 8);
    for (u32 i = 0; i < textlen; i++)
        p[5 + i / 4] |= (u32)(u8)m->text[i] << ((i & 3) * 8);

    u32 h = CL_HEADER(CL_PKT_MESSAGE, len);
    tx.words[tx.count++] = h;
    for (u32 i = 0; i < len; i++)
        tx.words[tx.count++] = p[i];
    tx.words[tx.count++] = CL_Checksum(h, p, len);
}

/* ---- helpers ----------------------------------------------------------- */

static void set_state(int s)
{
    g_state = s;
    g_state_frame = g_frame;
}

static void issue(void)
{
    if (!SITransfer(g_chan, out_buf, xfer.out_len, in_buf, xfer.in_len, xfer.cb, xfer.delay))
        g_retry = 1;
}

static void start_xfer(u32 out_len, u32 in_len, SICallback cb, OSTime delay)
{
    xfer.out_len = out_len;
    xfer.in_len = in_len;
    xfer.cb = cb;
    xfer.delay = delay;
    issue();
}

static void send_cmd(u8 cmd, u32 in_len, SICallback cb)
{
    out_buf[0] = cmd;
    start_xfer(1, in_len, cb, XFER_DELAY);
}

/* JOY bus write: the GBA sees `w` as a native little-endian u32. */
static void send_word(u32 w, SICallback cb)
{
    out_buf[0] = CMD_WRITE;
    out_buf[1] = (u8)w;
    out_buf[2] = (u8)(w >> 8);
    out_buf[3] = (u8)(w >> 16);
    out_buf[4] = (u8)(w >> 24);
    start_xfer(5, 1, cb, XFER_DELAY);
}

static u32 in_le32(void)
{
    return in_buf[0] | (in_buf[1] << 8) | (in_buf[2] << 16) | ((u32)in_buf[3] << 24);
}

static u32 in_be32(void)
{
    return ((u32)in_buf[0] << 24) | (in_buf[1] << 16) | (in_buf[2] << 8) | in_buf[3];
}

static u32 rom_le32(u32 off)
{
    const u8 *p = cl_gba_rom + off;
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static void fail(const char *why)
{
    OSReport("[GBALink] port %d: %s\n", g_chan + 1, why);
    g_busy = 0;
    g_alive = 0;
    set_state(ST_ERROR);
}

/* ---- multiboot (GBA BIOS JOY bus boot) ---------------------------------- */

static u32 mb_crc(u32 crc, u32 val)
{
    for (int i = 0; i < 32; i++) {
        if ((crc ^ val) & 1)
            crc = (crc >> 1) ^ 0xA1C1;
        else
            crc >>= 1;
        val >>= 1;
    }
    return crc;
}

static u32 mb_key(u32 size)
{
    u32 ret = 0;
    size = (size - 0x200) >> 3;
    int res1 = (size & 0x3F80) << 1;
    res1 |= (size & 0x4000) << 2;
    res1 |= (size & 0x7F);
    res1 |= 0x380000;
    int res2 = res1;
    res1 = res2 >> 0x10;
    int res3 = res2 >> 8;
    res3 += res1;
    res3 += res2;
    res3 <<= 24;
    res3 |= res2;
    res3 |= 0x80808080;

    if ((res3 & 0x200) == 0) {
        ret |= (((res3) & 0xFF) ^ 0x4B) << 24;
        ret |= (((res3 >> 8) & 0xFF) ^ 0x61) << 16;
        ret |= (((res3 >> 16) & 0xFF) ^ 0x77) << 8;
        ret |= (((res3 >> 24) & 0xFF) ^ 0x61);
    } else {
        ret |= (((res3) & 0xFF) ^ 0x73) << 24;
        ret |= (((res3 >> 8) & 0xFF) ^ 0x65) << 16;
        ret |= (((res3 >> 16) & 0xFF) ^ 0x64) << 8;
        ret |= (((res3 >> 24) & 0xFF) ^ 0x6F);
    }
    return ret;
}

static u32 mb_encrypt(u32 w, u32 pos)
{
    boot.key = boot.key * 0x6177614B + 1;
    w ^= boot.key;
    w ^= (~(pos + (0x20 << 20))) + 1;
    w ^= 0x20796220;
    return w;
}

static void boot_cb(s32 chan, u32 sr, void *ctx);

static void boot_send_next(void)
{
    if (boot.i < 0xC0) {                       /* header goes in the clear */
        u32 w = rom_le32(boot.i);
        boot.i += 4;
        send_word(w, boot_cb);
    } else if (boot.i < boot.size) {           /* body is encrypted */
        u32 w = rom_le32(boot.i);
        boot.crc = mb_crc(boot.crc, w);
        w = mb_encrypt(w, boot.i);
        boot.i += 4;
        send_word(w, boot_cb);
    } else if (!boot.crc_sent) {
        boot.crc_sent = 1;
        send_word(mb_encrypt(boot.crc | (boot.size << 16), boot.i), boot_cb);
    } else {
        boot.step = B_READCRC;
        send_cmd(CMD_READ, 5, boot_cb);
    }
}

static void boot_cb(s32 chan, u32 sr, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (sr & SI_ERROR_MASK) {
        fail("multiboot transfer error");
        return;
    }
    switch (boot.step) {
    case B_RESET:
        boot.step = B_STATUS;
        send_cmd(CMD_STATUS, 3, boot_cb);
        break;
    case B_STATUS:
        if (in_buf[2] & 0x10) {                /* BIOS ready for a JOY boot */
            boot.step = B_READKEY;
            send_cmd(CMD_READ, 5, boot_cb);
        } else if (++boot.polls > BOOT_MAX_POLLS) {
            fail("GBA never became ready (cartridge inserted?)");
        } else {
            boot.step = B_RESET;
            send_cmd(CMD_RESET, 3, boot_cb);
        }
        break;
    case B_READKEY:
        boot.key = __builtin_bswap32(in_be32() ^ 0x7365646F);
        boot.step = B_SEND;
        send_word(__builtin_bswap32(mb_key(boot.size)), boot_cb);
        break;
    case B_SEND:
        boot_send_next();
        break;
    case B_READCRC:
        g_busy = 0;
        set_state(ST_BOOT_WAIT);
        OSReport("[GBALink] port %d: upload complete (%u bytes)\n", g_chan + 1, boot.size);
        break;
    }
}

static void start_boot(void)
{
    u32 size = (u32)(cl_gba_rom_end - cl_gba_rom);
    boot.size = (size + 7) & ~7u;
    boot.step = B_RESET;
    boot.polls = 0;
    boot.i = 0;
    boot.crc = 0x15A0;
    boot.crc_sent = 0;
    OSReport("[GBALink] port %d: sending multiboot image\n", g_chan + 1);
    set_state(ST_BOOT);
    g_busy = 1;
    send_cmd(CMD_RESET, 3, boot_cb);
}

/* ---- GBA commands (TOOLS page) -------------------------------------------
 * The GBA keeps a command (id + 3-bit sequence) in its reply word until we echo
 * the sequence back in CL_STATUS_CMD, so each press is applied exactly once.
 * Runs on the main thread from cl_tick(), never in the SI callback. */

static u32 cmd_seq = 0xFF;              /* sequence of the last handled command, 0xFF = none */
static u32 cmd_result;

static void cmd_reset(void)
{
    cmd_seq = 0xFF;
    cmd_result = 0;
}

static u32 cmd_ack(void)
{
    return cmd_seq == 0xFF ? 0 : (cmd_seq & 7) | (cmd_result << 8) | CL_CMD_ACK_VALID;
}

static void add_capped(u32 addr, u32 amount, u32 max)
{
    volatile u32 *v = (volatile u32 *)addr;
    u32 cur = *v;
    *v = cur >= max || amount > max - cur ? max : cur + amount;
}

/* Shop: pay CL_SHOP_BATTERY_COST moolah, battery back to max. */
static u32 shop_battery(void)
{
    volatile u32 *moolah = (volatile u32 *)ADDR_MOOLAH;
    if (*moolah < CL_SHOP_BATTERY_COST)
        return CL_CMD_NO_FUNDS;
    *moolah -= CL_SHOP_BATTERY_COST;
    *(volatile float *)ADDR_BATTERY = *(volatile float *)ADDR_BATTERY_MAX;
    return CL_CMD_OK;
}

/* Returns a CL_CMD_* result. */
static u32 cmd_apply(u32 id)
{
    u8 gs = *(volatile u8 *)ADDR_GAME_STATE;
    if (gs != 0x01 && gs != 0x81)            /* only while actually playing */
        return CL_CMD_REFUSED;
    if (id == CL_CMD_SHOP_BATTERY)
        return shop_battery();
    switch (id) {
    case CL_CMD_MOOLAH_100:  add_capped(ADDR_MOOLAH, 100, CL_MAX_MOOLAH);  return CL_CMD_OK;
    case CL_CMD_MOOLAH_1000: add_capped(ADDR_MOOLAH, 1000, CL_MAX_MOOLAH); return CL_CMD_OK;
    case CL_CMD_HAPPY_100:   add_capped(ADDR_HAPPY, 100, CL_MAX_HAPPY);    return CL_CMD_OK;
    case CL_CMD_HAPPY_1000:  add_capped(ADDR_HAPPY, 1000, CL_MAX_HAPPY);   return CL_CMD_OK;
    case CL_CMD_SCRAP_10:    add_capped(ADDR_SCRAP, 10, CL_MAX_SCRAP);     return CL_CMD_OK;
    case CL_CMD_SCRAP_100:   add_capped(ADDR_SCRAP, 100, CL_MAX_SCRAP);    return CL_CMD_OK;
    }
    return CL_CMD_REFUSED;
}

static void cmd_update(void)
{
    u32 reply = cl_gba_reply;
    if ((reply >> 24) != CL_REPLY_MAGIC)
        return;
    u32 id = CL_REPLY_CMD_ID(reply), seq = CL_REPLY_CMD_SEQ(reply);
    if (id == CL_CMD_NONE || seq == cmd_seq)
        return;
    cmd_seq = seq;
    cmd_result = cmd_apply(id);
    OSReport("[GBALink] GBA command %u (seq %u): result %u\n", id, seq, cmd_result);
}

/* ---- probing / liveness ------------------------------------------------ */

static void check_cb(s32 chan, u32 sr, void *ctx)
{
    (void)chan;
    (void)ctx;
    u32 reply = in_le32();
    if (!(sr & SI_ERROR_MASK) && (reply >> 24) == CL_REPLY_MAGIC) {
        cl_gba_reply = reply;
        g_last_reply = g_frame;
        g_alive = 1;
        g_busy = 0;
        OSReport("[GBALink] port %d: GBA program running\n", g_chan + 1);
        cmd_reset();                           /* fresh GBA program: forget old commands */
        set_state(ST_LINK);
        return;
    }
    if (g_state == ST_CHECK) {
        start_boot();                          /* plain GBA BIOS: upload */
    } else {
        g_busy = 0;                            /* ST_BOOT_WAIT: try again later */
    }
}

static int is_gba(u32 type)
{
    return (type & (SI_ERROR_BUSY | SI_ERROR_NO_RESPONSE)) == 0 && (type & 0xFFFF0000u) == SI_GBA;
}

static void type_cb(s32 chan, u32 type)
{
    if (g_state != ST_PROBING || chan != g_chan)
        return;
    if (is_gba(type)) {
        OSReport("[GBALink] GBA found on port %d\n", chan + 1);
        set_state(ST_CHECK);
        g_busy = 1;
        send_cmd(CMD_READ, 5, check_cb);
    } else {
        g_chan = chan >= CHAN_LAST ? CHAN_FIRST : chan + 1;
        set_state(ST_IDLE);
    }
}

/* ---- link: status packets ---------------------------------------------- */

static void tx_packet(u32 type, const u32 *payload, u32 len)
{
    u32 h = CL_HEADER(type, len);
    tx.words[tx.count++] = h;
    for (u32 i = 0; i < len; i++)
        tx.words[tx.count++] = payload[i];
    tx.words[tx.count++] = CL_Checksum(h, payload, len);
}

static u32 f2u16(float f)
{
    s32 v = (s32)f;
    return v < 0 ? 0 : (v > 0xFFFF ? 0xFFFF : (u32)v);
}

static u32 f2s16(float f)
{
    s32 v = (s32)f;
    v = v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
    return (u32)v & 0xFFFF;
}

/* Pins are only meaningful for the current room: the pickup bitfields are the
   current stage's (same as the Archipelago client's check_location). Bit k of
   the mask = the k-th pin of the current room in CL_PINS order. */
static const struct { u8 stage; u8 bit; u8 kind; u32 addr; } cl_pins[] = {
#define CL_PIN_GC(stage, x, z, addr, bit, kind, name) { stage, bit, kind, addr },
    CL_PINS(CL_PIN_GC)
#undef CL_PIN_GC
};

/* 1 when the seed has the coin_locations option. The randomizer (GbaLinkPatcher.cs)
   sets this byte in the section it adds to main.dol; it's found by name in
   chibi_link.sym. Without it the coins are plain moolah, so their pins count as
   collected and the GBA isn't told to show them. */
volatile u8 cl_coin_pins = 0;

static void pins_collected(u32 *lo, u32 *hi)
{
    u8 stage = *(volatile u8 *)ADDR_STAGE_ID;
    u32 k = 0;
    *lo = *hi = 0;
    for (u32 i = 0; i < sizeof(cl_pins) / sizeof(cl_pins[0]); i++) {
        if (cl_pins[i].stage != stage)
            continue;
        u8 b = *(volatile u8 *)(cl_pins[i].addr + cl_pins[i].bit / 8); /* bit N of a LE u32 */
        if (((b >> (cl_pins[i].bit % 8)) & 1) || (cl_pins[i].kind == CL_PIN_COIN && !cl_coin_pins)) {
            if (k < 32)
                *lo |= 1u << k;
            else if (k < 64)
                *hi |= 1u << (k - 32);
        }
        k++;
    }
}

/* Chibi-Doors: open once their script flag is set. flag(N) = bit N%32 of the
   big-endian u32 at 0x8036781C + (N/32)*4 (checked against the sticker flags
   the Archipelago client reads, and flags 1195/1196 set by stage05 sub_576). */
static const struct { u8 stage; u16 flag; } cl_cdoors[] = {
#define CL_CDOOR_GC(stage, x, z, flag) { stage, flag },
    CL_CDOORS(CL_CDOOR_GC)
#undef CL_CDOOR_GC
};

static u32 cdoors_open(void)
{
    u8 stage = *(volatile u8 *)ADDR_STAGE_ID;
    u32 k = 0, mask = 0;
    for (u32 i = 0; i < sizeof(cl_cdoors) / sizeof(cl_cdoors[0]); i++) {
        if (cl_cdoors[i].stage != stage)
            continue;
        u32 w = *(volatile u32 *)(ADDR_SCRIPT_FLAGS + (cl_cdoors[i].flag / 32) * 4);
        if (((w >> (cl_cdoors[i].flag % 32)) & 1) && k < 32)
            mask |= 1u << k;
        k++;
    }
    return mask;
}

static void build_status(u32 *p)
{
    u8 gs = *(volatile u8 *)ADDR_GAME_STATE;
    u32 flags = CL_FLAG_MAX_VALID;

    if (gs == 0x01 || gs == 0x81 || gs == 0x07)
        flags |= CL_FLAG_PLAYING;
    if (gs == 0x07)
        flags |= CL_FLAG_PAUSED;
    if (gs == 0x40)
        flags |= CL_FLAG_LOADING;
    if (ADDR_NIGHT_FLAG) {
        flags |= CL_FLAG_NIGHT_VALID;
        if (*(volatile u8 *)ADDR_NIGHT_FLAG)
            flags |= CL_FLAG_NIGHT;
    }
    if (cl_coin_pins)
        flags |= CL_FLAG_COIN_PINS;

    p[CL_STATUS_BATTERY] = f2u16(*(volatile float *)ADDR_BATTERY) |
                           (f2u16(*(volatile float *)ADDR_BATTERY_MAX) << 16);
    p[CL_STATUS_MOOLAH]  = *(volatile u32 *)ADDR_MOOLAH;
    p[CL_STATUS_HAPPY]   = *(volatile u32 *)ADDR_HAPPY;
    p[CL_STATUS_POS]     = 0;
    if (ADDR_POS_X && ADDR_POS_Z) {
        flags |= CL_FLAG_POS_VALID;
        p[CL_STATUS_POS] = f2s16(*(volatile float *)ADDR_POS_X) |
                           (f2s16(*(volatile float *)ADDR_POS_Z) << 16);
    }
    p[CL_STATUS_ROOM]    = *(volatile u8 *)ADDR_STAGE_ID | (flags << 16);
    p[CL_STATUS_TIME]    = 0;
    p[CL_STATUS_MSG]     = msg_showing() ? msg_active()->seq : 0;
    /* radians -> 1/65536 turn (wraps naturally through the u16 mask) */
    p[CL_STATUS_FACING]  = (u32)(s32)(*(volatile float *)ADDR_FACING * 10430.378f) & 0xFFFF;
    pins_collected(&p[CL_STATUS_PINS], &p[CL_STATUS_PINS_HI]);
    p[CL_STATUS_CDOORS]  = cdoors_open();
    p[CL_STATUS_SCRAP]   = *(volatile u32 *)ADDR_SCRAP;
    p[CL_STATUS_CMD]     = cmd_ack();
}

static void link_cb(s32 chan, u32 sr, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (sr & SI_ERROR_MASK) {
        fail("link transfer error (cable pulled?)");
        return;
    }
    if (tx.pos < tx.count) {
        u32 w = tx.words[tx.pos++];
        send_word(w, link_cb);
    } else if (!tx.read_done) {
        tx.read_done = 1;
        send_cmd(CMD_READ, 5, link_cb);
    } else {
        u32 reply = in_le32();
        if ((reply >> 24) == CL_REPLY_MAGIC) {
            cl_gba_reply = reply;
            g_last_reply = g_frame;
        }
        g_busy = 0;
    }
}

static void link_send(int with_hello)
{
    u32 p[CL_STATUS_WORDS];
    tx.count = tx.pos = 0;
    tx.read_done = 0;
    if (with_hello) {
        u32 hello[2] = { CL_PROTOCOL_VERSION, *(volatile u32 *)ADDR_GAME_ID };
        tx_packet(CL_PKT_HELLO, hello, 2);
    }
    Msg *m = msg_active();
    if (m && m->seq && (!msg_sent || g_frame - msg_last_send >= 90)) {
        tx_message(m);                 /* resent until closed, in case the GBA missed it */
        if (!msg_sent)
            msg_first_send = g_frame;
        msg_sent = 1;
        msg_last_send = g_frame;
    }
    build_status(p);
    tx_packet(CL_PKT_STATUS, p, CL_STATUS_WORDS);

    g_busy = 1;
    tx.pos = 1;
    send_word(tx.words[0], link_cb);
}

/* ---- per-frame entry --------------------------------------------------- */

#if CL_AUTO_MESSAGES
static void auto_messages(u32 now)
{
    static int was_linked, low_warned;

    if (g_state == ST_LINK && !was_linked)
        cl_post_message(CL_ICON_PLUG, "GBA Linked!",
                        "Your GBA now shows the house map and Chibi-Robo's stats. L/R: switch pages.",
                        300, 0);
    was_linked = g_state == ST_LINK;

    if ((now & 31) == 0 && (*(volatile u8 *)ADDR_GAME_STATE == 0x01)) {
        float bat = *(volatile float *)ADDR_BATTERY;
        float max = *(volatile float *)ADDR_BATTERY_MAX;
        if (max > 0.0f) {
            if (!low_warned && bat < max * 0.2f) {
                low_warned = 1;
                cl_post_message(CL_ICON_BATTERY, "Low Battery!",
                                "Chibi-Robo's battery is almost empty.\nFind an outlet and plug in!", 0, 0);
            } else if (bat > max * 0.5f) {
                low_warned = 0;
            }
        }
    }
}
#endif

static void cl_tick(void)
{
    u32 now = ++g_frame;
    u32 age = now - g_state_frame;

#if CL_AUTO_MESSAGES
    auto_messages(now);
#endif
    msg_update(now);

    if (g_retry) {
        g_retry = 0;
        issue();
        return;
    }

    switch (g_state) {
    case ST_IDLE:
        /* Dolphin's integrated GBA only answers some probes, so also watch the
           SDK's per-port type cache (refreshed by the game's own probes too)
           and jump in the moment any port reports a GBA. */
        for (int ch = CHAN_FIRST; ch <= CHAN_LAST; ch++) {
            if (is_gba(((volatile u32 *)ADDR_SI_TYPE_CACHE)[ch])) {
                OSReport("[GBALink] GBA seen on port %d (type cache)\n", ch + 1);
                g_chan = ch;
                set_state(ST_CHECK);
                g_busy = 1;
                send_cmd(CMD_READ, 5, check_cb);
                break;
            }
        }
        if (g_state == ST_IDLE && age >= PROBE_INTERVAL) {
            set_state(ST_PROBING);
            SIGetTypeAsync(g_chan, type_cb);
        }
        break;

    case ST_PROBING:
        if (age > 120)                        /* callback never came */
            set_state(ST_IDLE);
        break;

    case ST_CHECK:
    case ST_BOOT:
        if (age > 60 * 30)                     /* watchdog: 30 s */
            fail("timed out");
        break;

    case ST_BOOT_WAIT:
        if (age > BOOT_START_TIMEOUT) {
            fail("GBA program did not start");
        } else if (age >= BOOT_SETTLE && !g_busy && (age & 7) == 0) {
            g_busy = 1;
            send_cmd(CMD_READ, 5, check_cb);
        }
        break;

    case ST_LINK:
        cmd_update();
        if (now - g_last_reply > LINK_TIMEOUT) {
            fail("GBA stopped answering");
        } else if (!g_busy && (now % STATUS_INTERVAL) == 0) {
            link_send(age < 2 || (now % HELLO_INTERVAL) == 0);
        } else if (g_busy && age > 60 && now - g_last_reply > 60) {
            g_busy = 0;                        /* lost callback: unstick */
        }
        break;

    case ST_ERROR:
        if (age >= ERROR_COOLDOWN) {
            g_busy = 0;
            set_state(ST_IDLE);
        }
        break;
    }
}

/* Replaces `bl VIWaitForRetrace` at 0x800156C8 in main(). */
__attribute__((section(".text.hook")))
void cl_frame_hook(void)
{
    VIWaitForRetrace();
    cl_tick();
}
