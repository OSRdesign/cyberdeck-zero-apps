/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the AudioPlayer facade (see audio_player.hpp). The volume code is Vinyl 0.3.0's controller (apps/vinyl at fdad829,
 * ui/vinyl_app.cpp: volume_target, sink_model, output_step, apply_gain, volume_step, the output scan) without its UI:
 * the same output_volume model, the same presses, the same log lines (prefix [vhs]).
 */
#include "play/audio_player.hpp"

#include "play/audio_player_deps.hpp"

#include "audio/alsa/alsa_api.hpp"
#include "launcher/launcher_link.hpp"
#include "live/clock.hpp"
#include "play/app/pulse_worker.hpp"
#include "play/app/sink_watch.hpp"
#include "play/engine/engine.hpp"
#include "play/engine/outputs.hpp"
#include "play/input/media_keys.hpp"
#include "play/volume/output_volume.hpp"
#include "play/volume/volume_state.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <sys/stat.h>
#include <thread>

namespace vhs {

namespace {
constexpr std::int64_t kExtrapolateMaxMs = 250; /* the clock runs on between two engine snapshots for this long at most */
constexpr unsigned kScanPeriodMs = 5000;        /* the sound server's lists, besides its change events */
constexpr unsigned kSlowMs = 100;               /* the scans, the mixer and the gain are looked at this often at most */
}  // namespace

std::unique_ptr<AudioPlayerDeps> real_audio_deps() {
    auto d = std::make_unique<AudioPlayerDeps>();
    d->pulse = make_real_pulse(); /* libpulse is mapped at the first connect, on the worker */
    d->alsa = make_real_alsa();   /* libasound at the first call */
    d->sinks = make_real_sink_provider(d->pulse.get(), *d->alsa);
    d->use_pulse_worker = true;
    d->read_launcher_files = true;
    d->media_keys = true;
    d->volume_state_file = volume_state_path();
    return d;
}

struct AudioPlayer::Impl {
    Impl(const std::string &key, std::unique_ptr<AudioPlayerDeps> d) {
        if (!d) d = std::make_unique<AudioPlayerDeps>();
        pulse = std::move(d->pulse);
        alsa = std::move(d->alsa);
        sinks = std::move(d->sinks);
        if (!sinks) {
            if (!alsa) alsa = make_real_alsa();
            sinks = make_real_sink_provider(pulse.get(), *alsa);
        }
        world = std::move(d->world);
        vol_file = d->volume_state_file;
        open_timeout_ms = d->open_timeout_ms;
        target = OutputTarget::from_key(key);
        if (d->use_pulse_worker && pulse) worker = std::make_unique<PulseWorker>(*pulse);
        if (d->read_launcher_files) launcher = std::make_unique<LauncherStatusReader>(std::move(d->facts_io));
        if (d->media_keys) {
            keys.start(d->keys_devices_path, d->keys_dev_dir);
            keys_on = true;
        }
        if (!vol_file.empty()) load_volume_state(vol_file, vol);
        engine = std::make_unique<Engine>(d->libs.ffmpeg ? d->libs : real_decode_libs(), *sinks);
        engine->set_output(target);
        snap = engine->snapshot();
        last_scan = mono_ms_now();
        rows_dirty = true;
        if (worker) scan(true);
        apply_gain();
    }

    ~Impl() { shutdown(); }

    /* destruction order: the engine (it uses the sinks) first, then the worker, then the sinks, ALSA and the server */
    mutable std::mutex mu;
    std::unique_ptr<PulseApi> pulse;
    std::unique_ptr<AlsaApi> alsa;
    std::unique_ptr<SinkProvider> sinks;
    std::unique_ptr<PulseWorker> worker;
    SinkWatch sink_watch;
    std::unique_ptr<LauncherStatusReader> launcher;
    MediaKeyReader keys;
    bool keys_on = false;
    std::unique_ptr<Engine> engine;

    OutputWorld world;
    OutputTarget target;
    LauncherStatus status;
    std::vector<OutputRow> out_rows;
    bool rows_dirty = true;
    uint64_t last_cards = 0, last_scan = 0, last_slow = 0, last_log = 0;
    EngineSnapshot snap;
    bool down = false;
    int open_timeout_ms = 5000;

