/*
 * SPDX-License-Identifier: MIT
 */

#include "input_state.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace meshzero {

namespace {

/* Bit n of a "B: KEY=" line: hexadecimal words, the highest word first, each word as wide as an unsigned long. */
bool key_bit(const std::vector<std::string> &words, unsigned n)
{
    if (words.empty()) return false;
    const unsigned width = words.size() > 1 ? static_cast<unsigned>(words.back().size()) * 4 : 64;
    const size_t from_end = n / width;
    if (from_end >= words.size()) return false;
    const std::string &w = words[words.size() - 1 - from_end];
    const unsigned long long v = std::strtoull(w.c_str(), nullptr, 16);
    return (v >> (n % width)) & 1ULL;
}

/* Devices that are not the user's keyboard: the launcher's own virtual keyboard ("applaunch-vkbd", created with every
 * app, all keys set) and anything on the virtual bus. USB (0003) and Bluetooth (0005, e.g. a uhid keyboard) count. */
bool block_is_virtual(const std::string &block)
{
    std::istringstream in(block);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("N: Name=", 0) == 0 && line.find("applaunch-") != std::string::npos) return true;
        if (line.rfind("I: Bus=", 0) == 0 && line.compare(7, 4, "0006") == 0) return true;
    }
    return false;
}

bool block_is_keyboard(const std::string &block)
{
    if (block_is_virtual(block)) return false;
    std::istringstream in(block);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("B: KEY=", 0) != 0) continue;
        std::istringstream ws(line.substr(7));
        std::vector<std::string> words;
        std::string w;
        while (ws >> w) words.push_back(w);
        // KEY_A 30, KEY_Z 44, KEY_ENTER 28, KEY_SPACE 57
        if (key_bit(words, 30) && key_bit(words, 44) && key_bit(words, 28) && key_bit(words, 57)) return true;
    }
    return false;
}

} // namespace

bool keyboard_listed(const std::string &text)
{
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find("\n\n", pos);
        if (end == std::string::npos) end = text.size();
        if (block_is_keyboard(text.substr(pos, end - pos))) return true;
        pos = end + 2;
    }
    return false;
}

bool keyboard_present(const char *path)
{
    std::ifstream in(path);
    if (!in) return true;          // cannot tell: do not block typing
    std::stringstream ss;
    ss << in.rdbuf();
    return keyboard_listed(ss.str());
}

} // namespace meshzero
