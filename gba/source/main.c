/*
 * Chibi-Robo! GBA Link - multiboot program sent by the GameCube.
 *
 * Receives CL_PKT_* packets over the JOY bus (see shared/chibi_link_protocol.h)
 * and shows a map of the Sanderson house plus Chibi-Robo's battery, moolah and
 * happy points.
 */
#include <tonc.h>
#include <string.h>
#include "chibi_link_protocol.h"
#include "joybus.h"
#include "gfx.h"
#include "rooms.h"
#include "roomdata.h"
#include "chibi_link_pins.h"

#define LINK_TIMEOUT_FRAMES 180 /* 3 s without data = link lost */

/* TOOLS (free moolah / happy points / scrap) is hidden for now since its only for debugging.
 * You can set this to 1 if you want the tab back.*/
#define SHOW_TOOLS_PAGE 0

#if SHOW_TOOLS_PAGE
enum { PAGE_HOME, PAGE_ROOM, PAGE_SHOP, PAGE_TOOLS, PAGE_INFO, PAGE_COUNT };
#else
enum { PAGE_HOME, PAGE_ROOM, PAGE_SHOP, PAGE_INFO, PAGE_COUNT, PAGE_TOOLS };
#endif
enum { LINK_WAITING, LINK_UP, LINK_LOST };

typedef struct {
    u32 battery, battery_max;
    u32 moolah, happy;
    u16 room_id, flags;
    s16 pos_x, pos_z;
    u16 day, minutes;
    u16 facing;          /* 65536 = one turn, 0 = +Z */
    u32 pins_lo, pins_hi; /* bit k = the room's k-th pin in CL_PINS collected */
    u32 cdoors;           /* bit k = the room's k-th Chibi-Door is open */
    u32 scrap;
    u32 cmd_ack;          /* CL_STATUS_CMD: last command the GameCube handled */
} Status;

static Status st;
static int have_status;
static int link_state = LINK_WAITING;
static int page = PAGE_HOME;
static u32 game_id, proto_version;
static u32 pkt_ok, pkt_bad;
static int drawn_room = -1;

/* popup message pushed by the GameCube (CL_PKT_MESSAGE) */
static struct {
    int active;          /* currently drawn                         */
    int pending_open;    /* received, draw on the next frame        */
    int pending_close;
    u32 seq, closed_seq; /* closed_seq: ignore resends of this one  */
    u32 icon, hint;
    char title[CL_MSG_TITLE_MAX + 1];
    char text[CL_MSG_TEXT_MAX + 1];
} pop;

static void unpack_str(char *dst, const u32 *src, u32 max_bytes, u32 avail_words)
{
    u32 n = 0;
    for (u32 i = 0; i < avail_words && n < max_bytes; i++)
        for (int b = 0; b < 4 && n < max_bytes; b++)
            dst[n++] = (char)(src[i] >> (b * 8));
    dst[n] = 0;
}

/* ---- packet parser ----------------------------------------------------- */

static struct {
    int state;           /* 0 = header, 1 = payload, 2 = checksum */
    u32 header, len, count;
    u32 payload[CL_MAX_PAYLOAD];
} rx;

static void handle_packet(u32 type, const u32 *p, u32 len)
{
    switch (type) {
    case CL_PKT_HELLO:
        if (len >= 2) {
            proto_version = p[0];
            game_id = p[1];
        }
        break;
    case CL_PKT_STATUS:
        if (len < CL_STATUS_WORDS)
            return;
        st.battery     = p[CL_STATUS_BATTERY] & 0xFFFF;
        st.battery_max = p[CL_STATUS_BATTERY] >> 16;
        st.moolah      = p[CL_STATUS_MOOLAH];
        st.happy       = p[CL_STATUS_HAPPY];
        st.room_id     = p[CL_STATUS_ROOM] & 0xFFFF;
        st.flags       = p[CL_STATUS_ROOM] >> 16;
        st.pos_x       = (s16)(p[CL_STATUS_POS] & 0xFFFF);
        st.pos_z       = (s16)(p[CL_STATUS_POS] >> 16);
        st.day         = p[CL_STATUS_TIME] & 0xFFFF;
        st.minutes     = p[CL_STATUS_TIME] >> 16;
        if (len >= CL_STATUS_PINS_HI + 1) {
            st.facing  = p[CL_STATUS_FACING] & 0xFFFF;
            st.pins_lo = p[CL_STATUS_PINS];
            st.pins_hi = p[CL_STATUS_PINS_HI];
        }
        if (len >= CL_STATUS_CDOORS + 1)
            st.cdoors  = p[CL_STATUS_CDOORS];
        if (len >= CL_STATUS_CMD + 1) {
            st.scrap   = p[CL_STATUS_SCRAP];
            st.cmd_ack = p[CL_STATUS_CMD];
        }
        have_status = 1;
        if (pop.active && (p[CL_STATUS_MSG] & 0xFF) != pop.seq)
            pop.pending_close = 1;       /* dismissed on the GameCube */
        break;
    case CL_PKT_MESSAGE: {
        if (len < 5)
            return;
        u32 seq = p[0] & 0xFF;
        if (seq == 0 || seq == pop.closed_seq || ((pop.active || pop.pending_open) && seq == pop.seq))
            return;                      /* resend of something we already handled */
        pop.seq = seq;
        pop.icon = (p[0] >> 8) & 0xFF;
        pop.hint = (p[0] >> 16) & 0xFF;
        unpack_str(pop.title, p + 1, CL_MSG_TITLE_MAX, 4);
        unpack_str(pop.text, p + 5, CL_MSG_TEXT_MAX, len - 5);
        pop.pending_open = 1;
        pop.pending_close = 0;
        break;
    }
    case CL_PKT_BYE:
        link_state = LINK_LOST;
        break;
    }
}