    /* the file */
    enum class OpenState { None, Opening, Open, Failed };
    OpenState open_state = OpenState::None;
    bool is_open = false, has_audio = false, ended = false, output_lost = false;
    std::string path, error;
    uint64_t seeks_issued = 0;
    std::int64_t seek_target_ms = 0;
    uint64_t epoch = 0; /* a new open, a seek, a play after the end: the clock may start from anywhere */
    std::int64_t last_heard_ms = 0;
    bool last_playing = false;
    uint64_t last_epoch = 0;
    std::deque<AudioPlayer::Key> key_q;

    /* the volume (Vinyl's D11 as the owner changed it) */
    VolumeState vol;
    std::string vol_file;
    int pending_level = -1;
    std::string pending_sink;
    uint64_t pending_until = 0;
    std::string mixer_card, mixer_choice_card;
    MixerChoice mixer_choice;
    MixerLevel mixer_level;
    uint64_t last_mixer_read = 0;
    mutable std::string model_sink;
    mutable int model_card = -2;
    mutable OutputVolumeModel model_of_sink;
    std::string vol_logged;

    /* ------------------------------------------------------------------------------------------ the engine's side */

    void shutdown() {
        if (down) return;
        down = true;
        if (engine) engine->shutdown();
        if (keys_on) keys.stop();
        if (worker) worker->stop();
        if (pulse) pulse->disconnect();
    }

    void service() {
        if (down) return;
        const uint64_t now = mono_ms_now();
        drain_events();
        snap = engine->snapshot();
        if (last_slow != 0 && now - last_slow < kSlowMs) return;
        last_slow = now;
        if (launcher && launcher->poll(now)) {
            status = launcher->status();
            world.facts = status.facts;
            rows_dirty = true;
        }
        take_scan();
        /* a sink changed (a volume key, a headset's buttons, a reconnect): look again now, not at the next periodic scan */
        if (worker && sink_watch.take_change()) {
            if (scan(true)) last_scan = now;
            else sink_watch.keep_change();
        }
        if (worker && now - last_scan > kScanPeriodMs) {
            last_scan = now;
            scan(true);
        }
        if (!world.server && alsa && now - last_cards >= kScanPeriodMs) { /* without a server the chooser lists the cards */
            last_cards = now;
            world.alsa_cards = alsa->cards();
            rows_dirty = true;
        }
        if (rows_dirty) {
            out_rows = build_output_rows(world, target);
            rows_dirty = false;
        }
        read_output_mixer(now, false);
        apply_gain();
        log_volume(now);
    }

    void drain_events() {
        EngineEvent ev;
        while (engine->poll_event(ev)) on_engine(ev);
    }

    void on_engine(const EngineEvent &ev) {
        switch (ev.kind) {
        case EngineEvent::Kind::Started:
            if (open_state == OpenState::Opening && ev.path == path) {
                open_state = OpenState::Open;
                is_open = true;
                has_audio = true;
            }
            break;
        case EngineEvent::Kind::Advanced: break;
        case EngineEvent::Kind::QueueEnd:
            if (is_open && has_audio) ended = true; /* drained, paused back at the start: the clock holds the end */
            break;
        case EngineEvent::Kind::OpenFailed:
            if (open_state == OpenState::Opening && ev.path == path) {
                if (ev.err == kNoAudioStream) { /* a video-only file: not an error */
                    open_state = OpenState::Open;
                    is_open = true;
                    has_audio = false;
                    std::fprintf(stderr, "[vhs] audio: %s has no audio stream\n", path.c_str());
                } else {
                    open_state = OpenState::Failed;
                    is_open = has_audio = false;
                    error = ev.text.empty() ? std::string("The file cannot be played.") : ev.text;
                    std::fprintf(stderr, "[vhs] audio: cannot open %s: %s\n", path.c_str(), error.c_str());
                }
            }
            break;
        case EngineEvent::Kind::OutputLost:
            if (is_open) {
                output_lost = true;
                error = (ev.text.empty() ? std::string("The output") : ev.text) + " disconnected. Paused.";
            }
            break;
        case EngineEvent::Kind::OutputFailed:
            if (is_open) {
                std::string what = output_label();
                if (what.empty()) what = "The output";
                error = what + " cannot play: " + ev.text + ".";
            }
            break;
        }
    }

