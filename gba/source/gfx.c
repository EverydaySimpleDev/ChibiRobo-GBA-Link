/*
 * Procedural graphics for the Chibi-Robo! GBA Link screen.
 * Every tile is generated at boot so the multiboot image stays tiny.
 */
#include <tonc.h>
#include <string.h>
#include "gfx.h"

extern const unsigned int sys8Glyphs[192]; /* libtonc 8x8 font, ASCII 32..127 */

/* ---- VRAM layout ------------------------------------------------------- */

#define CB_TEXT   0
#define CB_PANEL  1
#define CB_BACK   2
#define SB_TEXT   30
#define SB_PANEL  29
#define SB_BACK   28

/* BG0 tile indices */
#define T_GLYPH     0    /* 96 glyphs, index = ch - 32 (tile 0 = space)  */
#define T_BIG       96   /* big glyphs: 2 tiles each (top, bottom)        */
#define BIG_CHARS   "0123456789/W,"
#define T_ICON      (T_BIG + 2 * 13)
#define T_BAR       (T_ICON + ICON_COUNT) /* 9 fill levels (0..8 px)       */

/* BG1 tile indices */
#define T_FRAME     1    /* 9-slice, 3x3 */
#define T_CANVAS    16

/* OBJ tile indices */
#define O_CHIBI     0    /* 16x16 = 4 tiles */
#define O_BLIP      4
#define O_ARROW     5    /* 16x16 = 4 tiles */
#define O_PIN       9
#define O_WALK      16   /* 4 frames x 16 tiles (32x32) */

#define RGB8(r, g, b) RGB15((r) >> 3, (g) >> 3, (b) >> 3)

static OBJ_ATTR obj_buf[128];
extern const unsigned int chibi_walk_tiles[512]; /* chibi_walk.c (tools/gen_sprites.py) */
extern const unsigned int chibi_head_tiles[32];  /* chibi_head.c (gba/art/chibi_head.png) */
static u32 canvas[CANVAS_TW * CANVAS_TH * 8];
static int canvas_dirty;
static int canvas_tw = CANVAS_TW;
static int back_scroll;

/* ---- small tile helpers ------------------------------------------------ */

static inline void tpix(u32 *t, int x, int y, int c)
{
    t[y] = (t[y] & ~(0xFu << (x * 4))) | ((u32)c << (x * 4));
}

static inline int tget(const u32 *t, int x, int y)
{
    return (t[y] >> (x * 4)) & 0xF;
}

static void tile_upload(int cb, int idx, const u32 *t)
{
    memcpy32(&tile_mem[cb][idx], t, 8);
}

/* sys8 glyph row: one byte per row, bit 0 = leftmost pixel */
static inline int glyph_bit(int ch, int x, int y)
{
    const u8 *g = (const u8 *)sys8Glyphs + (ch - 32) * 8;
    return (g[y] >> x) & 1;
}

/* ---- palettes ---------------------------------------------------------- */

static void pal_text(int bank, COLOR ink, COLOR shadow)
{
    pal_bg_mem[bank * 16 + 1] = ink;
    pal_bg_mem[bank * 16 + 2] = shadow;
}

