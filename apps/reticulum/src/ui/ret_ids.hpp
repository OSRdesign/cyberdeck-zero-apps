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
    kMessages,      /* Home: the conversations */
    kContacts,      /* Home: the contacts */
    kNewMessage,    /* Messages: choose a contact to write to */
    kAddContact,
    kHeard,         /* Contacts: Heard recently */
    kSendMsg,       /* Contact: Send a message */
    kRenameContact,
    kRemoveContact,
    kPaste,         /* Add a contact: Paste address */
    kSaveContact,
    kContactName,   /* Add a contact: the name */
    kContactAddress,/* Add a contact: the address */
    kCopyAddress,
    kAnnounce,
    kDeleteConv,
    kConvContact,   /* Conversation options: the contact (or Add to contacts) */
    kRetryFailed,   /* Conversation options: send the failed messages again, each by hand */
    kAlerts,        /* Settings: new message alerts */
    kGoConnections, /* the blocked pages: open Connections */
    kConn = 100,   /* + index of the connection */
    kKind = 200,   /* + index in the kind list shown */
    kChoice = 300, /* + index in the choices */
    kOpt = 1000,   /* + index in the form's options */
    kConvRow = 2000,    /* + index in the conversations */
    kContactRow = 3000, /* + index in the contacts */
    kPerson = 4000,     /* + index in the heard people */
};

} // namespace retui
