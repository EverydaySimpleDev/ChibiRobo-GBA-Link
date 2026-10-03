#ifndef ROOMS_H
#define ROOMS_H

#include <tonc_types.h>

enum { ROOM_INDOOR, ROOM_OUTDOOR, ROOM_WATER };

typedef struct {
    u16 id;              /* stage id sent by the GameCube (CL_STATUS_ROOM lo16) */
    u8 kind;             /* ROOM_* */
    u8 hidden;           /* alias of another room: only drawn while it is current */
    const char *name;    /* full name, shown in the message box */
    const char *label;   /* short label drawn on the map (3x5 font) */
    s16 x, y, w, h;      /* rectangle on the 128x96 map canvas */
    s16 min_x, max_x, min_z, max_z;
} Room;

typedef struct { s16 x, y, w, h; } Door;

extern const Room g_rooms[];
extern const int g_room_count;
extern const Door g_doors[];
extern const int g_door_count;

const Room *room_find(u16 id);

#endif
