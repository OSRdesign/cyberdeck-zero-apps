/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the app model (see controller.hpp).
 */
#include "controller.hpp"

#include "audio/alsa/audio_capture.hpp"
#include "audio/alsa/open_policy.hpp"
#include "audio/fake_alsa.hpp"
#include "audio/mic_match.hpp"
#include "camera/identity.hpp"
#include "modes/codec_caps.hpp"
#include "record/av_libs.hpp"
#include "record/disk_guard.hpp"
#include "scenes/atomic_file.hpp"
#include "scenes/fileops.hpp"
#include "scenes/naming.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace vhs {

/* One recording: the recorder, the microphone's capture and its lamp, and the capture thread's tap into the recorder.
 * Declared so that the audio capture goes first, then the recorder (its sink), then the lamp (its period callback). */
struct Controller::RecSession : FrameTap {
    SoundLevel level;
    std::unique_ptr<Recorder> rec;
    std::unique_ptr<AudioCapture> audio;
    ModeReq mode;
    std::string mic;            /* "CM969 16000 Hz 2 ch" for the log */
    bool held = false;          /* the Video size is held on the capture thread */
    bool audio_ended = false;
    bool stopping = false;
    bool first_logged = false;
    std::uint64_t losses = 0;   /* the camera's losses at REC */
    std::int64_t pressed_ms = 0;
    void tap_frame(const std::uint8_t *data, std::size_t n, std::int64_t ts_us, const ModeReq &m) override
    {
        (void)m;
        rec->push_video(data, n, ts_us);
    }
};

int ram_mb(const std::string &root)
{
    std::ifstream f(root + "/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 9, "MemTotal:") != 0) continue;
        const long kb = std::strtol(line.c_str() + 9, nullptr, 10);
        return kb > 0 ? static_cast<int>(kb / 1024) : -1;
    }
    return -1;
}

Controller::Controller(ControllerConfig cfg) : cfg_(std::move(cfg)), store_(posix_file_ops())
{
    soc_ = detect_soc(cfg_.sys_root);
    for (SocClass c : {SocClass::Bcm2712, SocClass::Bcm2711, SocClass::Bcm2837, SocClass::Other})
        if (cfg_.soc_override == soc_name(c)) soc_ = c;
    /* the fake camera reads a settings file only when one is named (VHS_SETTINGS): never the user's */
    const bool no_file = cfg_.fake && cfg_.settings_path.empty() && !std::getenv("VHS_SETTINGS");
    if (!no_file) store_.load(cfg_.settings_path.empty() ? settings_path() : cfg_.settings_path);
    store_.set_read_only(!cfg_.persist);
    if (cfg_.fake && !cfg_.settings_overrides.empty()) {
        /* the scenes' settings (VHS_SET), never written: the fake camera's store is read-only */
        Settings s = store_.get();
        apply_settings_overrides(s, cfg_.settings_overrides);
        store_.set_read_only(true);
        store_.set(s);
    }
    sel_settings_ = store_.get();
    if (!cfg_.folder.empty()) {
        folder_ = cfg_.folder;
    } else if (cfg_.fake) {
        /* the fake camera never writes into the user's folder: the harness's run folder, else a temporary one */
        const char *sd = std::getenv("APPLAUNCH_STATE_DIR");
        std::string base = sd && sd[0] ? std::string(sd) : std::string();
        if (!base.empty()) {
            const std::size_t s = base.find_last_of('/');
            folder_ = (s == std::string::npos ? base : base.substr(0, s)) + "/vhs-folder";
        } else {
            const char *tmp = std::getenv("TMPDIR");
            folder_ = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/vhs-fake-" + std::to_string(::getpid());
        }
    } else {
        folder_ = expand_home(store_.get().folder);
    }
    box_w_ = cfg_.box_w;
    box_h_ = cfg_.box_h;
    V4l2Api *api = nullptr;
    if (cfg_.fake) {
        fake_ = std::make_unique<FakeCamera>(cfg_.fake_opt);
        if (!fake_->canned_error().empty()) std::fprintf(stderr, "[vhs] fake camera: %s\n", fake_->canned_error().c_str());
        finder_ = make_fake_finder(*fake_);
        api = fake_.get();
    } else {
        real_api_ = make_real_v4l2();
        api = real_api_.get();
        finder_ = cfg_.device.empty() ? make_system_finder(*api, cfg_.dev_root, cfg_.sys_root)
                                      : make_fixed_finder(cfg_.device, cfg_.sys_root);
    }
    finder_->prefer(store_.get().camera_id);
    const int ram = ram_mb(cfg_.sys_root);
    ram_mb_ = ram;
    alsa_ = cfg_.alsa;
    if (!alsa_) {
        if (cfg_.fake) alsa_ = std::make_unique<FakeAlsa>(fake_mic_options(cfg_.fake_mic));
        else alsa_ = make_real_alsa();
    }
    PipelineConfig pc;
    pc.api = api;
    pc.finder = finder_.get();
    pc.choose = [this](const std::vector<CamMode> &listed, const CamIdentity &id) { return choose(listed, id); };
    pc.mode = cfg_.mode;
    pc.denom = cfg_.denom;
    pc.fmt = cfg_.fmt;
    pc.box_w = cfg_.box_w;
    pc.box_h = cfg_.box_h;
    pc.fastdct = cfg_.fastdct;
    pc.ui_tid = cfg_.ui_tid;
    pc.log_every = cfg_.log_every;
    pc.buffers = (ram > 0 && ram <= 512) ? 3 : 4; /* brief 5.1 step 4 */
    pipe_ = std::make_unique<Pipeline>(pc);
    std::fprintf(stderr, "[vhs] board: soc %s, ram %d MB, %d capture buffers, photo default %s; %s\n", soc_name(soc_), ram,
                 pc.buffers, photo_mode_name(default_photo_mode(soc_)),
                 codec_caps_text(detect_codec_caps(cfg_.sys_root, cfg_.dev_root)).c_str());
    std::fprintf(stderr, "[vhs] folder %s, self-timer %d s, clock %s\n", folder_.c_str(), store_.get().self_timer_s,
                 synced() ? "synchronised" : "not synchronised (names say nodate, no EXIF date)");
    worker_.start();
    std::stringstream ss(cfg_.fake_script);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const std::size_t at = item.find('@');
        if (at == std::string::npos) continue;
        ScriptStep s{std::strtoll(item.c_str() + at + 1, nullptr, 10), item.substr(0, at), false};
        script_.push_back(s);
    }
}

