/*
 * SPDX-License-Identifier: MIT
 *
 * VHS 0.1.2: the sound input chooser (plan 062 "Cassette-style sound input chooser", pulled forward from P2). The list is
 * Automatic, every ALSA capture card (USB, HDA, built-in: the cards Cassette lists), then "No sound". A choice is stored as
 * Cassette's card descriptor "name|usb by-id|vid:pid" (never the card's index), "" for Automatic and "none" for No sound.
 *
 *   Automatic   the camera's own microphone when it has one (matched by USB ancestry, hw: only, as 0.1.1), else the first
 *               ALSA capture card, else no sound
 *   a card      opened hw: first (Cassette's policy: its own rate, mono or stereo, S16 or a 24 / 32-bit format cut to 16 bits);
 *               a card that refuses what it lists is opened as plughw: at 48 kHz stereo (ALSA converts); a card a sound server
 *               holds is opened through the server's "default" device
 *   No sound    nothing is opened
 * A stored card that is not there falls back to Automatic and the plan says so. LVGL-free.
 */
#pragma once

#include "alsa/alsa_api.hpp"
#include "alsa/card.hpp"
#include "alsa/open_policy.hpp"
#include "mic_match.hpp"

#include <string>
#include <vector>

namespace vhs {

/* Cassette's descriptor text of a card: name|by-id|vid:pid (0000:0000 left out). */
std::string sound_descriptor(const CardInfo &c);
CardDescriptor parse_sound_descriptor(const std::string &text);

struct SoundRow {
    std::string id;     /* "auto", "none" or the descriptor */
    std::string label;  /* "Automatic", "HDA Intel PCH", "No sound" */
    std::string value;  /* Automatic: what it uses now; a card: "Built-in" or "USB" */
    bool chosen = false;
};

enum class SoundSource { None, CameraMic, Card };

struct SoundPlan {
    SoundSource source = SoundSource::None;
    bool chose_none = false;   /* the stored choice is "No sound" */
    bool chose_card = false;   /* the stored choice is a card that is present (a failure must be shown, never hidden) */
    bool automatic = false;    /* no stored choice (or one that is gone) */
    CardInfo card;             /* CameraMic, Card */
    std::string label;         /* the card's distinct label, for the log and the toasts */
    std::string note;          /* "Sound input X is not here: using Automatic." */
};

/* The card the setting names, with Automatic as the fallback. cam_mic: the camera's own card (found = false when it has none). */
SoundPlan plan_sound_input(AlsaApi &api, const std::string &setting, const MicCard &cam_mic);

/* The rows of the chooser; `setting` marks the chosen one. */
std::vector<SoundRow> sound_rows(AlsaApi &api, const std::string &setting, const MicCard &cam_mic);

/* Opens the plan's input for recording (16-bit samples at the card's rate; the status says why not). None: status Failed
 * with an empty device. */
OpenResult open_sound(AlsaApi &api, const SoundPlan &plan);

/* "Sound input X failed: <reason>." text for a toast. */
std::string sound_failure_text(const SoundPlan &plan, const OpenResult &r);

} // namespace vhs
