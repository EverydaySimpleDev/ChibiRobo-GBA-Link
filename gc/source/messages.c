/*
 * Popup messages that game scripts can show on the GBA.
 *
 * From any stage script (unplug / randomizer .us files):
 *     set var(1880.d), 3.d          ; show message #3 on the GBA
 * When the player closes it, var(1881) is set to 3, so a script can wait:
 *     if eq(var(1881.d), 3.d), else *loc_still_open
 *
 * Titles are up to 16 characters, text up to 96 (it is word-wrapped on the
 * GBA into 5 lines of 24; '\n' forces a line break).
 */
#include "messages.h"
#include "chibi_link_protocol.h"

const CLScriptMessage cl_script_messages[] = {
    [1] = {CL_ICON_PLUG, "Chibi-Robo!", "Hi-dee-ho! A message from the game script. Press A to close it."},
    [2] = {CL_ICON_COIN, "Moolah!", "You found some moolah! Spend it at the Chibi-PC."},
    [3] = {CL_ICON_HEART, "Happy Points", "Someone in the house is a little happier thanks to you!"},
    [4] = {CL_ICON_BATTERY, "Recharge!", "Your battery is getting low.\nFind an outlet and plug in!"},
    [5] = {CL_ICON_HOUSE, "New Area", "A new part of the house is open. Check the map on your GBA!"},
    [6] = {CL_ICON_CLOCK, "Bedtime", "It's getting late. The Sandersons will be going to bed soon."},
    [7] = {CL_ICON_HEART, "Chibi-Robo!", "Do you think we did the right thing?"},
    [8] = {CL_ICON_PLUG, "Chibi-Robo!", "Doesnt Sunshine bear seem a bit off to you?"},
    [9] = {CL_ICON_PLUG, "Chibi-Robo!", "Princess Pitts seems very kind and friendly!"},
    [10] = {CL_ICON_PLUG, "Chibi-Robo!", "Did you know AP Items tasted like cookies?"},
};

const int cl_script_message_count = sizeof(cl_script_messages) / sizeof(cl_script_messages[0]);
