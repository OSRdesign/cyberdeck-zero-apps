/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the toast line (brief 12.1, canvas board Notices-480x320; Cassette's notice pattern): timed notices shown one
 * after the other, each for show_ms (4 s, Cassette's), a tap dismisses the one shown; a standing notice (Cassette's
 * standing_notice: Busy, NoAccess) shows while no timed one does, until its cause goes (a tap hides it until it changes).
 * Pure (unit-tested). LVGL-free.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace vhs {

/* The canvas texts (Notices-480x320); the last three come with recording (M3) and playback (M5). */
constexpr const char *kNoticeUnplugged = "Camera unplugged.";
constexpr const char *kNoticeBusy = "Camera busy: another program uses it.";
constexpr const char *kNoticeNoAccess = "No access to the camera: add your user to the video group.";
constexpr const char *kNoticePower = "Power too weak for the camera.";
constexpr const char *kNoticeDiskFull = "Disk almost full: recording stopped."; /* red */
constexpr const char *kNoticeNoSound = "No sound from the camera microphone.";
constexpr const char *kNoticeDrops = "This board drops frames at this size.";
/* Not on the canvas: provisional wording (brief 12.4 item 3, the Controller's at the gates). */
constexpr const char *kNoticeNoMjpegPhoto = "Photos need a camera that sends MJPEG.";
constexpr const char *kNoticePhotoFailed = "Photo not saved: "; /* + the reason */

struct Notice {
    std::string text; /* "" = none */
    bool red = false; /* the red toast (#e0553a) */
    bool operator==(const Notice &o) const { return text == o.text && red == o.red; }
};

class NoticeQueue {
public:
    explicit NoticeQueue(std::int64_t show_ms = 4000) : show_ms_(show_ms) {}
    /* Queued behind the one shown; a text already shown or waiting is not queued twice. */
    void push(const Notice &n);
    /* The standing notice ("" = none). */
    void set_standing(const Notice &n);
    /* A tap: the timed notice shown goes (the next one follows); a standing one hides until it changes. */
    void dismiss(std::int64_t now_ms);
    /* The notice to show at now_ms (advances the queue). */
    Notice current(std::int64_t now_ms);
    std::size_t waiting() const { return queue_.size(); }

private:
    std::int64_t show_ms_;
    std::deque<Notice> queue_;
    Notice shown_;
    std::int64_t since_ = 0;
    Notice standing_;
    bool standing_hidden_ = false;
};

} // namespace vhs
