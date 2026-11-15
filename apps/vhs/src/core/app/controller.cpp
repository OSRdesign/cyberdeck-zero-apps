/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the app model (see controller.hpp).
 */
#include "controller.hpp"

#include "modes/codec_caps.hpp"
#include "scenes/fileops.hpp"

#include <cstdio>
#include <cstdlib>
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
    sel_settings_ = store_.get();
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
    const CaptureStatus cam = pipe_->camera();
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
    return s;
}

void Controller::toggle_capture_mode()
{
    capture_mode_ = capture_mode_ == CaptureMode::Photo ? CaptureMode::Video : CaptureMode::Photo;
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