static void rx_word(u32 w)
{
    switch (rx.state) {
    case 0:
        if (CL_IS_HEADER(w) && CL_HEADER_LEN(w) <= CL_MAX_PAYLOAD) {
            rx.header = w;
            rx.len = CL_HEADER_LEN(w);
            rx.count = 0;
            rx.state = rx.len ? 1 : 2;
        }
        break;
    case 1:
        rx.payload[rx.count++] = w;
        if (rx.count == rx.len)
            rx.state = 2;
        break;
    case 2:
        rx.state = 0;
        if (w == CL_Checksum(rx.header, rx.payload, rx.len)) {
            pkt_ok++;
            handle_packet(CL_HEADER_TYPE(rx.header), rx.payload, rx.len);
        } else {
            pkt_bad++;
            if (CL_IS_HEADER(w)) /* we were out of sync; this may be a new header */
                rx_word(w);
        }
        break;
    }
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_title(void)
{
    txt_put(3, 0, "CHIBI-ROBO! LINK", TXT_YELLOW);
    txt_put(3, 1, "Plug Into Adventure!", TXT_WHITE);
    static const char *const tabs[] = {
        [PAGE_HOME] = "<HOME>", [PAGE_ROOM] = "<ROOM>", [PAGE_SHOP] = "<SHOP>",
        [PAGE_TOOLS] = "<TOOLS>", [PAGE_INFO] = "<INFO>",
    };
    txt_put_right(29, 0, tabs[page], TXT_WHITE);
    spr_chibi(4, 0, 1);
}

static void draw_map(void)
{
    const Room *cur = room_find(st.room_id);
    canvas_clear(MC_BG);
    for (int i = 0; i < g_room_count; i++) {
        const Room *r = &g_rooms[i];
        int hi = (r == cur);
        if (r->w == 0 || (r->hidden && !hi))
            continue;

        int floor = MC_FLOOR;
        int wall  = MC_WALL;
        if (hi) {
            floor = MC_HI_FLOOR;
            wall = MC_HI_WALL;
        }
        canvas_rect(r->x, r->y, r->w, r->h, floor);
        canvas_frame(r->x, r->y, r->w, r->h, wall);
        int tw = canvas_text_width(r->label);
        if (tw <= r->w - 4 && r->h >= 9)
            canvas_text(r->x + (r->w - tw) / 2, r->y + 3, r->label, hi ? MC_HI_LABEL : MC_LABEL);
    }
    for (int i = 0; i < g_door_count; i++)
        canvas_rect(g_doors[i].x, g_doors[i].y, g_doors[i].w, g_doors[i].h, MC_DOOR);
    drawn_room = st.room_id;
}

static void update_blip(void)
{
    const Room *r = room_find(st.room_id);
    if (page != PAGE_HOME || pop.active || !r || r->w == 0 || link_state != LINK_UP || !(st.flags & CL_FLAG_PLAYING)) {
        spr_blip(0, 0, 0);
        return;
    }
    int bx = r->w / 2, by = r->h / 2 + 3;
    if ((st.flags & CL_FLAG_POS_VALID) && r->max_x > r->min_x && r->max_z > r->min_z) {
        int fx = st.pos_x < r->min_x ? r->min_x : (st.pos_x > r->max_x ? r->max_x : st.pos_x);
        int fz = st.pos_z < r->min_z ? r->min_z : (st.pos_z > r->max_z ? r->max_z : st.pos_z);
        bx = 2 + (fx - r->min_x) * (r->w - 4) / (r->max_x - r->min_x);
        by = 2 + (fz - r->min_z) * (r->h - 4) / (r->max_z - r->min_z);
    }
    /* canvas sits at tile (1,3) = pixel (8,24) */
    spr_blip(8 + r->x + bx - 4, 24 + r->y + by - 4, (g_frame & 31) < 24);
}

static void fmt_time(char *buf, u16 minutes)
{
    int h = minutes / 60 % 24, m = minutes % 60;
    int h12 = h % 12 ? h % 12 : 12;
    char *p = buf;
    if (h12 >= 10)
        *p++ = '1';
    *p++ = (char)('0' + h12 % 10);
    *p++ = ':';
    *p++ = (char)('0' + m / 10);
    *p++ = (char)('0' + m % 10);
    strcpy(p, h < 12 ? " AM" : " PM");
}

static void draw_message_box(void)
{
    char buf[32];
    txt_clear_rect(1, 17, 28, 2);

    if (link_state == LINK_WAITING || !have_status) {
        txt_icon(1, 17, ICON_PLUG);
        txt_put(3, 17, "Waiting for GameCube", TXT_DARK);
        txt_put(23 , 17, "...." + (3 - (g_frame >> 4) % 4), TXT_DARK);
        return;
    }
    if (link_state == LINK_LOST) {
        txt_icon(1, 17, ICON_PLUG);
        txt_put(3, 17, "Link lost! Check cable", TXT_ORANGE);
        txt_put(3, 18, "Showing last known data", TXT_DARK);
        return;
    }

    if (st.flags & CL_FLAG_LOADING) {
        txt_icon(1, 17, ICON_CLOCK);
        txt_put(3, 17, "Loading", TXT_DARK);
        txt_put(10, 17, "...." + (3 - (g_frame >> 4) % 4), TXT_DARK);
        return;
    }
    if (!(st.flags & CL_FLAG_PLAYING)) {
        txt_icon(1, 17, ICON_PLUG);
        txt_put(3, 17, "Linked! Start a game", TXT_DARK);
        txt_put(3, 18, "to see Chibi-Robo's stats", TXT_BLUE);
        return;
    }

    const Room *r = room_find(st.room_id);
    txt_icon(1, 17, ICON_HOUSE);
    if (r) {
        txt_put(3, 17, r->name, TXT_DARK);
    } else {
        strcpy(buf, "Area ");
        fmt_u32(buf + 5, st.room_id, 0);
        txt_put(3, 17, buf, TXT_DARK);
    }

    if (st.flags & CL_FLAG_TIME_VALID) {
        txt_icon(1, 18, ICON_CLOCK);
        strcpy(buf, "Day ");
        fmt_u32(buf + 4, st.day, 0);
        txt_put(3, 18, buf, TXT_BLUE);
        fmt_time(buf, st.minutes);
        txt_put(10, 18, buf, TXT_BLUE);
    }
    if (st.flags & CL_FLAG_PAUSED)
        txt_put_right(29, 17, "PAUSED", TXT_ORANGE);
    if (st.flags & CL_FLAG_NIGHT_VALID)
        txt_put_right(29, 18, (st.flags & CL_FLAG_NIGHT) ? "Night" : "Day", TXT_ORANGE);
}

static void draw_home_static(void)
{
    canvas_set_width(16);
    panel_draw(0, 2, 18, 14, PANEL_BLUE);
    panel_canvas_place(1, 3);
    panel_draw(18, 2, 12, 14, PANEL_CREAM);
    panel_draw(0, 16, 30, 4, PANEL_CREAM);

    txt_icon(19, 3, ICON_BATTERY);
    txt_put(20, 3, "BATTERY", TXT_ORANGE);
    txt_icon(19, 8, ICON_COIN);
    txt_put(20, 8, "MOOLAH", TXT_ORANGE);
    txt_icon(19, 11, ICON_HEART);
    txt_put(20, 11, "HAPPY", TXT_ORANGE);
    draw_map();
}

static void draw_home_values(void)
{
    char buf[20];
    txt_clear_rect(19, 4, 10, 4);
    txt_clear_rect(19, 9, 10, 2);
    txt_clear_rect(19, 12, 10, 3);

    if (!have_status) {
        txt_put(20, 5, "---", TXT_DARK);
        txt_put(20, 9, "---", TXT_DARK);
        txt_put(20, 12, "---", TXT_DARK);
        return;
    }

    fmt_u32(buf, st.battery, 0);
    strcat(buf, "W");
    txt_big_right(29, 4, buf, TXT_BLUE);
    u32 max = (st.flags & CL_FLAG_MAX_VALID) && st.battery_max ? st.battery_max : 1000;
    txt_bar(19, 6, 10, st.battery, max);
    if (st.flags & CL_FLAG_MAX_VALID) {
        buf[0] = '/';
        fmt_u32(buf + 1, st.battery_max, 0);
        strcat(buf, "W");
        txt_put_right(29, 7, buf, TXT_DARK);
    }

    fmt_u32(buf, st.moolah, 1);
    txt_big_right(29, 9, buf, TXT_BLUE);
    fmt_u32(buf, st.happy, 1);
    txt_big_right(29, 12, buf, TXT_BLUE);
    txt_put_right(29, 14, "pts", TXT_DARK);

    if (drawn_room != st.room_id)
        draw_map();
}

static void put_row(int y, const char *label, const char *value)
{
    txt_put(2, y, label, TXT_ORANGE);
    txt_clear_rect(13, y, 16, 1);
    txt_put(13, y, value, TXT_DARK);
}

static void draw_info_static(void)
{
    panel_draw(0, 2, 30, 14, PANEL_CREAM);
    panel_draw(0, 16, 30, 4, PANEL_CREAM);
}

static void draw_info_values(void)
{
    char buf[24];

    buf[0] = (char)(game_id >> 24);
    buf[1] = (char)(game_id >> 16);
    buf[2] = (char)(game_id >> 8);
    buf[3] = (char)game_id;
    buf[4] = 0;
    put_row(3, "Game", game_id ? buf : "?");

    strcpy(buf, "v");
    fmt_u32(buf + 1, proto_version, 0);
    put_row(4, "Protocol", buf);

    fmt_u32(buf, pkt_ok, 1);
    put_row(5, "Packets", buf);
    fmt_u32(buf, pkt_bad, 1);
    put_row(6, "Errors", buf);

    fmt_u32(buf, st.room_id, 0);
    put_row(8, "Stage id", buf);

    if (st.flags & CL_FLAG_POS_VALID) {
        char *p = buf;
        if (st.pos_x < 0) *p++ = '-';
        fmt_u32(p, st.pos_x < 0 ? -st.pos_x : st.pos_x, 0);
        strcat(buf, ", ");
        p = buf + strlen(buf);
        if (st.pos_z < 0) *p++ = '-';
        fmt_u32(p, st.pos_z < 0 ? -st.pos_z : st.pos_z, 0);
        put_row(9, "Position", buf);
    } else {
        put_row(9, "Position", "n/a");
    }

    fmt_u32(buf, st.battery, 0);
    strcat(buf, " W");
    if (st.flags & CL_FLAG_MAX_VALID) {
        strcat(buf, " / ");
        fmt_u32(buf + strlen(buf), st.battery_max, 0);
    }
    put_row(11, "Battery", buf);
    fmt_u32(buf, st.moolah, 1);
    put_row(12, "Moolah", buf);
    fmt_u32(buf, st.happy, 1);
    put_row(13, "Happy Pts", buf);
    fmt_u32(buf, st.scrap, 1);
    put_row(14, "Scrap", buf);
}

/* ---- ROOM page: live map of the current room ----------------------------- */

#define ROOM_CW   104   /* canvas: 13x12 tiles at tile (1,3) = pixel (8,24) */
#define ROOM_CH    96

static const struct { u8 stage; u8 kind; s16 x, z; const char *name; } pins[] = {
#define CL_PIN_GBA(stage, x, z, addr, bit, kind, name) { stage, kind, x, z, name },
    CL_PINS(CL_PIN_GBA)
#undef CL_PIN_GBA
};
#define PIN_COUNT ((int)(sizeof(pins) / sizeof(pins[0])))

static const struct { u8 stage; s16 x, z; } cdoors[] = {
#define CL_CDOOR_GBA(stage, x, z, flag) { stage, x, z },
    CL_CDOORS(CL_CDOOR_GBA)
#undef CL_CDOOR_GBA
};
#define CDOOR_COUNT ((int)(sizeof(cdoors) / sizeof(cdoors[0])))

static struct {
    const MapRoom *room;
    s32 scale, ox, oy;
    int drawn_stage;
    u32 drawn_lo, drawn_hi, drawn_cdoors;
    s32 min_x, max_x, min_z, max_z;  /* live bounds: start from the room data, grow when the
                                        player walks outside them */
    int bounds_stage;
    int first_pin, pin_count;    /* this room's slice of pins[] (they're grouped by room) */
} rmap = { 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1 };

static const char icon_item[]   = ".RRR.RRWRRRWWWRRRWRR.RRR.";
static const char icon_coin[]   = ".OOO.OYOOOOOHOOOOOOO.OOO.";  /* coin_locations moolah */
static const char icon_outlet[] = "GGGGGGKGKGGKGKGGGGGG";
static const char icon_cdoor[]  = ".PPP.PPPPPPPPYPPPPPP";

static const MapRoom *map_room_find(u16 stage)
{
    for (int i = 0; i < g_map_room_count; i++)
        if (g_map_rooms[i].stage == stage)
            return &g_map_rooms[i];
    return 0;
}

/* world -> canvas pixel: screen right = +X, screen down = +Z, 16.16 fixed scale */
static void room_px(s32 x, s32 z, int *px, int *py)
{
    *px = rmap.ox + (((x - rmap.min_x) * rmap.scale) >> 16);
    *py = rmap.oy + (((z - rmap.min_z) * rmap.scale) >> 16);
}

static char door_code(s16 stage)
{
    switch (stage) {
    case 1: return 'K'; case 2: return 'F'; case 3: return 'B'; case 4: return 'J';
    case 5: return 'C'; case 6: return 'R'; case 7: return 'L'; case 9: return 'Y';
    case 11: return 'D';
    default: return '?';
    }
}

static int pin_checked(int k)   /* k = index within the room */
{
    return k < 32 ? (st.pins_lo >> k) & 1 : (st.pins_hi >> (k - 32)) & 1;
}

/* coin pins only exist when the seed has coin_locations (the GC says so) */
static int pin_shown(int k)
{
    return pins[rmap.first_pin + k].kind != CL_PIN_COIN || (st.flags & CL_FLAG_COIN_PINS);
}

static void room_pins_slice(void)
{
    rmap.first_pin = rmap.pin_count = 0;
    for (int i = 0; i < PIN_COUNT; i++)
        if (pins[i].stage == st.room_id) {
            if (!rmap.pin_count)
                rmap.first_pin = i;
            rmap.pin_count++;
        }
}

static void draw_room_map(void)
{
    char buf[24];
    rmap.room = map_room_find(st.room_id);
    rmap.drawn_stage = st.room_id;
    rmap.drawn_lo = st.pins_lo;
    rmap.drawn_hi = st.pins_hi;
    rmap.drawn_cdoors = st.cdoors;
    room_pins_slice();
    canvas_clear(MC_BG);
    txt_clear_rect(16, 4, 13, 5);
    if (!rmap.room) {
        canvas_text(28, 44, "NO MAP", MC_LABEL);
        txt_put(16, 4, "No map for", TXT_DARK);
        txt_put(16, 5, "this area", TXT_DARK);
        return;
    }
    const MapRoom *r = rmap.room;
    if (rmap.bounds_stage != st.room_id) {
        rmap.bounds_stage = st.room_id;
        rmap.min_x = r->min_x;
        rmap.max_x = r->max_x;
        rmap.min_z = r->min_z;
        rmap.max_z = r->max_z;
    }
    s32 dx = rmap.max_x - rmap.min_x, dz = rmap.max_z - rmap.min_z;
    s32 sx = ((ROOM_CW - 6) << 16) / dx, sz = ((ROOM_CH - 6) << 16) / dz;
    rmap.scale = sx < sz ? sx : sz;
    rmap.ox = (ROOM_CW - ((dx * rmap.scale) >> 16)) / 2;
    rmap.oy = (ROOM_CH - ((dz * rmap.scale) >> 16)) / 2;

    int x0, y0, x1, y1, px, py;
    room_px(rmap.min_x, rmap.min_z, &x0, &y0);
    room_px(rmap.max_x, rmap.max_z, &x1, &y1);
    canvas_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, MC_FLOOR);
    canvas_frame(x0, y0, x1 - x0 + 1, y1 - y0 + 1, MC_WALL);

    /* outlets */
    for (int i = 0; i < r->marker_count; i++) {
        const MapMarker *m = &g_map_markers[r->first_marker + i];
        room_px(m->x, m->z, &px, &py);
        if (m->kind == MK_OUTLET)
            canvas_icon(px, py, icon_outlet, 5, 4);
    }

    /* Chibi-Doors that haven't been opened yet */
    for (int i = 0, k = 0; i < CDOOR_COUNT; i++) {
        if (cdoors[i].stage != st.room_id)
            continue;
        if (!((st.cdoors >> k++) & 1)) {
            room_px(cdoors[i].x, cdoors[i].z, &px, &py);
            canvas_icon(px, py, icon_cdoor, 5, 4);
        }
    }

    /* exits, with the legend on the right */
    int row = 4;
    for (int i = 0; i < r->door_count; i++) {
        const MapDoor *d = &g_map_doors[r->first_door + i];
        room_px(d->x, d->z, &px, &py);
        canvas_rect(px - 3, py - 3, 7, 7, MC_HI_WALL);
        canvas_rect(px - 2, py - 2, 5, 5, MC_HI_FLOOR);
        buf[0] = door_code(d->dest);
        buf[1] = 0;
        canvas_text(px - 1, py - 2, buf, MC_HI_LABEL);

        int dup = 0;
        for (int j = 0; j < i; j++)
            if (g_map_doors[r->first_door + j].dest == d->dest)
                dup = 1;
        const Room *dest = room_find(d->dest);
        if (!dup && row <= 8 && dest) {
            buf[0] = door_code(d->dest);
            buf[1] = ' ';
            strncpy(buf + 2, dest->name, 11);
            buf[13] = 0;
            txt_put(16, row++, buf, TXT_DARK);
        }
    }

    /* item pins on top, unchecked only */
    for (int k = 0; k < rmap.pin_count; k++) {
        if (pin_checked(k) || !pin_shown(k))
            continue;
        room_px(pins[rmap.first_pin + k].x, pins[rmap.first_pin + k].z, &px, &py);
        canvas_icon(px, py, pins[rmap.first_pin + k].kind == CL_PIN_COIN ? icon_coin : icon_item, 5, 5);
    }
}

