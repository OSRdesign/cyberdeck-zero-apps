// Host tests of the RSS Reader core (no LVGL, no network except a local server). See run_tests.sh.
#include "test_common.hpp"

#include <cstdlib>

int g_fail = 0, g_checks = 0;

void test_util();
void test_url();
void test_json();
void test_dates();
void test_xml();
void test_feed_rss();
void test_feed_atom_rdf();
void test_feed_failures();
void test_feed_limits();
void test_looks_like_feed();
void test_discover();
void test_html_text();
void test_html_helpers();
void test_curl_argv();
void test_response_headers();
void test_image_fetch();
void test_curl_client_offline();
void test_curl_client_local(const std::string &base);
void test_catalog_data();
void test_catalog_queries();
void test_catalog_loader();
void test_atomic_files();
void test_store_settings();
void test_store_subscriptions();
void test_store_articles();
void test_store_saved();
void test_store_cap();
void test_store_recovery();
void test_store_saved_kept_in_cache();
void test_store_threads();
void test_store_newest_and_clear_read();
void test_merge();
void test_refresh_feed();
void test_refresh_edge();
void test_refresh_all();
void test_add_custom_feed();
void test_opml();

#define RUN(fn)                                                         \
    do {                                                                \
        int before_ = g_fail;                                           \
        fn();                                                           \
        std::printf("%-28s %s\n", #fn, g_fail == before_ ? "ok" : "FAILED"); \
    } while (0)

int main()
{
    RUN(test_util);
    RUN(test_url);
    RUN(test_json);
    RUN(test_dates);
    RUN(test_xml);
    RUN(test_feed_rss);
    RUN(test_feed_atom_rdf);
    RUN(test_feed_failures);
    RUN(test_feed_limits);
    RUN(test_looks_like_feed);
    RUN(test_discover);
    RUN(test_html_text);
    RUN(test_html_helpers);
    RUN(test_curl_argv);
    RUN(test_response_headers);
    RUN(test_image_fetch);
    RUN(test_curl_client_offline);
    if (const char *base = std::getenv("RSS_TEST_SERVER"); base && *base) {
        int before = g_fail;
        test_curl_client_local(base);
        std::printf("%-28s %s\n", "test_curl_client_local", g_fail == before ? "ok" : "FAILED");
    } else {
        std::printf("%-28s skipped (RSS_TEST_SERVER not set; run_tests.sh starts tests/unit/local_server.py)\n", "test_curl_client_local");
    }
    RUN(test_catalog_data);
    RUN(test_catalog_queries);
    RUN(test_catalog_loader);
    RUN(test_atomic_files);
    RUN(test_store_settings);
    RUN(test_store_subscriptions);
    RUN(test_store_articles);
    RUN(test_store_saved);
    RUN(test_store_cap);
    RUN(test_store_recovery);
    RUN(test_store_saved_kept_in_cache);
    RUN(test_store_threads);
    RUN(test_store_newest_and_clear_read);
    RUN(test_merge);
    RUN(test_refresh_feed);
    RUN(test_refresh_edge);
    RUN(test_refresh_all);
    RUN(test_add_custom_feed);
    RUN(test_opml);
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
