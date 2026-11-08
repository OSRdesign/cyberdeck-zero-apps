/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: capture cards, how to find one again, and how to tell two of them apart on screen.
 * A card is found by NAME, by /dev/snd/by-id path or by a saved descriptor, NEVER by index (P0: the index was 1, 0 and 2 on
 * different boots and boards).
 */
#pragma once

#include <string>
#include <vector>

#include "alsa_api.hpp"

namespace cassette {

/* What the settings store for the chosen card. Cheap cards may have no serial and report USB id 0000:0000 (the test headset:
 * maker, product and serial all "UC02"), so the descriptor is a set of hints, narrowest first. */
struct CardDescriptor {
    std::string name;       /* card short name: the main key */
    std::string id;         /* card id string: last-resort key when no name is stored */
    std::string usb_by_id;
    std::string usb_id;     /* "vvvv:pppp" */
    std::string usb_port;
    bool empty() const { return name.empty() && id.empty() && usb_by_id.empty(); }
};

CardDescriptor describe(const CardInfo &c);

/* "hw:CARD=<id>,DEV=0": the id string, never the index. Never plughw. */
std::string pcm_device(const CardInfo &c);
/* "hw:CARD=<id>" for the mixer. */
std::string mixer_device(const CardInfo &c);

/* USB ids that carry no information. */
bool usb_id_is_useful(const std::string &usb_id);

struct CaptureCard {
    CardInfo info;
    std::string label;     /* distinct among the listed cards: name, plus the USB port or by-id tail when names repeat */
    bool offer_known = false;
    CaptureOffer offer;    /* rates, channels, formats; unknown when the card was busy */
    int offer_error = 0;   /* the negative error when the query failed (-EBUSY ...) */
};

/* Capture cards only. With query_offers the capture PCM of each card is opened briefly: do not call while recording. */
std::vector<CaptureCard> list_capture_cards(AlsaApi &api, bool query_offers);

/* Sets `label` of every entry so that all are distinct. */
void assign_labels(std::vector<CaptureCard> &cards);

struct Selection {
    int pos = -1;             /* index into the vector passed in, -1 when no card matches */
    bool ambiguous = false;   /* several cards matched and the hints could not separate them (the first is used) */
    bool found() const { return pos >= 0; }
};

/* Chooses the card for a saved descriptor. Narrows by name (or by-id when no name matches), then by by-id path, USB id (when
 * it is not 0000:0000) and USB port, each step only when it leaves at least one card. */
Selection select_card(const std::vector<CardInfo> &cards, const CardDescriptor &want);

/* Builds a descriptor from user text: a "/dev/snd/by-id/..." path becomes usb_by_id, anything else a card name. */
CardDescriptor descriptor_from_text(const std::string &text);

/* "mono S16 48000, 44100 Hz" style summary of what a card offers. */
std::string offer_summary(const CaptureOffer &o);

} // namespace cassette