static void palettes_init(void)
{
    COLOR *p = pal_bg_mem;

    /* bank 0: main UI palette (icons, cream frames, backdrop) */
    p[0]  = RGB8(24, 40, 96);     /* backdrop                 */
    p[1]  = RGB8(26, 28, 44);     /* outline                  */
    p[2]  = RGB8(255, 255, 255);  /* white                    */
    p[3]  = RGB8(176, 184, 208);  /* silver                   */
    p[4]  = RGB8(255, 244, 214);  /* cream                    */
    p[5]  = RGB8(236, 212, 158);  /* cream shade              */
    p[6]  = RGB8(248, 136, 40);   /* plug orange              */
    p[7]  = RGB8(176, 80, 24);    /* dark orange              */
    p[8]  = RGB8(56, 104, 200);   /* backdrop blue            */
    p[9]  = RGB8(44, 84, 172);    /* backdrop blue dark       */
    p[10] = RGB8(84, 136, 224);   /* backdrop blue light      */
    p[11] = RGB8(72, 208, 88);    /* green                    */
    p[12] = RGB8(248, 208, 48);   /* yellow                   */
    p[13] = RGB8(232, 64, 64);    /* red                      */
    p[14] = RGB8(255, 160, 184);  /* pink                     */
    p[15] = RGB8(32, 40, 72);     /* eye navy                 */

    pal_text(TXT_WHITE,  RGB8(255, 255, 255), RGB8(20, 28, 64));
    pal_text(TXT_DARK,   RGB8(88, 52, 24),    RGB8(236, 212, 158));
    pal_text(TXT_ORANGE, RGB8(232, 112, 24),  RGB8(255, 214, 160));
    pal_text(TXT_YELLOW, RGB8(255, 224, 72),  RGB8(20, 28, 64));
    pal_text(TXT_BLUE,   RGB8(32, 64, 152),   RGB8(236, 212, 158));

    /* bank 5: blue frame variant of the 9-slice (same tiles, other colours) */
    p[5 * 16 + 1] = RGB8(16, 20, 40);
    p[5 * 16 + 4] = RGB8(20, 44, 104);
    p[5 * 16 + 6] = RGB8(136, 184, 248);
    p[5 * 16 + 7] = RGB8(56, 96, 192);

    /* bank 6: map canvas */
    p[6 * 16 + MC_BG]         = RGB8(20, 44, 104);
    p[6 * 16 + MC_FLOOR]      = RGB8(150, 186, 236);
    p[6 * 16 + MC_WALL]       = RGB8(232, 244, 255);
    p[6 * 16 + MC_HI_FLOOR]   = RGB8(252, 184, 72);
    p[6 * 16 + MC_HI_WALL]    = RGB8(255, 244, 168);
    p[6 * 16 + MC_LABEL]      = RGB8(24, 48, 112);
    p[6 * 16 + MC_HI_LABEL]   = RGB8(112, 48, 8);
    p[6 * 16 + MC_DOOR]       = RGB8(96, 132, 200);
    p[6 * 16 + MC_GRASS]      = RGB8(96, 184, 88);
    p[6 * 16 + MC_GRASS_WALL] = RGB8(200, 240, 160);
    p[6 * 16 + MC_WATER]      = RGB8(72, 144, 216);
    p[6 * 16 + MC_GRID]       = RGB8(28, 56, 124);
    p[6 * 16 + MC_ITEM]       = RGB8(232, 40, 56);
    p[6 * 16 + MC_OUTLET]     = RGB8(56, 200, 88);
    p[6 * 16 + MC_CDOOR]      = RGB8(168, 88, 232);

    /* banks 7..9: battery bar (1 = fill, 2 = empty, 3 = rim) */
    for (int b = 7; b <= 9; b++) {
        p[b * 16 + 2] = RGB8(64, 72, 96);
        p[b * 16 + 3] = RGB8(26, 28, 44);
    }
    p[7 * 16 + 1] = RGB8(72, 216, 88);
    p[8 * 16 + 1] = RGB8(248, 208, 48);
    p[9 * 16 + 1] = RGB8(240, 64, 56);

    /* OBJ bank 0 */
    COLOR *o = pal_obj_mem;
    o[1] = RGB8(26, 28, 44);
    o[2] = RGB8(255, 255, 255);
    o[3] = RGB8(176, 184, 208);
    o[4] = RGB8(32, 40, 72);
    o[5] = RGB8(248, 136, 40);
    o[6] = RGB8(255, 232, 96);
    o[7] = RGB8(120, 200, 255);
    o[11] = RGB8(72, 208, 88);
    o[13] = RGB8(232, 48, 48);
    o[14] = RGB8(255, 170, 190);
    o[8] = RGB8(120, 128, 156);
    o[9] = RGB8(216, 222, 236);
}

/* ---- art from strings -------------------------------------------------- */

/* '.' transparent, K outline, W white, S silver, E eye, O orange,
   Y yellow, R red, G green, C cream, D dark orange, P pink, B blue */
