/*
 * SPDX-License-Identifier: MIT
 *
 * Compile-time check: the libcurl constants and structures the app declares itself (src/core/curl_api.hpp, the app
 * opens libcurl at run time and includes no curl header) equal those of the real headers. Built by run.sh with
 * -I<curl include dir>: a mismatch does not compile. The headers of curl 8.14.1 (the boards' version) are the reference.
 */
#include <curl/curl.h>

#include "curl_api.hpp"

#include <cstddef>
#include <cstdio>

namespace c = ha::curl;

static_assert(c::OK == CURLE_OK, "CURLE_OK");
static_assert(c::COULDNT_RESOLVE_HOST == CURLE_COULDNT_RESOLVE_HOST, "CURLE_COULDNT_RESOLVE_HOST");
static_assert(c::COULDNT_CONNECT == CURLE_COULDNT_CONNECT, "CURLE_COULDNT_CONNECT");
static_assert(c::OPERATION_TIMEDOUT == CURLE_OPERATION_TIMEDOUT, "CURLE_OPERATION_TIMEDOUT");
static_assert(c::ABORTED_BY_CALLBACK == CURLE_ABORTED_BY_CALLBACK, "CURLE_ABORTED_BY_CALLBACK");
static_assert(c::GOT_NOTHING == CURLE_GOT_NOTHING, "CURLE_GOT_NOTHING");
static_assert(c::SEND_ERROR == CURLE_SEND_ERROR, "CURLE_SEND_ERROR");
static_assert(c::RECV_ERROR == CURLE_RECV_ERROR, "CURLE_RECV_ERROR");
static_assert(c::AGAIN == CURLE_AGAIN, "CURLE_AGAIN");

static_assert(c::OPT_WRITEDATA == CURLOPT_WRITEDATA, "CURLOPT_WRITEDATA");
static_assert(c::OPT_URL == CURLOPT_URL, "CURLOPT_URL");
static_assert(c::OPT_ERRORBUFFER == CURLOPT_ERRORBUFFER, "CURLOPT_ERRORBUFFER");
static_assert(c::OPT_WRITEFUNCTION == CURLOPT_WRITEFUNCTION, "CURLOPT_WRITEFUNCTION");
static_assert(c::OPT_POSTFIELDS == CURLOPT_POSTFIELDS, "CURLOPT_POSTFIELDS");
static_assert(c::OPT_USERAGENT == CURLOPT_USERAGENT, "CURLOPT_USERAGENT");
static_assert(c::OPT_NOPROGRESS == CURLOPT_NOPROGRESS, "CURLOPT_NOPROGRESS");
static_assert(c::OPT_POST == CURLOPT_POST, "CURLOPT_POST");
static_assert(c::OPT_XFERINFODATA == CURLOPT_XFERINFODATA, "CURLOPT_XFERINFODATA");
static_assert(c::OPT_POSTFIELDSIZE == CURLOPT_POSTFIELDSIZE, "CURLOPT_POSTFIELDSIZE");
static_assert(c::OPT_HTTPGET == CURLOPT_HTTPGET, "CURLOPT_HTTPGET");
static_assert(c::OPT_NOSIGNAL == CURLOPT_NOSIGNAL, "CURLOPT_NOSIGNAL");
static_assert(c::OPT_IPRESOLVE == CURLOPT_IPRESOLVE, "CURLOPT_IPRESOLVE");
static_assert(c::OPT_CONNECT_ONLY == CURLOPT_CONNECT_ONLY, "CURLOPT_CONNECT_ONLY");
static_assert(c::OPT_SOCKOPTFUNCTION == CURLOPT_SOCKOPTFUNCTION, "CURLOPT_SOCKOPTFUNCTION");
static_assert(c::OPT_SOCKOPTDATA == CURLOPT_SOCKOPTDATA, "CURLOPT_SOCKOPTDATA");
static_assert(c::OPT_TIMEOUT_MS == CURLOPT_TIMEOUT_MS, "CURLOPT_TIMEOUT_MS");
static_assert(c::OPT_CONNECTTIMEOUT_MS == CURLOPT_CONNECTTIMEOUT_MS, "CURLOPT_CONNECTTIMEOUT_MS");
static_assert(c::OPT_NOPROXY == CURLOPT_NOPROXY, "CURLOPT_NOPROXY");
static_assert(c::OPT_TCP_KEEPALIVE == CURLOPT_TCP_KEEPALIVE, "CURLOPT_TCP_KEEPALIVE");
static_assert(c::OPT_TCP_KEEPIDLE == CURLOPT_TCP_KEEPIDLE, "CURLOPT_TCP_KEEPIDLE");
static_assert(c::OPT_TCP_KEEPINTVL == CURLOPT_TCP_KEEPINTVL, "CURLOPT_TCP_KEEPINTVL");
static_assert(c::OPT_XFERINFOFUNCTION == CURLOPT_XFERINFOFUNCTION, "CURLOPT_XFERINFOFUNCTION");
static_assert(c::OPT_WS_OPTIONS == CURLOPT_WS_OPTIONS, "CURLOPT_WS_OPTIONS");

static_assert(c::INFO_RESPONSE_CODE == CURLINFO_RESPONSE_CODE, "CURLINFO_RESPONSE_CODE");
static_assert(c::INFO_ACTIVESOCKET == CURLINFO_ACTIVESOCKET, "CURLINFO_ACTIVESOCKET");
static_assert(c::GLOBAL_DEFAULT == CURL_GLOBAL_DEFAULT, "CURL_GLOBAL_DEFAULT");
static_assert(c::IPRESOLVE_V4 == CURL_IPRESOLVE_V4, "CURL_IPRESOLVE_V4");
static_assert(c::SOCKOPT_OK == CURL_SOCKOPT_OK, "CURL_SOCKOPT_OK");
static_assert(c::SOCKOPT_ERROR == CURL_SOCKOPT_ERROR, "CURL_SOCKOPT_ERROR");

static_assert(c::WS_TEXT == CURLWS_TEXT && c::WS_BINARY == CURLWS_BINARY && c::WS_CONT == CURLWS_CONT &&
                  c::WS_CLOSE == CURLWS_CLOSE && c::WS_PING == CURLWS_PING && c::WS_OFFSET == CURLWS_OFFSET &&
                  c::WS_PONG == CURLWS_PONG,
              "CURLWS_* flags");

static_assert(sizeof(c::WsFrame) == sizeof(struct curl_ws_frame), "curl_ws_frame size");
static_assert(offsetof(c::WsFrame, flags) == offsetof(struct curl_ws_frame, flags), "curl_ws_frame.flags");
static_assert(offsetof(c::WsFrame, offset) == offsetof(struct curl_ws_frame, offset), "curl_ws_frame.offset");
static_assert(offsetof(c::WsFrame, bytesleft) == offsetof(struct curl_ws_frame, bytesleft), "curl_ws_frame.bytesleft");
static_assert(offsetof(c::WsFrame, len) == offsetof(struct curl_ws_frame, len), "curl_ws_frame.len");
static_assert(sizeof(c::off_t64) == sizeof(curl_off_t), "curl_off_t");
static_assert(offsetof(c::VersionInfo, version) == offsetof(curl_version_info_data, version), "version");
static_assert(offsetof(c::VersionInfo, protocols) == offsetof(curl_version_info_data, protocols), "protocols");
static_assert(sizeof(curl_socket_t) == sizeof(int), "curl_socket_t is an int");

int main()
{
    std::puts("libcurl ABI as declared by the app: identical to the headers (" LIBCURL_VERSION ")");
    return 0;
}