Controller::~Controller()
{
    stop();
    worker_.stop(); /* a save in progress finishes first */
}

bool Controller::synced() const
{
    if (cfg_.clock_synced >= 0) return cfg_.clock_synced != 0;
    return clock_synchronized();
}

void Controller::start()
{
    if (pipe_->running()) return;
    if (started_ms_ == 0) {
        started_ms_ = mono_ms();
        run_script(started_ms_); /* the steps at 0 ms happen before the camera is first opened */
    }
    pipe_->start();
}

void Controller::stop()
{
    cancel_countdown(); /* brief 13: a countdown ends when the camera is released */
    finish_recording(RecStop::Released); /* hide, close, the review, SIGTERM's teardown: the file is finished first */
    pipe_->stop();
}

bool Controller::running() const
{
    return pipe_->running();
}

void Controller::set_box(int w, int h)
{
    {
        std::lock_guard<std::mutex> lock(sel_m_);
        box_w_ = w;
        box_h_ = h;
    }
    pipe_->set_box(w, h);
}

Selection Controller::selection() const
{
    std::lock_guard<std::mutex> lock(sel_m_);
    return sel_;
}

ModeReq Controller::choose(const std::vector<CamMode> &listed, const CamIdentity &id)
{
    (void)id;
    SelectInput in;
    in.listed = listed;
    in.soc = soc_;
    Selection s;
    {
        std::lock_guard<std::mutex> lock(sel_m_);
        in.have_photo_mode = sel_settings_.have_photo_mode;
        in.photo_mode = sel_settings_.photo_mode;
        in.photo_size = sel_settings_.photo_size;
        in.video_size = sel_settings_.video_size;
        in.box_w = box_w_;
        in.box_h = box_h_;
    }
    s = select_modes(in);
    std::fprintf(stderr, "[vhs] %s\n", selection_text(s, soc_).c_str());
    std::lock_guard<std::mutex> lock(sel_m_);
    sel_ = s;
    return s.ok ? s.preview : ModeReq();
}

