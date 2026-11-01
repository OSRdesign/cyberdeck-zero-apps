/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the device sprites (see sprites.hpp).
 *
 * Monitor characters: # outline, W bezel light, F bezel face, L bezel shade, s screen, g glare, t text line (blank when the
 * screen is off), e power LED, n neck, b base. Server: # outline, H light edge, S shade edge, D face, B drive bay, h bay
 * handle, v vent, G green LED, A amber LED, R red power button.
 */
#include "sprites.hpp"

#include <algorithm>

namespace netwalk {

namespace {

const char *const kMonitorSmall[] = {
    "#########",
    "#WWWWWWW#",
    "#WgssssL#",
    "#WsttssL#",
    "#WsssssL#",
    "#LLLLLeL#",
    ".#bbbbb#.",
    ".#######.",
};

const char *const kMonitorNormal[] = {
    "#############",
    "#WWWWWWWWWWW#",
    "#WF#######FL#",
    "#WF#gssss#FL#",
    "#WF#sttts#FL#",
    "#WF#sssss#FL#",
    "#WF#sttss#FL#",
    "#WF#######FL#",
    "#WFFFFFFFFeL#",
    "#LLLLLLLLLLL#",
    "#############",
    "....#nnn#....",
    ".#bbbbbbbbb#.",
    ".###########.",
};

const char *const kServerSmall[] = {
    "#######",
    "#HGARS#",
    "#HvvvS#",
    "#HBBhS#",
    "#HDDDS#",
    "#HBBhS#",
    "#HDDDS#",
    "#HvvvS#",
    "#######",
    ".#...#.",
};

const char *const kServerNormal[] = {
    "###########",
    "#HGDGADRRS#",
    "#HDDDDDRRS#",
    "#HvvvvvvvS#",
    "#HDDDDDDDS#",
    "#HDBBBBBhS#",
    "#HDBBBBBhS#",
    "#HDDDDDDDS#",
    "#HDBBBBBhS#",
    "#HDBBBBBhS#",
    "#HvvvvvvvS#",
    "###########",
    ".##.....##.",
};

const Sprite kSprites[2][2] = {
    /* Monitor: normal, small */ {{13, 14, kMonitorNormal}, {9, 8, kMonitorSmall}},
    /* Server:  normal, small */ {{11, 13, kServerNormal}, {7, 10, kServerSmall}},
};

} // namespace

bool use_small_sprite(int tile)
{
    return tile < kSmallTile;
}

const Sprite &sprite(SpriteKind kind, bool small)
{
    return kSprites[static_cast<int>(kind)][small ? 1 : 0];
}

int sprite_factor(const Sprite &s, int tile)
{
    return std::max(1, std::min((tile - 6) / s.w, (tile - 6) / s.h));
}

long sprite_color(SpriteKind kind, char c, bool lit)
{
    if (kind == SpriteKind::Monitor) {
        switch (c) {
        case '#': return 0x303030;
        case 'W': return 0xFFFFFF;
        case 'F': return 0xCCCCCC;
        case 'L': return 0x666666;
        case 's': return lit ? 0x00FF00 : 0x1818C8;
        case 'g': return lit ? 0xB8FFB8 : 0x6068F0;
        case 't': return lit ? 0x008800 : 0x1818C8;
        case 'e': return lit ? 0x40FF40 : 0x305030;
        case 'n': return 0x808080;
        case 'b': return 0xA0A0A0;
        default: return -1;
        }
    }
    switch (c) {
    case '#': return 0x181818;
    case 'H': return 0x8A8A8A;
    case 'S': return 0x383838;
    case 'D': return 0x555555;
    case 'B': return 0x1C1C1C;
    case 'h': return 0xC8C8C8;
    case 'v': return 0x2A2A2A;
    case 'G': return 0x40FF40;
    case 'A': return 0xFFB000;
    case 'R': return 0xE02020;
    default: return -1;
    }
}

} // namespace netwalk
