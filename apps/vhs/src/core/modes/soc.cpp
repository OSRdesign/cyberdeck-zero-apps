/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the board's SoC class (see soc.hpp).
 */
#include "soc.hpp"

#include <fstream>
#include <iterator>

namespace vhs {
namespace {

struct Row {
    const char *compatible;
    SocClass cls;
};
/* the table (brief 4.1 rule 4) */
const Row kTable[] = {
    {"brcm,bcm2712", SocClass::Bcm2712},
    {"brcm,bcm2711", SocClass::Bcm2711},
    {"brcm,bcm2837", SocClass::Bcm2837},
};

std::string read_all(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

const char *soc_name(SocClass s)
{
    switch (s) {
    case SocClass::Bcm2712: return "bcm2712";
    case SocClass::Bcm2711: return "bcm2711";
    case SocClass::Bcm2837: return "bcm2837";
    case SocClass::Other: return "other";
    }
    return "other";
}

SocClass soc_from_compatible(const std::string &text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i;
        while (j < text.size() && text[j] != '\0' && text[j] != '\n') ++j;
        const std::string item = text.substr(i, j - i);
        for (const Row &r : kTable)
            if (item == r.compatible) return r.cls;
        i = j + 1;
    }
    return SocClass::Other;
}

SocClass detect_soc(const std::string &root)
{
    std::string c = read_all(root + "/proc/device-tree/compatible");
    if (c.empty()) c = read_all(root + "/sys/firmware/devicetree/base/compatible");
    return soc_from_compatible(c);
}

} // namespace vhs