void Controller::run_script(std::int64_t now_ms)
{
    if (!fake_ || started_ms_ == 0) return;
    for (ScriptStep &s : script_) {
        if (s.done || now_ms - started_ms_ < s.at_ms) continue;
        s.done = true;
        std::fprintf(stderr, "[vhs] fake camera: %s\n", s.action.c_str());
        if (s.action == "unplug") fake_->unplug();
        else if (s.action == "replug") fake_->replug();
        else if (s.action == "turn") fake_->set_portrait(!fake_->modes().empty() && !camera_portrait(fake_->modes()));
        else if (s.action == "busy") fake_->set_busy(true);
        else if (s.action == "free") fake_->set_busy(false);
        else if (s.action == "noaccess") fake_->set_no_access(true);
        else if (s.action == "access") fake_->set_no_access(false);
        else if (s.action == "dwc") {
            CamIdentity id = fake_->identity();
            id.hc_driver = "dwc_otg";
            fake_->set_identity(id);
        }
    }
}

Snapshot Controller::step(std::int64_t now_ms)
{
    run_script(now_ms);
    worker_.drain(); /* the scenes worker's completions (photo saved, review list and picture) */
    const CaptureStatus cam = pipe_->camera();
    if (timer_.active() && timer_.kind() == TimerKind::Video) { /* the video's self-timer */
        if (cam.state != CamState::Streaming && cam.state != CamState::Turned) cancel_countdown();
        else if (timer_.fire(now_ms)) start_recording(now_ms);
    }
    photo_step(now_ms, cam);
    Snapshot s;
    s.state = cam.state;
    s.mode = cam.mode;
    s.yuyv = cam.mode.fourcc == kYUYV;
    s.camera = !cam.id.product.empty() ? cam.id.product : cam.card;
    s.detail = cam.detail;
    s.replug_hint = cam.state == CamState::NotBack && bus_needs_replug(cam.bus_driver);
    s.capture_mode = capture_mode_;
    s.gen = cam.gen;

    /* the camera remembered by identity (D-15) */
    if ((cam.state == CamState::Streaming || cam.state == CamState::Turned) && cam.id.valid() &&
        cam.id.key() != store_.get().camera_id) {
        Settings st = store_.get();
        st.camera_id = cam.id.key();
        const int r = store_.set(st);
        if (r < 0) std::fprintf(stderr, "[vhs] settings: could not write %s (%d)\n", store_.path().c_str(), r);
        finder_->prefer(st.camera_id);
    }
    /* notices */
    if (cam.losses > losses_seen_) {
        losses_seen_ = cam.losses;
        notices_.push(Notice{kNoticeUnplugged, false});
    }
    if (cam.state == CamState::Busy) notices_.set_standing(Notice{kNoticeBusy, false});
    else if (cam.state == CamState::NoAccess) notices_.set_standing(Notice{kNoticeNoAccess, false});
    else notices_.set_standing(Notice());
    const bool open = cam.state == CamState::Streaming || cam.state == CamState::Turned;
    if (pipe_->has_interval()) {
        const Interval iv = pipe_->last_interval();
        if (iv.index != interval_seen_) {
            interval_seen_ = iv.index;
            if (open && undervolt_.step(iv.under_voltage() || cfg_.force_undervolt, now_ms))
                notices_.push(Notice{kNoticePower, false});
        }
    }
    rec_step(now_ms, cam, s);
    s.notice = notices_.current(now_ms);
    s.countdown = timer_.active() ? std::max(1, timer_.shown(now_ms)) : 0;
    s.countdown_video = timer_.active() && timer_.kind() == TimerKind::Video;
    s.timer_total = timer_.active() ? timer_.total() : 0;
    s.flash = flash_;
    s.photo_busy = photo_ != PhotoStep::Idle;
    s.photos_saved = photos_saved_;
    s.last_photo = last_photo_;
    return s;
}

void Controller::toggle_capture_mode()
{
    if (timer_.active() || rec_) return; /* dimmed during the countdown; "Photo now" while recording */
    capture_mode_ = capture_mode_ == CaptureMode::Photo ? CaptureMode::Video : CaptureMode::Photo;
}

/* ------------------------------------------------------------------------------------------------ the photo */

bool Controller::photo_needs_switch(const Selection &s) const
{
    /* Light: the preview streams the Light mode; a photo switches to the Photo size (brief 4.1 rule 3). An explicit
     * --mode (bench, tests) takes the frame it streams. */
    return s.ok && !s.yuyv_only && s.mode == PhotoMode::Light && !s.light_is_instant && !cfg_.mode.valid();
}