    /* false: the file cannot be opened at all (the error is set); true: the engine thread is opening it */
    bool begin_open(const std::string &p, std::int64_t start_ms, bool play) {
        is_open = has_audio = ended = output_lost = false;
        error.clear();
        path = p;
        ++epoch;
        struct stat st;
        if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            error = track_error_text(::stat(p.c_str(), &st) != 0 ? -errno : kNotAudio, p);
            open_state = OpenState::Failed;
            engine->unload();
            return false;
        }
        open_state = OpenState::Opening;
        engine->load(0, p, start_ms < 0 ? 0 : static_cast<uint64_t>(start_ms), play);
        return true;
    }

    AudioClock clock() {
        service();
        AudioClock c;
        c.open = is_open;
        c.has_audio = has_audio;
        c.error = error;
        c.output_lost = output_lost;
        c.output_label = output_label();
        if (!is_open || !has_audio) return c;
        c.total_ms = static_cast<std::int64_t>(snap.total_ms());
        if (ended) {
            c.ended = true;
            c.heard_ms = c.total_ms;
            last_playing = false;
            return c;
        }
        c.playing = snap.state == PlayState::Playing;
        std::int64_t ms;
        if (snap.seeks < seeks_issued) { /* the seek is not applied yet: the target, standing still */
            ms = seek_target_ms;
            last_playing = false;
        } else {
            ms = static_cast<std::int64_t>(snap.pos_ms());
            if (c.playing) {
                const std::int64_t dt = (mono_us() - snap.at_us) / 1000;
                ms += std::max<std::int64_t>(0, std::min<std::int64_t>(dt, kExtrapolateMaxMs));
                if (last_playing && epoch == last_epoch && ms < last_heard_ms) ms = last_heard_ms; /* never backwards */
            }
            last_playing = c.playing;
        }
        if (c.total_ms > 0 && ms > c.total_ms) ms = c.total_ms;
        last_heard_ms = ms;
        last_epoch = epoch;
        c.heard_ms = ms;
        return c;
    }

    /* ------------------------------------------------------------------------------------------ keys */

    AudioPlayer::Key poll_key() {
        if (keys_on)
            for (const MediaEvent &e : keys.take()) {
                if (!e.down) continue;
                switch (e.action) {
                case MediaAction::Toggle: key_q.push_back(AudioPlayer::Key::PlayPause); break;
                case MediaAction::Next: key_q.push_back(AudioPlayer::Key::Next); break;
                case MediaAction::Prev: key_q.push_back(AudioPlayer::Key::Prev); break;
                case MediaAction::Stop: key_q.push_back(AudioPlayer::Key::Stop); break;
                case MediaAction::None: break;
                }
            }
        if (key_q.empty()) return AudioPlayer::Key::None;
        const AudioPlayer::Key k = key_q.front();
        key_q.pop_front();
        return k;
    }

    /* ------------------------------------------------------------------------------------------ outputs */

    /* false: a scan is already running (ask again later) */
    bool scan(bool connect) {
        if (!worker) return false;
        ScanRequest rq;
        rq.connect = connect;
        rq.app_name = "VHS";
        rq.connect_timeout_ms = 1500;
        rq.callback = sink_watch.callback(); /* the server's change events, once per connection */
        return worker->request_scan(rq);
    }

    void take_scan() {
        if (!worker) return;
        ScanResult r;
        if (!worker->take_scan(r)) return;
        sink_watch.scanned(r);
        world.server = r.connected;
        world.sinks.clear();
        for (const PulseDevice &s : r.sinks)
            if (!s.is_monitor) world.sinks.push_back(s);
        world.cards = r.cards;
        if (r.have_info) world.default_sink = r.info.default_sink;
        for (const PulseDevice &s : world.sinks)
            if (!s.bt_address.empty() && !s.description.empty()) {
                std::string mac = s.bt_address;
                for (char &c : mac) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                world.bt_names[mac] = s.description;
            }
        world.facts = status.facts;
        rows_dirty = true;
    }