static void draw_room_static(void)
{
    canvas_set_width(13);
    panel_draw(0, 2, 15, 14, PANEL_BLUE);
    panel_canvas_place(1, 3);
    panel_draw(15, 2, 15, 14, PANEL_CREAM);
    panel_draw(0, 16, 30, 4, PANEL_CREAM);
    txt_icon(16, 3, ICON_HOUSE);
    txt_put(18, 3, "EXITS", TXT_ORANGE);
    draw_room_map();
}

static void draw_room_values(void)
{
    char buf[24];
    /* rooms with rough bounds (few placed objects) correct themselves as the player explores */
    int grow = 0;
    if (rmap.room && rmap.bounds_stage == st.room_id && (st.flags & CL_FLAG_POS_VALID)) {
        if (st.pos_x < rmap.min_x) { rmap.min_x = st.pos_x - 30; grow = 1; }
        if (st.pos_x > rmap.max_x) { rmap.max_x = st.pos_x + 30; grow = 1; }
        if (st.pos_z < rmap.min_z) { rmap.min_z = st.pos_z - 30; grow = 1; }
        if (st.pos_z > rmap.max_z) { rmap.max_z = st.pos_z + 30; grow = 1; }
    }
    if (grow || rmap.drawn_stage != st.room_id || rmap.drawn_lo != st.pins_lo || rmap.drawn_hi != st.pins_hi ||
        rmap.drawn_cdoors != st.cdoors)
        draw_room_map();

    txt_clear_rect(16, 10, 14, 5);   /* up to column 29, so no leftover characters */
    if (!rmap.room)
        return;

    /* counts */
    int left = 0, total = 0, outlets = 0, cd_total = 0, cd_left = 0, best = -1;
    s32 best_d = 0x7FFFFFFF;
    for (int k = 0; k < rmap.pin_count; k++) {
        if (!pin_shown(k))
            continue;
        total++;
        if (pin_checked(k))
            continue;
        left++;
        s32 dx = pins[rmap.first_pin + k].x - st.pos_x, dz = pins[rmap.first_pin + k].z - st.pos_z;
        s32 d = dx * dx + dz * dz;
        if (d < best_d) {
            best_d = d;
            best = rmap.first_pin + k;
        }
    }
    for (int i = 0; i < rmap.room->marker_count; i++)
        if (g_map_markers[rmap.room->first_marker + i].kind == MK_OUTLET)
            outlets++;
    for (int i = 0; i < CDOOR_COUNT; i++)
        if (cdoors[i].stage == st.room_id) {
            if (!((st.cdoors >> cd_total) & 1))
                cd_left++;
            cd_total++;
        }

    txt_icon(16, 10, ICON_PIN);
    strcpy(buf, "Items ");
    fmt_u32(buf + strlen(buf), left, 0);
    strcat(buf, "/");
    fmt_u32(buf + strlen(buf), total, 0);
    txt_put(18, 10, buf, TXT_DARK);

    txt_icon(16, 11, ICON_PLUG);
    strcpy(buf, "Outlets ");
    fmt_u32(buf + strlen(buf), outlets, 0);
    txt_put(18, 11, buf, TXT_DARK);

    txt_icon(16, 12, ICON_CDOOR);
    strcpy(buf, "Doors ");
    fmt_u32(buf + strlen(buf), cd_left, 0);
    strcat(buf, "/");
    fmt_u32(buf + strlen(buf), cd_total, 0);
    txt_put(18, 12, buf, TXT_DARK);

    if (best >= 0 && (st.flags & CL_FLAG_POS_VALID)) {
        txt_put(16, 13, "Nearest:", TXT_ORANGE);
        strncpy(buf, pins[best].name, 13);
        buf[13] = 0;
        txt_put(16, 14, buf, TXT_BLUE);
    } else if (total && !left) {
        txt_put(16, 13, "All checked!", TXT_ORANGE);
    }
}