void Controller::release_hold()
{
    if (!held_) return;
    held_ = false;
    pipe_->capture().release_hold();
}

void Controller::shutter(std::int64_t now_ms)
{
    if (rec_ || capture_mode_ == CaptureMode::Video) { /* the red button: start or stop a scene (brief 6.8, 12.3) */
        record(now_ms);
        return;
    }
    if (photo_ != PhotoStep::Idle) return;
    const CaptureStatus cam = pipe_->camera();
    if (cam.state != CamState::Streaming) return;
    if (cam.mode.fourcc != kMJPG) {
        notices_.push(Notice{kNoticeNoMjpegPhoto, false}); /* section 20: YUYV photos are refused */
        return;
    }
    const int secs = store_.get().self_timer_s;
    if (secs > 0) {
        timer_.start(secs, now_ms, TimerKind::Photo);
        photo_ = PhotoStep::Countdown;
        const Selection s = selection();
        if (photo_needs_switch(s)) { /* the stream switches now, so zero acts at once (brief 6.7) */
            pipe_->capture().hold_mode(s.photo);
            held_ = true;
        }
        std::fprintf(stderr, "[vhs] self-timer: %d s%s\n", secs, held_ ? ", the Photo size streams meanwhile" : "");
        return;
    }
    fire(now_ms);
}

bool Controller::cancel_countdown()
{
    if (!timer_.active()) return false;
    timer_.cancel();
    release_hold();
    photo_ = PhotoStep::Idle;
    std::fprintf(stderr, "[vhs] self-timer: cancelled, nothing taken\n");
    return true;
}

void Controller::fire(std::int64_t now_ms)
{
    const Selection s = selection();
    ModeReq expect; /* Instant: the next good frame of whatever streams */
    if (!rec_ && photo_needs_switch(s)) { /* during a recording: the recording's current frame (brief 7 step 1) */
        expect = s.photo;
        if (!held_) {
            pipe_->capture().hold_mode(s.photo);
            held_ = true;
        }
    }
    pipe_->capture().request_photo(expect, ++photo_id_);
    photo_ = PhotoStep::Waiting;
    photo_t0_ms_ = now_ms;
}

void Controller::photo_step(std::int64_t now_ms, const CaptureStatus &cam)
{
    if (photo_ == PhotoStep::Countdown) {
        if (cam.state != CamState::Streaming && cam.state != CamState::Turned) {
            cancel_countdown(); /* the camera went away: nothing is taken */
        } else if (timer_.fire(now_ms)) {
            fire(now_ms);
        }
        return;
    }
    if (photo_ != PhotoStep::Waiting) return;
    PhotoShot shot;
    if (!pipe_->capture().take_photo(shot)) {
        if (now_ms - photo_t0_ms_ > 8000) { /* the camera never delivered one */
            pipe_->capture().cancel_photo();
            release_hold();
            photo_ = PhotoStep::Idle;
            notices_.push(Notice{std::string(kNoticePhotoFailed) + "the camera sent no frame.", false});
        }
        return;
    }
    release_hold();
    if (!shot.ok || shot.id != photo_id_) {
        photo_ = PhotoStep::Idle;
        notices_.push(Notice{std::string(kNoticePhotoFailed) + (shot.error.empty() ? "no frame." : shot.error + "."), false});
        return;
    }
    ++flash_; /* one flash: the photo is taken (brief 6.7, 12.4 item 2) */
    photo_ = PhotoStep::Saving;
    std::fprintf(stderr, "[vhs] photo: %s frame %u taken %lld ms after the press\n", mode_text(shot.mode).c_str(),
                 shot.seq, static_cast<long long>(now_ms - photo_t0_ms_));
    ExifFields f;
    f.make = cam.id.manufacturer;
    f.model = !cam.id.product.empty() ? cam.id.product : cam.card;
    f.software = kSoftware;
    const std::string folder = folder_;
    const bool sync = synced();
    const bool utc = cfg_.utc;
    auto data = std::make_shared<std::vector<std::uint8_t>>(std::move(shot.data));
    const std::int64_t wall = static_cast<std::int64_t>(std::time(nullptr));
    worker_.post([this, folder, f, sync, utc, data, wall]() -> std::function<void()> {
        const PhotoSave r = save_photo(posix_file_ops(), folder, data->data(), data->size(), f, wall, sync, utc);
        if (r.ok)
            std::fprintf(stderr, "[vhs] photo: saved %s (%zu bytes%s)\n", r.path.c_str(), r.bytes,
                         r.facts.dht_inserted ? ", Annex K tables added" : "");
        else std::fprintf(stderr, "[vhs] photo: not saved: %s\n", r.error.c_str());
        return [this, r] {
            photo_ = PhotoStep::Idle;
            if (r.ok) {
                ++photos_saved_;
                last_photo_ = r.name;
            } else {
                notices_.push(Notice{std::string(kNoticePhotoFailed) + r.error + ".", false});
            }
        };
    });
}

