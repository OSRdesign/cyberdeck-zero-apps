/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the app model (see controller.hpp).
 */
#include "controller.hpp"

#include "modes/codec_caps.hpp"
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
    s.notice = notices_.current(now_ms);
    s.countdown = timer_.active() ? std::max(1, timer_.shown(now_ms)) : 0;
    s.timer_total = timer_.active() ? timer_.total() : 0;
    s.flash = flash_;
    s.photo_busy = photo_ != PhotoStep::Idle;
    s.photos_saved = photos_saved_;
    s.last_photo = last_photo_;
    return s;
}

void Controller::toggle_capture_mode()
{
    if (timer_.active()) return; /* dimmed during the countdown */
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
    if (capture_mode_ != CaptureMode::Photo) return; /* a video comes with M3 */
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
    if (photo_needs_switch(s)) {
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