static void update_room_sprites(void)
{
    int show = page == PAGE_ROOM && !pop.active && rmap.room && link_state == LINK_UP;
    int px = 0, py = 0;

    if (show && (st.flags & CL_FLAG_POS_VALID)) {
        room_px(st.pos_x, st.pos_z, &px, &py);
        /* affine rotation turns the art counter-clockwise; the art points
           down (+Z) and facing grows toward +X (screen right) */
        spr_player(8 + px, 24 + py, st.facing, 1);
    } else {
        spr_player(0, 0, 0, 0);
    }
}

/* ---- TOOLS page: send commands to the GameCube ----------------------------
 * The chosen command stays in our reply word (CL_REPLY_CMD) until the GameCube
 * echoes its sequence number back in CL_STATUS_CMD, so it's applied exactly once. */

static const struct { const char *label; u8 cmd; u8 icon; } tools[] = {
    { "Moolah  +100",  CL_CMD_MOOLAH_100,  ICON_COIN },
    { "Moolah  +1000", CL_CMD_MOOLAH_1000, ICON_COIN },
    { "Happy   +100",  CL_CMD_HAPPY_100,   ICON_HEART },
    { "Happy   +1000", CL_CMD_HAPPY_1000,  ICON_HEART },
    { "Scrap   +10",   CL_CMD_SCRAP_10,    ICON_PLUG },
    { "Scrap   +100",  CL_CMD_SCRAP_100,   ICON_PLUG },
};
#define TOOL_COUNT ((int)(sizeof(tools) / sizeof(tools[0])))