/* ------------------------------------------------------------------------------------------------ recording */

ModeReq Controller::video_mode(const CaptureStatus &cam) const
{
    if (cfg_.mode.valid()) return cam.mode; /* an explicit --mode: what streams */
    const Selection s = selection();
    return (s.ok && s.video.valid() && s.video.fourcc == kMJPG) ? s.video : cam.mode;
}

std::uint64_t Controller::free_bytes(const std::string &folder) const
{
    if (cfg_.fake && cfg_.fake_free_mb >= 0) return static_cast<std::uint64_t>(cfg_.fake_free_mb) << 20;
    std::uint64_t b = 0;
    if (posix_file_ops().free_space(folder, b) != 0) return UINT64_MAX; /* unknown: not refused */
    return b;
}

void Controller::record(std::int64_t now_ms)
{
    if (rec_) {
        if (!rec_->stopping) stop_recording(RecStop::User, false); /* it asks nothing (canvas) */
        return;
    }
    if (timer_.active() || photo_ != PhotoStep::Idle) return;
    const CaptureStatus cam = pipe_->camera();
    if (cam.state != CamState::Streaming) return;
    if (cam.mode.fourcc != kMJPG) { /* P1 records the camera's MJPEG (section 20: no YUYV recording) */
        notices_.push(Notice{kNoticeNoMjpegVideo, false});
        return;
    }
    const int secs = store_.get().self_timer_s;
    if (secs > 0) {
        /* the Video size streams from the countdown's start, so zero acts at once (brief 6.7) */
        timer_.start(secs, now_ms, TimerKind::Video);
        const ModeReq m = video_mode(cam);
        pipe_->capture().hold_mode(m);
        held_ = true;
        std::fprintf(stderr, "[vhs] self-timer: %d s, then a video (%s streams meanwhile)\n", secs, mode_text(m).c_str());
        return;
    }
    start_recording(now_ms);
}

void Controller::photo_now(std::int64_t now_ms)
{
    if (!rec_ || rec_->stopping || photo_ != PhotoStep::Idle) return;
    fire(now_ms);
}

