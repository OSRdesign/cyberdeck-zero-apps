/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the app model (see controller.hpp).
 */
#include "controller.hpp"

#include "audio/alsa/audio_capture.hpp"
#include "audio/alsa/open_policy.hpp"
#include "audio/fake_alsa.hpp"
#include "drives/demo_drives.hpp"
#include "audio/mic_match.hpp"
#include "play/fake_output.hpp"
#include "camera/identity.hpp"
#include "live/clock.hpp"
#include "modes/codec_caps.hpp"
#include "record/av_libs.hpp"
#include "record/disk_guard.hpp"
#include "record/journal.hpp"
#include "scenes/demo.hpp"
#include "scenes/locks.hpp"
#include "scenes/thumbs.hpp"
#include "scenes/atomic_file.hpp"
#include "scenes/fileops.hpp"
#include "scenes/naming.hpp"

#include <sys/resource.h>
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
    std::uint64_t gen = 0;      /* the stream session at REC (a later one: the stream switched for the scene) */
    ModeReq from;               /* the mode streaming at REC */
    int mic_ms = -1;            /* matching and opening the microphone */
    std::int64_t pressed_ms = 0;
    void tap_frame(const std::uint8_t *data, std::size_t n, std::int64_t ts_us, const ModeReq &m) override
    {
        (void)m;
        rec->push_video(data, n, ts_us);
    }
};

/* the process's CPU time (user + system), ms: the playback's core share in the log */
static std::int64_t process_cpu_ms()
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return (static_cast<std::int64_t>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000) + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;
}

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
    if (cfg_.fake) {
        const std::size_t s = folder_.find_last_of('/');
        cache_dir_ = (s == std::string::npos ? folder_ : folder_.substr(0, s)) + "/vhs-cache"; /* never the user's */
    } else {
        cache_dir_ = cache_dir();
    }
    if (cfg_.fake && !cfg_.demo_scenes.empty()) { /* the headless scenes' demo folder (never the user's) */
        const std::string folder = folder_, set = cfg_.demo_scenes;
        const AvLibs *forced = cfg_.av;
        worker_.post([folder, set, forced]() -> std::function<void()> {
            std::string why;
            const AvLibs *av = forced ? forced : real_av_libs(&why);
            const int n = make_demo_scenes(posix_file_ops(), folder, av, set);
            std::fprintf(stderr, "[vhs] demo scenes (%s): %d file(s) in %s\n", set.c_str(), n, folder.c_str());
            return nullptr;
        });
    }
    startup_repair();
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
    copy_.reset(); /* a running copy cancelled (its .part removed), then the drives VHS mounted unmounted */
    settings_close();
    playback_close();
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
    /* the owner's decision (2026-11-15): Video mode previews the Video size stream, Photo mode the Photo size (Instant)
     * or the Light stream; a camera without an MJPEG Video size keeps the preview */
    if (s.ok && video_preview_.load() && s.video.valid()) return s.video;
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
    if (copy_) copy_->step();
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
    play_step();
    s.notice = notices_.current(now_ms);
    s.countdown = timer_.active() ? std::max(1, timer_.shown(now_ms)) : 0;
    s.countdown_video = timer_.active() && timer_.kind() == TimerKind::Video;
    s.timer_total = timer_.active() ? timer_.total() : 0;
    s.flash = flash_;
    s.photo_busy = photo_ != PhotoStep::Idle;
    s.photos_saved = photos_saved_;
    s.last_photo = last_photo_;
    s.scene_count = scene_count_;
    return s;
}

void Controller::toggle_capture_mode()
{
    if (timer_.active() || rec_) return; /* dimmed during the countdown; "Photo now" while recording */
    capture_mode_ = capture_mode_ == CaptureMode::Photo ? CaptureMode::Video : CaptureMode::Photo;
    video_preview_ = capture_mode_ == CaptureMode::Video;
    pipe_->capture().rechoose(); /* the stream follows the mode (switched in place; the last picture stays meanwhile) */
    std::fprintf(stderr, "[vhs] mode: %s, the %s streams\n", capture_mode_ == CaptureMode::Video ? "Video" : "Photo",
                 capture_mode_ == CaptureMode::Video ? "Video size" : "Photo preview");
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
                recount();
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
    if (!rec_->rec->status().started) return; /* dimmed while the scene starts */
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

    /* the camera microphone (brief 5.4): matched by USB ancestry, hw: only; opened while the stream switches */
    const std::int64_t mic_t0 = mono_ms();
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

    rs->mic_ms = static_cast<int>(mono_ms() - mic_t0);
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
    rs->gen = cam.gen;
    rs->from = cam.mode;
    rs->pressed_ms = now_ms;
    const std::string split = rc.split_bytes ? std::to_string(st.split_mib) + " MiB" : std::string("off");
    std::fprintf(stderr, "[vhs] recording: REC %s, %s, folder %s, split %s, floor %d MB, ring %d s\n", mode_text(rs->mode).c_str(),
                 rs->mic.empty() ? "no sound" : ("sound " + rs->mic).c_str(), folder_.c_str(), split.c_str(), st.min_free_mb,
                 rc.ring_seconds);
    pipe_->capture().set_tap(rs.get(), rs->mode); /* frames after the warm-up of the Video size */
    rec_ = std::move(rs);
    send_dim(true); /* brief 10: the screen stays on while recording or playing */
}

