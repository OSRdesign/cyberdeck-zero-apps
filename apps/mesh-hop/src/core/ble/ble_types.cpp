/*
 * SPDX-License-Identifier: MIT
 */

#include "ble_types.hpp"

#include <cctype>

namespace meshzero::ble {

bool is_candidate(const BleDevice &d)
{
    return d.nus || d.name.rfind(kNamePrefix, 0) == 0;
}

bool valid_address(const std::string &a)
{
    if (a.size() != 17) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i % 3 == 2) {
            if (a[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(a[i]))) {
            return false;
        }
    }
    return true;
}

std::string address_from_path(const std::string &path)
{
    const size_t at = path.rfind("/dev_");
    if (at == std::string::npos) return "";
    std::string a = path.substr(at + 5);
    if (a.size() != 17) return "";
    for (char &c : a)
        if (c == '_') c = ':';
    for (char &c : a) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return valid_address(a) ? a : "";
}

std::string device_path(const std::string &adapter_path, const std::string &address)
{
    std::string a = address;
    for (char &c : a) c = c == ':' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return adapter_path + "/dev_" + a;
}

} // namespace meshzero::ble
