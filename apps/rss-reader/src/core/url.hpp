/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: the little URL handling the app needs (http / https only; no I/O).
 */
#pragma once

#include <string>

namespace rssreader {

bool is_http_url(const std::string &url);                    /* http:// or https:// with a host */
/* Resolves `ref` (an href / src found in a document) against `base`. Returns "" when the result is not an http(s) URL
 * (javascript:, data:, mailto:, an empty ref, a base that is not a URL and a relative ref). The fragment is dropped. */
std::string resolve_url(const std::string &base, const std::string &ref);
std::string url_origin(const std::string &url);              /* "https://host[:port]" or "" */
std::string url_host(const std::string &url);                /* lower case host without port or "" */
std::string url_path(const std::string &url);                /* "/a/b" (at least "/") or "" */
/* "example.com", "example.com/feed", "feed://example.com/x" -> an http(s) URL; "" if nothing usable. */
std::string normalize_user_url(const std::string &input);

} // namespace rssreader