void Controller::stop_recording(RecStop why, bool wait)
{
    if (!rec_) return;
    if (!rec_->stopping) {
        rec_->stopping = true;
        pipe_->capture().clear_tap(); /* no frame after this */
        rec_->audio_ended = true;     /* our stop, not a lost microphone: nothing to log about it */
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
    recount();
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
    send_dim(false);
}

void Controller::log_rec_start(std::int64_t now_ms, const RecorderStatus &st)
{
    /* one line per REC: where the time from the press to the first written frame went (M4 item 2) */
    auto ms = [](int v) { return v < 0 ? std::string("-") : std::to_string(v) + " ms"; };
    const StreamTiming t = pipe_->capture().stream_timing();
    std::string sw;
    if (t.gen > rec_->gen) {
        sw = "switch " + std::to_string(rec_->from.w) + "x" + std::to_string(rec_->from.h) + " -> " +
             std::to_string(rec_->mode.w) + "x" + std::to_string(rec_->mode.h) + (t.in_place ? " in place" : " by reopening") +
             ": stream close " + ms(t.close_ms) + ", reopen " + ms(t.open_ms) + ", mode set " + ms(t.set_ms) +
             ", controls " + ms(t.controls_ms) + ", buffers + STREAMON " + ms(t.bufs_ms) + ", first camera frame " +
             ms(t.first_ms) + ", warm-up " + std::to_string(kWarmupFrames) + " frames " + ms(t.warm_ms);
    } else {
        sw = "no switch (the Video size was streaming and warm)";
    }
    std::fprintf(stderr, "[vhs] rec-start: %lld ms from REC to the first written frame; %s; mic open %s and writer open %s "
                         "(both while the stream starts); first frame written %lld ms\n",
                 static_cast<long long>(now_ms - rec_->pressed_ms), sw.c_str(), ms(rec_->mic_ms).c_str(), ms(st.open_ms).c_str(),
                 static_cast<long long>(now_ms - rec_->pressed_ms));
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
            log_rec_start(now_ms, st);
        }
        if (rec_->stopping && rec_->rec->status().finished) {
            end_recording();
        } else {
            s.recording = true;
            s.rec_started = st.started;
            s.rec_starting = !st.started && !rec_->stopping;
            s.rec_since_ms = rec_->pressed_ms;
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

/* ------------------------------------------------------------------------------------------------ the scenes */

void Controller::startup_repair()
{
    /* brief 6.3: every open journal repaired on the scenes worker at start; FFmpeg is loaded only when one is open
     * (brief 3.3 rule 4) */
    const std::string folder = folder_;
    worker_.post([this, folder]() -> std::function<void()> {
        const int open = open_journal_count(posix_file_ops(), folder);
        int repaired = 0, damaged = 0;
        if (open > 0) {
            std::string why;
            const AvLibs *av = cfg_.av ? cfg_.av : real_av_libs(&why);
            if (!av) {
                std::fprintf(stderr, "[vhs] repair: %d open journal(s), no FFmpeg: %s\n", open, why.c_str());
            } else {
                for (const RepairItem &it : repair_open_journals(posix_file_ops(), *av, folder)) {
                    repaired += it.repaired;
                    damaged += it.damaged;
                }
                std::fprintf(stderr, "[vhs] repair at start: %d open journal(s): %d part(s) repaired, %d damaged\n", open,
                             repaired, damaged);
            }
        }
        int movies = 0, photos = 0;
        count_scenes(posix_file_ops(), folder, movies, photos);
        return [this, movies, photos] { scene_count_ = movies + photos; };
    });
}

void Controller::recount()
{
    const std::string folder = folder_;
    worker_.post([this, folder]() -> std::function<void()> {
        int movies = 0, photos = 0;
        count_scenes(posix_file_ops(), folder, movies, photos);
        return [this, movies, photos] { scene_count_ = movies + photos; };
    });
}

void Controller::scenes_open(int thumb_w, int thumb_h, bool short_form)
{
    scenes_.open = true;
    thumb_w_ = std::max(8, thumb_w);
    thumb_h_ = std::max(8, thumb_h);
    short_form_ = short_form;
    if (all_rows_.empty()) scenes_.loading = true;
    scenes_scan();
}

void Controller::scenes_close()
{
    scenes_.open = false;
    scan_id_->fetch_add(1); /* thumbnails still queued are skipped */
}

void Controller::scenes_set_filter(SceneFilter f)
{
    if (scenes_.filter == f) return;
    scenes_.filter = f;
    scenes_filter_rows();
}

void Controller::scenes_filter_rows()
{
    scenes_.rows.clear();
    scenes_.movies = scenes_.photos = 0;
    for (const SceneRow &r : all_rows_) {
        const bool photo = r.e.kind == SceneKind::Photo;
        (photo ? scenes_.photos : scenes_.movies) += 1;
        if (scenes_.filter == SceneFilter::All || (scenes_.filter == SceneFilter::Photos) == photo) scenes_.rows.push_back(r);
    }
    ++scenes_.gen;
}

void Controller::scenes_scan()
{
    const std::uint64_t id = scan_id_->fetch_add(1) + 1;
    const std::string folder = folder_, cache = cache_dir_, prefix = store_.get().prefix;
    const bool short_form = short_form_, utc = cfg_.utc;
    const AvLibs *forced = cfg_.av;
    worker_.post([this, id, folder, cache, prefix, short_form, utc, forced]() -> std::function<void()> {
        FileOps &ops = posix_file_ops();
        auto rows = std::make_shared<std::vector<SceneRow>>();
        MetaCache mc;
        mc.load(ops, cache + "/index.json");
        const AvLibs *av = nullptr;
        bool av_tried = false;
        for (SceneEntry &e : list_scenes(ops, folder)) {
            if (!mc.get(e, e.meta)) {
                if (e.kind == SceneKind::Photo) {
                    e.meta = probe_photo(ops, e.path);
                } else {
                    if (!av_tried) { /* FFmpeg at the first scan that needs it (brief 3.3 rule 4) */
                        av_tried = true;
                        std::string why;
                        av = forced ? forced : real_av_libs(&why);
                        if (!av) std::fprintf(stderr, "[vhs] scenes: no FFmpeg (%s): movies listed without their details\n", why.c_str());
                    }
                    if (av) e.meta = probe_movie(*av, e.path);
                }
                mc.put(e, e.meta);
            }
            SceneRow r;
            r.label = scene_label(e, short_form, prefix);
            r.meta = scene_meta_text(e, short_form, utc);
            r.date = short_form ? std::string() : scene_date_text(e.mtime, utc);
            r.e = std::move(e);
            rows->push_back(std::move(r));
        }
        mc.save(ops);
        std::fprintf(stderr, "[vhs] scenes: %zu file(s) listed (%zu in the metadata cache)\n", rows->size(), mc.size());
        return [this, id, rows] {
            if (id != scan_id_->load()) return;
            /* thumbnails already made stay with their rows */
            std::map<std::string, std::shared_ptr<Picture>> had;
            for (const SceneRow &r : all_rows_)
                if (r.thumb) had[r.e.name + "|" + std::to_string(r.e.size) + "|" + std::to_string(r.e.mtime)] = r.thumb;
            all_rows_ = *rows;
            for (SceneRow &r : all_rows_) {
                const auto it = had.find(r.e.name + "|" + std::to_string(r.e.size) + "|" + std::to_string(r.e.mtime));
                if (it != had.end()) r.thumb = it->second;
            }
            scenes_.loading = false;
            scene_count_ = static_cast<int>(all_rows_.size());
            scenes_filter_rows();
            scenes_thumbs(id);
        };
    });
}

void Controller::scenes_thumbs(std::uint64_t id)
{
    /* one job per missing thumbnail, newest first: the rows fill in as they arrive (brief 9.1) */
    const std::string cache = cache_dir_;
    const int tw = thumb_w_, th = thumb_h_;
    const PixFmt fmt = cfg_.fmt;
    const AvLibs *forced = cfg_.av;
    auto ids = scan_id_;
    for (const SceneRow &r : all_rows_) {
        if (r.thumb) continue;
        const SceneEntry e = r.e;
        worker_.post([this, id, ids, e, cache, tw, th, fmt, forced]() -> std::function<void()> {
            if (id != ids->load()) return nullptr; /* a newer listing, or the page closed */
            auto pic = std::make_shared<Picture>();
            const std::string path = thumb_path(cache, e);
            bool ok = thumb_load(path, tw, th, fmt, *pic);
            if (!ok) {
                const AvLibs *av = nullptr;
                if (e.kind == SceneKind::Movie) {
                    std::string why;
                    av = forced ? forced : real_av_libs(&why);
                }
                std::string why;
                ok = make_thumb(posix_file_ops(), av, e, tw, th, fmt, *pic, &why);
                if (ok) thumb_save(posix_file_ops(), path, *pic);
                else std::fprintf(stderr, "[vhs] scenes: %s: no thumbnail (%s)\n", e.name.c_str(), why.c_str());
            }
            if (!ok) return nullptr;
            return [this, id, e, pic] {
                if (id != scan_id_->load()) return;
                for (SceneRow &r : all_rows_)
                    if (r.e.name == e.name && r.e.mtime == e.mtime) r.thumb = pic;
                scenes_filter_rows();
            };
        });
    }
}

void Controller::scene_rename(const std::string &name, const std::string &prefix)
{
    const std::string folder = folder_, cache = cache_dir_;
    worker_.post([this, folder, cache, name, prefix]() -> std::function<void()> {
        std::string to, why;
        const int r = rename_scene(posix_file_ops(), folder, name, prefix, to, why);
        std::fprintf(stderr, "[vhs] scenes: rename %s -> %s: %s\n", name.c_str(), r == 0 ? to.c_str() : prefix.c_str(),
                     r == 0 ? "done" : why.c_str());
        if (r == 0 && to != name) thumb_remove(posix_file_ops(), cache, name);
        return [this, r, why] {
            if (r != 0) notices_.push(Notice{std::string(kNoticeNotRenamed) + why + ".", false});
            if (scenes_.open) scenes_scan();
        };
    });
}

void Controller::scene_toggle_lock(const std::string &name)
{
    const std::string folder = folder_;
    worker_.post([this, folder, name]() -> std::function<void()> {
        const bool now = read_locks(posix_file_ops(), folder).count(name) == 0;
        const int r = set_locked(posix_file_ops(), folder, name, now);
        std::fprintf(stderr, "[vhs] scenes: %s %s: %s\n", now ? "lock" : "unlock", name.c_str(), r == 0 ? "done" : std::strerror(-r));
        return [this] {
            if (scenes_.open) scenes_scan();
        };
    });
}

void Controller::scene_delete(const std::string &name)
{
    const std::string folder = folder_, cache = cache_dir_;
    worker_.post([this, folder, cache, name]() -> std::function<void()> {
        std::string why;
        const int r = delete_scene(posix_file_ops(), folder, name, why);
        if (r == 0) {
            thumb_remove(posix_file_ops(), cache, name);
            MetaCache mc;
            mc.load(posix_file_ops(), cache + "/index.json");
            mc.erase(name);
            mc.save(posix_file_ops());
        }
        std::fprintf(stderr, "[vhs] scenes: delete %s: %s\n", name.c_str(), r == 0 ? "done" : why.c_str());
        return [this, r, why] {
            if (r != 0) notices_.push(Notice{std::string(kNoticeNotDeleted) + why + ".", false});
            if (scenes_.open) scenes_scan();
            else recount();
        };
    });
}

/* ------------------------------------------------------------------------------------------------ playback */

std::int64_t Controller::play_clock_ms() const
{
    /* the engine's heard position while it plays the sound; else the monotonic clock (no sound, or the output failed) */
    /* G6: computed at every call: the monotonic clock, steered toward the sound by play_step(). Before, it returned the
     * sound's position as play_step() last read it (the 250 ms status timer): it stood still, then jumped 250 ms */
    std::lock_guard<std::mutex> lock(play_m_);
    return mono_clock_.at(mono_ms());
}

bool Controller::play_running() const
{
    std::lock_guard<std::mutex> lock(play_m_);
    return playing_;
}

void Controller::send_dim(bool deny)
{
    if (dim_denied_ == deny) return;
    dim_denied_ = deny;
    if (cfg_.fake) { /* the harness has no launcher: the request is logged only */
        std::fprintf(stderr, "[vhs] launcher: dim %s (fake: not sent)\n", deny ? "deny" : "allow");
        return;
    }
    launcher_dim(deny);
}

std::unique_ptr<AudioPlayer> Controller::make_audio_player(const std::string &output)
{
    if (cfg_.audio_factory) return cfg_.audio_factory(output);
    if (cfg_.fake) { /* the headless scenes: Vinyl's fake output (VHS_FAKE_OUTPUT: ok | lost | none | instant), never a device */
        const char *m = std::getenv("VHS_FAKE_OUTPUT");
        return std::make_unique<AudioPlayer>(output, fake_audio_deps(fake_output_options(m && m[0] ? m : "ok")));
    }
    return std::make_unique<AudioPlayer>(output);
}

void Controller::launcher_dim(bool deny)
{
    /* brief 10: "dim deny" while playing or recording, "dim allow" on stop, only where board.state's requests lists dim
     * (Vinyl's launcher link: the requests go out on its own thread) */
    if (!launcher_status_) launcher_status_ = std::make_unique<LauncherStatusReader>();
    launcher_status_->poll(static_cast<std::uint64_t>(mono_ms()));
    if (!launcher_status_->status().has_request("dim")) {
        std::fprintf(stderr, "[vhs] launcher: dim %s not sent (this board's launcher does not take dim)\n", deny ? "deny" : "allow");
        return;
    }
    if (!launcher_req_) launcher_req_ = std::make_unique<LauncherRequests>();
    launcher_req_->send(deny ? "dim deny" : "dim allow");
    std::fprintf(stderr, "[vhs] launcher: dim %s\n", deny ? "deny" : "allow");
}

void Controller::playback_open(const std::string &name, int box_w, int box_h)
{
    playback_close();
    play_view_ = PlaybackView();
    play_view_.open = true;
    play_view_.playing = true; /* it plays at once */
    play_view_.name = name;
    const std::size_t dot = name.find_last_of('.');
    play_view_.title = dot == std::string::npos ? name : name.substr(0, dot);
    std::string why;
    const AvLibs *av = cfg_.av ? cfg_.av : real_av_libs(&why);
    if (!av) {
        play_view_.error = kNoticeCannotPlay;
        notices_.push(Notice{kNoticeCannotPlay, false});
        std::fprintf(stderr, "[vhs] playback: no FFmpeg: %s\n", why.c_str());
        return;
    }
    const std::string path = folder_ + "/" + name;
    movie_ = std::make_unique<Movie>(*av, [this] { return play_clock_ms(); }, [this] { return play_running(); });
    if (!movie_->open(path, box_w, box_h, cfg_.fmt, &why)) {
        play_view_.error = why;
        notices_.push(Notice{kNoticeCannotPlay, false});
        std::fprintf(stderr, "[vhs] playback: %s: %s\n", name.c_str(), why.c_str());
        movie_.reset();
        return;
    }
    audio_ = make_audio_player(store_.get().output);
    /* the engine opens the file on its own thread (FFmpeg, the output): the picture starts on the monotonic clock and
     * follows the heard position from the moment the sound plays (play_step) */
    audio_ready_ = false;
    if (audio_) {
        AudioPlayer *ap = audio_.get();
        audio_open_ = std::async(std::launch::async, [ap, path] { return ap->open(path, 0, false); });
    }
    {
        std::lock_guard<std::mutex> lock(play_m_);
        audio_clock_ = AudioClock();
        mono_clock_ = PlayClock();
        mono_clock_.set(0, mono_ms()); /* held at 0 until the first picture is decoded (G6: no frame is late at the start) */
        clock_held_ = true;
        wait_gen_ = 0;
        playing_ = true;
    }
    clock_mode_ = "monotonic (the sound not open yet)";
    sound_trusted_ = sound_distrust_ = rate_ok_ = steered_once_ = false;
    rate_bad_ = clock_jumps_ = 0;
    rw_audio0_ = -1;
    stats_at_ = mono_ms();
    shown_pts_ = -1;
    shown_n_ = stats_shown0_ = stats_late0_ = stats_unshown0_ = 0;
    const MovieStatus ms = movie_->status();
    std::fprintf(stderr, "[vhs] playback: %s: %s %dx%d, %s, %s\n", name.c_str(), ms.codec.c_str(), ms.w, ms.h,
                 duration_text(ms.duration_ms).c_str(), ms.frame_ms > 0 ? (std::to_string(static_cast<int>(1000.0 / ms.frame_ms + 0.5)) + " fps").c_str() : "?");
    play_t0_us_ = mono_us();
    play_cpu0_ = process_cpu_ms();
    send_dim(true);
}

void Controller::playback_close()
{
    if (movie_) {
        const MovieStatus ms = movie_->status();
        const double wall = static_cast<double>(mono_us() - play_t0_us_) / 1e6;
        const double cpu = static_cast<double>(process_cpu_ms() - play_cpu0_) / 1000.0;
        std::fprintf(stderr, "[vhs] playback: closed after %.1f s: %llu frames decoded (%.1f ms each), %llu shown, %llu dropped late, %llu not shown, %.2f core; clock %s, %d jump(s)\n",
                     wall, static_cast<unsigned long long>(ms.decoded), ms.decode_ms, static_cast<unsigned long long>(shown_n_),
                     static_cast<unsigned long long>(ms.dropped), static_cast<unsigned long long>(ms.shown_dropped), wall > 0 ? cpu / wall : 0.0,
                     clock_mode_.c_str(), clock_jumps_);
        last_play_ = ms;
    }
    movie_.reset();
    if (audio_open_.valid()) audio_open_.wait(); /* the engine's open returns within its timeout */
    audio_ready_ = false;
    if (audio_) audio_->shutdown();
    audio_.reset();
    {
        std::lock_guard<std::mutex> lock(play_m_);
        playing_ = false;
        audio_clock_ = AudioClock();
    }
    play_pic_.reset();
    play_view_ = PlaybackView();
    send_dim(false);
}

void Controller::playback_toggle()
{
    if (!movie_) return;
    const std::int64_t cur = play_clock_ms();
    const std::int64_t now = mono_ms();
    bool play;
    {
        std::lock_guard<std::mutex> lock(play_m_);
        play = !playing_;
        /* the monotonic clock takes the position heard now (pausing), or starts from it (playing); at the end Play
         * starts again from the beginning */
        mono_clock_.set(play && play_view_.ended ? 0 : cur, now);
        playing_ = play;
        if (play && !clock_held_) mono_clock_.resume(now);
        else mono_clock_.pause(now);
    }
    rw_audio0_ = -1; /* the sound's rate is checked again before it steers */
    rate_ok_ = false;
    if (play && play_view_.ended) {
        play_view_.ended = false;
        play_hold(movie_->seek(0));
        if (audio_ && audio_ready_.load()) audio_->seek_ms(0);
    }
    if (audio_ && audio_ready_.load()) {
        if (play) audio_->play();
        else audio_->pause();
    }
    send_dim(play);
}

bool Controller::playback_pause()
{
    if (!movie_ || !play_running()) return false;
    playback_toggle();
    play_view_.playing = play_running(); /* the view at once: no step() runs while hidden */
    return true;
}

void Controller::playback_seek_to(std::int64_t ms)
{
    if (!movie_) return;
    const std::int64_t dur = std::max<std::int64_t>(0, movie_->status().duration_ms);
    ms = std::max<std::int64_t>(0, dur > 0 ? std::min(ms, dur - 1) : ms);
    {
        std::lock_guard<std::mutex> lock(play_m_);
        mono_clock_.set(ms, mono_ms());
        audio_clock_.heard_ms = ms; /* until the engine reports again */
    }
    play_view_.ended = false;
    play_hold(movie_->seek(ms));
    if (audio_ && audio_ready_.load()) audio_->seek_ms(ms);
    seek_shown_ms_ = ms;
}

void Controller::playback_seek_by(std::int64_t delta_ms)
{
    playback_seek_to(play_clock_ms() + delta_ms);
}

void Controller::playback_volume(int dir)
{
    if (audio_) audio_->volume_step(dir);
}

std::shared_ptr<Picture> Controller::playback_picture()
{
    if (!movie_) return nullptr;
    play_release_hold();
    std::int64_t pts = -1;
    auto pic = movie_->presenter().pick(play_clock_ms(), &pts);
    if (pic) {
        play_pic_ = pic;
        shown_pts_ = pts;
        ++shown_n_;
    }
    return pic;
}

void Controller::play_hold(std::uint64_t gen)
{
    std::lock_guard<std::mutex> lock(play_m_);
    mono_clock_.pause(mono_ms()); /* stands at the seek's target until its first picture is there */
    clock_held_ = true;
    wait_gen_ = gen;
    rw_audio0_ = -1;
    rate_ok_ = false;
}

void Controller::play_release_hold()
{
    if (!movie_) return;
    std::lock_guard<std::mutex> lock(play_m_);
    if (!clock_held_ || !movie_->picture_ready(wait_gen_)) return;
    clock_held_ = false;
    if (playing_) mono_clock_.resume(mono_ms());
}

/* G6: the picture's clock follows the sound only when the sound is really heard and its position moves at real time:
 * not on the sound server's null sink (Dummy Output), not while a seek is pending, and only after a 1 s window found
 * it within 15 % of the monotonic clock (three bad windows in a row: never again for this movie) */
void Controller::play_steer(const AudioClock &c, std::int64_t now)
{
    const bool usable = c.open && c.has_audio && c.error.empty() && !c.output_lost;
    std::string mode;
    if (!usable) mode = "monotonic (no sound)";
    else if (c.dummy) mode = "monotonic (" + c.output_label + ": nothing is heard)";
    else if (sound_distrust_) mode = clock_mode_;
    else if (steered_once_) mode = "steered to the sound";
    else mode = "monotonic (the sound's clock is checked first)";
    if (mode != clock_mode_) {
        clock_mode_ = mode;
        std::fprintf(stderr, "[vhs] playback: the picture's clock: %s\n", mode.c_str());
    }
    sound_trusted_ = usable && !c.dummy && !sound_distrust_;
    bool held, playing;
    {
        std::lock_guard<std::mutex> lock(play_m_);
        held = clock_held_;
        playing = playing_;
    }
    if (!sound_trusted_ || held || !playing || !c.playing || c.seeking) {
        rw_audio0_ = -1;
        return;
    }
    if (rw_audio0_ < 0) {
        rw_audio0_ = c.heard_ms;
        rw_mono0_ = now;
    } else if (now - rw_mono0_ >= 1000) {
        const double r = static_cast<double>(c.heard_ms - rw_audio0_) / static_cast<double>(now - rw_mono0_);
        rate_ok_ = r >= 0.85 && r <= 1.15;
        if (rate_ok_ && !steered_once_) {
            steered_once_ = true;
            clock_mode_ = "steered to the sound";
            std::fprintf(stderr, "[vhs] playback: the picture's clock: steered to the sound (its position moves at %.3fx)\n", r);
        }
        rate_bad_ = rate_ok_ ? 0 : rate_bad_ + 1;
        if (rate_bad_ >= 3) {
            sound_distrust_ = true;
            sound_trusted_ = false;
            char buf[96];
            std::snprintf(buf, sizeof buf, "monotonic (the sound's position moves at %.2fx real time)", r);
            clock_mode_ = buf;
            std::fprintf(stderr, "[vhs] playback: the picture's clock: %s\n", buf);
            return;
        }
        rw_audio0_ = c.heard_ms;
        rw_mono0_ = now;
    }
    if (!rate_ok_) return;
    std::lock_guard<std::mutex> lock(play_m_);
    if (mono_clock_.steer(c.heard_ms, now)) ++clock_jumps_;
}

/* G6: one line a second while playing, for the board runs */
void Controller::play_stats(const MovieStatus &ms, std::int64_t now)
{
    if (now - stats_at_ < 1000) return;
    stats_at_ = now;
    if (!play_running()) return;
    std::int64_t clock;
    double rate;
    {
        std::lock_guard<std::mutex> lock(play_m_);
        clock = mono_clock_.at(now);
        rate = mono_clock_.rate();
    }
    AudioClock c;
    {
        std::lock_guard<std::mutex> lock(play_m_);
        c = audio_clock_;
    }
    char snd[96] = "no sound";
    if (audio_ready_.load() && c.has_audio)
        std::snprintf(snd, sizeof snd, "sound %lld, delay %lld ms", static_cast<long long>(c.heard_ms), static_cast<long long>(c.delay_ms));
    std::fprintf(stderr, "[vhs] playback: clock %lld ms (%s; %s, rate %.3f), shown pts %lld; last second: %llu shown, %llu not shown, %llu dropped late (before decoding); decode %.1f ms, queue %zu\n",
                 static_cast<long long>(clock), snd, clock_mode_.c_str(), rate, static_cast<long long>(shown_pts_),
                 static_cast<unsigned long long>(shown_n_ - stats_shown0_), static_cast<unsigned long long>(ms.shown_dropped - stats_unshown0_),
                 static_cast<unsigned long long>(ms.dropped - stats_late0_), ms.decode_ms, movie_->presenter().size());
    stats_shown0_ = shown_n_;
    stats_unshown0_ = ms.shown_dropped;
    stats_late0_ = ms.dropped;
}

void Controller::play_step()
{
    if (!movie_) return;
    if (audio_ && !audio_ready_.load() && audio_open_.valid() &&
        audio_open_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool ok = audio_open_.get();
        const AudioClock c0 = audio_->clock();
        if (ok && c0.has_audio && c0.error.empty()) {
            /* the sound joins where the picture is */
            audio_->seek_ms(play_clock_ms());
            if (play_running()) audio_->play();
            audio_ready_ = true;
            std::fprintf(stderr, "[vhs] playback: sound on %s\n", c0.output_label.c_str());
        } else {
            clock_mode_ = "monotonic (no sound)";
            std::fprintf(stderr, "[vhs] playback: no sound (%s): the picture keeps the monotonic clock\n",
                         !ok ? (c0.error.empty() ? "the engine did not open it" : c0.error.c_str()) : (c0.has_audio ? c0.error.c_str() : "no audio stream"));
        }
    }
    if (audio_ && audio_ready_.load()) {
        const AudioClock c = audio_->clock();
        {
            std::lock_guard<std::mutex> lock(play_m_);
            audio_clock_ = c;
        }
        play_steer(c, mono_ms());
        for (AudioPlayer::Key k = audio_->poll_key(); k != AudioPlayer::Key::None; k = audio_->poll_key()) {
            if (k == AudioPlayer::Key::PlayPause) playback_toggle();
            else if (k == AudioPlayer::Key::Play && !play_running()) playback_toggle();
            else if ((k == AudioPlayer::Key::Pause || k == AudioPlayer::Key::Stop) && play_running()) playback_toggle();
        }
    }
    play_release_hold();
    const MovieStatus ms = movie_->status();
    const std::int64_t pos = play_clock_ms();
    play_stats(ms, mono_ms());
    if (movie_->take_drops_notice()) { /* brief 10: after 2 s of drops (again only after the toast's 4 s: DropWindow) */
        notices_.push(Notice{kNoticeDrops, false});
        std::int64_t mono_pos;
        {
            std::lock_guard<std::mutex> lock(play_m_);
            mono_pos = mono_clock_.at(mono_ms());
        }
        std::fprintf(stderr, "[vhs] playback: frames late for 2 s at %.1f s (clock %s, monotonic %.1f s): %llu dropped late, %llu decoded (%.1f ms each), %llu not shown\n",
                     static_cast<double>(pos) / 1000.0, clock_mode_.c_str(),
                     static_cast<double>(mono_pos) / 1000.0, static_cast<unsigned long long>(ms.dropped),
                     static_cast<unsigned long long>(ms.decoded), ms.decode_ms, static_cast<unsigned long long>(ms.shown_dropped));
    }
    if (!ms.error.empty() && play_view_.error.empty()) {
        play_view_.error = ms.error;
        notices_.push(Notice{kNoticeCannotPlay, false});
    }
    /* the end: the last frame decoded and the clock past it: paused there */
    bool sound_ended;
    {
        std::lock_guard<std::mutex> lock(play_m_);
        sound_ended = audio_ready_.load() && audio_clock_.ended;
    }
    if (ms.ended && play_running() && ms.duration_ms > 0 &&
        (pos >= ms.duration_ms - static_cast<std::int64_t>(ms.frame_ms) || sound_ended)) {
        std::lock_guard<std::mutex> lock(play_m_);
        playing_ = false;
        mono_clock_.pause(mono_ms());
        play_view_.ended = true;
    }
    if (play_view_.ended && audio_ && audio_ready_.load()) audio_->pause();
    play_view_.playing = play_running();
    play_view_.duration_ms = ms.duration_ms;
    play_view_.pos_ms = std::max<std::int64_t>(0, ms.duration_ms > 0 ? std::min(pos, ms.duration_ms) : pos);
    play_view_.codec = ms.codec;
    play_view_.volume = audio_ ? audio_->volume_percent() : -1;
    play_view_.output = audio_ ? audio_->volume_label() : std::string();
    play_view_.volume_note = audio_ ? audio_->volume_note() : std::string();
    {
        std::lock_guard<std::mutex> lock(play_m_);
        play_view_.has_audio = audio_clock_.has_audio && audio_clock_.error.empty();
    }
    if (!play_view_.playing) send_dim(false);
}

/* ------------------------------------------------------------------------------------------------ copy to a drive */

void Controller::copy_request(const std::string &name)
{
    if (!copy_) {
        /* the scenes: the demo drives (VHS_FAKE_DRIVES) in a folder beside the fake camera's, and a slow copy
         * (VHS_FAKE_COPY_KBPS) so its dialog shows for a few seconds */
        FileOps *ops = &posix_file_ops();
        if (cfg_.fake) {
            const char *kbps = std::getenv("VHS_FAKE_COPY_KBPS");
            if (kbps && std::atol(kbps) > 0) {
                copy_ops_ = throttled_file_ops(posix_file_ops(), static_cast<std::uint64_t>(std::atol(kbps)) * 1000);
                ops = copy_ops_.get();
            }
        }
        const std::string demo_root = cfg_.fake ? dir_name(folder_) + "/vhs-drives" : std::string();
        copy_ = std::make_unique<CopyFlow>([demo_root] { return make_drive_backend(demo_root); },
                                           [this](const Notice &n) { notices_.push(n); }, *ops);
    }
    copy_->request(folder_ + "/" + name);
}

void Controller::copy_choose(int index)
{
    if (copy_) copy_->choose(index);
}

void Controller::copy_dismiss()
{
    if (copy_) copy_->dismiss();
}

void Controller::copy_cancel()
{
    if (copy_) copy_->cancel();
}

const CopyView &Controller::copy_view() const
{
    static const CopyView kIdle;
    return copy_ ? copy_->view() : kIdle;
}

/* ------------------------------------------------------------------------------------------------ settings */

int Controller::settings_apply(const Settings &next)
{
    Settings s = next;
    if (!valid_prefix(s.prefix)) s.prefix = store_.get().prefix;
    s.self_timer_s = clamp_self_timer(s.self_timer_s);
    const bool folder_changed = s.folder != store_.get().folder;
    const int r = store_.set(s);
    {
        std::lock_guard<std::mutex> lock(sel_m_);
        sel_settings_ = store_.get(); /* the chooser's copy: the stream rows apply at the next open */
    }
    if (folder_changed && !cfg_.fake) {
        folder_ = expand_home(store_.get().folder);
        ensure_folder(posix_file_ops(), folder_);
        all_rows_.clear();
        recount();
    }
    std::fprintf(stderr, "[vhs] settings: %s (folder %s, prefix %s, photo %s, photo size %s, video size %s, timer %d s, split %s, output \"%s\")\n",
                 r == 0 ? "saved" : (r == 1 ? "unchanged" : "not saved"), store_.get().folder.c_str(), store_.get().prefix.c_str(),
                 store_.get().have_photo_mode ? photo_mode_name(store_.get().photo_mode) : "(SoC default)",
                 store_.get().photo_size.valid() ? size_text(store_.get().photo_size).c_str() : "(largest)",
                 store_.get().video_size.valid() ? size_text(store_.get().video_size).c_str() : "(1280x720)", store_.get().self_timer_s,
                 store_.get().split ? "on" : "off", store_.get().output.c_str());
    return r;
}

void Controller::settings_open()
{
    if (!settings_audio_) settings_audio_ = make_audio_player(store_.get().output); /* lists the outputs (Vinyl's chooser) */
}

void Controller::settings_close()
{
    if (settings_audio_) settings_audio_->shutdown();
    settings_audio_.reset();
}

std::vector<AudioOutputRow> Controller::output_rows() const
{
    return settings_audio_ ? settings_audio_->output_rows() : std::vector<AudioOutputRow>();
}

void Controller::set_output(const std::string &key)
{
    if (settings_audio_) settings_audio_->set_output(key);
    Settings s = store_.get();
    s.output = key == "auto" ? std::string() : key; /* "" = Automatic */
    settings_apply(s);
}

/* ------------------------------------------------------------------------------------------------ the review */

void Controller::review_open(int box_w, int box_h, const std::string &start)
{
    review_start_ = start;
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
            if (!review_start_.empty()) { /* opened from the Scenes page: at that photo */
                for (std::size_t i = 0; i < review_list_.size(); ++i)
                    if (review_list_[i].name == review_start_) review_.index = static_cast<int>(i);
                review_start_.clear();
            }
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
    const std::string name = review_list_[static_cast<std::size_t>(review_.index)].name;
    const std::string folder = folder_, cache = cache_dir_;
    const std::uint64_t req = ++review_req_;
    review_.loading = true;
    worker_.post([this, folder, cache, name, req]() -> std::function<void()> {
        std::string why;
        const int r = delete_scene(posix_file_ops(), folder, name, why); /* a locked photo is refused */
        if (r == 0) thumb_remove(posix_file_ops(), cache, name);
        std::fprintf(stderr, "[vhs] review: delete %s: %s\n", name.c_str(), r == 0 ? "done" : why.c_str());
        return [this, req, r, why] {
            if (r != 0) notices_.push(Notice{std::string(kNoticeNotDeleted) + why + ".", false});
            recount();
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