static int art_color(char ch, int obj)
{
    switch (ch) {
    case 'K': return 1;
    case 'W': return 2;
    case 'S': return 3;
    case 'E': return obj ? 4 : 15;
    case 'O': return obj ? 5 : 6;
    case 'Y': return obj ? 6 : 12;
    case 'B': return obj ? 7 : 10;
    case 'C': return 4;
    case 'D': return 7;
    case 'G': return 11;
    case 'R': return 13;
    case 'P': return 14;
    case 'M': return obj ? 8 : 3;   /* mid grey (metal shading)   */
    case 'L': return obj ? 9 : 2;   /* light grey (metal highlight) */
    default:  return 0;
    }
}

static void art_tile(u32 *t, const char *rows, int stride, int ox, int oy, int obj)
{
    memset(t, 0, 32);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            tpix(t, x, y, art_color(rows[(oy + y) * stride + ox + x], obj));
}

// Basic icons
static const char icon_art[ICON_COUNT][65] = {
    /* battery */
    "..KKKK.."
    ".KGGGGK."
    ".KGWGGK."
    ".KGGGGK."
    ".KGGGGK."
    ".KYYYYK."
    ".KYYYYK."
    "..KKKK..",
    /* coin (moolah) */
    "..KKKK.."
    ".KYYYYK."
    "KYWYYYOK"
    "KYYOOYOK"
    "KYYOOYOK"
    "KYYYYYOK"
    ".KOOOOK."
    "..KKKK..",
    /* heart (happy points) */
    ".KK..KK."
    "KPPKKRRK"
    "KPWPRRRK"
    "KPPRRRRK"
    ".KRRRRK."
    "..KRRK.."
    "...KK..."
    "........",
    /* house */
    "...KK..."
    "..KOOK.."
    ".KOOOOK."
    "KOOOOOOK"
    ".KWWWWK."
    ".KWKKWK."
    ".KWKKWK."
    ".KKKKKK.",
    /* clock */
    "..KKKK.."
    ".KWWWWK."
    "KWWKWWWK"
    "KWWKWWWK"
    "KWWKKKWK"
    "KWWWWWWK"
    ".KWWWWK."
    "..KKKK..",
    /* plug */
    ".K...K.."
    ".K...K.."
    "KKKKKKK."
    "KOOOOOK."
    "KOOOOOK."
    ".KOOOK.."
    "..KSK..."
    "...K....",
    /* arrow (popup "more / close") */
    "........"
    "KKKKKKK."
    "KOOOOOK."
    ".KOOOK.."
    ".KOOOK.."
    "..KOK..."
    "..KOK..."
    "...K....",
    /* item pin (map legend) */
    "..KKKK.."
    ".KRRRRK."
    "KRRWPRRK"
    "KRRPRRRK"
    ".KRRRRK."
    "..KRRK.."
    "...KK..."
    "...K....",
    /* chibi-door (map legend) */
    "..KKKK.."
    ".KBBBBK."
    "KBBBBBBK"
    "KBBBBBBK"
    "KBBBBYBK"
    "KBBBBBBK"
    "KBBBBBBK"
    "KKKKKKKK",
};

/* The title-bar head is gba/art/chibi_head.png, converted by tools/gen_sprites.py into
   chibi_head.c (edit the PNG, then run build.bat). */


/* player marker, pointing down = facing 0 (+Z) before rotation */
static const char arrow_art[16 * 16 + 1] =
    "................"
    "................"
    "................"
    "......KKKK......"
    ".....KWWWWK....."
    "....KWWWWWWK...."
    "....KWWSSWWK...."
    "....KWWSSWWK...."
    "....KWWWWWWK...."
    ".....KWWWWK....."
    "....KKOOOOKK...."
    ".....KOOOOK....."
    "......KOOK......"
    ".......KK......."
    "................"
    "................";

static const char pin_art[65] =
    "..KKKK.."
    ".KRRRRK."
    "KRRWPRRK"
    "KRRPRRRK"
    ".KRRRRK."
    "..KRRK.."
    "...KK..."
    "...K....";