    /* the output in use as the group shows it */
    std::string output_label() const {
        if (!snap.output_label.empty()) return snap.output_label;
        if (target.kind == OutputTarget::Kind::Auto) {
            for (const PulseDevice &s : world.sinks)
                if (s.name == world.default_sink) return sink_label(s); /* the device's name, not the profile words */
            return world.server ? "" : "Default output";
        }
        for (const OutputRow &r : out_rows)
            if (r.target == target) return r.label;
        return "";
    }

    /* ------------------------------------------------------------------------------------------ volume (D11) */

    struct VolTarget {
        enum class Kind { None, Sink, Mixer } kind = Kind::None;
        std::string sink;      /* Sink: the sink's name */
        unsigned channels = 2; /* Sink: its channels */
        std::string card;      /* Mixer: "hw:CARD=<id>" or "default" */
        std::string key;       /* output_volume_key: a card without a mixer keeps its software level under it */
    };

    /* the sink of the server that a target plays to, "" when it is not there (a Bluetooth device switched off) */
    std::string sink_for_target() const {
        if (target.kind == OutputTarget::Kind::Auto) return world.default_sink;
        if (target.kind == OutputTarget::Kind::Pulse) return target.id;
        if (target.kind == OutputTarget::Kind::Bt)
            for (const PulseDevice &d : world.sinks) {
                std::string mac = pulse_mac_from_name(d.name);
                for (char &c : mac) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (!mac.empty() && mac == target.id && d.name.compare(0, 6, "bluez_") == 0) return d.name;
            }
        return "";
    }

    /* the output the sound goes to (or will go to: nothing is open while paused) */
    VolTarget volume_target() const {
        VolTarget t;
        std::string sink;
        if (!snap.output_name.empty() && snap.output_pulse) sink = snap.output_name;
        else if (snap.output_name.empty() && world.server && target.kind != OutputTarget::Kind::Alsa) sink = sink_for_target();
        if (!sink.empty()) {
            t.kind = VolTarget::Kind::Sink;
            t.sink = sink;
            for (const PulseDevice &d : world.sinks)
                if (d.name == sink && d.channels > 0) t.channels = d.channels;
            t.key = output_volume_key(sink, true, target);
            return t;
        }
        if (!snap.output_name.empty() && !snap.output_pulse) {
            t.kind = VolTarget::Kind::Mixer;
            t.card = mixer_card_for(snap.output_name);
        } else if (target.kind == OutputTarget::Kind::Alsa || (!world.server && target.kind == OutputTarget::Kind::Auto)) {
            t.kind = VolTarget::Kind::Mixer;
            t.card = mixer_card_for(target.kind == OutputTarget::Kind::Alsa ? target.id : std::string("default"));
        }
        t.key = output_volume_key(snap.output_name, snap.output_pulse, target);
        return t;
    }

    /* an ALSA output without a playback mixer (or no mixer could be read): the software gain */
    bool software_volume(const VolTarget &t) const { return t.kind == VolTarget::Kind::Mixer && !mixer_level.has_mixer; }

    /* reads the playing card's mixer (at most every 300 ms, at once when forced) */
    void read_output_mixer(uint64_t now, bool force) {
        const VolTarget t = volume_target();
        if (t.kind != VolTarget::Kind::Mixer || !alsa) {
            mixer_level = MixerLevel();
            return;
        }
        if (!force && t.card == mixer_card && now - last_mixer_read < 300) return;
        last_mixer_read = now;
        if (t.card != mixer_card) mixer_choice_card.clear();
        const bool changed = t.card != mixer_card || !mixer_choice.ok();
        mixer_card = t.card;
        read_mixer(*alsa, t.card, mixer_choice_card, mixer_choice, mixer_level);
        if (changed && mixer_choice.ok())
            std::fprintf(stderr, "[vhs] volume: %s '%s' raw %ld..%ld%s: %s\n", t.card.c_str(), mixer_choice.volume.c_str(),
                         mixer_choice.min, mixer_choice.max, mixer_choice.min_is_mute ? " (min is mute)" : "",
                         volume_model_text(mixer_choice.model).c_str());
    }

