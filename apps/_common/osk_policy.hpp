/*
 * SPDX-License-Identifier: MIT
 *
 * The app-side on-screen keyboard policy (report 024 section 1, APP-CONTRACT section 10).
 *
 * The launcher never draws over a fullscreen app, so an app with text fields shows cp0_lvgl's own keyboard
 * (cp0::ui::osk, the same widget the launcher uses) under the shared rule: cp0_app_osk_allowed() on input.state, the
 * file the launcher keeps up to date (keyboards present, when they went away, Settings > System > On-screen keyboard,
 * APPLAUNCH_OSK). Physical keyboard first; the keyboard is never summoned by hand.
 *
 * input.state is looked at (stat) at most once a second and parsed again only when the file changed; its contents are
 * taken when its gen differs from the one in use. The rule itself is evaluated on every call (it depends on the time:
 * the 2 s grace and the 600 s Bluetooth sleep).
 *
 * No input.state (the app was started over ssh, or by an older launcher): the app's own probe (APP-CONTRACT section 7):
 * the rule APPLAUNCH_OSK (force | off | auto, default auto) with the real keyboards of /proc/bus/input/devices, at
 * most once a second too.
 *
 * The PDA writing zone's band (launcher report 044, APP-CONTRACT section 3 "The band in an app"): an app the launcher
 * runs upright on a portrait panel with the writing zone gets APPLAUNCH_PDA=1, and its display backend builds the band.
 * install() binds cp0_lvgl's shared rule (cp0::ui::zone_host) to input.state hand=: while hand=1 (Settings > System >
 * On-screen keyboard is Handwriting + keyboard) the band shows exactly where this keyboard would (same rule, same
 * triggers), the strip's Keyb puts the keyboard in its place until its close cross, and the band's keys reach the app as
 * the keyboard's do. hand=0, no file, or no band (APPLAUNCH_PDA=0, landscape, the SDL simulator): the keyboard, as before.
 */
#pragma once

#include "cp0_app_contract.h"

#include <cstdint>
#include <functional>
#include <string>

namespace cdk {
namespace osk_policy {

class Policy {
public:
    /* path: input.state ("" = cp0_app_state_path("input.state") when first needed); env_osk: APPLAUNCH_OSK as found
     * ("" = unset); keyboards_now: real keyboards present (-1 = unknown) for the no-file case. */
    Policy(std::string path, std::string env_osk, std::function<int()> keyboards_now);

    /* May the keyboard show now? now_ms: CLOCK_MONOTONIC milliseconds (cp0_app_monotonic_s() * 1000 scale). */
    bool allowed(std::int64_t now_ms);

    /* The user chose Handwriting + keyboard: input.state hand=1 as last read (no file, or a file without the line: no).
     * Read by allowed(); asking it reads nothing. */
    bool hand() const { return have_file_ && input_.hand != 0; }

    /* For tests and the info page. */
    bool from_file() const { return have_file_; }
    unsigned gen() const { return input_.gen; }
    const cp0_app_input_t &input() const { return input_; }
    unsigned parses() const { return parses_; } /* times the file was read and parsed */
    unsigned looks() const { return looks_; }   /* times the file was looked at (stat) */
    int fallback_keyboards() const { return fallback_keyboards_; }

private:
    void refresh(std::int64_t now_ms);

    std::string path_;
    std::string env_osk_;
    std::function<int()> keyboards_now_;
    cp0_app_input_t input_{};
    bool have_file_ = false;
    bool looked_ = false;
    std::int64_t next_look_ms_ = 0;
    /* what the file looked like when it was last parsed: inode, size, mtime (an atomic rename changes the inode) */
    unsigned long long ino_ = 0;
    long long size_ = -1, mtime_ns_ = -1;
    int fallback_keyboards_ = -1;
    unsigned parses_ = 0, looks_ = 0;
};

/* The rule for an APPLAUNCH_OSK value and a keyboard count when there is no input.state (pure). */
bool fallback_allows(const std::string &env_osk, int keyboards);

/* Gives cp0::ui::osk its policy: a Policy on input.state, APPLAUNCH_OSK and /proc/bus/input/devices, asked with the
 * monotonic clock; then binds the writing zone's rule (cp0::ui::zone_host::bind) with the policy's hand(), no gate (an
 * app in front is always its own text input) and nothing to publish. When hand() changes, zone_host::hand_changed() runs
 * on the next LVGL cycle (lv_async_call: the policy is asked inside the keyboard service's own update). Once; later
 * calls do nothing. LVGL thread. */
void install();

/* The installed policy (NULL before install()). */
Policy *installed();

/* Times a change of hand() was passed on to the writing zone (zone_host::hand_changed()), for tests. */
unsigned hand_changes();

} // namespace osk_policy
} // namespace cdk