static struct {
    int cursor;
    u32 seq;                /* sequence of our last command (0..7)        */
    u32 pending;            /* CL_CMD_* waiting for the GameCube, or 0    */
    u32 sent_frame;
    const char *msg;        /* result line                                */
    int msg_bank;
} tool = { 0, 0, 0, 0, "", TXT_DARK };

static void draw_tools_static(void)
{
    panel_draw(0, 2, 30, 14, PANEL_CREAM);
    panel_draw(0, 16, 30, 4, PANEL_CREAM);
    txt_icon(1, 3, ICON_HOUSE);
    txt_put(3, 3, "TOOLS", TXT_ORANGE);
    txt_put(9, 3, "Up/Down + A", TXT_BLUE);
    for (int i = 0; i < TOOL_COUNT; i++) {
        txt_icon(3, 5 + i, tools[i].icon);
        txt_put(5, 5 + i, tools[i].label, TXT_DARK);
    }
}

static void draw_tools_values(void)
{
    char buf[24];
    for (int i = 0; i < TOOL_COUNT; i++)
        txt_put(1, 5 + i, i == tool.cursor ? ">" : " ", TXT_ORANGE);

    txt_clear_rect(19, 5, 10, 6);
    txt_put(19, 5, "Moolah", TXT_ORANGE);
    fmt_u32(buf, st.moolah, 1);
    txt_put_right(29, 6, buf, TXT_BLUE);
    txt_put(19, 7, "Happy", TXT_ORANGE);
    fmt_u32(buf, st.happy, 1);
    txt_put_right(29, 8, buf, TXT_BLUE);
    txt_put(19, 9, "Scrap", TXT_ORANGE);
    fmt_u32(buf, st.scrap, 1);
    txt_put_right(29, 10, buf, TXT_BLUE);

    txt_clear_rect(1, 13, 28, 2);
    txt_put(2, 13, tool.msg, tool.msg_bank);
}

