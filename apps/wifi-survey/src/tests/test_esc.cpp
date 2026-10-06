// g++ -std=c++17 -I../main/ui test_esc.cpp -o test_esc && ./test_esc
#include "esc_policy.hpp"
#include <cstdio>
#include <cstring>
int main()
{
    using namespace wifisurvey;
    static_assert(esc_short_press(View::Networks, View::Networks).hint, "Networks: hint only");
    static_assert(esc_short_press(View::Channels, View::Channels).hint, "Channels: hint only");
    static_assert(!esc_short_press(View::Detail, View::Networks).hint &&
                      esc_short_press(View::Detail, View::Networks).next == View::Networks,
                  "Detail: back to Networks");
    static_assert(esc_short_press(View::Detail, View::Channels).next == View::Channels, "Detail: back to Channels");
    if (std::strcmp(kEscHint, "Hold Esc 3 s to exit") != 0) return 1;
    std::puts("wifi-survey esc policy ok");
    return 0;
}