    /* A sink's levels (sink_volume_model): the range of the ALSA card behind it, moved under the sink's 0 dB; -60..0 dB for
     * a sink without one (Bluetooth, software sinks). Found once per sink and card. */
    const OutputVolumeModel &sink_model(const PulseDevice &d) const {
        if (d.name == model_sink && d.alsa_card == model_card) return model_of_sink;
        model_sink = d.name;
        model_card = d.alsa_card;
        MixerChoice mc;
        bool have = false;
        std::string card;
        if (d.alsa_card >= 0 && alsa) {
            card = "hw:" + std::to_string(d.alsa_card);
            std::vector<CtlInfo> ctls;
            if (alsa->mixer_list(card, ctls) >= 0) have = choose_playback_mixer(ctls, mc) && mc.has_db;
        }
        model_of_sink = sink_volume_model(have ? &mc : nullptr);
        std::string from = have ? card + " '" + mc.volume + "' " + volume_model_text(mc.model) : std::string("no card range known");
        std::fprintf(stderr, "[vhs] volume: sink %s: %s (%s; base volume 0x%x, %u volume steps, flags 0x%x)\n", d.name.c_str(),
                     volume_model_text(model_of_sink).c_str(), from.c_str(), d.base_volume, d.n_volume_steps, d.sink_flags);
        return model_of_sink;
    }

    static OutputVolumeModel software_model() { return default_volume_model(); }

    /* The playing output's model and level (-1: not known; 0: muted by the control's own mute value or a silent sink). */
    int output_step(bool *muted = nullptr, OutputVolumeModel *model = nullptr) const {
        if (muted) *muted = false;
        const VolTarget t = volume_target();
        if (t.kind == VolTarget::Kind::Sink) {
            const PulseDevice *d = sink_device_of(world, t.sink);
            if (!d || d->volume_raw < 0) return -1;
            const OutputVolumeModel &m = sink_model(*d);
            if (model) *model = m;
            const int lv = sink_level_for_volume(m, static_cast<uint32_t>(d->volume_raw));
            if (pending_level >= 0 && pending_sink == t.sink && mono_ms_now() < pending_until && lv != pending_level)
                return pending_level; /* asked for, the server's event not back yet */
            if (muted) *muted = d->muted || lv == 0;
            return lv;
        }
        if (t.kind == VolTarget::Kind::Mixer) {
            if (software_volume(t)) {
                if (model) *model = software_model();
                return software_level(vol, t.key, muted); /* VHS's own mute: the level kept */
            }
            if (model) *model = mixer_choice.model;
            const MixerLevel shown = mixer_level_shown(mixer_choice, mixer_level, vol, t.key);
            if (muted) *muted = shown.muted;
            return shown.level;
        }
        return -1;
    }

    /* the percent the group shows: the level's place among the output's levels; -1 = not known */
    int output_level(bool *muted = nullptr) const {
        OutputVolumeModel m;
        const int lv = output_step(muted, &m);
        return lv < 0 ? -1 : m.pct_of(lv);
    }

    bool output_level_adjustable() const {
        const VolTarget t = volume_target();
        if (t.kind == VolTarget::Kind::Sink) return output_level() >= 0;
        return t.kind == VolTarget::Kind::Mixer;
    }

    /* the engine's gain: the software level only for a card without a mixer (linear in dB over -60..0); silent while VHS's
     * own mute holds (a card without a mixer, a control with neither a switch nor a mute value); else unity (the output's
     * own volume does the work, the stream stays at 100 %) */
    void apply_gain() {
        if (!engine) return;
        const VolTarget t = volume_target();
        float gain = 1.0f;
        if (software_volume(t)) {
            const OutputVolumeModel m = software_model();
            bool muted = false;
            const int lv = software_level(vol, t.key, &muted);
            gain = muted || lv <= 0 ? 0.0f : volume_gain_db(m.db_of(lv));
        } else if (t.kind == VolTarget::Kind::Mixer && mixer_level.has_mixer && mute_by(mixer_choice) == MuteBy::Vinyl &&
                   mixer_level_shown(mixer_choice, mixer_level, vol, t.key).muted)
            gain = 0.0f;
        engine->set_gain(gain);
    }

