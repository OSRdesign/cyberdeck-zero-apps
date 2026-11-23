/*
 * SPDX-License-Identifier: MIT
 *
 * The row ids of the app's lists (what a row gives back when it is activated).
 */
#pragma once

#include <cstdint>

namespace retui {

enum : std::intptr_t {
    kNone = 0,
    kConnections,
    kStatus,
    kSettings,
    kRetry,
    kAdd,
    kMore,
    kSwitchOn,
    kAllowSend,
    kRemove,
    kName,
    kSave,
    kKeep,
    kDisplayName,
    kAddress,
    kStop,
    kOsk,
    kVersion,
    kProblem,
    kConn = 100,   /* + index of the connection */
    kKind = 200,   /* + index in the kind list shown */
    kChoice = 300, /* + index in the choices */
    kOpt = 1000,   /* + index in the form's options */
};

} // namespace retui
