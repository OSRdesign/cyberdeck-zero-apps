/* SPDX-License-Identifier: MIT */
#include <set>

#include "card.hpp"
#include "check.hpp"
#include "fake_alsa.hpp"

using namespace cassette;

namespace {

std::vector<CardInfo> infos(FakeAlsa &f) { return f.cards(); }

void by_name_when_index_changes() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0), fake_uc02(1)};
    CardDescriptor saved = describe(f.cards_[0].info); /* the PCM2902 on this boot */
    CHECK_EQ(saved.name, "USB Audio CODEC");

    for (int boot = 0; boot < 3; ++boot) {
        f.reboot_shuffle(); /* the index changes: 1, 0 and 2 on real boots */
        std::vector<CardInfo> v = infos(f);
        Selection s = select_card(v, saved);
        CHECK(s.found());
        CHECK(!s.ambiguous);
        CHECK_EQ(v[s.pos].id, "Device");
        CHECK_EQ(pcm_device(v[s.pos]), "hw:CARD=Device,DEV=0"); /* the id, not the index */
        CHECK_EQ(pcm_device(v[s.pos]).find("plughw"), std::string::npos);
    }
    CHECK_EQ(mixer_device(f.cards_[0].info), "hw:CARD=" + f.cards_[0].info.id);

    /* a third card on index 2 */
    f.cards_.push_back(fake_card_no_controls(2));
    f.cards_[2].info.index = 0; /* collides with nothing that matters: index is never the key */
    CHECK(select_card(infos(f), saved).found());
}

void same_name_gets_distinct_labels() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0, "Device", "1-1"), fake_pcm2902(1, "Device_1", "1-2")};
    std::vector<CaptureCard> cards = list_capture_cards(f, false);
    CHECK_EQ(cards.size(), 2u);
    CHECK(cards[0].label != cards[1].label);
    CHECK(cards[0].label.find("1-1") != std::string::npos);
    CHECK(cards[1].label.find("1-2") != std::string::npos);

    /* the saved descriptor of the second one still finds the second one */
    CardDescriptor saved = describe(f.cards_[1].info);
    Selection s = select_card(infos(f), saved);
    CHECK(s.found());
    CHECK(!s.ambiguous);
    CHECK_EQ(infos(f)[s.pos].id, "Device_1");

    /* a name alone cannot choose between them: reported, the first is used */
    CardDescriptor by_name = descriptor_from_text("USB Audio CODEC");
    // the two cards share by-id and usb id in this fixture, so only the port could separate them
    Selection a = select_card(infos(f), by_name);
    CHECK(a.found());
    CHECK(a.ambiguous);

    /* no port, no by-id: the id string distinguishes the labels */
    FakeAlsa g;
    g.cards_ = {fake_pcm2902(0, "Device", ""), fake_pcm2902(1, "Device_1", "")};
    for (auto &c : g.cards_) {
        c.info.usb_port.clear();
        c.info.usb_by_id.clear();
    }
    std::vector<CaptureCard> gc = list_capture_cards(g, false);
    CHECK(gc[0].label != gc[1].label);

    /* every field equal and even the id repeated (should not happen): numbered, still distinct */
    std::vector<CaptureCard> dup(3);
    for (auto &c : dup) c.info.name = "X";
    assign_labels(dup);
    std::set<std::string> uniq{dup[0].label, dup[1].label, dup[2].label};
    CHECK_EQ(uniq.size(), 3u);

    /* a unique name keeps the plain name */
    FakeAlsa h;
    h.cards_ = {fake_pcm2902(0), fake_uc02(1)};
    std::vector<CaptureCard> hc = list_capture_cards(h, false);
    CHECK_EQ(hc[0].label, "USB Audio CODEC");
    CHECK_EQ(hc[1].label, "UC02");
}

