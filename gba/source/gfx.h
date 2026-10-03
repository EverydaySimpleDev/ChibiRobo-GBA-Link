#ifndef GFX_H
#define GFX_H

#include <tonc_types.h>

/*
 * Screen layers (mode 0, 30x20 tiles visible):
 *   BG0  text / icons / bars      (charblock 0, screenblock 30, priority 0)
 *   BG1  panel frames + map canvas (charblock 1, screenblock 29, priority 1)
 *   BG2  scrolling backdrop pattern (charblock 2, screenblock 28, priority 2)
 *   OBJ  Chibi-Robo sprite + map blip
 */

/* BG0 palette banks for text */
enum {
    TXT_WHITE  = 1,  /* white ink, navy shadow  - on blue backdrop   */
    TXT_DARK   = 2,  /* brown ink, cream shadow - on cream panels     */
    TXT_ORANGE = 3,  /* orange ink              - headings on cream   */
    TXT_YELLOW = 4,  /* yellow ink, navy shadow - titles on blue      */
    TXT_BLUE   = 10, /* navy ink, cream shadow  - values on cream     */
};

/* Panel styles for BG1 frames */
enum { PANEL_CREAM = 0, PANEL_BLUE = 5 };

/* 8x8 icons drawn on BG0 */
/* order matches CL_ICON_* in chibi_link_protocol.h */
enum { ICON_BATTERY, ICON_COIN, ICON_HEART, ICON_HOUSE, ICON_CLOCK, ICON_PLUG, ICON_ARROW, ICON_PIN,
       ICON_CDOOR, ICON_COUNT };

/* Map canvas: up to 16x12 tiles = 128x96 pixels, lives on BG1. The width is
   set per page with canvas_set_width() (HOME map 16 tiles, ROOM map 13). */
#define CANVAS_TW  16
#define CANVAS_TH  12
#define CANVAS_W   (CANVAS_TW * 8)
#define CANVAS_H   (CANVAS_TH * 8)

/* Canvas colours (palette bank 6 indices) */
enum {
    MC_BG = 1, MC_FLOOR, MC_WALL, MC_HI_FLOOR, MC_HI_WALL, MC_LABEL, MC_HI_LABEL,
    MC_DOOR, MC_GRASS, MC_GRASS_WALL, MC_WATER, MC_GRID,
    MC_ITEM, MC_OUTLET, MC_CDOOR,
};

void gfx_init(void);
void gfx_vsync(void);              /* wait for VBlank, then flush OAM, canvas and scroll */
void gfx_fade(int from, int to);   /* 0 = black, 16 = normal */

/* BG0 text layer */
void txt_clear(void);
void txt_clear_rect(int x, int y, int w, int h);
void txt_put(int x, int y, const char *s, int bank);
void txt_put_right(int right_x, int y, const char *s, int bank); /* s ends at column right_x-1 */
void txt_big(int x, int y, const char *s, int bank);             /* 8x16 digits, 2 rows tall */
void txt_big_right(int right_x, int y, const char *s, int bank);
void txt_icon(int x, int y, int icon);
void txt_bar(int x, int y, int w_tiles, u32 value, u32 max);

/* BG1 panel layer */
void panel_clear(void);
void panel_draw(int x, int y, int w, int h, int style);
void panel_canvas_place(int x, int y); /* map the canvas tiles at tile (x,y) */
void canvas_set_width(int tiles);      /* 1..CANVAS_TW, clears the canvas     */
int  canvas_width(void);               /* current width in pixels             */

/* Map canvas drawing (flushed to VRAM on next gfx_vsync) */
void canvas_clear(int c);
void canvas_rect(int x, int y, int w, int h, int c);
void canvas_frame(int x, int y, int w, int h, int c);
void canvas_pixel(int x, int y, int c);
void canvas_text(int x, int y, const char *s, int c); /* 3x5 pixel font, uppercase */
int  canvas_text_width(const char *s);
/* small icon from strings: '.' skip, R item red, G outlet green, P chibi-door purple,
   W light, K dark, Y yellow; rows of `w` chars, centred on x,y */
void canvas_icon(int x, int y, const char *rows, int w, int h);

/* Sprites */
void spr_chibi(int x, int y, int visible);   /* 16x16 Chibi-Robo head */
void spr_blip(int x, int y, int visible);    /* 8x8 map marker        */
void spr_player(int cx, int cy, u16 facing, int visible); /* rotating arrow, centred on cx,cy */
void spr_pin(int i, int x, int y, int visible);           /* item pin i (0..7), tip at x,y   */
void spr_walk(int x, int y, int frame, int flip, int visible); /* 32x32 walking Chibi-Robo (faces left, flip = face right) */

/* Two-note chime on DMG square channel 1 (call snd_update every frame) */
void snd_init(void);
void snd_chime(void);
void snd_update(void);

/* Number to decimal string with thousands separators (buf >= 16 chars) */
char *fmt_u32(char *buf, u32 v, int commas);

#endif
