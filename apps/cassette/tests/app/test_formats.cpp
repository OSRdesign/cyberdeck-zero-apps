/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: recording FLAC and Ogg Opus takes end to end (phase B) on the fake sound system, with the test
 * encoders (tests/fixture_codecs.hpp) in place of libFLAC / libopusenc. The stored setting chooses ("format" and
 * "format_preset" in settings.json; the Format row is not part of this), effective_choice() decides per card and per
 * library; the take name, the list row, the journal, the recorder's format text and the hours left follow; LOAD, PLAY,
 * the preview and trim refuse a FLAC / Opus take with one notice; take numbers run on across formats; repair at launch;
 * a full card.
 */
#include "helpers.hpp"

#include "../fixture_codecs.hpp"
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

/* The libraries "found" and the test encoders in place of libFLAC / libopusenc. */
void use_test_codecs(Rig &r, bool flac = true, bool opus = true) {
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

    /* phase C gate: one notice, nothing plays, nothing hangs */
    c.ack_notices();
    CHECK(!c.load(row.name));
    CHECK(notice_has(r.snap(), "FLAC playback is not available yet"));
    c.ack_notices();
    CHECK(!c.preview_take(row.name));
    CHECK(notice_has(r.snap(), "FLAC playback is not available yet"));
    c.ack_notices();
    CHECK(!c.open_trim(row.name));
    CHECK(!r.snap().trim.open);
    CHECK(notice_has(r.snap(), "FLAC playback is not available yet"));
    c.ack_notices();
    CHECK(!c.play_take(row.name));
    c.ack_notices();
    c.key_event(KeyEvent::PlayPause); /* the loaded take is this FLAC take */
    CHECK(r.wait_state(kDeckIdle, 2000));
    CHECK(notice_has(r.snap(), "FLAC playback is not available yet"));
    CHECK_EQ(r.sink.opens(), 0u); /* the player never opened the output */
    /* rename and delete work on FLAC takes */
    std::string nn;
    CHECK_EQ(c.rename_take(row.name, "field rec", &nn), 0);
    CHECK(nn.compare(0, 14, "field rec_001_") == 0 && ends_with(nn, ".flac"));
    rows = c.takes();
    CHECK(rows.size() == 1 && rows[0].name == nn && rows[0].format_tag == "FLAC");
    CHECK_EQ(r.snap().take.name, nn); /* still loaded under its new name */
    CHECK_EQ(c.delete_take(nn), 0);
    CHECK(c.takes().empty());
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
    r.ctl().ack_notices();
    CHECK(!r.ctl().load(rows[0].name));
    CHECK(notice_has(r.snap(), "Opus playback is not available yet"));
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

}  // namespace

void test_formats_app() {
    t_flac_take();
    t_opus_take();
    t_fallback_to_wav();
    t_mixed_folder();
    t_repair_at_launch();
    t_flac_disk_full();
    t_set_format_choice();
}
