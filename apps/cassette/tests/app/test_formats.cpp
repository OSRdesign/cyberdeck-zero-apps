/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: recording FLAC and Ogg Opus takes end to end (phase B) on the fake sound system, with the test
 * encoders (tests/fixture_codecs.hpp) in place of libFLAC / libopusenc. The stored setting chooses ("format" and
 * "format_preset" in settings.json; the Format row is not part of this), effective_choice() decides per card and per
 * library; the take name, the list row, the journal, the recorder's format text and the hours left follow; take numbers
 * run on across formats; repair at launch; a full card. Phase C: with the fake decoders (tests/fake_decoders.hpp) a FLAC
 * or Opus take loads, plays to its end (every sample checked), previews, opens on the trim page with its peaks, and its
 * trim and split are saved in its own format; without its decoder library LOAD, PLAY, the preview and TRIM refuse with
 * one notice and nothing opens the file.
 */
#include "helpers.hpp"

#include "../fake_decoders.hpp"
#include "../fixture_codecs.hpp"
#include "codec/opus_decoder.hpp"
#include "codec/formats.hpp"
#include "codec/stream_scan.hpp"
#include "recordings.hpp"

#include <algorithm>

using namespace ta;

namespace {

void write_settings(const std::string &home, const std::string &json) {
    posix_file_ops().mkdir(home + "/.config");
    posix_file_ops().mkdir(home + "/.config/cassette");
    FILE *f = std::fopen((home + "/.config/cassette/settings.json").c_str(), "wb");
    if (!f) return;
    std::fputs(json.c_str(), f);
    std::fclose(f);
}

/* The fake decoders (one table each for the whole run). */
DecoderLibs fake_decoders() {
    static const FlacDecLib flac = fakedec::make_flac_dec_lib();
    static const OpusDecLib opus = fakedec::make_opus_dec_lib();
    DecoderLibs d;
    d.flac = &flac;
    d.opus = &opus;
    return d;
}

/* The libraries "found" and the test encoders in place of libFLAC / libopusenc; the fake decoders unless `decoders` is
 * false (then no decoder library is "installed"). */
void use_test_codecs(Rig &r, bool flac = true, bool opus = true, bool decoders = true) {
    r.cfg.codecs = [flac, opus] {
        CodecAvailability a;
        a.flac = flac;
        a.opus = opus;
        return a;
    };
    r.cfg.make_encoder = [](const FormatChoice &c, const AudioFormat &f) -> std::unique_ptr<Encoder> {
        if (c.key.format == RecFormat::Flac) return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096));
        return std::unique_ptr<Encoder>(new fx::PlainOggOpus(f, static_cast<unsigned>(c.opus_bitrate_bps / 8 / 50), 50));
    };
    r.cfg.decoders = [decoders] { return decoders ? fake_decoders() : DecoderLibs(); };
}

std::vector<unsigned char> read_file_bytes(const std::string &p) {
    const std::string s = slurp(p);
    return std::vector<unsigned char>(s.begin(), s.end());
}