    void save_vol() {
        if (vol_file.empty()) return;
        const int r = save_volume_state(vol_file, vol);
        if (r < 0) std::fprintf(stderr, "[vhs] cannot save %s: %s\n", vol_file.c_str(), std::strerror(-r));
    }

    /* "-" / "+" (sink_press, mixer_press), one level of the output a press: while muted "-" does nothing and "+" brings the
     * sound back at its level; "-" at level 1 mutes */
    void volume_step(int d) {
        const VolTarget t = volume_target();
        bool muted = false;
        OutputVolumeModel m;
        const int cur = output_step(&muted, &m);
        if (t.kind == VolTarget::Kind::Sink) {
            const VolumePress p = sink_press(m, cur, muted, d);
            if (p.kind == VolumePress::Kind::None) return;
            const int level = p.kind == VolumePress::Kind::Mute ? cur : p.level;
            const uint32_t v = sink_volume_for_db(m.db_of(level));
            const int mute = p.kind == VolumePress::Kind::Mute ? 1 : 0;
            if (worker) {
                /* "set sink <name> step i/N P % target X dB"; the worker adds " raw=<set> read back raw=<what the sink
                 * holds>" from a fresh read */
                std::string log = mute ? "set sink " + t.sink + " muted (level " + std::to_string(level) + " kept)"
                                       : volume_set_log_text("sink " + t.sink, m, level, v, -1, 0);
                if (const size_t cut_at = log.find(" raw="); cut_at != std::string::npos) log.resize(cut_at);
                worker->request_sink_volume(t.sink, t.channels, v, mute, log);
                if (!mute) {
                    pending_level = level;
                    pending_sink = t.sink;
                    pending_until = mono_ms_now() + 1500;
                }
            } else { /* a world without a sound-server worker (the tests' fake world): the sink changes in memory */
                for (PulseDevice &s2 : world.sinks)
                    if (s2.name == t.sink) {
                        s2.volume_raw = v;
                        s2.volume_pct = static_cast<int>((static_cast<uint64_t>(v) * 100 + 0x8000) / 0x10000);
                        s2.muted = mute != 0;
                    }
                std::fprintf(stderr, "[vhs] volume: %s\n", volume_set_log_text("sink " + t.sink, m, mute ? 0 : level, v, 0, v).c_str());
            }
        } else if (t.kind == VolTarget::Kind::Mixer) {
            if (software_volume(t)) {
                const OutputVolumeModel sm = software_model();
                const VolumePress p = software_press(vol, t.key, d); /* VHS's own mute keeps the level */
                if (p.kind == VolumePress::Kind::None) return;
                save_vol();
                apply_gain();
                bool now_muted = false;
                const int level = software_level(vol, t.key, &now_muted);
                char db[48] = "";
                if (now_muted) std::snprintf(db, sizeof db, "muted, level %d kept", level);
                else std::snprintf(db, sizeof db, "%.2f dB", sm.db_of(level));
                std::fprintf(stderr, "[vhs] volume: software step %d/%d %d %% (%s) for %s (no playback mixer)\n", now_muted ? 0 : level,
                             sm.n, now_muted ? 0 : sm.pct_of(level), db, t.key.c_str());
            } else if (alsa) {
                const MixerLevel shown = mixer_level_shown(mixer_choice, mixer_level, vol, t.key);
                const VolumePress p = mixer_press(mixer_choice, shown, d);
                const MuteBy by = mute_by(mixer_choice);
                const bool on_off = p.kind == VolumePress::Kind::Mute || p.kind == VolumePress::Kind::Unmute;
                int r = 0;
                if (p.kind == VolumePress::Kind::Set) r = write_mixer_level(*alsa, t.card, mixer_choice, p.level);
                else if (on_off && by == MuteBy::Switch)
                    r = write_mixer_switch(*alsa, t.card, mixer_choice, p.kind == VolumePress::Kind::Unmute);
                else if (!on_off) return;
                if (r >= 0) {
                    note_mixer_press(vol, t.key, mixer_choice, p);
                    if (by == MuteBy::Vinyl && on_off) save_vol();
                }
                if (on_off && by == MuteBy::Vinyl)
                    std::fprintf(stderr, "[vhs] volume: %s '%s' %s by VHS (no switch, no mute value; level %d kept)\n",
                                 t.card.c_str(), mixer_choice.volume.c_str(), p.kind == VolumePress::Kind::Mute ? "muted" : "un-muted",
                                 shown.level);
                if (r < 0) std::fprintf(stderr, "[vhs] volume: %s %s: %s\n", t.card.c_str(), mixer_choice.volume.c_str(), alsa_error_text(r).c_str());
                if (p.kind == VolumePress::Kind::Set && r >= 0) { /* what was written and what the control holds now */
                    long back = 0;
                    const int rr = alsa->mixer_get(t.card, mixer_choice.volume, back); /* a fresh read of the control */
                    std::fprintf(stderr, "[vhs] volume: %s\n",
                                 volume_set_log_text(t.card + " '" + mixer_choice.volume + "'", mixer_choice.model, p.level,
                                                     mixer_raw_for_level(mixer_choice, p.level), rr, back)
                                     .c_str());
                } else if (r >= 0 && by == MuteBy::Switch)
                    std::fprintf(stderr, "[vhs] volume: set %s '%s' %s\n", t.card.c_str(), mixer_choice.sw.c_str(),
                                 p.kind == VolumePress::Kind::Mute ? "off (muted)" : "on (un-muted)");
                read_output_mixer(mono_ms_now(), true);
                apply_gain();
            }
        }
    }