static const char blip_art[65] =
    "..KKKK.."
    ".KOOOOK."
    "KOYYYOOK"
    "KOYWYOOK"
    "KOYYYOOK"
    "KOOOOOOK"
    ".KOOOOK."
    "..KKKK..";

/* ---- tile generation --------------------------------------------------- */

static void make_text_tiles(void)
{
    u32 t[8];

    /* normal glyphs with a 1px drop shadow */
    for (int ch = 32; ch < 128; ch++) {
        memset(t, 0, 32);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (glyph_bit(ch, x, y)) {
                    tpix(t, x, y, 1);
                    if (x < 7 && y < 7 && !glyph_bit(ch, x + 1, y + 1))
                        tpix(t, x + 1, y + 1, 2);
                }
        /* shadow must not overwrite ink placed later in scan order */
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (glyph_bit(ch, x, y))
                    tpix(t, x, y, 1);
        tile_upload(CB_TEXT, T_GLYPH + ch - 32, t);
    }

    /* big glyphs: sys8 scaled 1x2, emboldened, with shadow */
    const char *big = BIG_CHARS;
    for (int i = 0; big[i]; i++) {
        int ch = big[i];
        u8 ink[16][8];
        memset(ink, 0, sizeof(ink));
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 7; x++)
                if (glyph_bit(ch, x, y >> 1) || (x > 0 && glyph_bit(ch, x - 1, y >> 1)))
                    ink[y][x] = 1;
        u32 top[8], bot[8];
        memset(top, 0, 32);
        memset(bot, 0, 32);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++) {
                int c = 0;
                if (ink[y][x])
                    c = 1;
                else if (x > 0 && y > 0 && ink[y - 1][x - 1])
                    c = 2;
                if (c)
                    tpix(y < 8 ? top : bot, x, y & 7, c);
            }
        tile_upload(CB_TEXT, T_BIG + i * 2, top);
        tile_upload(CB_TEXT, T_BIG + i * 2 + 1, bot);
    }

    for (int i = 0; i < ICON_COUNT; i++) {
        art_tile(t, icon_art[i], 8, 0, 0, 0);
        tile_upload(CB_TEXT, T_ICON + i, t);
    }

    /* battery bar segments, fill level 0..8 pixels */
    for (int f = 0; f <= 8; f++) {
        for (int y = 0; y < 8; y++) {
            t[y] = 0;
            for (int x = 0; x < 8; x++) {
                int c = (y == 0 || y == 7) ? 3 : (x < f ? 1 : 2);
                tpix(t, x, y, c);
            }
        }
        tile_upload(CB_TEXT, T_BAR + f, t);
    }
}

/* Rounded frame: render a 24x24 box and cut it into a 3x3 9-slice. */
static int frame_pixel(int x, int y)
{
    const int n = 24, r = 6;
    int cx = x < r ? r : (x >= n - r ? n - 1 - r : x);
    int cy = y < r ? r : (y >= n - r ? n - 1 - r : y);
    int dx = x - cx, dy = y - cy;
    int d2 = dx * dx + dy * dy;
    int edge; /* distance from the outside, in pixels */

    if (dx || dy) {
        if (d2 > r * r)
            return 0;
        /* integer approximation of r - sqrt(d2) */
        edge = 0;
        while (edge < r && (r - edge - 1) * (r - edge - 1) >= d2)
            edge++;
    } else {
        int ex = x < n - 1 - x ? x : n - 1 - x;
        int ey = y < n - 1 - y ? y : n - 1 - y;
        edge = ex < ey ? ex : ey;
    }
    if (edge < 1) return 1;       /* outline      */
    if (edge < 3) return 6;       /* border       */
    if (edge < 4) return 7;       /* border shade */
    if (edge < 5) return 1;       /* inner line   */
    return 4;                     /* interior     */
}

static void make_panel_tiles(void)
{
    u32 t[8];
    memset(t, 0, 32);
    tile_upload(CB_PANEL, 0, t);

    for (int ty = 0; ty < 3; ty++)
        for (int tx = 0; tx < 3; tx++) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    tpix(t, x, y, frame_pixel(tx * 8 + x, ty * 8 + y));
            tile_upload(CB_PANEL, T_FRAME + ty * 3 + tx, t);
        }
}

