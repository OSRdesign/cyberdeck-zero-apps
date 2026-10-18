/*
 * SPDX-License-Identifier: MIT
 */
#include "datetime.hpp"

#include "util.hpp"

#include <cctype>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace rssreader {

std::int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

namespace {

bool dig(char c) { return c >= '0' && c <= '9'; }

bool make(int y, int mo, int d, int h, int mi, int s, int offset_min, std::int64_t &out)
{
    if (y < 1970 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 24 || mi < 0 || mi > 59 || s < 0 || s > 60) return false;
    static const int mdays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > mdays[mo - 1] || (mo == 2 && d == 29 && !leap)) return false;
    out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s - static_cast<std::int64_t>(offset_min) * 60;
    return true;
}

/* n digits at s[p..]; -1 if not all digits */
int num(const std::string &s, std::size_t p, std::size_t n)
{
    if (p + n > s.size()) return -1;
    int v = 0;
    for (std::size_t k = 0; k < n; ++k) {
        if (!dig(s[p + k])) return -1;
        v = v * 10 + (s[p + k] - '0');
    }
    return v;
}

/* zone at s[p..]: Z, UTC, GMT, +hh, +hhmm, +hh:mm (optionally after spaces); minutes east of UTC */
bool zone(const std::string &s, std::size_t p, int &off)
{
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    off = 0;
    if (p >= s.size()) return true;
    char c = s[p];
    if (c == 'Z' || c == 'z') return true;
    if (c == '+' || c == '-') {
        int sign = c == '-' ? -1 : 1;
        int hh = num(s, p + 1, 2);
        if (hh < 0 || hh > 14) return false;
        std::size_t q = p + 3;
        if (q < s.size() && s[q] == ':') ++q;
        int mm = num(s, q, 2);
        if (mm < 0) mm = 0;
        off = sign * (hh * 60 + mm);
        return true;
    }
    return true;   /* trailing text such as "UTC" / "GMT" / "(CEST)": UTC */
}

bool parse_iso(const std::string &s, std::int64_t &out)
{
    std::size_t p = 0;
    int y = num(s, 0, 4);
    if (y < 0) return false;
    p = 4;
    bool dash = p < s.size() && s[p] == '-';
    if (dash) ++p;
    int mo = num(s, p, 2);
    if (mo < 0) return false;
    p += 2;
    int d = 1;
    if (p < s.size() && (s[p] == '-' || dig(s[p]))) {
        if (s[p] == '-') ++p;
        d = num(s, p, 2);
        if (d < 0) return false;
        p += 2;
    }
    if (p >= s.size()) return make(y, mo, d, 0, 0, 0, 0, out);
    if (s[p] != 'T' && s[p] != 't' && s[p] != ' ') return false;
    ++p;
    int h = num(s, p, 2);
    if (h < 0) return false;
    p += 2;
    if (p < s.size() && s[p] == ':') ++p;
    int mi = num(s, p, 2);
    if (mi < 0) return false;
    p += 2;
    int sec = 0;
    if (p < s.size() && (s[p] == ':' || dig(s[p]))) {
        if (s[p] == ':') ++p;
        sec = num(s, p, 2);
        if (sec < 0) return false;
        p += 2;
        if (p < s.size() && (s[p] == '.' || s[p] == ',')) {
            ++p;
            while (p < s.size() && dig(s[p])) ++p;
        }
    }
    int off;
    if (!zone(s, p, off)) return false;
    return make(y, mo, d, h, mi, sec, off, out);
}

