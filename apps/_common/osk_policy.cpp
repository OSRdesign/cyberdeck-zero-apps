/*
 * SPDX-License-Identifier: MIT
 *
 * The app-side on-screen keyboard policy (see osk_policy.hpp).
 */
#include "osk_policy.hpp"

#include "cp0_keyboard_presence.h"
#include "cp0_ui_widgets.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <time.h>
#include <utility>

namespace cdk {
namespace osk_policy {
namespace {

constexpr std::int64_t kLookEveryMs = 1000;

std::unique_ptr<Policy> g_policy;

std::int64_t monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

Policy::Policy(std::string path, std::string env_osk, std::function<int()> keyboards_now)
    : path_(std::move(path)), env_osk_(std::move(env_osk)), keyboards_now_(std::move(keyboards_now))
{
}

bool fallback_allows(const std::string &env_osk, int keyboards)
{
    cp0_app_vkb_t vkb = CP0_APP_VKB_AUTO;
    if (!env_osk.empty() && cp0_app_vkb_parse(env_osk.c_str(), &vkb) != 0) vkb = CP0_APP_VKB_AUTO;
    /* unknown count (no /proc): no keyboard assumed, as the launcher does when it has no presence watcher */
    const unsigned count = keyboards > 0 ? static_cast<unsigned>(keyboards) : 0u;
    return cp0_app_osk_rule(vkb, count, 0, 0, 0) != 0;
}

void Policy::refresh(std::int64_t now_ms)
{
    if (looked_ && now_ms < next_look_ms_) return;
    looked_ = true;
    next_look_ms_ = now_ms + kLookEveryMs;
    ++looks_;

    if (path_.empty()) {
        char buf[CP0_APP_PATH_MAX + 32];
        if (cp0_app_state_path(CP0_APP_INPUT_FILE, buf, sizeof(buf)) == 0) path_ = buf;
    }
    struct stat st;
    if (path_.empty() || stat(path_.c_str(), &st) != 0) {
        have_file_ = false;
        ino_ = 0;
        size_ = mtime_ns_ = -1;
        fallback_keyboards_ = keyboards_now_ ? keyboards_now_() : -1;
        return;
    }
    const long long mtime_ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    if (have_file_ && static_cast<unsigned long long>(st.st_ino) == ino_ && st.st_size == size_ && mtime_ns == mtime_ns_)
        return; /* unchanged: no read */

    cp0_app_input_t next;
    ++parses_;
    if (cp0_app_input_read(path_.c_str(), &next) != 0) { /* not v=1 (or vanished meanwhile): as no file */
        have_file_ = false;
        ino_ = 0;
        size_ = mtime_ns_ = -1;
        fallback_keyboards_ = keyboards_now_ ? keyboards_now_() : -1;
        return;
    }
    ino_ = static_cast<unsigned long long>(st.st_ino);
    size_ = st.st_size;
    mtime_ns_ = mtime_ns;
    if (!have_file_ || next.gen != input_.gen) input_ = next; /* a new generation: take it */
    have_file_ = true;
}

bool Policy::allowed(std::int64_t now_ms)
{
    refresh(now_ms);
    if (!have_file_) return fallback_allows(env_osk_, fallback_keyboards_);
    return cp0_app_osk_allowed(&input_, now_ms / 1000) != 0;
}

void install()
{
    if (g_policy) return;
    const char *env = std::getenv("APPLAUNCH_OSK");
    g_policy = std::make_unique<Policy>(std::string(), env ? env : "", [] { return cp0_kbd_presence_count(nullptr); });
    Policy *policy = g_policy.get();
    cp0::ui::osk::set_policy([policy] { return policy->allowed(monotonic_ms()); });
}

Policy *installed() { return g_policy.get(); }

} // namespace osk_policy
} // namespace cdk
