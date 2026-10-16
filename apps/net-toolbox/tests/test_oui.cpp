/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the OUI table parse and lookup, and the locally-administered (random) MAC rule - a MAC whose
 * first byte has the 0x02 bit set is shown as a private address with no vendor.
 */
#include "check.hpp"

#include "oui.hpp"

using namespace nt;

void test_oui()
{
    const std::string tsv =
        "000000\tXEROX\n"
        "B827EB\tRaspberry Pi\n"              /* b8:27:eb */
        "DCA632\tRaspberry Pi Trading\n"
        "240AC4\tEspressif\n"
        "bad line without a tab\n"
        "ZZZZZZ\tnot hex\n"
        "001122\t   \n";                      /* empty vendor dropped */
    const auto table = parse_oui_table(tsv);
    CHECK_EQ(table.size(), static_cast<std::size_t>(4)); /* the four good lines */

    // mac_oui splits the first three bytes, accepting the three separators
    std::uint32_t oui = 0;
    CHECK(mac_oui("b8:27:eb:12:34:56", oui) && oui == 0xB827EB);
    CHECK(mac_oui("b8-27-eb-12-34-56", oui) && oui == 0xB827EB);
    CHECK(mac_oui("b827eb123456", oui) && oui == 0xB827EB);
    CHECK(!mac_oui("zz:27:eb:12:34:56", oui));
    CHECK(!mac_oui("b8", oui));

    // a registered vendor
    {
        const VendorLookup v = lookup_vendor(table, "b8:27:eb:aa:bb:cc");
        CHECK(!v.private_address && v.vendor == "Raspberry Pi");
    }
    {
        const VendorLookup v = lookup_vendor(table, "24:0a:c4:11:22:33");
        CHECK(!v.private_address && v.vendor == "Espressif");
    }
    // a registered but absent prefix: no vendor, not private
    {
        const VendorLookup v = lookup_vendor(table, "00:50:56:11:22:33");
        CHECK(!v.private_address && v.vendor.empty());
    }
    // the locally-administered bit (0x02): a private / random address, no vendor even if the prefix is in the table
    CHECK(mac_is_locally_administered("02:00:00:11:22:33"));
    CHECK(mac_is_locally_administered("ba:27:eb:11:22:33")); /* ba = 1011_1010, 0x02 set */
    CHECK(!mac_is_locally_administered("b8:27:eb:11:22:33")); /* b8 = 1011_1000, 0x02 clear */
    CHECK(mac_is_locally_administered("a6:aa:bb:cc:dd:ee"));  /* a6 = 1010_0110 */
    CHECK(!mac_is_locally_administered("a4:aa:bb:cc:dd:ee")); /* a4 = 1010_0100 */
    {
        const VendorLookup v = lookup_vendor(table, "ba:27:eb:11:22:33");
        CHECK(v.private_address && v.vendor.empty()); /* random: never a vendor guess */
    }
    // garbage MAC: nothing
    {
        const VendorLookup v = lookup_vendor(table, "nonsense");
        CHECK(!v.private_address && v.vendor.empty());
    }
}
