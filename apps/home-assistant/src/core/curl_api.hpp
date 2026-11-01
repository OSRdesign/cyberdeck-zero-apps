/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: libcurl, opened at run time (dlopen "libcurl.so.4", Debian's libcurl4t64), as Mesh Hop
 * opens libsystemd: the app links no new library and needs no change in the shared build (apps/_common/scons), and the
 * few types and constants are declared here as libcurl's C ABI has them (curl/curl.h and curl/websockets.h of 8.14.1,
 * the boards' version; stable since 7.x, the WebSocket API since 8.11). tests/integration/check_curl_abi.cpp compares
 * every constant with the real headers. Without the library, or with a libcurl built without WebSocket support (8.10
 * and older, or a build without it), load() says why and the app shows it.
 *
 * HA_LIBCURL=<path> loads another libcurl (the tests use a WebSocket-enabled build on a PC whose own libcurl lacks it).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ha {
namespace curl {

using Handle = void; /* CURL */
using Code = int;    /* CURLcode */
using off_t64 = long long; /* curl_off_t on the 64-bit Linux targets */

struct WsFrame { /* struct curl_ws_frame */
    int age;
    int flags;
    off_t64 offset;
    off_t64 bytesleft;
    std::size_t len;
};

struct VersionInfo { /* the head of curl_version_info_data (age 0 fields, stable) */
    int age;
    const char *version;
    unsigned int version_num;
    const char *host;
    int features;
    const char *ssl_version;
    long ssl_version_num;
    const char *libz_version;
    const char *const *protocols;
};

/* CURLcode */
constexpr Code OK = 0;
constexpr Code COULDNT_RESOLVE_HOST = 6;
constexpr Code COULDNT_CONNECT = 7;
constexpr Code OPERATION_TIMEDOUT = 28;
constexpr Code ABORTED_BY_CALLBACK = 42;
constexpr Code GOT_NOTHING = 52;
constexpr Code SEND_ERROR = 55;
constexpr Code RECV_ERROR = 56;
constexpr Code AGAIN = 81;

/* CURLoption: LONG + n, OBJECTPOINT 10000 + n, FUNCTIONPOINT 20000 + n */
constexpr int OPT_WRITEDATA = 10001;
constexpr int OPT_URL = 10002;
constexpr int OPT_ERRORBUFFER = 10010;
constexpr int OPT_WRITEFUNCTION = 20011;
constexpr int OPT_POSTFIELDS = 10015;
constexpr int OPT_USERAGENT = 10018;
constexpr int OPT_NOPROGRESS = 43;
constexpr int OPT_POST = 47;
constexpr int OPT_XFERINFODATA = 10057;
constexpr int OPT_POSTFIELDSIZE = 60;
constexpr int OPT_HTTPGET = 80;
constexpr int OPT_NOSIGNAL = 99;
constexpr int OPT_IPRESOLVE = 113;
constexpr int OPT_CONNECT_ONLY = 141;
constexpr int OPT_SOCKOPTFUNCTION = 20148;
constexpr int OPT_SOCKOPTDATA = 10149;
constexpr int OPT_TIMEOUT_MS = 155;
constexpr int OPT_CONNECTTIMEOUT_MS = 156;
constexpr int OPT_NOPROXY = 10177;
constexpr int OPT_TCP_KEEPALIVE = 213;
constexpr int OPT_TCP_KEEPIDLE = 214;
constexpr int OPT_TCP_KEEPINTVL = 215;
constexpr int OPT_XFERINFOFUNCTION = 20219;
constexpr int OPT_WS_OPTIONS = 320;

/* CURLINFO */
constexpr int INFO_RESPONSE_CODE = 0x200000 + 2;
constexpr int INFO_ACTIVESOCKET = 0x500000 + 44;

constexpr long GLOBAL_DEFAULT = 3; /* CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32 */
constexpr long IPRESOLVE_V4 = 1;
constexpr int SOCKOPT_OK = 0;
constexpr int SOCKOPT_ERROR = 1;

/* curl_ws_frame flags */
constexpr int WS_TEXT = 1 << 0;
constexpr int WS_BINARY = 1 << 1;
constexpr int WS_CONT = 1 << 2;
constexpr int WS_CLOSE = 1 << 3;
constexpr int WS_PING = 1 << 4;
constexpr int WS_OFFSET = 1 << 5;
constexpr int WS_PONG = 1 << 6;

using WriteFn = std::size_t (*)(char *ptr, std::size_t size, std::size_t nmemb, void *user);
using SockoptFn = int (*)(void *user, int fd, int purpose);
using XferinfoFn = int (*)(void *user, off_t64 dltotal, off_t64 dlnow, off_t64 ultotal, off_t64 ulnow);

struct Api {
    Code (*global_init)(long) = nullptr;
    Handle *(*easy_init)() = nullptr;
    Code (*easy_setopt)(Handle *, int, ...) = nullptr;
    Code (*easy_perform)(Handle *) = nullptr;
    void (*easy_cleanup)(Handle *) = nullptr;
    Code (*easy_getinfo)(Handle *, int, ...) = nullptr;
    const char *(*easy_strerror)(Code) = nullptr;
    Code (*ws_recv)(Handle *, void *, std::size_t, std::size_t *, const WsFrame **) = nullptr;
    Code (*ws_send)(Handle *, const void *, std::size_t, std::size_t *, off_t64, unsigned int) = nullptr;
    VersionInfo *(*version_info)(int) = nullptr;
    std::string version;
};

/* Opens libcurl once (any thread; the connection thread calls it first). False with a reason in plain words. */
bool load(std::string &why);
const Api &api();
std::string strerror(Code c);

} // namespace curl
} // namespace ha