bool ends_with(const std::string &s, const std::string &t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

bool notice_has(const Snapshot &s, const std::string &text) {
    for (const Notice &n : s.notices)
        if (n.text.find(text) != std::string::npos) return true;
    return false;
}

/* REC, about `seconds` of audio, STOP; true when the deck came back to idle. */
bool record(Rig &r, double seconds) {
    if (!r.wait_meter()) return false;
    r.ctl().key_event(KeyEvent::Rec);
    if (!r.wait_state(kDeckRecording)) return false;
    if (!r.wait([seconds](const Snapshot &x) { return x.time.elapsed_s >= seconds; }, 8000)) return false;
    r.ctl().key_event(KeyEvent::Stop);
    return r.wait_state(kDeckIdle);
}

JournalEntry journal_of(Rig &r, const std::string &name) {
    JournalEntry je;
    CHECK_EQ(journal_read(posix_file_ops(), journal_file(r.rec(), file_stem(name)), je), 0);
    return je;
}

bool wait_peaks(Rig &r) {
    return r.wait([](const Snapshot &x) { return x.trim.open && x.trim.peaks_state == 2; }, 8000);
}
bool wait_save(Rig &r, int state) {
    return r.wait([state](const Snapshot &x) { return x.trim.save_state == state; }, 8000);
}

/* The samples a fake-decoded take delivers (interleaved, output scale). */
std::vector<int32_t> decoded(const std::string &path, DecodedInfo *info_out = nullptr) {
    std::vector<int32_t> out;
    DecodedInfo info;
    int err = 0;
    std::unique_ptr<Decoder> d = make_decoder(posix_file_ops(), path, fake_decoders(), info, &err);
    if (!d) return out;
    const unsigned bytes = info.output.bits / 8u, ch = info.output.channels;
    std::vector<unsigned char> b(4096 * bytes * ch);
    for (;;) {
        const long n = d->read(b.data(), 4096);
        if (n <= 0) break;
        for (long i = 0; i < n * static_cast<long>(ch); ++i) out.push_back(tp::decode(&b[static_cast<size_t>(i) * bytes], bytes));
    }
    if (info_out) *info_out = info;
    return out;
}

/* PLAY on the loaded take: the deck plays it to its end (the fake sink takes it at once) and the sink heard all of it. */
bool play_through(Rig &r, const std::string &path, uint64_t frames) {
    r.sink.clear_heard();
    const unsigned opens = r.sink.opens();
    r.ctl().key_event(KeyEvent::PlayPause);
    if (!r.wait([](const Snapshot &x) { return x.deck.state == kDeckIdle; }, 5000)) return false;
    if (r.sink.opens() <= opens) return false;
    const std::vector<int32_t> want = decoded(path);
    const std::vector<int32_t> got = r.sink.heard();
    if (got.size() != want.size() || want.size() / std::max<size_t>(1, r.sink.format().channels) != frames) {
        std::fprintf(stderr, "  played %zu samples, the take has %zu (%llu frames)\n", got.size(), want.size(),
                     static_cast<unsigned long long>(frames));
        return false;
    }
    return got == want;
}

/* The trim page on a compressed take: peaks, then a split saved as new files in the take's format. */
void trim_and_split(Rig &r, const TakeRow &row, const char *ext) {
    Controller &c = r.ctl();
    CHECK(c.open_trim(row.name));
    CHECK(wait_peaks(r));
    Snapshot s = r.snap();
    CHECK_EQ(s.trim.frames, row.frames);
    const std::vector<PeakBar> bars = c.peak_window(100);
    CHECK_EQ(bars.size(), 100u);
    int loud = 0;
    for (const PeakBar &b : bars) loud += (b.max - b.min) > 1000 ? 1 : 0;
    CHECK(loud > 50); /* the take's audio, not silence */
    CHECK(c.set_zoom(s.trim.max_zoom));
    CHECK_EQ(c.peak_window(80).size(), 80u); /* the exact bars at the closest zoom decode the take */
    const uint64_t F = row.frames;
    CHECK(c.set_position(F / 10));
    CHECK(c.set_in());
    CHECK(c.set_position(F - F / 10));
    CHECK(c.set_out());
    CHECK(c.set_position(F / 2));
    CHECK(c.add_split());
    CHECK(c.preview(true));
    CHECK(r.wait([](const Snapshot &x) { return !x.trim.previewing || x.trim.position > 0; }, 3000));
    CHECK(c.preview(false));
    CHECK(c.save(SaveKind::NewFiles));
    CHECK(wait_save(r, 2));
    s = r.snap();
    CHECK_EQ(s.trim.save_outputs.size(), 2u);
    const std::vector<int32_t> src = decoded(row.path);
    const unsigned ch = static_cast<unsigned>(row.channels);
    const uint64_t cuts[] = {F / 10, F / 2, F - F / 10};
    for (size_t i = 0; i < s.trim.save_outputs.size() && i < 2; ++i) {
        const std::string &n = s.trim.save_outputs[i];
        CHECK(ends_with(n, std::string("-") + std::to_string(i + 1) + ext));
        DecodedInfo info;
        const std::vector<int32_t> part = decoded(r.rec() + "/" + n, &info);
        CHECK_EQ(info.total_frames, cuts[i + 1] - cuts[i]);
        CHECK(part == std::vector<int32_t>(src.begin() + static_cast<long>(cuts[i] * ch), src.begin() + static_cast<long>(cuts[i + 1] * ch)));
    }
    c.close_trim();
    const std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 3u); /* the original and the two parts, all listed */
}