void headset_identity_not_unique() {
    FakeAlsa f;
    f.cards_ = {fake_uc02(0, "1-1.2")};
    CardDescriptor saved = describe(f.cards_[0].info);
    CHECK_EQ(saved.usb_id, "0000:0000");
    CHECK(!usb_id_is_useful(saved.usb_id));
    CHECK(usb_id_is_useful("08bb:2902"));
    CHECK(!usb_id_is_useful(""));

    /* same single headset on another boot with another index and port: still found by name */
    f.cards_[0].info.index = 2;
    f.cards_[0].info.usb_port = "1-1.4";
    CHECK(select_card(infos(f), saved).found());

    /* two headsets, both "UC02" with 0000:0000: only the USB port separates them */
    FakeCard second = fake_uc02(1, "1-1.3");
    second.info.id = "UC02_1";
    f.cards_ = {fake_uc02(0, "1-1.2"), second};
    saved = describe(f.cards_[1].info);
    Selection s = select_card(infos(f), saved);
    CHECK(s.found());
    CHECK(!s.ambiguous);
    CHECK_EQ(infos(f)[s.pos].id, "UC02_1");
    std::vector<CaptureCard> lc = list_capture_cards(f, false);
    CHECK(lc[0].label != lc[1].label);
    CHECK(lc[0].label.find("1-1.2") != std::string::npos);

    /* a saved by-id path alone (no name) finds the card */
    FakeAlsa g;
    g.cards_ = {fake_pcm2902(0), fake_uc02(1)};
    Selection b = select_card(infos(g), descriptor_from_text("/dev/snd/by-id/usb-UC02_UC02_UC02-00"));
    CHECK(b.found());
    CHECK_EQ(infos(g)[b.pos].name, "UC02");
}

void selection_edges() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0), fake_uc02(1)};
    CHECK(!select_card(infos(f), descriptor_from_text("Nonexistent")).found());
    CHECK(!select_card(infos(f), CardDescriptor()).found());
    CHECK(CardDescriptor().empty());
    CHECK(!select_card({}, descriptor_from_text("UC02")).found());

    /* a card without capture is never listed or selected */
    f.cards_[1].info.capture = false;
    CHECK_EQ(list_capture_cards(f, false).size(), 1u);
    CHECK(!select_card(infos(f), descriptor_from_text("UC02")).found());

    /* renamed by name but same by-id: found by the by-id fallback */
    f.cards_[1].info.capture = true;
    CardDescriptor d = describe(f.cards_[1].info);
    d.name = "Old name";
    CHECK(select_card(infos(f), d).found()); /* the name matches nothing: the by-id path still identifies the card */
    d.usb_by_id.clear();
    CHECK(!select_card(infos(f), d).found()); /* a stale name with nothing else finds nothing */

    /* text forms */
    CHECK_EQ(descriptor_from_text("/dev/snd/by-id/usb-X-00").usb_by_id, "/dev/snd/by-id/usb-X-00");
    CHECK_EQ(descriptor_from_text("Some card").name, "Some card");
}

void offers() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0), fake_uc02(1)};
    std::vector<CaptureCard> c = list_capture_cards(f, true);
    CHECK(c[0].offer_known);
    CHECK_EQ(offer_summary(c[0].offer), "mono S16 at 44100, 48000 Hz");
    CHECK(c[0].offer.has_rate(44100) && c[0].offer.has_rate(48000));
    CHECK_EQ(offer_summary(c[1].offer), "mono S16 at 48000 Hz");
    CHECK(!c[1].offer.has_rate(44100));

    /* a busy card cannot be queried: listed, offer unknown, error kept */
    f.cards_[1].busy = true;
    c = list_capture_cards(f, true);
    CHECK_EQ(c.size(), 2u);
    CHECK(!c[1].offer_known);
    CHECK_EQ(c[1].offer_error, -16);
    CHECK_EQ(offer_summary(c[1].offer), "unknown");

    /* without querying nothing is opened */
    f.queried.clear();
    list_capture_cards(f, false);
    CHECK(f.queried.empty());
}

} // namespace

void test_card() {
    by_name_when_index_changes();
    same_name_gets_distinct_labels();
    headset_identity_not_unique();
    selection_edges();
    offers();
}