void Controller::start_recording(std::int64_t now_ms)
{
    const CaptureStatus cam = pipe_->camera();
    auto refuse = [this](const Notice &n) {
        release_hold(); /* the countdown's hold */
        if (!n.text.empty()) notices_.push(n);
    };
    if (cam.state != CamState::Streaming) return refuse(Notice());
    if (cam.mode.fourcc != kMJPG) return refuse(Notice{kNoticeNoMjpegVideo, false});
    std::string why;
    const AvLibs *av = cfg_.av ? cfg_.av : real_av_libs(&why);
    if (!av) {
        std::fprintf(stderr, "[vhs] recording: no FFmpeg: %s\n", why.c_str());
        return refuse(Notice{kNoticeNoFfmpeg, false});
    }
    const Settings st = store_.get();
    const std::uint64_t min_free = static_cast<std::uint64_t>(st.min_free_mb) << 20;
    if (ensure_folder(posix_file_ops(), folder_) != 0) {
        std::fprintf(stderr, "[vhs] recording: the folder %s cannot be made\n", folder_.c_str());
        return refuse(Notice{std::string(kNoticeRecFailed) + "the folder cannot be written.", false});
    }
    const std::uint64_t free_b = free_bytes(folder_);
    if (DiskGuard::refuse_at_rec(free_b, min_free)) { /* brief 6.4: refused under the floor, the same red toast */
        std::fprintf(stderr, "[vhs] recording: refused, %llu MB free (the floor is %d MB)\n",
                     static_cast<unsigned long long>(free_b >> 20), st.min_free_mb);
        return refuse(Notice{kNoticeDiskFull, true});
    }
    auto rs = std::make_unique<RecSession>();
    rs->mode = video_mode(cam);
    if (!held_) pipe_->capture().hold_mode(rs->mode); /* the countdown's hold is taken over */
    held_ = false;
    rs->held = true;

    /* the camera microphone (brief 5.4): matched by USB ancestry, hw: only */
    OpenResult mic;
    bool have_mic = false;
    const MicCard mc = fake_ ? camera_mic_by_port(*alsa_, cam.id.devpath)
                             : camera_mic(cfg_.sys_root, usb_device_dir(cfg_.sys_root, video_name_of(cam.device)));
    if (!mc.found) {
        std::fprintf(stderr, "[vhs] recording: no sound card on the camera's USB device: video only\n");
        notices_.push(Notice{kNoticeNoMic, false});
    } else {
        OpenOptions oo;
        oo.allow_server = false; /* busy means video only; the server route comes with the P2 chooser */
        oo.want_bits = 16;
        mic = open_capture(*alsa_, mc.card, oo);
        if (mic.ok() && mic.format.bits == 16) {
            have_mic = true;
            rs->mic = mc.card.id + " " + std::to_string(mic.format.rate) + " Hz " + std::to_string(mic.format.channels) + " ch";
        } else {
            std::fprintf(stderr, "[vhs] recording: the camera microphone %s: %s: video only\n", pcm_device(mc.card).c_str(),
                         mic.ok() ? "not 16-bit" : mic.message.c_str());
            notices_.push(Notice{mic.status == OpenStatus::Busy ? kNoticeMicBusy : kNoticeMicFailed, false});
            mic = OpenResult();
        }
    }

    RecorderConfig rc;
    rc.av = av;
    rc.ops = &posix_file_ops();
    rc.folder = folder_;
    rc.prefix = st.prefix;
    rc.when = when_text(static_cast<std::int64_t>(std::time(nullptr)), synced(), cfg_.utc);
    rc.video.codec = VideoCodec::Mjpeg; /* P1: the camera's MJPEG copied (D24) */
    rc.video.w = rs->mode.w;
    rc.video.h = rs->mode.h;
    rc.video.fps = rs->mode.fps;
    if (have_mic) {
        rc.audio_rate = static_cast<int>(mic.format.rate);
        rc.audio_channels = mic.format.channels;
    }
    rc.split_bytes = st.split ? static_cast<std::uint64_t>(st.split_mib) << 20 : 0;
    rc.min_free_bytes = min_free;
    rc.ring_seconds = (ram_mb_ > 0 && ram_mb_ <= 512) ? 2 : 4; /* brief 6.1 */
    if (cfg_.fake && cfg_.fake_free_mb >= 0) {
        const std::uint64_t fb = static_cast<std::uint64_t>(cfg_.fake_free_mb) << 20;
        rc.free_space = [fb] { return fb; };
    }
    rc.started_unix = static_cast<std::int64_t>(std::time(nullptr));
    rs->rec = std::make_unique<Recorder>(rc);
    rs->rec->start();
    if (have_mic) {
        rs->level.reset(rc.audio_rate, rc.audio_channels);
        RecSession *p = rs.get();
        CaptureOptions co;
        co.on_period = [p](const void *f, size_t n, const AudioFormat &) { p->level.feed(static_cast<const std::int16_t *>(f), n); };
        co.on_gap = [p](std::uint64_t lost) { p->rec->audio_gap(lost); };
        co.on_clock = [p](std::int64_t ns, std::uint64_t pos) { p->rec->audio_clock(ns, pos); };
        rs->audio = std::make_unique<AudioCapture>(std::move(mic), *rs->rec, co);
        if (!rs->audio->start()) {
            std::fprintf(stderr, "[vhs] recording: the microphone did not start: video only\n");
            rs->audio.reset();
            rs->rec->audio_end();
            rs->audio_ended = true;
            notices_.push(Notice{kNoticeMicFailed, false});
        }
    }
    rs->losses = cam.losses;
    rs->pressed_ms = now_ms;
    const std::string split = rc.split_bytes ? std::to_string(st.split_mib) + " MiB" : std::string("off");
    std::fprintf(stderr, "[vhs] recording: REC %s, %s, folder %s, split %s, floor %d MB, ring %d s\n", mode_text(rs->mode).c_str(),
                 rs->mic.empty() ? "no sound" : ("sound " + rs->mic).c_str(), folder_.c_str(), split.c_str(), st.min_free_mb,
                 rc.ring_seconds);
    pipe_->capture().set_tap(rs.get(), rs->mode); /* frames after the warm-up of the Video size */
    rec_ = std::move(rs);
}

