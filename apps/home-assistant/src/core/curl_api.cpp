/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: libcurl opened at run time (see curl_api.hpp).
 */
#include "curl_api.hpp"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <type_traits>

namespace ha {
namespace curl {

namespace {

Api g_api;
bool g_loaded = false;
std::string g_why;
std::once_flag g_once;

void do_load()
{
    const char *path = std::getenv("HA_LIBCURL");
    if (!path || !*path) path = "libcurl.so.4";
    void *lib = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char *e = ::dlerror();
        g_why = std::string("libcurl is missing (install the libcurl4t64 package): ") + (e ? e : "dlopen failed");
        return;
    }
    bool ok = true;
    auto sym = [&](auto &fn, const char *name) {
        void *p = ::dlsym(lib, name);
        if (!p) ok = false;
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
    };
    sym(g_api.global_init, "curl_global_init");
    sym(g_api.easy_init, "curl_easy_init");
    sym(g_api.easy_setopt, "curl_easy_setopt");
    sym(g_api.easy_perform, "curl_easy_perform");
    sym(g_api.easy_cleanup, "curl_easy_cleanup");
    sym(g_api.easy_getinfo, "curl_easy_getinfo");
    sym(g_api.easy_strerror, "curl_easy_strerror");
    sym(g_api.ws_recv, "curl_ws_recv");
    sym(g_api.ws_send, "curl_ws_send");
    sym(g_api.version_info, "curl_version_info");
    if (!ok) {
        g_why = "this libcurl is too old (no WebSocket functions): libcurl 8.11 or newer is needed";
        return;
    }
    VersionInfo *vi = g_api.version_info(0);
    bool ws = false;
    if (vi) {
        g_api.version = vi->version ? vi->version : "";
        for (const char *const *p = vi->protocols; p && *p; ++p)
            if (std::strcmp(*p, "ws") == 0) ws = true;
    }
    if (!ws) {
        g_why = "this libcurl (" + g_api.version + ") was built without WebSocket support: libcurl 8.11 or newer is needed";
        return;
    }
    if (g_api.global_init(GLOBAL_DEFAULT) != OK) {
        g_why = "libcurl did not start (curl_global_init)";
        return;
    }
    g_loaded = true;
}

} // namespace

bool load(std::string &why)
{
    std::call_once(g_once, do_load);
    if (!g_loaded) why = g_why;
    return g_loaded;
}

const Api &api()
{
    return g_api;
}

std::string strerror(Code c)
{
    if (g_api.easy_strerror) {
        const char *s = g_api.easy_strerror(c);
        if (s) return s;
    }
    return "curl error " + std::to_string(c);
}

} // namespace curl
} // namespace ha