    /* one log line when the level shown or the control changes (for the board probes: which volume is in force) */
    void log_volume(uint64_t now) {
        if (now - last_log < 500) return;
        last_log = now;
        const VolTarget t = volume_target();
        bool m = false;
        const int v = output_level(&m);
        std::string line;
        OutputVolumeModel model;
        const int step = output_step(nullptr, &model);
        if (t.kind == VolTarget::Kind::Sink) {
            line = "sink " + t.sink + " " + volume_log_text(v);
            if (step >= 0) {
                char b[64];
                std::snprintf(b, sizeof b, ", step %d/%d%s", step, model.n, step > 0 ? "" : " (silent)");
                line += b;
                if (step > 0) {
                    std::snprintf(b, sizeof b, ", %.2f dB", model.db_of(step));
                    line += b;
                }
            }
        } else if (t.kind == VolTarget::Kind::Mixer && software_volume(t))
            line = "software " + volume_log_text(v) + " for " + t.key + " (" + t.card + ": no playback mixer)";
        else if (t.kind == VolTarget::Kind::Mixer) {
            char db[64] = "";
            if (mixer_choice.has_db) std::snprintf(db, sizeof db, ", step %d/%d, %.2f dB", step, model.n, mixer_level.cdb / 100.0);
            else std::snprintf(db, sizeof db, ", step %d/%d", step, model.n);
            line = "mixer " + t.card + " '" + mixer_choice.volume + "' " + volume_log_text(v) + (v >= 0 ? db : "") +
                   (v >= 0 ? ", raw=" + std::to_string(mixer_level.raw) : std::string());
        } else line = "no output";
        if (m && v >= 0) line += " (muted)";
        if (line == vol_logged) return;
        vol_logged = line;
        std::fprintf(stderr, "[vhs] volume: %s\n", line.c_str());
    }
};

AudioPlayer::AudioPlayer(std::string output_target) : AudioPlayer(std::move(output_target), real_audio_deps()) {}

AudioPlayer::AudioPlayer(std::string output_target, std::unique_ptr<AudioPlayerDeps> deps)
    : d_(std::make_unique<Impl>(output_target, std::move(deps))) {}

AudioPlayer::~AudioPlayer() { shutdown(); }

void AudioPlayer::shutdown() {
    if (!d_) return;
    std::lock_guard<std::mutex> g(d_->mu);
    d_->shutdown();
}

