/* SPDX-License-Identifier: MIT */
#include "output.hpp"

#include "../alsa/card.hpp"
#include "../pulse/bt_input.hpp"

#include <algorithm>
#include <cctype>

namespace cassette {

namespace {
bool is_hdmi(const CardInfo &c) {
    auto has = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s.find("hdmi") != std::string::npos;
    };
    return has(c.id) || has(c.name);
}

OutputOption option_for(const CardInfo &c) {
    OutputOption o;
    o.device = pcm_device(c);
    o.label = c.name.empty() ? c.id : c.name;
    o.card_id = c.id;
    return o;
}
} // namespace

OutputOption bt_output_option(const BtOutput &b) {
    OutputOption o;
    o.device = b.id;
    o.label = b.label;
    o.bluetooth = true;
    o.kind = b.kind;
    o.rate = b.rate;
    o.connected = b.connected;
    o.usable = b.usable;
    o.hint_profile = b.hint_profile;
    o.reason = b.reason;
    return o;
}

OutputPlan choose_output(const std::vector<CardInfo> &cards, const std::string &capture_card_id,
                         const std::string &saved_device, const std::vector<BtOutput> *bt,
                         const std::string &saved_bt_label) {
    std::vector<OutputOption> opts;
    for (const CardInfo &c : cards)
        if (c.playback && !capture_card_id.empty() && c.id == capture_card_id) opts.push_back(option_for(c));
    const bool own = !opts.empty();
    for (const CardInfo &c : cards)
        if (c.playback && c.id != capture_card_id && !is_hdmi(c)) opts.push_back(option_for(c));
    const size_t bt_first = opts.size();
    if (bt)
        for (const BtOutput &b : *bt) opts.push_back(bt_output_option(b));
    const std::string saved_mac = bt_mac_of_id(saved_device); /* a chosen Bluetooth output that is not there stays listed */
    if (!saved_mac.empty()) {
        bool seen = false;
        for (size_t i = bt_first; i < opts.size(); ++i)
            if (opts[i].device == bt_id_of(saved_mac)) seen = true;
        if (!seen) {
            OutputOption ph;
            ph.device = bt_id_of(saved_mac);
            ph.label = saved_bt_label.empty() ? std::string("Bluetooth device") : saved_bt_label;
            ph.bluetooth = true;
            ph.kind = "other";
            ph.connected = false;
            ph.usable = false;
            ph.hint_profile = "unknown";
            ph.reason = "Not connected";
            opts.push_back(ph);
        }
    }
    OutputOption def;
    def.device = kDefaultOutput;
    def.label = "Default output";
    opts.push_back(def);

    /* two cards with the same name: tell them apart by id */
    std::vector<bool> dup(opts.size(), false);
    for (size_t i = 0; i < opts.size(); ++i)
        for (size_t k = 0; k < opts.size(); ++k)
            if (i != k && !opts[i].card_id.empty() && opts[i].label == opts[k].label) dup[i] = true;
    for (size_t i = 0; i < opts.size(); ++i)
        if (dup[i]) opts[i].label += " (" + opts[i].card_id + ")";
    std::vector<bool> bdup(opts.size(), false);
    for (size_t i = 0; i < opts.size(); ++i)
        for (size_t k = 0; k < opts.size(); ++k)
            if (i != k && opts[i].bluetooth && opts[k].bluetooth && opts[i].label == opts[k].label) bdup[i] = true;
    for (size_t i = 0; i < opts.size(); ++i) /* two Bluetooth devices with one alias: the end of the address tells them apart */
        if (bdup[i]) opts[i].label += " (" + opts[i].device.substr(opts[i].device.size() - 5) + ")";

    OutputPlan plan;
    plan.chosen = own ? opts.front() : opts.back();
    const std::string want = saved_mac.empty() ? saved_device : bt_id_of(saved_mac);
    if (!want.empty())
        for (const OutputOption &o : opts)
            if (o.device == want) plan.chosen = o;
    if (opts.size() > 1) plan.choices = opts;
    return plan;
}

} // namespace cassette