/* Diagonal lattice backdrop, 16x16 px (2x2 tiles). */
static void make_back_tiles(void)
{
    u32 t[8];
    for (int ty = 0; ty < 2; ty++)
        for (int tx = 0; tx < 2; tx++) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int px = tx * 8 + x, py = ty * 8 + y;
                    int a = (px + py) & 15, b = (px - py + 16) & 15;
                    int c = 8;
                    if (a == 0 || b == 0)
                        c = 10;
                    else if (a == 1 || b == 1)
                        c = 9;
                    tpix(t, x, y, c);
                }
            tile_upload(CB_BACK, ty * 2 + tx, t);
        }

    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            se_mem[SB_BACK][y * 32 + x] = ((y & 1) << 1) | (x & 1);
}

static void make_obj_tiles(void)
{
    u32 t[8];
    memcpy32(&tile_mem[4][O_CHIBI], chibi_head_tiles, 32);
    art_tile(t, blip_art, 8, 0, 0, 1);
    memcpy32(&tile_mem[4][O_BLIP], t, 8);
    for (int i = 0; i < 4; i++) {
        art_tile(t, arrow_art, 16, (i & 1) * 8, (i >> 1) * 8, 1);
        memcpy32(&tile_mem[4][O_ARROW + i], t, 8);
    }
    art_tile(t, pin_art, 8, 0, 0, 1);
    memcpy32(&tile_mem[4][O_PIN], t, 8);
    memcpy32(&tile_mem[4][O_WALK], chibi_walk_tiles, 512);
}

/* ---- public API -------------------------------------------------------- */

void gfx_init(void)
{
    REG_DISPCNT = DCNT_BLANK;

    palettes_init();
    make_text_tiles();
    make_panel_tiles();
    make_back_tiles();
    make_obj_tiles();

    txt_clear();
    panel_clear();
    canvas_clear(MC_BG);

    oam_init(obj_buf, 128);

    REG_BG0CNT = BG_CBB(CB_TEXT)  | BG_SBB(SB_TEXT)  | BG_4BPP | BG_REG_32x32 | BG_PRIO(0);
    REG_BG1CNT = BG_CBB(CB_PANEL) | BG_SBB(SB_PANEL) | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);
    REG_BG2CNT = BG_CBB(CB_BACK)  | BG_SBB(SB_BACK)  | BG_4BPP | BG_REG_32x32 | BG_PRIO(2);

    REG_BLDCNT = BLD_BUILD(BLD_BG0 | BLD_BG1 | BLD_BG2 | BLD_OBJ | BLD_BACKDROP, 0, 3);
    REG_BLDY = 16;

    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
}

void gfx_vsync(void)
{
    VBlankIntrWait();
    oam_copy(oam_mem, obj_buf, 128);
    if (canvas_dirty) {
        memcpy32(&tile_mem[CB_PANEL][T_CANVAS], canvas, sizeof(canvas) / 4);
        canvas_dirty = 0;
    }
    back_scroll++;
    REG_BG2HOFS = back_scroll >> 2;
    REG_BG2VOFS = back_scroll >> 2;
}

void gfx_fade(int from, int to)
{
    int step = from < to ? 1 : -1;
    for (int v = from; v != to + step; v += step) {
        REG_BLDY = 16 - v;
        gfx_vsync();
    }
    if (to == 16)
        REG_BLDCNT = 0;
    else
        REG_BLDCNT = BLD_BUILD(BLD_BG0 | BLD_BG1 | BLD_BG2 | BLD_OBJ | BLD_BACKDROP, 0, 3);
}

void txt_clear(void)
{
    memset32(se_mem[SB_TEXT], 0, 32 * 32 / 2);
}

void txt_clear_rect(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            se_mem[SB_TEXT][(y + j) * 32 + x + i] = 0;
}