int month_of(const std::string &w)
{
    static const char *m[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (w.size() < 3) return 0;
    std::string l = lower(w.substr(0, 3));
    for (int k = 0; k < 12; ++k)
        if (l == m[k]) return k + 1;
    return 0;
}

bool zone_word(const std::string &w, int &off)
{
    struct Z { const char *n; int min; };
    static const Z z[] = {{"z", 0}, {"ut", 0}, {"utc", 0}, {"gmt", 0}, {"wet", 0}, {"est", -300}, {"edt", -240}, {"cst", -360},
                          {"cdt", -300}, {"mst", -420}, {"mdt", -360}, {"pst", -480}, {"pdt", -420}, {"akst", -540},
                          {"akdt", -480}, {"hst", -600}, {"cet", 60}, {"cest", 120}, {"met", 60}, {"mest", 120}, {"bst", 60},
                          {"west", 60}, {"eet", 120}, {"eest", 180}, {"msk", 180}, {"jst", 540}, {"kst", 540}, {"aest", 600},
                          {"aedt", 660}};
    std::string l = lower(w);
    for (const Z &e : z)
        if (l == e.n) { off = e.min; return true; }
    return false;
}

bool parse_loose(const std::string &s, std::int64_t &out)
{
    int month = 0, day = 0, year = -1, h = 0, mi = 0, sec = 0, off = 0;
    bool have_time = false, pm = false, am = false;
    std::vector<int> small_nums;        /* numbers that are not the time or the year */
    std::vector<int> big_nums;
    std::size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (dig(c)) {
            std::size_t j = i;
            while (j < s.size() && dig(s[j])) ++j;
            int v = std::atoi(s.substr(i, j - i).c_str());
            if (j < s.size() && s[j] == ':' && !have_time && j + 2 < s.size() && dig(s[j + 1])) {
                h = v;
                std::size_t k = j + 1, e = k;
                while (e < s.size() && dig(s[e])) ++e;
                mi = std::atoi(s.substr(k, e - k).c_str());
                if (e < s.size() && s[e] == ':' && e + 1 < s.size() && dig(s[e + 1])) {
                    k = e + 1;
                    e = k;
                    while (e < s.size() && dig(s[e])) ++e;
                    sec = std::atoi(s.substr(k, e - k).c_str());
                    if (e < s.size() && (s[e] == '.' || s[e] == ',')) {
                        ++e;
                        while (e < s.size() && dig(s[e])) ++e;
                    }
                }
                have_time = true;
                j = e;
            } else if (j - i >= 3) {
                big_nums.push_back(v);
            } else {
                small_nums.push_back(v);
            }
            i = j;
        } else if ((c == '+' || c == '-') && have_time && i + 2 < s.size() && dig(s[i + 1]) && dig(s[i + 2])) {
            if (!zone(s, i, off)) return false;
            i += 3;
            while (i < s.size() && (dig(s[i]) || s[i] == ':')) ++i;
        } else if (std::isalpha(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
            std::string w = s.substr(i, j - i);
            std::string lw = lower(w);
            int m = month_of(w);
            if (m && !month) month = m;
            else if (lw == "pm") pm = true;
            else if (lw == "am") am = true;
            else if (have_time && !m) {
                int zo;
                if (zone_word(w, zo)) off = zo;
            }
            i = j;
        } else {
            ++i;
        }
    }
    if (!month) return false;
    if (!big_nums.empty()) year = big_nums.back();
    if (small_nums.empty()) return false;
    day = small_nums[0];
    if (year < 0) {
        if (small_nums.size() < 2) return false;
        year = small_nums[1];
        year += year < 70 ? 2000 : 1900;
    }
    if (pm && h < 12) h += 12;
    if (am && h == 12) h = 0;
    return make(year, month, day, h, mi, sec, off, out);
}

} // namespace

bool parse_date(const std::string &text, std::int64_t &epoch)
{
    std::string s = trim(text);
    if (s.empty() || s.size() > 100) return false;
    bool all_digits = true;
    for (char c : s)
        if (!dig(c)) { all_digits = false; break; }
    if (all_digits) {
        if (s.size() == 10) { epoch = std::atoll(s.c_str()); return epoch >= 0 && epoch < 4102444800LL; }
        if (s.size() == 13) { epoch = std::atoll(s.c_str()) / 1000; return epoch >= 0 && epoch < 4102444800LL; }
        if (s.size() == 8) return make(num(s, 0, 4), num(s, 4, 2), num(s, 6, 2), 0, 0, 0, 0, epoch);
        return false;
    }
    if (s.size() >= 6 && dig(s[0]) && dig(s[1]) && dig(s[2]) && dig(s[3]) && (s[4] == '-' || dig(s[4])))
        if (parse_iso(s, epoch)) return true;
    return parse_loose(s, epoch);
}

std::string format_utc(std::int64_t epoch)
{
    std::int64_t days = epoch / 86400, rem = epoch % 86400;
    if (rem < 0) { rem += 86400; --days; }
    /* civil_from_days */
    std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02d:%02d", static_cast<long long>(y), m, d, static_cast<int>(rem / 3600), static_cast<int>(rem % 3600 / 60));
    return buf;
}

} // namespace rssreader
