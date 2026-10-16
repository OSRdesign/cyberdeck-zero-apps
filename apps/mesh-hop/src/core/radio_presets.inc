// SPDX-License-Identifier: MIT
//
// DATA FILE: the radio presets suggested by the MeshCore firmware flasher and apps (26 entries).
//
//   Source:  https://api.meshcore.nz/api/v1/config   (the suggested radio settings list)
//   Fetched: 2026-10-07 (feasibility report 014, answer b); the list changes over time (the "Deprecated" entries show it).
//   The firmware has no command that lists presets, so the list is kept here. Update this file (and the date) when it changes.
//
// Included by presets.cpp. One preset per line: {name, frequency MHz, bandwidth kHz, spreading factor, coding rate 5..8 (4/5..4/8)}.

#define MESHHOP_PRESETS_SOURCE_URL "https://api.meshcore.nz/api/v1/config"
#define MESHHOP_PRESETS_DATE "2026-10-07"

{"Australia", 915.800, 250, 10, 5},
{"Australia (Narrow)", 916.575, 62.5, 7, 7},
{"Australia (Mid)", 915.075, 125, 9, 5},
{"Australia: SA, WA", 923.125, 62.5, 8, 8},
{"Australia: QLD", 923.125, 62.5, 8, 5},
{"Brazil", 923.125, 62.5, 8, 8},
{"Canada", 910.525, 62.5, 7, 5},
{"Costa Rica", 910.525, 125, 11, 5},
{"EU/UK (Narrow)", 869.618, 62.5, 8, 8},
{"EU/UK (Deprecated)", 869.525, 250, 11, 5},
{"Czech Republic (Narrow)", 869.432, 62.5, 7, 5},
{"EU 433MHz (Long Range)", 433.650, 250, 11, 5},
{"EU 433MHz (Narrow)", 433.650, 62.5, 8, 8},
{"Hungary", 869.618, 62.5, 7, 5},
{"Netherlands", 869.618, 62.5, 7, 5},
{"Netherlands (Limburg)", 869.618, 62.5, 8, 8},
{"New Zealand (Narrow)", 917.375, 62.5, 7, 5},
{"New Zealand (Gisborne)", 917.375, 250, 11, 5},
{"Portugal 433", 433.375, 62.5, 9, 6},
{"Portugal 868", 869.618, 62.5, 7, 6},
{"Slovakia", 869.618, 62.5, 7, 5},
{"Switzerland", 869.618, 62.5, 8, 8},
{"USA", 910.525, 62.5, 7, 5},
{"USA - Southern California", 927.875, 62.5, 7, 5},
{"Vietnam (Narrow)", 920.250, 62.5, 8, 5},
{"Vietnam (Deprecated)", 920.250, 250, 11, 5},
