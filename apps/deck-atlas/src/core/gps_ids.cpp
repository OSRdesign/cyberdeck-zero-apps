/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS receivers Port = Auto may open (see gps_ids.hpp). Keep it small: an id goes here only when it is
 * known to be a GPS receiver (the Linux kernel drivers' tables, the vendor's own products). A receiver that is not on
 * the list is chosen by hand in Settings > GPS receiver > Port.
 */
#include "gps_ids.hpp"

namespace atlas {

namespace {

struct Id {
    std::uint16_t vid, pid;   /* pid 0xFFFF: every product of the vendor */
    const char *name;
};

constexpr std::uint16_t kAnyPid = 0xFFFF;

const Id kIds[] = {
    /* u-blox AG: every product of the vendor is a GNSS receiver (the u-blox 7 / 8 / 9 dongles, 1546:01a7, 01a8, 01a9, ...;
     * native USB, /dev/ttyACM*; the cdc_acm driver) */
    {0x1546, kAnyPid, "u-blox GNSS receiver"},
    /* Prolific PL2303: the serial bridge inside the common GPS pucks (USGlobalSat BU-353 and "GPS mouse" pucks: 067b:2303,
     * the pl2303 driver's PL2303_PRODUCT_ID; /dev/ttyUSB*) */
    {0x067b, 0x2303, "Prolific PL2303 GPS puck"},
    /* Prolific PL2303GC, the current bridge chip of the same pucks (067b:23a3, the pl2303 driver's PL2303_PRODUCT_ID_GC) */
    {0x067b, 0x23a3, "Prolific PL2303GC GPS puck"},
};

const Id *find(std::uint16_t vid, std::uint16_t pid)
{
    for (const Id &i : kIds)
        if (i.vid == vid && (i.pid == kAnyPid || i.pid == pid)) return &i;
    return nullptr;
}

} // namespace

bool gps_known_receiver(std::uint16_t vid, std::uint16_t pid) { return find(vid, pid) != nullptr; }

const char *gps_receiver_name(std::uint16_t vid, std::uint16_t pid)
{
    const Id *i = find(vid, pid);
    return i ? i->name : "";
}

} // namespace atlas