bool AudioPlayer::open(const std::string &path, std::int64_t start_ms, bool play) {
    std::chrono::steady_clock::time_point deadline;
    {
        std::lock_guard<std::mutex> g(d_->mu);
        if (d_->down) return false;
        if (!d_->begin_open(path, start_ms, play)) return false;
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(d_->open_timeout_ms);
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> g(d_->mu);
            d_->drain_events();
            if (d_->open_state != Impl::OpenState::Opening) return d_->open_state == Impl::OpenState::Open;
            if (d_->down) return false;
            if (std::chrono::steady_clock::now() >= deadline) {
                d_->open_state = Impl::OpenState::Failed;
                d_->error = "The file took too long to open.";
                d_->engine->unload();
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void AudioPlayer::play() {
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->down || !d_->is_open || !d_->has_audio) return;
    d_->error.clear();
    d_->output_lost = false;
    if (d_->ended) {
        d_->ended = false;
        ++d_->epoch;
    }
    d_->engine->play();
}

void AudioPlayer::pause() {
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->down || !d_->is_open || !d_->has_audio) return;
    d_->engine->pause();
}

void AudioPlayer::seek_ms(std::int64_t ms) {
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->down || !d_->is_open || !d_->has_audio) return;
    d_->seek_target_ms = std::max<std::int64_t>(0, ms);
    ++d_->seeks_issued;
    d_->ended = false;
    ++d_->epoch;
    d_->engine->seek_ms(static_cast<uint64_t>(d_->seek_target_ms));
}

void AudioPlayer::close() {
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->down) return;
    d_->engine->unload();
    d_->is_open = d_->has_audio = d_->ended = d_->output_lost = false;
    d_->open_state = Impl::OpenState::None;
    d_->path.clear();
    d_->error.clear();
}

AudioClock AudioPlayer::clock() const {
    std::lock_guard<std::mutex> g(d_->mu);
    return d_->clock();
}

int AudioPlayer::volume_percent() const {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->service();
    bool muted = false;
    const int v = d_->output_level(&muted);
    return v < 0 ? -1 : (muted ? 0 : v);
}

void AudioPlayer::volume_step(int dir) {
    if (dir == 0) return;
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->down) return;
    d_->service();
    d_->volume_step(dir < 0 ? -1 : 1);
}

std::string AudioPlayer::volume_label() const {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->service();
    return d_->output_label();
}

bool AudioPlayer::volume_adjustable() const {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->service();
    return d_->output_level_adjustable();
}

bool AudioPlayer::volume_muted() const {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->service();
    bool muted = false;
    d_->output_level(&muted);
    return muted;
}

AudioPlayer::Key AudioPlayer::poll_key() {
    std::lock_guard<std::mutex> g(d_->mu);
    return d_->poll_key();
}

std::vector<AudioOutputRow> AudioPlayer::output_rows() const {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->service();
    if (!d_->world.server && d_->alsa) { /* the chooser lists the cards as they are now */
        d_->world.alsa_cards = d_->alsa->cards();
        d_->out_rows = build_output_rows(d_->world, d_->target);
        d_->rows_dirty = false;
    }
    std::vector<AudioOutputRow> rows;
    for (const OutputRow &r : d_->out_rows) {
        AudioOutputRow o;
        o.key = r.target.key();
        o.label = r.label;
        o.value = r.value;
        o.chosen = r.chosen;
        o.dimmed = r.dimmed;
        o.bluetooth = r.bluetooth;
        rows.push_back(o);
    }
    return rows;
}

std::string AudioPlayer::output_key() const {
    std::lock_guard<std::mutex> g(d_->mu);
    return d_->target.key();
}

void AudioPlayer::set_output(const std::string &key) {
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->down) return;
    const OutputTarget t = OutputTarget::from_key(key);
    if (t == d_->target) return;
    d_->target = t;
    d_->rows_dirty = true;
    d_->engine->set_output(t);
}

bool AudioPlayer::bluetooth_not_set_up() const {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->service();
    return vhs::bluetooth_not_set_up(d_->world);
}

}  // namespace vhs
