// Host tests of the Markdown Notes core (no LVGL, no board). See run_tests.sh.
#include "test_common.hpp"

#include <ctime>

int g_fail = 0, g_checks = 0;

void test_utf8();
void test_codec_roundtrip();
void test_tasks();
void test_dates();
void test_load_save();
void test_atomic_save();
void test_disk_check();
void test_create_note();
void test_trash();
void test_settings();
void test_walk();
void test_names();
void test_fuzzy();
void test_links();
void test_markdown_blocks();
void test_markdown_inline();
void test_markdown_frontmatter();
void test_markdown_cap_and_lines();
void test_package();

#define RUN(fn)                                                                  \
    do {                                                                         \
        int before_ = g_fail;                                                    \
        fn();                                                                    \
        std::printf("%-30s %s\n", #fn, g_fail == before_ ? "ok" : "FAILED");      \
    } while (0)

int main()
{
    ::setenv("TZ", "UTC", 1);
    ::tzset();
    RUN(test_utf8);
    RUN(test_codec_roundtrip);
    RUN(test_tasks);
    RUN(test_dates);
    RUN(test_load_save);
    RUN(test_atomic_save);
    RUN(test_disk_check);
    RUN(test_create_note);
    RUN(test_trash);
    RUN(test_settings);
    RUN(test_walk);
    RUN(test_names);
    RUN(test_fuzzy);
    RUN(test_links);
    RUN(test_markdown_blocks);
    RUN(test_markdown_inline);
    RUN(test_markdown_frontmatter);
    RUN(test_markdown_cap_and_lines);
    RUN(test_package);
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