void Controller::stop_recording(RecStop why, bool wait)
{
    if (!rec_) return;
    if (!rec_->stopping) {
        rec_->stopping = true;
        pipe_->capture().clear_tap(); /* no frame after this */
        if (rec_->audio) rec_->audio->stop();
        rec_->rec->request_stop(why);
        std::fprintf(stderr, "[vhs] recording: stop (%s)\n", rec_stop_name(why));
    }
    if (wait) end_recording();
}

void Controller::end_recording()
{
    if (!rec_) return;
    rec_->rec->join();
    const RecorderStatus st = rec_->rec->status();
    CaptureStats as;
    if (rec_->audio) as = rec_->audio->stats();
    rec_->audio.reset();
    if (rec_->held) pipe_->capture().release_hold(); /* back to the preview's mode */
    last_rec_ = st;
    if (st.frames > 0) {
        ++scenes_recorded_;
        last_scene_ = st.files.empty() ? std::string() : st.files.front();
    }
    std::fprintf(stderr,
                 "[vhs] recording: finished %s: %llu frames, %.1f s, %d part(s), %llu bytes, %llu frames dropped (ring), "
                 "%llu audio frames dropped, %llu lost in overruns, stop %s\n",
                 st.stem.c_str(), static_cast<unsigned long long>(st.frames), static_cast<double>(st.written_ms) / 1000.0, st.parts,
                 static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.ring_drops),
                 static_cast<unsigned long long>(st.audio_drops), static_cast<unsigned long long>(as.frames_lost_xrun),
                 rec_stop_name(st.stop));
    if (st.stop == RecStop::DiskFull) notices_.push(Notice{kNoticeDiskFull, true});
    else if (st.stop == RecStop::Error)
        notices_.push(Notice{std::string(kNoticeRecFailed) + (st.error.empty() ? "an error" : st.error) + ".", false});
    rec_.reset();
}

void Controller::finish_recording(RecStop why)
{
    if (rec_) stop_recording(why, true);
}

void Controller::rec_step(std::int64_t now_ms, const CaptureStatus &cam, Snapshot &s)
{
    if (watch_until_ != 0) { /* a loss stopped the scene: the camera came back turned (brief 5.5) */
        if (cam.turns > watch_turns_) {
            notices_.push(Notice{kNoticeRecTurned, false});
            watch_until_ = 0;
        } else if (now_ms > watch_until_) {
            watch_until_ = 0;
        }
    }
    if (rec_) {
        const RecorderStatus st = rec_->rec->status();
        if (!rec_->stopping) {
            if (cam.losses > rec_->losses) { /* finished at the last frame; "Camera unplugged." comes with the loss */
                watch_turns_ = cam.turns;
                watch_until_ = now_ms + 15000;
                stop_recording(RecStop::CameraLost, false);
            } else if (st.finished) { /* the writer ended by itself: the disk guard or an error */
                stop_recording(st.stop, false);
            }
        }
        if (rec_->audio && !rec_->audio_ended && !rec_->audio->running()) {
            /* the microphone went away (unplugged with the camera, or failed): video goes on alone (brief 5.4) */
            rec_->audio_ended = true;
            rec_->rec->audio_end();
            const CaptureStats as = rec_->audio->stats();
            std::fprintf(stderr, "[vhs] recording: the microphone stopped (%s): video goes on\n",
                         as.message.empty() ? capture_error_name(as.stop_reason) : as.message.c_str());
        }
        if (rec_->level.silence_event()) notices_.push(Notice{kNoticeNoSound, false}); /* once per scene (D38) */
        if (st.started && !rec_->first_logged) {
            rec_->first_logged = true;
            std::fprintf(stderr, "[vhs] recording: first frame written %lld ms after REC\n",
                         static_cast<long long>(now_ms - rec_->pressed_ms));
        }
        if (rec_->stopping && rec_->rec->status().finished) {
            end_recording();
        } else {
            s.recording = true;
            s.rec_started = st.started;
            s.rec_ms = st.written_ms;
            s.rec_bytes = st.bytes;
            s.rec_left_s = st.seconds_left;
            s.rec_audio = rec_->audio != nullptr && !rec_->audio_ended;
            s.lamp = s.rec_audio && rec_->level.lamp();
            s.rec_mode = rec_->mode;
        }
    }
    s.scenes_recorded = scenes_recorded_;
    s.last_scene = last_scene_;
}