void txt_put(int x, int y, const char *s, int bank)
{
    SCR_ENTRY *row = &se_mem[SB_TEXT][y * 32];
    for (; *s && x < 30; s++, x++) {
        int ch = (u8)*s;
        if (ch < 32 || ch > 127)
            ch = '?';
        row[x] = (T_GLYPH + ch - 32) | SE_PALBANK(bank);
    }
}

void txt_put_right(int right_x, int y, const char *s, int bank)
{
    txt_put(right_x - (int)strlen(s), y, s, bank);
}

void txt_big(int x, int y, const char *s, int bank)
{
    const char *big = BIG_CHARS;
    for (; *s && x < 30; s++, x++) {
        const char *p = strchr(big, *s);
        if (!p || !*s) {
            se_mem[SB_TEXT][y * 32 + x] = 0;
            se_mem[SB_TEXT][(y + 1) * 32 + x] = 0;
            continue;
        }
        int t = T_BIG + (int)(p - big) * 2;
        se_mem[SB_TEXT][y * 32 + x] = t | SE_PALBANK(bank);
        se_mem[SB_TEXT][(y + 1) * 32 + x] = (t + 1) | SE_PALBANK(bank);
    }
}

void txt_big_right(int right_x, int y, const char *s, int bank)
{
    txt_big(right_x - (int)strlen(s), y, s, bank);
}

void txt_icon(int x, int y, int icon)
{
    se_mem[SB_TEXT][y * 32 + x] = (T_ICON + icon) | SE_PALBANK(0);
}

void txt_bar(int x, int y, int w_tiles, u32 value, u32 max)
{
    if (max == 0)
        max = 1;
    if (value > max)
        value = max;
    u32 px = (value * (u32)(w_tiles * 8) + max / 2) / max;
    int bank = value * 4 >= max ? 7 : (value * 8 >= max ? 8 : 9);
    for (int i = 0; i < w_tiles; i++) {
        int f = (int)px - i * 8;
        f = f < 0 ? 0 : (f > 8 ? 8 : f);
        se_mem[SB_TEXT][y * 32 + x + i] = (T_BAR + f) | SE_PALBANK(bank);
    }
}

void panel_clear(void)
{
    memset32(se_mem[SB_PANEL], 0, 32 * 32 / 2);
}

void panel_draw(int x, int y, int w, int h, int style)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = i == 0 ? 0 : (i == w - 1 ? 2 : 1);
            int sy = j == 0 ? 0 : (j == h - 1 ? 2 : 1);
            se_mem[SB_PANEL][(y + j) * 32 + x + i] = (T_FRAME + sy * 3 + sx) | SE_PALBANK(style);
        }
}

void panel_canvas_place(int x, int y)
{
    for (int j = 0; j < CANVAS_TH; j++)
        for (int i = 0; i < canvas_tw; i++)
            se_mem[SB_PANEL][(y + j) * 32 + x + i] = (T_CANVAS + j * canvas_tw + i) | SE_PALBANK(6);
}

/* ---- map canvas -------------------------------------------------------- */

void canvas_pixel(int x, int y, int c)
{
    if ((unsigned)x >= (unsigned)(canvas_tw * 8) || (unsigned)y >= CANVAS_H)
        return;
    u32 *t = &canvas[((y >> 3) * canvas_tw + (x >> 3)) * 8];
    tpix(t, x & 7, y & 7, c);
    canvas_dirty = 1;
}

void canvas_clear(int c)
{
    u32 fill = 0x11111111u * (u32)c;
    for (unsigned i = 0; i < sizeof(canvas) / 4; i++)
        canvas[i] = fill;
    /* faint blueprint grid */
    for (int y = 0; y < CANVAS_H; y += 8)
        for (int x = 0; x < canvas_tw * 8; x += 2)
            canvas_pixel(x, y, MC_GRID);
    for (int x = 0; x < canvas_tw * 8; x += 8)
        for (int y = 0; y < CANVAS_H; y += 2)
            canvas_pixel(x, y, MC_GRID);
    canvas_dirty = 1;
}

void canvas_rect(int x, int y, int w, int h, int c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            canvas_pixel(x + i, y + j, c);
}

