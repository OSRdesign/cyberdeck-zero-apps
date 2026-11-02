/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app unit tests: main() (no LVGL, no network). The groups:
 *   json     the reader on HA's message shapes, numbers, escapes, limits
 *   text     URL / form encoding, query strings, the server address, private addresses
 *   model    the registry lists (areas, devices, entities for display), rooms and their counts, a / c / r changes
 *   format   the words and numbers of the rows, the confirmation questions
 *   engine   the client state machine with scripted events: pairing, sign-in, connect, the messages it sends (exact
 *            JSON), subscriptions, commands, back-off, token refresh timing, auth refusals, ping, freeze, sign out
 *   sim      whole flows against the simulated Home Assistant (the render harness's worlds)
 *   store    the files: atomic writes, modes 0600 / 0700, round trips
 *   qr       the pairing QR code
 *   package  the .desktop file, app.json, the icon and the licence file
 *   tiles    0.2.0's dashboard: tabs, columns by mm, rows and 2-D focus, keys, colours / words / glyphs, the icon order,
 *            the MDI name table, the layout cache, the icon requests
 */
#include "test_util.hpp"

int main()
{
    test_json_group();
    test_text_group();
    test_model_group();
    test_format_group();
    test_engine_group();
    test_sim_group();
    test_store_group();
    test_qr_group();
    test_package_group();
    test_tiles_group();
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
