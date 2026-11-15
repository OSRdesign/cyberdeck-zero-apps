/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the app model (brief 3.1 / 3.2). It owns every thread (the live-view pipeline: capture, decode, sampler; the
 * scenes worker), the settings, the board facts and the toast line; the UI holds one Controller and talks to it on the
 * LVGL thread: commands in, a snapshot out (polled every 250 ms) and the pictures (taken at the display refresh).
 * LVGL-free.
 *
 * M1 (live view): camera discovery and identity (camera_id remembered), the mode per board (select.hpp, settings and the
 * SoC class), the states of section 5 with their notices ("Camera unplugged." at a loss; "Camera busy..." and "No access
 * ..." standing while they last; "Power too weak for the camera." at most once a minute while the camera is open), the
 * Photo / Video mode of the Capture page, and the fake camera's script for the scenes.
 *
 * M2 (photo, brief section 7, 4.1 rule 3, 6.7): the shutter takes the next good frame (Instant) or switches the stream
 * to the Photo size and back (Light: hold, 15 frames, one kept, release); the self-timer counts down first (in Light the
 * switch happens at its start, so zero acts at once) and can be cancelled; the frame goes to the scenes worker, which
 * writes photo_<NNN>_<when>.jpg (EXIF, Annex K tables when missing, atomic); one flash marks the photo. A camera without
 * MJPEG refuses photos. The photo review (minimal, brief 9.5) lists the folder's photos newest first, decodes one at a
 * time on the worker, and deletes one after the UI has asked.
 */
#pragma once

#include "app/notice_queue.hpp"
#include "camera/capture.hpp"
#include "camera/fake_camera.hpp"
#include "live/pipeline.hpp"
#include "modes/select.hpp"
#include "modes/soc.hpp"
#include "power/undervolt.hpp"
#include "scenes/photos.hpp"
#include "scenes/worker.hpp"
#include "settings/settings.hpp"
#include "timer/self_timer.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vhs {

enum class CaptureMode { Photo, Video };

struct ControllerConfig {
    bool fake = false;
    FakeOptions fake_opt;
    std::string fake_script;       /* "unplug@1500,turn@1600,replug@2500,busy@..,free@..,noaccess@..,access@.." (ms
                                      after start): the scenes' events */
    std::string device;            /* --device PATH; "" = discovery */
    std::string dev_root = "/dev";
    std::string sys_root;          /* "" = the real /sys and /proc */
    std::string settings_path;     /* "" = settings_path() */
    std::string settings_overrides;/* the fake camera's "photo_mode=light,self_timer_s=3" (VHS_SET, scenes) */
    std::string folder;            /* "" = Settings > Folder (expanded); the fake camera: a folder of its own */
    bool persist = true;           /* write settings (camera_id); off with the fake camera */
    int clock_synced = -1;         /* -1: systemd-timesyncd's marker; 0 / 1 forced (VHS_CLOCK_SYNCED, tests) */
    bool utc = false;              /* names and EXIF times in UTC (tests) */
    PixFmt fmt = PixFmt::RGB565;
    int box_w = 352, box_h = 198;
    bool fastdct = true;
    int ui_tid = 0;
    int log_every = 10;            /* the stats line every n seconds (VHS_STATS=1: 1) */
    ModeReq mode;                  /* --mode; invalid = the selection's preview */
    int denom = 0;                 /* --scale; 0 = the fit rule */
    bool force_undervolt = false;  /* VHS_FAKE_UNDERVOLT: the scenes' power notice */
    std::string soc_override;      /* VHS_SOC: "bcm2712" ... "other"; "" = detected */
};

struct Snapshot {
    CamState state = CamState::Searching;
    ModeReq mode;              /* streaming now (invalid otherwise) */
    std::string camera;        /* "UGREEN FineCam 1080P CM969" (the USB product, else the card name) */
    bool yuyv = false;         /* the stream is YUYV (a camera without MJPEG) */
    std::string detail;        /* Failed: why (the NoCamera page shows it) */
    bool replug_hint = false;  /* NotBack on a dwc_otg / dwc2 bus (C12) */
    Notice notice;             /* the toast to show ("" = none) */
    CaptureMode capture_mode = CaptureMode::Photo;
    std::uint64_t gen = 0;     /* the capture session */
    int countdown = 0;         /* the self-timer's number (0: no countdown) */
    int timer_total = 0;       /* the self-timer's setting while it counts */
    std::uint64_t flash = 0;   /* +1 at every photo taken (the UI flashes once) */
    bool photo_busy = false;   /* a photo is being taken or saved */
    int photos_saved = 0;      /* in this run */
    std::string last_photo;    /* its file name */
};

/* The photo review's state (brief 9.5). */
struct ReviewView {
    bool open = false;
    bool loading = false;      /* the list or a picture is on its way */
    int index = 0, count = 0;  /* "1 of 14": index + 1 of count */
    std::string name;          /* the file name */
    std::string meta;          /* "1920x1080, 98 KB, Nov 14 2026 18:47" */
    std::uint64_t picture_gen = 0; /* +1 for every new picture */
};

class Controller {
public:
    explicit Controller(ControllerConfig cfg);
    ~Controller();
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    /* The camera opens (start) and is released (stop): the Capture page shown / left, the app shown / hidden. */
    void start();
    void stop();
    bool running() const;

    void set_box(int w, int h);

    /* LVGL thread, every 250 ms (and on demand): advances the notices, the photo, the worker's completions and the
     * fake script; returns the snapshot. */
    Snapshot step(std::int64_t now_ms);

    void toggle_capture_mode();
    CaptureMode capture_mode() const { return capture_mode_; }
    void dismiss_notice(std::int64_t now_ms);
    /* A toast from outside (the scenes' VHS_DEMO_NOTICE, later the recorder). */
    void push_notice(const Notice &n);

    /* The shutter (the button, Space in Photo mode): a photo now, or the self-timer's countdown first. */
    void shutter(std::int64_t now_ms);
    /* A tap on the picture, Cancel, Esc or Stop during the countdown: nothing is taken. False when none ran. */
    bool cancel_countdown();
    bool counting_down() const { return timer_.active(); }

    /* The photo review: open (lists the folder), move, delete the shown photo (after the UI asked), close. */
    void review_open(int box_w, int box_h);
    void review_move(int delta);
    void review_delete();
    void review_close();
    ReviewView review() const;
    /* The review's picture (LVGL thread; valid until the next step()). */
    const Picture *review_picture() const { return review_pic_.get(); }

    Pipeline &pipeline() { return *pipe_; }
    const Settings &settings() const { return store_.get(); }
    const std::string &folder() const { return folder_; }
    SocClass soc() const { return soc_; }
    Selection selection() const;
    FakeCamera *fake() { return fake_.get(); }

private:
    enum class PhotoStep { Idle, Countdown, Waiting, Saving };
    ModeReq choose(const std::vector<CamMode> &listed, const CamIdentity &id);
    void run_script(std::int64_t now_ms);
    bool photo_needs_switch(const Selection &s) const;
    void fire(std::int64_t now_ms);
    void photo_step(std::int64_t now_ms, const CaptureStatus &cam);
    void release_hold();
    bool synced() const;
    void review_list();
    void review_decode();

    ControllerConfig cfg_;
    SocClass soc_ = SocClass::Other;
    std::unique_ptr<FakeCamera> fake_;
    std::unique_ptr<V4l2Api> real_api_;
    std::unique_ptr<CameraFinder> finder_;
    SettingsStore store_;
    std::unique_ptr<Pipeline> pipe_;
    Worker worker_;
    std::string folder_;

    mutable std::mutex sel_m_;
    Selection sel_;
    Settings sel_settings_;  /* the settings the chooser reads (a copy: the capture thread's) */
    int box_w_ = 0, box_h_ = 0;

    NoticeQueue notices_;
    UndervoltNotice undervolt_;
    std::uint64_t losses_seen_ = 0;
    std::uint64_t interval_seen_ = 0;
    CaptureMode capture_mode_ = CaptureMode::Photo;
    std::int64_t started_ms_ = 0;
    struct ScriptStep {
        std::int64_t at_ms;
        std::string action;
        bool done;
    };
    std::vector<ScriptStep> script_;

    /* the photo */
    SelfTimer timer_;
    PhotoStep photo_ = PhotoStep::Idle;
    std::uint64_t photo_id_ = 0;
    std::int64_t photo_t0_ms_ = 0;
    bool held_ = false;
    std::uint64_t flash_ = 0;
    int photos_saved_ = 0;
    std::string last_photo_;

    /* the review */
    ReviewView review_;
    std::vector<PhotoEntry> review_list_;
    std::shared_ptr<Picture> review_pic_;
    int review_box_w_ = 0, review_box_h_ = 0;
    std::uint64_t review_req_ = 0; /* drops stale decodes */
};

/* MemTotal of root/proc/meminfo in MB (board.state's ram_mb), -1 when unknown. */
int ram_mb(const std::string &root = "");

} // namespace vhs