void canvas_frame(int x, int y, int w, int h, int c)
{
    for (int i = 0; i < w; i++) {
        canvas_pixel(x + i, y, c);
        canvas_pixel(x + i, y + h - 1, c);
    }
    for (int j = 0; j < h; j++) {
        canvas_pixel(x, y + j, c);
        canvas_pixel(x + w - 1, y + j, c);
    }
}

void canvas_set_width(int tiles)
{
    canvas_tw = tiles < 1 ? 1 : (tiles > CANVAS_TW ? CANVAS_TW : tiles);
    canvas_clear(MC_BG);
}

int canvas_width(void)
{
    return canvas_tw * 8;
}

void canvas_icon(int x, int y, const char *rows, int w, int h)
{
    x -= w / 2;
    y -= h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int c;
            switch (rows[j * w + i]) {
            case 'R': c = MC_ITEM; break;
            case 'G': c = MC_OUTLET; break;
            case 'P': c = MC_CDOOR; break;
            case 'W': c = MC_WALL; break;
            case 'K': c = MC_BG; break;
            case 'Y': c = MC_HI_WALL; break;
            case 'O': c = MC_HI_FLOOR; break;   /* coin gold */
            case 'H': c = MC_HI_LABEL; break;   /* coin centre */
            default: continue;
            }
            canvas_pixel(x + i, y + j, c);
        }
}

/* 3x5 pixel font: 15 chars per glyph, rows top to bottom. */
static const char font3x5[][16] = {
    ".X.X.XXXXX.XX.X", /* A */ "XX.X.XXX.X.XXX.", /* B */
    ".XXX..X..X...XX", /* C */ "XX.X.XX.XX.XXX.", /* D */
    "XXXX..XX.X..XXX", /* E */ "XXXX..XX.X..X..", /* F */
    ".XXX..X.XX.X.XX", /* G */ "X.XX.XXXXX.XX.X", /* H */
    "XXX.X..X..X.XXX", /* I */ "..X..X..XX.X.X.", /* J */
    "X.XX.XXX.X.XX.X", /* K */ "X..X..X..X..XXX", /* L */
    "X.XXXXXXXX.XX.X", /* M */ "XX.X.XX.XX.XX.X", /* N */
    ".X.X.XX.XX.X.X.", /* O */ "XX.X.XXX.X..X..", /* P */
    ".X.X.XX.XXX..XX", /* Q */ "XX.X.XXX.X.XX.X", /* R */
    ".XXX...X...XXX.", /* S */ "XXX.X..X..X..X.", /* T */
    "X.XX.XX.XX.XXXX", /* U */ "X.XX.XX.XX.X.X.", /* V */
    "X.XX.XXXXXXXX.X", /* W */ "X.XX.X.X.X.XX.X", /* X */
    "X.XX.X.X..X..X.", /* Y */ "XXX..X.X.X..XXX", /* Z */
};

static const char *glyph3x5(char ch)
{
    static const char apos[16] = ".X..X..........";
    static const char dash[16] = "......XXX......";
    if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    if (ch >= 'A' && ch <= 'Z')
        return font3x5[ch - 'A'];
    if (ch == '\'')
        return apos;
    if (ch == '-')
        return dash;
    return 0;
}

int canvas_text_width(const char *s)
{
    int w = 0;
    for (; *s; s++)
        w += (*s == ' ' || *s == '\'') ? 2 : 4;
    return w ? w - 1 : 0;
}

void canvas_text(int x, int y, const char *s, int c)
{
    for (; *s; s++) {
        const char *g = glyph3x5(*s);
        if (g)
            for (int j = 0; j < 5; j++)
                for (int i = 0; i < 3; i++)
                    if (g[j * 3 + i] == 'X')
                        canvas_pixel(x + i, y + j, c);
        x += (*s == ' ' || *s == '\'') ? 2 : 4;
    }
}

/* ---- sprites ----------------------------------------------------------- */

