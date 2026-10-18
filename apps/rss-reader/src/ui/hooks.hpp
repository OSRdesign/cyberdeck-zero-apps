/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: what the scripted tests may replace before the app starts (tests/flows/script_runner.cpp): the HTTP
 * client (a fake that serves the fixtures of tests/data: no network in the tests) and the clock (stable ages in the
 * shots). The device build never sets them: the app uses CurlClient and the real time.
 */
#pragma once

#include "fetch.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace rssui {

struct Hooks {
    /* The client every network call of the app goes through (a refresh, Find feed). Called once at the app's start. */
    std::function<std::unique_ptr<rssreader::HttpClient>()> make_http;
    /* Epoch seconds. */
    std::function<std::int64_t()> now;
};

Hooks &hooks();

/* hooks().now, else the real time. */
std::int64_t now_epoch();

} // namespace rssui