/* start sending `cmd`; tools_update() matches the GameCube's answer */
static void send_command(u32 cmd)
{
    tool.seq = (tool.seq + 1) & 7;
    tool.pending = cmd;
    tool.sent_frame = g_frame;
    tool.msg = "Sending...";
    tool.msg_bank = TXT_BLUE;
}

/* ---- SHOP page: spend moolah on things in the game -------------------------- */

static void draw_shop_static(void)
{
    panel_draw(0, 2, 30, 14, PANEL_CREAM);
    panel_draw(0, 16, 30, 4, PANEL_CREAM);
    txt_icon(1, 3, ICON_COIN);
    txt_put(3, 3, "SHOP", TXT_ORANGE);
    txt_put(8, 3, "A to buy", TXT_BLUE);
    txt_put(1, 5, ">", TXT_ORANGE);
    txt_icon(3, 5, ICON_BATTERY);
    txt_put(5, 5, "Battery Refill", TXT_DARK);
    txt_put(5, 6, "Fills Chibi-Robo's", TXT_BLUE);
    txt_put(5, 7, "battery to max", TXT_BLUE);
    txt_icon(20, 5, ICON_COIN);
    txt_put(22, 5, "1,000", TXT_ORANGE);
}

static void draw_shop_values(void)
{
    char buf[24];
    txt_clear_rect(1, 9, 28, 6);
    txt_put(2, 9, "Moolah", TXT_ORANGE);
    fmt_u32(buf, st.moolah, 1);
    txt_put_right(29, 9, buf, TXT_BLUE);
    txt_put(2, 10, "Battery", TXT_ORANGE);
    fmt_u32(buf, st.battery, 0);
    strcat(buf, " / ");
    fmt_u32(buf + strlen(buf), st.battery_max, 0);
    strcat(buf, "W");
    txt_put_right(29, 10, buf, TXT_BLUE);
    txt_bar(19, 11, 10, st.battery, st.battery_max ? st.battery_max : 1);
    txt_put(2, 13, tool.msg, tool.msg_bank);
}

static void shop_input(void)
{
    if (!key_hit(KEY_A) || tool.pending)
        return;
    if (link_state != LINK_UP) {
        tool.msg = "Not linked to the GameCube";
        tool.msg_bank = TXT_ORANGE;
    } else if (st.moolah < CL_SHOP_BATTERY_COST) {
        tool.msg = "Not enough moolah";
        tool.msg_bank = TXT_ORANGE;
    } else if (st.battery_max && st.battery >= st.battery_max) {
        tool.msg = "Battery is already full";
        tool.msg_bank = TXT_ORANGE;
    } else {
        send_command(CL_CMD_SHOP_BATTERY);
    }
    draw_shop_values();
}

/* called every frame on the TOOLS page */
static void tools_input(void)
{
    int redraw = 0;
    if (key_hit(KEY_UP)) {
        tool.cursor = (tool.cursor + TOOL_COUNT - 1) % TOOL_COUNT;
        redraw = 1;
    } else if (key_hit(KEY_DOWN)) {
        tool.cursor = (tool.cursor + 1) % TOOL_COUNT;
        redraw = 1;
    } else if (key_hit(KEY_A) && !tool.pending) {
        if (link_state != LINK_UP) {
            tool.msg = "Not linked to the GameCube";
            tool.msg_bank = TXT_ORANGE;
        } else {
            send_command(tools[tool.cursor].cmd);
        }
        redraw = 1;
    }
    if (redraw)
        draw_tools_values();
}

/* runs every frame on any page: match the GameCube's acknowledgement */
static void tools_update(void)
{
    if (!tool.pending)
        return;
    if ((st.cmd_ack & CL_CMD_ACK_VALID) && CL_CMD_ACK_SEQ(st.cmd_ack) == tool.seq) {
        u32 result = CL_CMD_ACK_RESULT(st.cmd_ack);
        if (result == CL_CMD_OK) {
            tool.msg = tool.pending == CL_CMD_SHOP_BATTERY ? "Battery refilled!" : "Done!";
            tool.msg_bank = TXT_BLUE;
        } else if (result == CL_CMD_NO_FUNDS) {
            tool.msg = "Not enough moolah";
            tool.msg_bank = TXT_ORANGE;
        } else {
            tool.msg = "Only while playing";
            tool.msg_bank = TXT_ORANGE;
        }
        tool.pending = 0;
    } else if (g_frame - tool.sent_frame > 180) {
        tool.msg = "No answer - try again";
        tool.msg_bank = TXT_ORANGE;
        tool.pending = 0;
    } else {
        return;
    }
    if (!pop.active && page == PAGE_TOOLS)
        draw_tools_values();
    else if (!pop.active && page == PAGE_SHOP)
        draw_shop_values();
}

static void draw_page(void)
{
    txt_clear();
    panel_clear();
    draw_title();
    if (page == PAGE_HOME)
        draw_home_static();
    else if (page == PAGE_ROOM)
        draw_room_static();
    else if (page == PAGE_SHOP)
        draw_shop_static();
    else if (page == PAGE_TOOLS)
        draw_tools_static();
    else
        draw_info_static();
}

/* ---- popup ------------------------------------------------------------ */

#define POP_X     1
#define POP_Y     3
#define POP_W     28
#define POP_H     13
#define POP_COLS  24
#define POP_LINES 5