void t_flac_take() {
    Rig r(one_card()); /* PCM2902: mono, 48 kHz */
    use_test_codecs(r);
    write_settings(r.home, "{\"version\": 1, \"format\": \"flac\"}");
    r.cfg.writer_sync_ms = 500;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK_EQ(s.take.format_text, "48k/16 FLAC");
    CHECK(s.settings.format == RecFormat::Flac);
    /* the hours left come from the FLAC estimate (about 0.3 of PCM for 16-bit), not from the WAV rate */
    const double wav_hours = hours_left(s.take.free_bytes, mono48());
    CHECK(s.take.hours_left > 3.0 * wav_hours && s.take.hours_left < 3.5 * wav_hours);
    CHECK(record(r, 2.0));
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    const TakeRow row = rows[0];
    CHECK(row.name.compare(0, 9, "take_001_") == 0);
    CHECK(ends_with(row.name, ".flac"));
    CHECK_EQ(row.format_tag, "FLAC");
    CHECK_EQ(row.format_text, "48k/16 FLAC");
    CHECK(!row.damaged && !row.repaired);
    CHECK(row.frames >= 2 * 48000u && row.frames < 3 * 48000u);
    CHECK_EQ(row.rate, 48000u);
    CHECK(row.current); /* the new take is loaded, as for WAV */
    CHECK_EQ(r.snap().take.name, row.name);
    const StreamScan sc = scan_flac(posix_file_ops(), row.path);
    CHECK(sc.ok && sc.valid_end == sc.file_size);
    CHECK_EQ(sc.stored_total, row.frames);
    const std::vector<int32_t> pcm = fx::decode_verbatim_flac(read_file_bytes(row.path));
    CHECK_EQ(pcm.size(), row.frames);
    int32_t peak = 0;
    for (int32_t v : pcm) peak = std::max(peak, std::abs(v));
    CHECK(peak > 15000 && peak < 17500); /* the 440 Hz tone at half scale went through the encoder */
    const std::string tags = slurp(row.path).substr(0, 600);
    CHECK(tags.find("TITLE=" + file_stem(row.name)) != std::string::npos);
    CHECK(tags.find("ENCODER=Cassette") != std::string::npos);
    CHECK(tags.find("DATE=") != std::string::npos); /* the clock counts as set here */
    const JournalEntry je = journal_of(r, row.name);
    CHECK_EQ(je.state, "closed");
    CHECK_EQ(je.codec, "flac");
    CHECK_EQ(je.codec_preset, "");
    CHECK_EQ(je.fmt.rate, 48000u);
    CHECK_EQ(je.frames, row.frames);

    /* phase C: LOAD, PLAY to the end, the preview, PLAY from the list, the trim page and a split */
    c.ack_notices();
    CHECK(c.load(row.name));
    CHECK(play_through(r, row.path, row.frames));
    CHECK(r.snap().notices.empty());
    CHECK(c.preview_take(row.name));
    CHECK(r.wait_state(kDeckIdle, 5000));
    CHECK(c.play_take(row.name));
    CHECK(r.wait_state(kDeckIdle, 5000));
    trim_and_split(r, row, ".flac");
    /* rename and delete work on FLAC takes */
    std::string nn;
    CHECK_EQ(c.rename_take(row.name, "field rec", &nn), 0);
    CHECK(nn.compare(0, 14, "field rec_001_") == 0 && ends_with(nn, ".flac"));
    rows = c.takes();
    CHECK(rows.size() == 3 && rows[0].format_tag == "FLAC");
    CHECK_EQ(r.snap().take.name, nn); /* still loaded under its new name */
    CHECK_EQ(c.delete_take(nn), 0);
    CHECK_EQ(c.takes().size(), 2u);
}

/* A stereo card at 48 kHz with the Opus speech preset: one coded channel, the file still says stereo. */
void t_opus_take() {
    FakeCard card = fake_stereo22(0);
    card.offer.rates = {48000};
    card.offer.min_channels = 2;
    Rig r({card});
    use_test_codecs(r);
    write_settings(r.home, "{\"version\": 1, \"format\": \"opus\", \"format_preset\": \"speech24\"}");
    r.cfg.writer_sync_ms = 500;
    r.open();
    CHECK(r.wait_meter());
    CHECK_EQ(r.snap().take.format_text, "48k/16 stereo OPUS");
    CHECK(record(r, 1.5));
    const std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    CHECK(ends_with(rows[0].name, ".opus"));
    CHECK_EQ(rows[0].format_tag, "OPUS");
    CHECK_EQ(rows[0].format_text, "48k/16 stereo OPUS");
    CHECK_EQ(rows[0].channels, 2);
    CHECK(rows[0].frames >= 72000u && rows[0].frames < 3 * 48000u);
    const StreamScan sc = scan_ogg_opus(posix_file_ops(), rows[0].path);
    CHECK(sc.ok && sc.eos && sc.valid_end == sc.file_size);
    CHECK_EQ(sc.frames, rows[0].frames);
    CHECK_EQ(sc.fmt.channels, uint16_t(2));
    const JournalEntry je = journal_of(r, rows[0].name);
    CHECK_EQ(je.codec, "opus");
    CHECK_EQ(je.codec_preset, "speech24");
    CHECK_EQ(je.bitrate_bps, 24000);
    CHECK(je.force_mono);
    CHECK_EQ(je.state, "closed");
    /* phase C: it plays (at 48 kHz, stereo, every sample of the decoder), previews, trims and splits as Opus */
    r.ctl().ack_notices();
    CHECK(r.ctl().load(rows[0].name));
    CHECK(play_through(r, rows[0].path, rows[0].frames));
    CHECK_EQ(r.sink.format().rate, 48000u);
    CHECK(r.ctl().preview_take(rows[0].name));
    CHECK(r.wait_state(kDeckIdle, 5000));
    trim_and_split(r, rows[0], ".opus");
}

