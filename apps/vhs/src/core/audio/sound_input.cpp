/*
 * SPDX-License-Identifier: MIT
 *
 * VHS 0.1.2: the sound input chooser (see sound_input.hpp).
 */
#include "sound_input.hpp"

#include <cerrno>

namespace vhs {

std::string sound_descriptor(const CardInfo &c)
{
    const bool no_id = !usb_id_is_useful(c.usb_id);
    return c.name + "|" + c.usb_by_id + "|" + (no_id ? std::string() : c.usb_id);
}

CardDescriptor parse_sound_descriptor(const std::string &text)
{
    CardDescriptor d;
    const std::size_t a = text.find('|');
    if (a == std::string::npos) {
        d.name = text;
        return d;
    }
    const std::size_t b = text.find('|', a + 1);
    d.name = text.substr(0, a);
    d.usb_by_id = text.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (b != std::string::npos) d.usb_id = text.substr(b + 1);
    return d;
}

namespace {

std::string kind_of(const CardInfo &c)
{
    return c.usb_id.empty() && c.usb_port.empty() && c.usb_by_id.empty() ? "Built-in" : "USB";
}

} // namespace

SoundPlan plan_sound_input(AlsaApi &api, const std::string &setting, const MicCard &cam_mic)
{
    SoundPlan p;
    if (setting == "none") {
        p.chose_none = true;
        return p;
    }
    const std::vector<CaptureCard> cards = list_capture_cards(api, false);
    if (!setting.empty()) {
        std::vector<CardInfo> infos;
        for (const CaptureCard &c : cards) infos.push_back(c.info);
        const CardSelection sel = select_card(infos, parse_sound_descriptor(setting));
        if (sel.found()) {
            p.source = SoundSource::Card;
            p.chose_card = true;
            p.card = cards[static_cast<std::size_t>(sel.pos)].info;
            p.label = cards[static_cast<std::size_t>(sel.pos)].label;
            return p;
        }
        p.note = "Sound input " + parse_sound_descriptor(setting).name + " is not here: using Automatic.";
    }
    p.automatic = true;
    if (cam_mic.found) {
        p.source = SoundSource::CameraMic;
        p.card = cam_mic.card;
        p.label = "camera microphone";
        for (const CaptureCard &c : cards)
            if (c.info.id == cam_mic.card.id) p.label = c.label;
        return p;
    }
    if (!cards.empty()) {
        p.source = SoundSource::Card;
        p.card = cards.front().info;
        p.label = cards.front().label;
    }
    return p;
}

std::vector<SoundRow> sound_rows(AlsaApi &api, const std::string &setting, const MicCard &cam_mic)
{
    std::vector<SoundRow> out;
    const std::vector<CaptureCard> cards = list_capture_cards(api, false);
    std::vector<CardInfo> infos;
    for (const CaptureCard &c : cards) infos.push_back(c.info);
    int chosen = -1;
    if (!setting.empty() && setting != "none") chosen = select_card(infos, parse_sound_descriptor(setting)).pos;
    const SoundPlan now = plan_sound_input(api, "", cam_mic);
    SoundRow a;
    a.id = "auto";
    a.label = "Automatic";
    a.value = now.source == SoundSource::CameraMic ? "Camera microphone"
              : now.source == SoundSource::Card   ? now.label
                                                  : "No sound card";
    a.chosen = setting.empty() || (setting != "none" && chosen < 0); /* a card that is gone: Automatic is in force */
    out.push_back(a);
    for (std::size_t i = 0; i < cards.size(); ++i) {
        SoundRow r;
        r.id = sound_descriptor(cards[i].info);
        r.label = cards[i].label;
        r.value = kind_of(cards[i].info);
        r.chosen = static_cast<int>(i) == chosen;
        out.push_back(r);
    }
    SoundRow n;
    n.id = "none";
    n.label = "No sound";
    n.chosen = setting == "none";
    out.push_back(n);
    return out;
}

OpenResult open_sound(AlsaApi &api, const SoundPlan &plan)
{
    OpenResult r;
    if (plan.source == SoundSource::None) {
        r.status = OpenStatus::Failed;
        r.message = "No sound input";
        return r;
    }
    OpenOptions oo;
    oo.want_bits = 16; /* the recorder takes 16-bit sound */
    CaptureOffer hint;
    hint.min_channels = 1;
    hint.max_channels = 2;
    if (plan.source == SoundSource::CameraMic) {
        oo.allow_server = false; /* as 0.1.1: busy means video only */
        return open_capture(api, plan.card, oo);
    }
    oo.allow_server = true; /* a card a sound server holds is reached through the server's default device */
    oo.server_hint = &hint;
    r = open_capture(api, plan.card, oo);
    if (r.ok() || (r.status != OpenStatus::Unsupported && r.status != OpenStatus::Failed)) return r;
    /* hw: refused what it lists (or lists nothing usable): plughw: converts (ALSA's plug plugin), 48 kHz stereo */
    OpenResult p;
    HwRequest req;
    req.rate = 48000;
    req.channels = 2;
    req.format = SampleFormat::S16_LE;
    req.period_frames = oo.period_frames;
    req.buffer_frames = ((req.rate * oo.min_buffer_ms / 1000 + oo.period_frames - 1) / oo.period_frames) * oo.period_frames;
    p.device = "plughw:CARD=" + plan.card.id + ",DEV=0";
    const int err = api.open_capture(p.device, req, p.pcm, p.hw);
    if (err != 0) return r; /* the first failure's message is the better one */
    p.status = OpenStatus::Ok;
    p.format.rate = p.hw.rate ? p.hw.rate : req.rate;
    p.format.channels = static_cast<uint16_t>(p.hw.channels ? p.hw.channels : req.channels);
    p.format.bits = 16;
    p.message = "Converted by ALSA (plughw)";
    return p;
}

std::string sound_failure_text(const SoundPlan &plan, const OpenResult &r)
{
    std::string why = r.status == OpenStatus::Busy ? "the card is in use by another program" : r.message;
    if (why.empty()) why = "it cannot be opened";
    if (!why.empty() && why.back() == '.') why.pop_back();
    return "Sound input " + (plan.label.empty() ? plan.card.name : plan.label) + " failed: " + why + ".";
}

} // namespace vhs