/* Greedy word wrap into POP_LINES lines of POP_COLS characters. */
static void draw_wrapped(int x, int y, const char *s, int bank)
{
    char line[POP_COLS + 1];
    for (int row = 0; row < POP_LINES && *s; row++) {
        int n = 0, brk = -1;
        while (s[n] && s[n] != '\n' && n < POP_COLS) {
            if (s[n] == ' ')
                brk = n;
            n++;
        }
        int take = n, skip = n;
        if (s[n] && s[n] != '\n' && brk > 0) { /* too long: break at last space */
            take = brk;
            skip = brk + 1;
        } else if (s[n] == '\n' || s[n] == ' ') {
            skip = n + 1;
        }
        memcpy(line, s, take);
        line[take] = 0;
        txt_put(x, y + row, line, bank);
        s += skip;
    }
}

static void draw_hint(void)
{
    char buf[32];
    buf[0] = 0;
    static const struct { u32 bit; const char *name; } names[] = {
        { CL_HINT_GC_A, "A" }, { CL_HINT_GC_B, "B" }, { CL_HINT_GC_START, "START" }, { CL_HINT_GC_Z, "Z" },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (pop.hint & names[i].bit) {
            if (buf[0])
                strcat(buf, "/");
            strcat(buf, names[i].name);
        }
    if (buf[0]) {
        strcat(buf, " on GameCube");
        txt_put(POP_X + 2, POP_Y + 10, buf, TXT_BLUE);
    }
    if (pop.hint & CL_HINT_GBA_A)
        txt_put(POP_X + 2, POP_Y + 11, buf[0] ? "or A on GBA to close" : "A on GBA to close", TXT_BLUE);
    else if (buf[0])
        txt_put(POP_X + 2, POP_Y + 11, "to close", TXT_BLUE);
}

static void popup_draw(void)
{
    txt_clear_rect(POP_X, POP_Y, POP_W, POP_H);
    panel_draw(POP_X, POP_Y, POP_W, POP_H, PANEL_CREAM);
    if (pop.icon < ICON_ARROW)
        txt_icon(POP_X + 2, POP_Y + 1, pop.icon);
    txt_put(POP_X + 4, POP_Y + 1, pop.title[0] ? pop.title : "Message", TXT_ORANGE);
    draw_wrapped(POP_X + 2, POP_Y + 3, pop.text, TXT_DARK);
    draw_hint();
}

static void popup_blink(void)
{
    SCR_ENTRY *se = &se_mem[30][(POP_Y + POP_H - 2) * 32 + POP_X + POP_W - 3];
    if ((g_frame & 31) < 20)
        txt_icon(POP_X + POP_W - 3, POP_Y + POP_H - 2, ICON_ARROW);
    else
        *se = 0;
}

/* ---- demo feed ----------------------------------------------------------
 * Build with `make demo` to get chibi_link_demo_mb.gba, which fakes the
 * GameCube side (through the real packet parser) so the screens can be
 * checked in mGBA without a GameCube.
 */
#ifdef CL_DEMO
static void demo_send(u32 type, const u32 *p, u32 len)
{
    u32 h = CL_HEADER(type, len);
    rx_word(h);
    for (u32 i = 0; i < len; i++)
        rx_word(p[i]);
    rx_word(CL_Checksum(h, p, len));
    g_last_rx_frame = g_frame;
}

static void demo_tick(void)
{
    static const u16 tour[] = { 7, 7, 7, 1, 2, 4, 6, 3, 9, 11, 5, 99 };

    u32 f = g_frame;

    if (f == 30) {
        u32 hello[2] = { CL_PROTOCOL_VERSION, ('G' << 24) | ('G' << 16) | ('T' << 8) | 'E' };
        demo_send(CL_PKT_HELLO, hello, 2);
    }

    if (f < 90 || (f & 7))
        return;

    // Yay math..
    u32 t = f - 90;
    u32 room = tour[(t / 240) % (sizeof(tour) / sizeof(tour[0]))];
    u32 bat = 1000 - (t / 4) % 1000;

    u32 flags = CL_FLAG_PLAYING | CL_FLAG_MAX_VALID | CL_FLAG_TIME_VALID | CL_FLAG_NIGHT_VALID | CL_FLAG_COIN_PINS;
    
    if (t % 1200 >= 840 && t % 1200 < 1140)   /* pretend to load an area: shows the walk animation */
        flags = (flags & ~CL_FLAG_PLAYING) | CL_FLAG_LOADING;

    u32 minutes = (8 * 60 + t / 8) % 1440;

    if (minutes >= 20 * 60 || minutes < 6 * 60)
        flags |= CL_FLAG_NIGHT;

    u32 p[CL_STATUS_WORDS];
    p[CL_STATUS_BATTERY] = bat | (1000u << 16);
    p[CL_STATUS_MOOLAH]  = 1250 + t / 16;
    p[CL_STATUS_HAPPY]   = 34120 + t;
    p[CL_STATUS_ROOM]    = room | (flags << 16);

    /* walk a loop; facing follows the direction of travel */
    u32 a = (t * 96) & 0xFFFF;
    s32 wx = (lu_sin(a) * 220) >> 12, wz = (lu_cos(a) * 280) >> 12;
    p[CL_STATUS_POS]     = ((u32)(wx - 10) & 0xFFFF) | ((u32)(wz) << 16);
    flags |= CL_FLAG_POS_VALID;
    p[CL_STATUS_ROOM]    = room | (flags << 16);
    p[CL_STATUS_FACING]  = (a + 0x4000) & 0xFFFF;
    /* check the room's pins one by one, then start over */
    u32 n = (t / 60) % 40;
    p[CL_STATUS_PINS]    = n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1;
    p[CL_STATUS_PINS_HI] = n >= 32 ? (1u << (n - 32)) - 1 : 0;
    p[CL_STATUS_CDOORS]  = n >= 20 ? 1 : 0;
    p[CL_STATUS_TIME]    = 3 | (minutes << 16);

    /* a popup every 20 s, "dismissed on the GameCube" 6 s later */
    static u32 demo_seq;
    u32 phase = t % 1200;
    if (phase >= 240 && phase < 248) {
        static const char title[16] = "Chibi-Robo!";
        static const char text[CL_MSG_TEXT_MAX] =
            "Hi! This popup came from the GameCube. Press A to close it and get back to work!";
        u32 m[CL_MSG_WORDS(CL_MSG_TEXT_MAX)];
        demo_seq = demo_seq % 255 + 1;
        m[0] = demo_seq | (CL_ICON_PLUG << 8) | ((CL_HINT_GC_A | CL_HINT_GC_B | CL_HINT_GBA_A) << 16);
        memcpy(&m[1], title, 16);
        memcpy(&m[5], text, CL_MSG_TEXT_MAX);
        demo_send(CL_PKT_MESSAGE, m, CL_MSG_WORDS(CL_MSG_TEXT_MAX));
    }
    
    p[CL_STATUS_MSG] = (phase >= 240 && phase < 600) ? demo_seq : 0;
    {
        static u32 demo_moolah = 1250, demo_happy = 34120, demo_scrap = 12, demo_seq = 0xFF, demo_refill = 0;
        if (tool.pending && tool.seq != demo_seq && g_frame - tool.sent_frame > 20) {
            demo_seq = tool.seq;
            switch (tool.pending) {
            case CL_CMD_MOOLAH_100:  demo_moolah += 100;  break;
            case CL_CMD_MOOLAH_1000: demo_moolah += 1000; break;
            case CL_CMD_HAPPY_100:   demo_happy += 100;   break;
            case CL_CMD_HAPPY_1000:  demo_happy += 1000;  break;
            case CL_CMD_SCRAP_10:    demo_scrap += 10;    break;
            case CL_CMD_SCRAP_100:   demo_scrap += 100;   break;
            case CL_CMD_SHOP_BATTERY:
                if (demo_moolah >= CL_SHOP_BATTERY_COST) {
                    demo_moolah -= CL_SHOP_BATTERY_COST;
                    demo_refill = t;
                }
                break;
            }
        }
        p[CL_STATUS_BATTERY] = (1000 - (t - demo_refill) / 4 % 1000) | (1000u << 16);
        p[CL_STATUS_MOOLAH] = demo_moolah;
        p[CL_STATUS_HAPPY]  = demo_happy;
        p[CL_STATUS_SCRAP]  = demo_scrap;
        p[CL_STATUS_CMD]    = demo_seq == 0xFF ? 0 : (demo_seq | (CL_CMD_OK << 8) | CL_CMD_ACK_VALID);
    }
    demo_send(CL_PKT_STATUS, p, CL_STATUS_WORDS);
}
#endif

/* ---- main -------------------------------------------------------------- */

int main(void)
{
    joybus_init();
    gfx_init();
    snd_init();

    draw_page();
    draw_home_values();
    draw_message_box();
    gfx_fade(0, 16);

    u32 last_ok = ~0u, last_reset = g_reset_count;
    int last_link = -1;

    for (;;) {
        u32 w;
        if (g_reset_count != last_reset) {
            last_reset = g_reset_count;
            rx.state = 0;
        }
        while (joybus_pop(&w))
            rx_word(w);
#ifdef CL_DEMO
        demo_tick();
        if (g_frame % 600 == 599 && !pop.active) { /* flip pages every 10 s */
            page = (page + 1) % PAGE_COUNT;
            draw_page();
            last_ok = ~0u;
        }
#endif

        if (pkt_ok && g_frame - g_last_rx_frame < LINK_TIMEOUT_FRAMES) {
            if (link_state != LINK_UP && have_status)
                link_state = LINK_UP;
        } else if (link_state == LINK_UP) {
            link_state = LINK_LOST;
        }

        key_poll();

        /* popup open / close */
        if (pop.pending_close) {
            pop.pending_close = 0;
            if (pop.active) {
                pop.active = 0;
                pop.closed_seq = pop.seq;
                rmap.drawn_stage = -1;
                draw_page();
                last_ok = ~0u;
            }
        }
        if (pop.pending_open) {
            pop.pending_open = 0;
            pop.active = 1;
            popup_draw();
            snd_chime();
        }
        if (pop.active && (pop.hint & CL_HINT_GBA_A) && key_hit(KEY_A))
            pop.pending_close = 1; /* the GC also sees A in our reply and dismisses */

        if (pop.active) {
            popup_blink();
        } else if (key_hit(KEY_R | KEY_RIGHT)) {
            page = (page + 1) % PAGE_COUNT;
            if (!tool.pending)
                tool.msg = "";
            draw_page();
            last_ok = ~0u;
        } else if (key_hit(KEY_L | KEY_LEFT)) {
            page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
            if (!tool.pending)
                tool.msg = "";
            draw_page();
            last_ok = ~0u;
        } else if (page == PAGE_TOOLS) {
            tools_input();
        } else if (page == PAGE_SHOP) {
            shop_input();
        }
        tools_update();

        if (!pop.active && (pkt_ok != last_ok || link_state != last_link)) {
            if (page == PAGE_HOME)
                draw_home_values();
            else if (page == PAGE_ROOM)
                draw_room_values();
            else if (page == PAGE_SHOP)
                draw_shop_values();
            else if (page == PAGE_TOOLS)
                draw_tools_values();
            else
                draw_info_values();
            last_ok = pkt_ok;
            last_link = link_state;
        }
        if ((g_frame & 15) == 0 || link_state != last_link)
            draw_message_box();

        update_blip();
        update_room_sprites();

        /* loading animation: Chibi-Robo walks along the bottom of the left panel while we
           wait for the GameCube, or while the game is loading an area */
        {
            static int walk_x = 12, walk_dir = 1;
            int waiting = link_state != LINK_UP || !have_status || (st.flags & CL_FLAG_LOADING);
            int max_x = page == PAGE_ROOM ? 8 + 104 - 34 : 8 + 128 - 34;
            if (waiting && !pop.active) {
                if ((g_frame & 1) == 0) {
                    walk_x += walk_dir;
                    if (walk_x >= max_x) { walk_x = max_x; walk_dir = -1; }
                    if (walk_x <= 10)    { walk_x = 10;    walk_dir = 1; }
                }
                spr_walk(walk_x, 24 + 96 - 34, (g_frame / 8) & 3, walk_dir > 0, 1);
            } else {
                spr_walk(0, 0, 0, 0, 0);
            }
        }
        snd_update();
        joybus_set_reply(CL_REPLY(~REG_KEYINPUT & KEY_MASK, CL_REPLY_READY | (page << 1)) |
                         CL_REPLY_CMD(tool.seq, tool.pending));
        gfx_vsync();
    }
}