/* No decoder library: LOAD, PLAY from the list, the preview and TRIM refuse with one notice; nothing opens the file;
 * WAV takes still play. */
void t_missing_decoder() {
    Rig r(one_card());
    use_test_codecs(r, true, true, false);
    write_settings(r.home, "{\"version\": 1, \"format\": \"flac\"}");
    r.open();
    Controller &c = r.ctl();
    CHECK(record(r, 1.0));
    const std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (rows.empty()) return;
    const std::string name = rows[0].name;
    c.ack_notices();
    CHECK(!c.load(name));
    CHECK(notice_has(r.snap(), "This take cannot be played: FLAC not installed (libflac14)"));
    c.ack_notices();
    CHECK(!c.preview_take(name));
    CHECK(notice_has(r.snap(), "FLAC not installed (libflac14)"));
    c.ack_notices();
    CHECK(!c.play_take(name));
    CHECK(notice_has(r.snap(), "FLAC not installed (libflac14)"));
    c.ack_notices();
    CHECK(!c.open_trim(name));
    CHECK(!r.snap().trim.open);
    CHECK(notice_has(r.snap(), "This take cannot be trimmed: FLAC not installed (libflac14)"));
    c.ack_notices();
    c.key_event(KeyEvent::PlayPause); /* the loaded take is this FLAC take (the new recording) */
    CHECK(r.wait_state(kDeckIdle, 2000));
    CHECK(notice_has(r.snap(), "FLAC not installed (libflac14)"));
    CHECK_EQ(r.sink.opens(), 0u); /* the player never opened the output */
    make_take(r.rec(), "take_009_20261108-1105.wav", 1.0);
    c.refresh_takes();
    CHECK(c.load("take_009_20261108-1105.wav"));
    c.key_event(KeyEvent::PlayPause);
    CHECK(r.wait([&](const Snapshot &) { return r.sink.opens() > 0; }, 3000));
}

/* The stored choice cannot record here: WAV, as effective_choice() says. */
void t_fallback_to_wav() {
    {
        Rig r({fake_stereo22(0)}); /* 32 kHz stereo: not an Opus rate */
        use_test_codecs(r);
        write_settings(r.home, "{\"version\": 1, \"format\": \"opus\", \"format_preset\": \"music64\"}");
        r.open();
        CHECK(r.wait_meter());
        CHECK_EQ(r.snap().take.format_text, "32k/16 stereo WAV");
        CHECK(record(r, 0.5));
        CHECK_EQ(r.wavs().size(), 1u);
        CHECK(r.ctl().takes().size() == 1 && r.ctl().takes()[0].format_tag == "WAV");
    }
    {
        Rig r(one_card()); /* FLAC wanted, the library missing */
        use_test_codecs(r, false, false);
        write_settings(r.home, "{\"version\": 1, \"format\": \"flac\"}");
        r.open();
        CHECK(r.wait_meter());
        CHECK_EQ(r.snap().take.format_text, "48k/16 WAV");
        const double wav_hours = hours_left(r.snap().take.free_bytes, mono48());
        CHECK(std::fabs(r.snap().take.hours_left - wav_hours) < 0.01 * wav_hours + 0.01);
        CHECK(record(r, 0.5));
        CHECK_EQ(r.wavs().size(), 1u);
        const JournalEntry je = journal_of(r, r.wavs()[0]);
        CHECK(je.codec.empty()); /* a WAV journal has no codec record */
    }
}