/* ------------------------------------------------------------------------------------------------ the review */

void Controller::review_open(int box_w, int box_h)
{
    review_ = ReviewView();
    review_.open = true;
    review_box_w_ = box_w;
    review_box_h_ = box_h;
    review_pic_.reset();
    review_list_.clear();
    review_list();
}

void Controller::review_list()
{
    review_.loading = true;
    const std::string folder = folder_;
    const std::uint64_t req = ++review_req_;
    worker_.post([this, folder, req]() -> std::function<void()> {
        auto list = std::make_shared<std::vector<PhotoEntry>>(list_photos(posix_file_ops(), folder));
        return [this, list, req] {
            if (req != review_req_ || !review_.open) return;
            review_list_ = *list;
            review_.count = static_cast<int>(review_list_.size());
            review_.index = std::min(review_.index, std::max(0, review_.count - 1));
            review_.loading = false;
            review_decode();
        };
    });
}

void Controller::review_decode()
{
    if (review_list_.empty()) {
        review_.name.clear();
        review_.meta.clear();
        review_pic_.reset();
        ++review_.picture_gen;
        return;
    }
    const PhotoEntry e = review_list_[static_cast<std::size_t>(review_.index)];
    review_.name = e.name;
    review_.loading = true;
    const std::uint64_t req = ++review_req_;
    const int bw = review_box_w_, bh = review_box_h_;
    const PixFmt fmt = cfg_.fmt;
    const bool utc = cfg_.utc;
    worker_.post([this, e, req, bw, bh, fmt, utc]() -> std::function<void()> {
        std::string bytes;
        auto pic = std::make_shared<Picture>();
        std::string err;
        bool ok = read_small_file(posix_file_ops(), e.path, bytes, 64u << 20) == 0 &&
                  decode_jpeg_fit(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), bw, bh, fmt, *pic, &err);
        const std::string meta = ok ? photo_meta(pic->mode.w, pic->mode.h, e.size, e.mtime, utc) : std::string();
        if (!ok) std::fprintf(stderr, "[vhs] review: %s: %s\n", e.path.c_str(), err.empty() ? "cannot read" : err.c_str());
        return [this, pic, ok, meta, req] {
            if (req != review_req_ || !review_.open) return;
            review_.loading = false;
            review_pic_ = ok ? pic : nullptr;
            review_.meta = meta;
            ++review_.picture_gen;
        };
    });
}

void Controller::review_move(int delta)
{
    if (!review_.open || review_list_.empty()) return;
    const int n = static_cast<int>(review_list_.size());
    const int i = std::max(0, std::min(n - 1, review_.index + delta));
    if (i == review_.index) return;
    review_.index = i;
    review_decode();
}

void Controller::review_delete()
{
    if (!review_.open || review_list_.empty()) return;
    const std::string path = review_list_[static_cast<std::size_t>(review_.index)].path;
    const std::uint64_t req = ++review_req_;
    review_.loading = true;
    worker_.post([this, path, req]() -> std::function<void()> {
        const int r = posix_file_ops().unlink(path);
        std::fprintf(stderr, "[vhs] review: delete %s: %s\n", path.c_str(), r == 0 ? "done" : std::strerror(-r));
        return [this, req] {
            if (req != review_req_ || !review_.open) return;
            review_list(); /* the list again; the index stays (the next older photo), clamped */
        };
    });
}

void Controller::review_close()
{
    review_ = ReviewView();
    review_list_.clear();
    review_pic_.reset();
    ++review_req_;
}

ReviewView Controller::review() const
{
    return review_;
}

void Controller::dismiss_notice(std::int64_t now_ms)
{
    notices_.dismiss(now_ms);
}

void Controller::push_notice(const Notice &n)
{
    notices_.push(n);
}

} // namespace vhs
