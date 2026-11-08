/* SPDX-License-Identifier: MIT */
#include "card.hpp"

#include <algorithm>
#include <map>

namespace cassette {

CardDescriptor describe(const CardInfo &c) {
    CardDescriptor d;
    d.name = c.name;
    d.id = c.id;
    d.usb_by_id = c.usb_by_id;
    d.usb_id = c.usb_id;
    d.usb_port = c.usb_port;
    return d;
}

std::string pcm_device(const CardInfo &c) { return "hw:CARD=" + c.id + ",DEV=0"; }
std::string mixer_device(const CardInfo &c) { return "hw:CARD=" + c.id; }

bool usb_id_is_useful(const std::string &usb_id) { return !usb_id.empty() && usb_id != "0000:0000"; }

std::vector<CaptureCard> list_capture_cards(AlsaApi &api, bool query_offers) {
    std::vector<CaptureCard> out;
    for (const CardInfo &c : api.cards()) {
        if (!c.capture) continue;
        CaptureCard cc;
        cc.info = c;
        if (query_offers) {
            cc.offer_error = api.query_capture(pcm_device(c), cc.offer);
            cc.offer_known = cc.offer_error == 0;
        }
        out.push_back(cc);
    }
    assign_labels(out);
    return out;
}

namespace {
std::string tail_of(const std::string &path) {
    size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}
} // namespace

void assign_labels(std::vector<CaptureCard> &cards) {
    std::map<std::string, int> count;
    for (const auto &c : cards) ++count[c.info.name];
    std::vector<std::string> used;
    for (auto &c : cards) {
        std::string label = c.info.name.empty() ? c.info.id : c.info.name;
        if (count[c.info.name] > 1) {
            std::string extra = !c.info.usb_port.empty() ? c.info.usb_port
                                : !c.info.usb_by_id.empty() ? tail_of(c.info.usb_by_id)
                                                            : c.info.id;
            label += " (" + extra + ")";
        }
        /* still equal to an earlier label (same port text, empty identity): number it */
        std::string base = label;
        int n = 1;
        while (std::find(used.begin(), used.end(), label) != used.end()) label = base + " #" + std::to_string(++n);
        used.push_back(label);
        c.label = label;
    }
}

Selection select_card(const std::vector<CardInfo> &cards, const CardDescriptor &want) {
    Selection sel;
    std::vector<int> cand;
    for (size_t i = 0; i < cards.size(); ++i)
        if (cards[i].capture && !want.name.empty() && cards[i].name == want.name) cand.push_back(static_cast<int>(i));
    if (cand.empty() && !want.usb_by_id.empty())
        for (size_t i = 0; i < cards.size(); ++i)
            if (cards[i].capture && cards[i].usb_by_id == want.usb_by_id) cand.push_back(static_cast<int>(i));
    if (cand.empty() && want.name.empty() && !want.id.empty())
        for (size_t i = 0; i < cards.size(); ++i)
            if (cards[i].capture && cards[i].id == want.id) cand.push_back(static_cast<int>(i));
    if (cand.empty()) return sel;

    auto narrow = [&](auto field_of, const std::string &value) {
        if (cand.size() < 2 || value.empty()) return;
        std::vector<int> keep;
        for (int i : cand)
            if (field_of(cards[i]) == value) keep.push_back(i);
        if (!keep.empty()) cand = keep;
    };
    narrow([](const CardInfo &c) { return c.usb_by_id; }, want.usb_by_id);
    if (usb_id_is_useful(want.usb_id)) narrow([](const CardInfo &c) { return c.usb_id; }, want.usb_id);
    narrow([](const CardInfo &c) { return c.usb_port; }, want.usb_port);

    sel.pos = cand.front();
    sel.ambiguous = cand.size() > 1;
    return sel;
}

CardDescriptor descriptor_from_text(const std::string &text) {
    CardDescriptor d;
    if (text.rfind("/dev/snd/by-id/", 0) == 0) d.usb_by_id = text;
    else d.name = text;
    return d;
}

std::string offer_summary(const CaptureOffer &o) {
    std::string s;
    if (o.max_channels == 0) return "unknown";
    s += o.max_channels == 1 ? "mono" : (o.min_channels == o.max_channels ? std::to_string(o.max_channels) + " ch"
                                                                           : std::to_string(o.min_channels) + "-" +
                                                                                 std::to_string(o.max_channels) + " ch");
    for (SampleFormat f : o.formats) s += std::string(" ") + format_name(f);
    s += " at";
    for (size_t i = 0; i < o.rates.size(); ++i) s += (i ? ", " : " ") + std::to_string(o.rates[i]);
    s += " Hz";
    return s;
}

} // namespace cassette