/* WAV, then FLAC, then Opus in one folder (the settings file changed between runs): one numbering, one list. */
void t_mixed_folder() {
    Rig r(one_card());
    use_test_codecs(r);
    const std::string home = r.home;
    r.open();
    CHECK(record(r, 0.5)); /* take 001, WAV */
    r.close();
    write_settings(home, "{\"version\": 1, \"format\": \"flac\"}");
    r.open();
    CHECK(record(r, 0.5)); /* take 002, FLAC */
    r.close();
    write_settings(home, "{\"version\": 1, \"format\": \"opus\", \"format_preset\": \"music128\"}");
    r.open();
    CHECK(record(r, 0.5)); /* take 003, Opus */
    std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 3u);
    if (rows.size() == 3) {
        CHECK(rows[0].name.compare(0, 9, "take_003_") == 0 && rows[0].format_tag == "OPUS");
        CHECK(rows[1].name.compare(0, 9, "take_002_") == 0 && rows[1].format_tag == "FLAC");
        CHECK(rows[2].name.compare(0, 9, "take_001_") == 0 && rows[2].format_tag == "WAV");
        for (const TakeRow &t : rows) CHECK(!t.damaged && t.frames > 20000);
        CHECK(rows[0].current);
        /* the WAV take still loads and plays */
        CHECK(r.ctl().load(rows[2].name));
        r.ctl().key_event(KeyEvent::PlayPause);
        CHECK(r.wait_state(kDeckPlaying, 2000));
        r.ctl().key_event(KeyEvent::Stop);
        CHECK(r.wait_state(kDeckIdle));
        /* a deleted Opus take's number is never handed out again */
        CHECK_EQ(r.ctl().delete_take(rows[0].name), 0);
    }
    CHECK(record(r, 0.5));
    rows = r.ctl().takes();
    CHECK(!rows.empty() && rows[0].name.compare(0, 9, "take_004_") == 0 && rows[0].format_tag == "OPUS");
}

/* A FLAC take cut by a crash: repaired at launch from its journal, a notice, the "repaired" mark in the list. */
void t_repair_at_launch() {
    Rig r(one_card());
    use_test_codecs(r);
    posix_file_ops().mkdir(r.rec());
    MemoryByteSink s;
    fx::VerbatimFlac e(mono48(), 4096);
    e.start(s, {{"TITLE", "take_009_nodate"}});
    const std::vector<unsigned char> pcm = fx::pcm(mono48(), 0, 48000);
    e.write(pcm.data(), 48000); /* 11 whole frames written, the rest pending: never finished */
    std::vector<unsigned char> cut = s.data;
    cut.resize(cut.size() - 100); /* the last frame cut by the power loss */
    {
        FILE *f = std::fopen((r.rec() + "/take_009_nodate.flac").c_str(), "wb");
        std::fwrite(cut.data(), 1, cut.size(), f);
        std::fclose(f);
    }
    JournalEntry je;
    je.take = "take_009_nodate";
    je.path = r.rec() + "/take_009_nodate.flac";
    je.parts = {je.path};
    je.fmt = mono48();
    je.codec = "flac";
    je.state = "open";
    CHECK_EQ(journal_write(posix_file_ops(), r.rec(), je), 0);
    r.open();
    Snapshot sn = r.snap();
    CHECK(has_notice(sn, Notice::Repaired));
    CHECK(notice_has(sn, "Recovered take_009_nodate"));
    const std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        CHECK(rows[0].repaired);
        CHECK_EQ(rows[0].format_tag, "FLAC");
        CHECK_EQ(rows[0].frames, uint64_t(10 * 4096));
        CHECK(!rows[0].damaged);
    }
    CHECK_EQ(journal_of(r, "take_009_nodate.flac").state, "repaired");
    /* the next take continues the numbering */
    write_settings(r.home, "{\"version\": 1, \"format\": \"flac\"}");
    r.close();
    r.open();
    CHECK(!has_notice(r.snap(), Notice::Repaired)); /* once only */
    CHECK(record(r, 0.5));
    CHECK(r.ctl().takes().size() == 2 && r.ctl().takes()[0].name.compare(0, 9, "take_010_") == 0);
}

