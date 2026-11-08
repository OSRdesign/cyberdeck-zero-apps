/* SPDX-License-Identifier: MIT */
#include "output.hpp"

#include "card.hpp"

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

OutputPlan choose_output(const std::vector<CardInfo> &cards, const std::string &capture_card_id,
                         const std::string &saved_device) {
    std::vector<OutputOption> opts;
    for (const CardInfo &c : cards)
        if (c.playback && !capture_card_id.empty() && c.id == capture_card_id) opts.push_back(option_for(c));
    const bool own = !opts.empty();
    for (const CardInfo &c : cards)
        if (c.playback && c.id != capture_card_id && !is_hdmi(c)) opts.push_back(option_for(c));
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

    OutputPlan plan;
    plan.chosen = own ? opts.front() : opts.back();
    if (!saved_device.empty())
        for (const OutputOption &o : opts)
            if (o.device == saved_device) plan.chosen = o;
    if (opts.size() > 1) plan.choices = opts;
    return plan;
}

} // namespace cassette
