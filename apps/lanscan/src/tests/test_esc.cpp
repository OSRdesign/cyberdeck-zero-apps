// g++ -std=c++17 -I../main/ui test_esc.cpp -o test_esc && ./test_esc
#include "esc_policy.hpp"
#include <cstdio>
#include <cstring>
int main()
{
    using namespace lanscan;
    static_assert(esc_short_press(View::Hosts).hint, "Hosts: hint only");
    static_assert(!esc_short_press(View::Ports).hint && esc_short_press(View::Ports).next == View::Hosts,
                  "Ports: back to Hosts");
    if (std::strcmp(kEscHint, "Hold Esc 3 s to exit") != 0) return 1;
    std::puts("lanscan esc policy ok");
    return 0;
}