/* The card fills during a FLAC take: the take stops, is kept and readable, the notice says why. */
void t_flac_disk_full() {
    Rig r(one_card());
    use_test_codecs(r);
    write_settings(r.home, "{\"version\": 1, \"format\": \"flac\"}");
    r.ops.enospc_after.store(150000);
    r.open();
    CHECK(r.wait_meter());
    r.ctl().key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckIdle, 8000)); /* stopped by itself */
    const Snapshot s = r.snap();
    CHECK(s.problems.disk_full);
    CHECK(s.problems.message.find("disk full") != std::string::npos);
    CHECK(notice_has(s, "Recording stopped"));
    const std::vector<TakeRow> rows = r.ctl().takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        CHECK(!rows[0].damaged && ends_with(rows[0].name, ".flac"));
        CHECK(rows[0].frames > 20000 && rows[0].frames < 80000);
        const StreamScan sc = scan_flac(posix_file_ops(), rows[0].path);
        CHECK(sc.ok && sc.valid_end == sc.file_size && sc.stored_total == sc.frames); /* mended at once */
        CHECK_EQ(journal_of(r, rows[0].name).state, "error");
    }
}

/* The Format picker's command: format + preset stored in settings.json, the recorder follows at once; the snapshot carries
 * the capture format and the libraries the picker needs; refused while a take runs, and for a key the model does not have. */
void t_set_format_choice() {
    Rig r(one_card()); /* mono, 48 kHz */
    use_test_codecs(r);
    r.cfg.writer_sync_ms = 500;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK(s.format_inputs.state == CaptureState::Known);
    CHECK_EQ(s.format_inputs.capture.rate, 48000u);
    CHECK_EQ(s.format_inputs.capture.channels, uint16_t(1));
    CHECK(s.format_inputs.codecs.flac && s.format_inputs.codecs.opus);
    CHECK_EQ(s.take.format_text, "48k/16 WAV");
    CHECK(c.set_format_choice(RecFormat::Flac, ""));
    s = r.snap();
    CHECK(s.settings.format == RecFormat::Flac && s.settings.format_preset.empty());
    CHECK_EQ(s.take.format_text, "48k/16 FLAC");
    CHECK(c.set_format_choice(RecFormat::Opus, "music64"));
    s = r.snap();
    CHECK(s.settings.format == RecFormat::Opus && s.settings.format_preset == "music64");
    CHECK_EQ(s.take.format_text, "48k/16 OPUS");
    CHECK(!c.set_format_choice(RecFormat::Opus, "speech99")); /* not a preset of the model: unchanged */
    CHECK(!c.set_format_choice(RecFormat::Flac, "music64"));
    CHECK(!c.set_format_choice(RecFormat::Wav, "speech24"));
    CHECK(r.snap().settings.format_preset == "music64");
    /* stored: the settings file says it */
    const std::string stored = slurp(r.home + "/.config/cassette/settings.json");
    CHECK(stored.find("\"opus\"") != std::string::npos && stored.find("music64") != std::string::npos);
    /* the hours left follow the choice */
    CHECK(c.set_format_choice(RecFormat::Wav, ""));
    s = r.snap();
    CHECK_EQ(s.take.format_text, "48k/16 WAV");
    CHECK(s.settings.format_preset.empty());
    /* not while a take runs */
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(!c.set_format_choice(RecFormat::Flac, ""));
    CHECK(r.snap().settings.format == RecFormat::Wav);
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
}

/* what the controller asked the fake card for (under the fake's lock: the controller thread opens it) */
HwRequest last_request(Rig &r) {
    HwRequest q;
    r.alsa.with([&q](ToneAlsa &a) { q = a.requests.back(); });
    return q;
}
size_t request_count(Rig &r) {
    size_t n = 0;
    r.alsa.with([&n](ToneAlsa &a) { n = a.requests.size(); });
    return n;
}

/* The three parameters on a card with several rates and 24-bit capture: the stored rate and quality reopen the capture at once
 * (the hardware request follows), the take is written in them, Opus gets any bit rate of the list, a refused value or a take
 * running changes nothing, and the settings file keeps the new keys beside the old ones. */