void spr_chibi(int x, int y, int visible)
{
    OBJ_ATTR *o = &obj_buf[0];
    if (!visible) {
        obj_hide(o);
        return;
    }
    obj_set_attr(o, ATTR0_SQUARE, ATTR1_SIZE_16, ATTR2_ID(O_CHIBI) | ATTR2_PALBANK(0) | ATTR2_PRIO(0));
    obj_set_pos(o, x, y);
}

void spr_blip(int x, int y, int visible)
{
    OBJ_ATTR *o = &obj_buf[1];
    if (!visible) {
        obj_hide(o);
        return;
    }
    obj_set_attr(o, ATTR0_SQUARE, ATTR1_SIZE_8, ATTR2_ID(O_BLIP) | ATTR2_PALBANK(0) | ATTR2_PRIO(0));
    obj_set_pos(o, x, y);
}

/* ---- sound ------------------------------------------------------------ */

static int chime_t = -1;

void snd_init(void)
{
    REG_SNDSTAT = SSTAT_ENABLE;
    REG_SNDDMGCNT = SDMG_BUILD_LR(SDMG_SQR1, 7);
    REG_SNDDSCNT = SDS_DMG100;
    REG_SND1SWEEP = SSW_OFF;
    REG_SND1CNT = SSQR_ENV_BUILD(12, 0, 3) | SSQR_DUTY1_2;
    REG_SND1FREQ = 0;
}

void snd_chime(void)
{
    chime_t = 0;
}

void snd_update(void)
{
    if (chime_t < 0)
        return;
    if (chime_t == 0)
        REG_SND1FREQ = SFREQ_RESET | SND_RATE(NOTE_G, 1);
    else if (chime_t == 6)
        REG_SND1FREQ = SFREQ_RESET | SND_RATE(NOTE_C, 2);
    else if (chime_t > 6)
        chime_t = -2;
    chime_t++;
}

void spr_player(int cx, int cy, u16 facing, int visible)
{
    OBJ_ATTR *o = &obj_buf[2];
    if (!visible) {
        obj_hide(o);
        return;
    }
    /* affine matrix 0 lives in the fill fields of obj_buf[0..3]; same values
       as tonc's obj_aff_rotate (counter-clockwise rotation by `facing`) */
    s16 ss = lu_sin(facing) >> 4, cc = lu_cos(facing) >> 4;
    obj_buf[0].fill = cc;
    obj_buf[1].fill = -ss;
    obj_buf[2].fill = ss;
    obj_buf[3].fill = cc;
    obj_set_attr(o, ATTR0_SQUARE | ATTR0_AFF, ATTR1_SIZE_16 | ATTR1_AFF_ID(0),
                 ATTR2_ID(O_ARROW) | ATTR2_PALBANK(0) | ATTR2_PRIO(0));
    obj_set_pos(o, cx - 8, cy - 8);
}

void spr_pin(int i, int x, int y, int visible)
{
    if (i < 0 || i > 7)
        return;
    OBJ_ATTR *o = &obj_buf[3 + i];
    if (!visible) {
        obj_hide(o);
        return;
    }
    obj_set_attr(o, ATTR0_SQUARE, ATTR1_SIZE_8, ATTR2_ID(O_PIN) | ATTR2_PALBANK(0) | ATTR2_PRIO(0));
    obj_set_pos(o, x - 3, y - 7);
}

void spr_walk(int x, int y, int frame, int flip, int visible)
{
    OBJ_ATTR *o = &obj_buf[12];
    if (!visible) {
        obj_hide(o);
        return;
    }
    /* `flip` mirrors the art (gba/art/chibi_*.webp face left) */
    obj_set_attr(o, ATTR0_SQUARE, ATTR1_SIZE_32 | (flip ? ATTR1_HFLIP : 0),
                 ATTR2_ID(O_WALK + (frame & 3) * 16) | ATTR2_PALBANK(0) | ATTR2_PRIO(0));
    obj_set_pos(o, x, y);
}

/* ---- formatting -------------------------------------------------------- */

char *fmt_u32(char *buf, u32 v, int commas)
{
    char tmp[16];
    int n = 0, digits = 0;
    do {
        if (commas && digits && digits % 3 == 0)
            tmp[n++] = ',';
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
        digits++;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}
