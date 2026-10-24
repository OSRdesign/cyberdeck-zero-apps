/*
 * SPDX-License-Identifier: MIT
 */
#include "hooks.hpp"

#include <ctime>

namespace notesui {

Hooks &hooks()
{
    static Hooks h;
    return h;
}

std::int64_t now_epoch()
{
    if (hooks().now) return hooks().now();
    return static_cast<std::int64_t>(std::time(nullptr));
}

} // namespace notesui