void t_rec_params() {
    std::vector<FakeCard> cards = one_card(); /* mono */
    cards[0].offer.rates = {16000, 44100, 48000};
    cards[0].offer.formats = {SampleFormat::S16_LE, SampleFormat::S24_3LE};
    Rig r(cards);
    use_test_codecs(r);
    static std::atomic<int> seen_bps;
    seen_bps = 0;
    r.cfg.make_encoder = [](const FormatChoice &c, const AudioFormat &f) -> std::unique_ptr<Encoder> {
        seen_bps = c.opus_bitrate_bps;
        if (c.key.format == RecFormat::Flac) return std::unique_ptr<Encoder>(new fx::VerbatimFlac(f, 4096));
        return std::unique_ptr<Encoder>(new fx::PlainOggOpus(f, static_cast<unsigned>(c.opus_bitrate_bps / 8 / 50), 50));
    };
    r.cfg.writer_sync_ms = 500;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    Snapshot s = r.snap();
    CHECK(s.format_inputs.rates == (std::vector<uint32_t>{16000, 44100, 48000}));
    CHECK(s.format_inputs.has16 && s.format_inputs.has24);
    CHECK_EQ(s.take.format_text, "48k/16 WAV"); /* no rate and no quality stored: the card's default, as before */
    CHECK(last_request(r).rate == 48000 && last_request(r).format == SampleFormat::S16_LE);

    /* a rate: the capture reopens at it */
    CHECK(c.set_rec_params(RecFormat::Wav, "", 44100, 0));
    CHECK(r.wait_meter());
    s = r.snap();
    CHECK_EQ(s.take.format_text, "44.1k/16 WAV");
    CHECK(s.settings.rate == 44100 && s.settings.quality == 0 && s.settings.format == RecFormat::Wav);
    CHECK_EQ(last_request(r).rate, 44100u);
    CHECK(record(r, 1.0));
    std::vector<TakeRow> rows = c.takes();
    CHECK(rows.size() == 1 && rows[0].rate == 44100u && rows[0].bits == 16);
    /* a bit depth: the card's 24-bit format, packed */
    CHECK(c.set_rec_params(RecFormat::Wav, "", 0, 24)); /* rate back to the default (48 kHz) */
    CHECK(r.wait_meter());
    s = r.snap();
    CHECK_EQ(s.take.format_text, "48k/24 WAV");
    CHECK(last_request(r).rate == 48000 && last_request(r).format == SampleFormat::S24_3LE);
    CHECK(record(r, 0.5));
    rows = c.takes();
    CHECK(rows.size() == 2 && rows[0].bits == 24 && rows[0].rate == 48000u);
    /* FLAC at 16 kHz 16-bit */
    CHECK(c.set_rec_params(RecFormat::Flac, "", 16000, 16));
    CHECK(r.wait_meter());
    CHECK_EQ(r.snap().take.format_text, "16k/16 FLAC");
    CHECK(record(r, 0.5));
    rows = c.takes();
    CHECK(rows.size() == 3 && rows[0].format_tag == "FLAC" && rows[0].rate == 16000u);
    /* Opus: every bit rate of the list reaches the encoder (and the journal), at 48 kHz and at 16 kHz */
    for (int k : kOpusKbps) {
        CHECK(c.set_rec_params(RecFormat::Opus, "", k % 32 == 0 ? 16000 : 0, k));
        CHECK(r.wait_meter());
        const bool at16 = k % 32 == 0;
        CHECK_EQ(r.snap().take.format_text, at16 ? "16k/16 OPUS" : "48k/16 OPUS");
        seen_bps = 0;
        CHECK(record(r, 0.3));
        CHECK_EQ(seen_bps.load(), k * 1000);
        rows = c.takes();
        const JournalEntry je = journal_of(r, rows[0].name);
        CHECK_EQ(je.bitrate_bps, k * 1000);
        CHECK_EQ(je.codec, "opus");
        CHECK_EQ(je.fmt.rate, at16 ? 16000u : 48000u);
        CHECK_EQ(je.codec_preset, preset_of_kbps(k));
        CHECK(!je.force_mono); /* a mono card: nothing to fold */
    }
    /* a stored Opus rate the codec cannot use (hand-edited file, 44.1 kHz): the card's own default for Opus, 48 kHz */
    CHECK(c.set_rec_params(RecFormat::Opus, "", 44100, 64));
    CHECK(r.wait_meter());
    CHECK_EQ(r.snap().take.format_text, "48k/16 OPUS");

    /* refused values and a running take: nothing changes */
    CHECK(!c.set_rec_params(RecFormat::Opus, "", 0, 40));      /* not in the list */
    CHECK(!c.set_rec_params(RecFormat::Wav, "", 0, 20));       /* not a depth */
    CHECK(!c.set_rec_params(RecFormat::Wav, "", 100, 0));      /* not a rate */
    CHECK(!c.set_rec_params(RecFormat::Wav, "speech24", 0, 0)); /* a preset of another codec */
    CHECK(!c.set_rec_params(RecFormat::Opus, "bogus", 0, 0));
    s = r.snap();
    CHECK(s.settings.format == RecFormat::Opus && s.settings.rate == 44100 && s.settings.quality == 64);
    CHECK(c.set_rec_params(RecFormat::Wav, "", 0, 0));
    CHECK(r.wait_meter());
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    const size_t reqs = request_count(r);
    CHECK(!c.set_rec_params(RecFormat::Wav, "", 16000, 0));
    CHECK(!c.set_format_choice(RecFormat::Flac, ""));
    CHECK(r.snap().settings.rate == 0 && request_count(r) == reqs); /* no reopen while recording */
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));

    /* the file keeps the new keys beside the old ones; a preset resets rate and quality */
    CHECK(c.set_rec_params(RecFormat::Opus, "", 16000, 96));
    std::string body = slurp(r.home + "/.config/cassette/settings.json");
    CHECK(body.find("\"rate\": 16000") != std::string::npos || body.find("\"rate\":16000") != std::string::npos);
    CHECK(body.find("96") != std::string::npos && body.find("\"opus\"") != std::string::npos);
    CHECK(c.set_format_choice(RecFormat::Opus, "speech24"));
    CHECK(r.wait_meter());
    s = r.snap();
    CHECK(s.settings.rate == 0 && s.settings.quality == 0 && s.settings.format_preset == "speech24");
    CHECK_EQ(s.take.format_text, "48k/16 OPUS");
    body = slurp(r.home + "/.config/cassette/settings.json");
    CHECK(body.find("\"rate\"") == std::string::npos && body.find("\"quality\"") == std::string::npos);
    /* the next start reads them back */
    CHECK(c.set_rec_params(RecFormat::Flac, "", 44100, 24));
    r.close();
    r.open();
    CHECK(r.wait_meter());
    s = r.snap();
    CHECK(s.settings.format == RecFormat::Flac && s.settings.rate == 44100 && s.settings.quality == 24);
    CHECK_EQ(s.take.format_text, "44.1k/24 FLAC");
    CHECK_EQ(last_request(r).rate, 44100u);
    CHECK(last_request(r).format == SampleFormat::S24_3LE);
}

