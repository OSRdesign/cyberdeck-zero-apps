/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: `iw dev <if> link` through async_run (see wifi_link.hpp).
 */
#include "wifi_link.hpp"

#include <memory>
#include <utility>

namespace nt {

cdk::async_run::JobId request_wifi_link(const std::string &iface,
                                        std::function<void(const IwLink &link, bool ran)> done)
{
    auto text = std::make_shared<std::string>();
    cdk::async_run::Spec spec;
    spec.argv = iw_link_argv(iface);
    spec.timeout_ms = kIwTimeoutMs;
    spec.merge_stderr = false;
    spec.max_line = 512;
    spec.on_line = [text](const std::string &line) {
        if (text->size() < 16384) *text += line + "\n";
    };
    spec.on_done = [text, done = std::move(done)](const cdk::async_run::Result &r) {
        if (!done) return;
        if (!r.ok()) {
            done(IwLink(), false);
            return;
        }
        done(parse_iw_link(*text), true);
    };
    return cdk::async_run::run(std::move(spec));
}

} // namespace nt
