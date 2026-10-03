/*
 * Stage table for the Sanderson house (GGTE01 stage ids, see
 * Chibi-Robo-Randomizer "chibi code docs.txt" / `unplug list stages`).
 *
 * Map rectangles are a schematic floor plan on the 128x96 canvas:
 *   top row     upstairs     - Jenny's Room, Bedroom
 *   middle row  ground floor - Kitchen, Living Room (+ Chibi-House), Foyer
 *   bottom row  basement + outside - Basement, Backyard (+ Drain, UFO)
 * Small boxes inside a room are separate stages you enter from it:
 * Chibi-House + Mother Spider's Room (Living Room), Drain + UFO (Backyard),
 * Bedroom (Past) (Bedroom).
 *
 * World extents (min/max X/Z) are 0 until someone measures them in Dolphin;
 * until then the blip sits in the middle of the current room.
 */
#include "rooms.h"

#define R(id, kind, hidden, name, label, x, y, w, h) \
    { id, kind, hidden, name, label, x, y, w, h, 0, 0, 0, 0 }

const Room g_rooms[] = {
    /* upstairs */
    R(4,  ROOM_INDOOR,  0, "Jenny's Room",         "JENNY'S ROOM", 2,  2,  60, 28),
    R(6,  ROOM_INDOOR,  0, "Bedroom",              "BEDROOM",      66, 2,  60, 28),
    R(18, ROOM_INDOOR,  0, "Bedroom (Past)",       "",             108, 16, 14, 10),

    /* ground floor */
    R(1,  ROOM_INDOOR,  0, "Kitchen",              "KITCHEN",      2,  34, 38, 28),
    R(7,  ROOM_INDOOR,  0, "Living Room",          "LIVING ROOM",  42, 34, 52, 28),
    R(14, ROOM_INDOOR,  1, "Living Room (Party)",  "LIVING ROOM",  42, 34, 52, 28),
    R(22, ROOM_INDOOR,  0, "Mother Spider's Room", "",             76, 49, 14, 10),
    R(2,  ROOM_INDOOR,  0, "Foyer",                "FOYER",        96, 34, 30, 28),
    R(5,  ROOM_INDOOR,  0, "Chibi-House",          "",             46, 49, 14, 10),

    /* basement + outside */
    R(3,  ROOM_INDOOR,  0, "Basement",             "BASEMENT",     2,  66, 60, 28),
    R(9,  ROOM_OUTDOOR, 0, "Backyard",             "BACKYARD",     66, 66, 60, 28),
    R(11, ROOM_WATER,   0, "Drain",                "",             110, 80, 14, 12),
    R(16, ROOM_INDOOR,  0, "UFO",                  "",             92, 80, 14, 12),

    /* not on the map, names only */
    R(0,  ROOM_INDOOR,  1, "Debug Room",           "", 0, 0, 0, 0),
    R(10, ROOM_INDOOR,  1, "Staff Credits",        "", 0, 0, 0, 0),
    R(13, ROOM_INDOOR,  1, "Chibi-Manual",         "", 0, 0, 0, 0),
};

const int g_room_count = sizeof(g_rooms) / sizeof(g_rooms[0]);

const Door g_doors[] = {
    { 40, 46, 2, 8 },   /* kitchen <-> living room   */
    { 94, 46, 2, 8 },   /* living room <-> foyer     */
    { 106, 30, 10, 4 }, /* foyer stairs -> upstairs  */
    { 62, 12, 4, 8 },   /* jenny's room <-> bedroom  */
    { 72, 62, 12, 4 },  /* living room -> backyard   */
    { 16, 62, 12, 4 },  /* kitchen -> basement       */
};

const int g_door_count = sizeof(g_doors) / sizeof(g_doors[0]);

const Room *room_find(u16 id)
{
    for (int i = 0; i < g_room_count; i++)
        if (g_rooms[i].id == id)
            return &g_rooms[i];
    return 0;
}