/* An old settings file (format and preset only) records exactly as before on a card with several rates, and on a card that
 * has only 44.1 kHz an Opus choice can be stored and records as WAV at the card's rate (Opus dimmed, never resampled). */
void t_rec_params_old_and_44k() {
    {
        std::vector<FakeCard> cards = one_card();
        cards[0].offer.rates = {16000, 44100, 48000};
        Rig r(cards);
        use_test_codecs(r);
        write_settings(r.home, "{\"version\": 1, \"format\": \"opus\", \"format_preset\": \"music64\"}");
        r.cfg.writer_sync_ms = 500;
        r.open();
        CHECK(r.wait_meter());
        CHECK_EQ(r.snap().take.format_text, "48k/16 OPUS");
        CHECK_EQ(last_request(r).rate, 48000u);
        CHECK(record(r, 0.4));
        const std::vector<TakeRow> rows = r.ctl().takes();
        CHECK(rows.size() == 1 && rows[0].format_tag == "OPUS");
        const JournalEntry je = journal_of(r, rows[0].name);
        CHECK_EQ(je.bitrate_bps, 64000);
        CHECK_EQ(je.codec_preset, "music64");
    }
    {
        std::vector<FakeCard> cards = one_card();
        cards[0].offer.rates = {44100};
        Rig r(cards);
        use_test_codecs(r);
        r.cfg.writer_sync_ms = 500;
        r.open();
        Controller &c = r.ctl();
        CHECK(r.wait_meter());
        CHECK_EQ(r.snap().take.format_text, "44.1k/16 WAV");
        CHECK(codec_rates(r.snap().format_inputs, RecFormat::Opus).empty());
        CHECK(c.set_rec_params(RecFormat::Opus, "", 0, 64)); /* stored (the UI never offers it) */
        CHECK_EQ(r.snap().take.format_text, "44.1k/16 WAV"); /* the safety net: WAV */
        CHECK_EQ(last_request(r).rate, 44100u);
        CHECK(record(r, 0.4));
        const std::vector<TakeRow> rows = c.takes();
        CHECK(rows.size() == 1 && rows[0].format_tag == "WAV" && rows[0].rate == 44100u);
    }
}

}  // namespace

void test_formats_app() {
    t_flac_take();
    t_opus_take();
    t_missing_decoder();
    t_fallback_to_wav();
    t_mixed_folder();
    t_repair_at_launch();
    t_flac_disk_full();
    t_set_format_choice();
    t_rec_params();
    t_rec_params_old_and_44k();
}
