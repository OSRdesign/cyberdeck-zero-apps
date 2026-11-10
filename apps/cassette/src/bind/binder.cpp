/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding: actions to commands, state to the view model (see binder.hpp).
 */
#include "binder.hpp"

#include "core/takes/naming.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace cassette {
namespace bind {

namespace {

std::string error_text(int r)
{
    if (r == -EBUSY) return "the take is in use";
    if (r == -EEXIST) return "that name is taken";
    if (r == -EINVAL || r == kRenameBadChar) return "use letters, digits, space, - and _";
    if (r == kRenameTooLong) return "up to 24 characters";
    if (r == kRenameEmpty) return "the name is empty";
    if (r == kRenameNotTake) return "this file is not a Cassette take and cannot be renamed";
    return std::strerror(-r);
}

const char *prefix_problem_text(PrefixProblem p)
{
    switch (p) {
    case PrefixProblem::Empty: return "the name is empty";
    case PrefixProblem::TooLong: return "up to 24 characters";
    default: return "use letters, digits, space, - and _";
    }
}

} // namespace

Binder::Binder(Port &port, Worker &worker, Options opt) : port_(port), worker_(worker), opt_(opt) {}

Binder::~Binder()
{
    worker_.finish();
}

void Binder::notice_from_job(const std::string &text)
{
    std::lock_guard<std::mutex> g(inbox_.mu);
    inbox_.notices.push_back(text);
}

void Binder::push_notice(const std::string &text)
{
    if (!text.empty()) notices_.push_back(text);
}

bool Binder::take_nav(ui::Screen &screen)
{
    if (!nav_pending_) return false;
    nav_pending_ = false;
    screen = nav_;
    return true;
}

std::string Binder::take_at(int index) const
{
    if (index < 0 || index >= static_cast<int>(vm_.list.takes.size())) return "";
    return vm_.list.takes[static_cast<size_t>(index)].id;
}

void Binder::leave_trim()
{
    local_.save_shown = false;
    if (local_.confirm == LocalState::Confirm::Replace) local_.confirm = LocalState::Confirm::None;
    local_.tap_selected = false;
    ++names_token_; /* names still on their way: no box on another page */
    pending_open_.clear();
    post([this] { port_.preview(false); });
    if (snap_.trim.save_state == 1) pending_close_ = true; /* never cancel a save by leaving the page */
    else post([this] { port_.close_trim(); });
}

ui::UiActions Binder::actions()
{
    ui::UiActions a;
    a.deck_key = [this](ui::KeyId k, ui::KeyEdge e) {
        KeyEvent ev = KeyEvent::Esc;
        if (k == ui::KeyId::Rew && e == ui::KeyEdge::Down) ev = KeyEvent::RewDown;
        else if (k == ui::KeyId::Rew && e == ui::KeyEdge::Up) ev = KeyEvent::RewUp;
        else if (k == ui::KeyId::Fwd && e == ui::KeyEdge::Down) ev = KeyEvent::FwdDown;
        else if (k == ui::KeyId::Fwd && e == ui::KeyEdge::Up) ev = KeyEvent::FwdUp;
        else if (k == ui::KeyId::PlayPause && e == ui::KeyEdge::Click) ev = KeyEvent::PlayPause;
        else if (k == ui::KeyId::Stop && e == ui::KeyEdge::Click) ev = KeyEvent::Stop;
        else if (k == ui::KeyId::Rec && e == ui::KeyEdge::Click) ev = KeyEvent::Rec;
        else return;
        /* a dimmed key does nothing (REC without a sound input; the deck's own dimmed keys); an Up always goes through */
        if (e != ui::KeyEdge::Up && have_vm_ && !vm_.rec.keys[static_cast<int>(k)].enabled) return;
        post([this, ev] { port_.key_event(ev); });
    };
    a.countdown_cancel = [this] { post([this] { port_.key_event(KeyEvent::TapOverlay); }); };
    a.card_gain = [this](int dir) {
        if (dir == 0) return;
        const double step = opt_.card_step_db;
        post([this, dir, step] {
            const CardSnap c = port_.snapshot().card;
            if (!c.has_level) return;
            /* the card's own step may be coarser than 1 dB: grow the step until the level moves (or the end is hit) */
            for (int k = 1; k <= 12; ++k) {
                const double want = std::clamp(c.level_db + dir * step * k, c.level_min_db, c.level_max_db);
                port_.set_level_db(want);
                const double now = port_.snapshot().card.level_db;
                if (std::fabs(now - c.level_db) > 0.01 || want <= c.level_min_db || want >= c.level_max_db) break;
            }
        });
    };
    a.soft_gain = [this](int dir) {
        if (dir == 0) return;
        const double step = opt_.soft_step_db;
        post([this, dir, step] {
            const double cur = port_.snapshot().card.soft_gain_db;
            port_.set_soft_gain_db(std::clamp(cur + dir * step, kSoftGainMin, kSoftGainMax));
        });
    };
    a.notice_dismiss = [this] { local_.notice.clear(); }; /* the next notice comes with the next tick */
    a.open = [this](ui::Screen s) {
        const ui::Screen was = screen_;
        screen_ = s;
        if (was == ui::Screen::List && s != ui::Screen::List && list_preview_) {
            /* closing the list ends the preview the list started (a playback the recorder started goes on) */
            list_preview_ = false;
            post([this] {
                if (playing_family(port_.snapshot().deck.state)) port_.key_event(KeyEvent::Stop);
            });
        }
        if (was == ui::Screen::Trim && s != ui::Screen::Trim) leave_trim();
        if (s == ui::Screen::Settings) post([this] { port_.refresh_cards(); });
        if (s == ui::Screen::List) {
            list_preview_ = false;
            local_.list_sel = select_after_nav_; /* empty: the loaded take */
            select_after_nav_.clear();
            post([this] { port_.refresh_takes(); });
        }
    };

    a.setting_choice = [this](const std::string &id, int c) {
        if (id == "input") {
            post([this, c] { port_.select_card(c); });
        } else if (id == "format") {
            /* the Format picker's entry: the model's list for this card, in the row's order */
            const Snapshot s = have_snap_ ? snap_ : Snapshot();
            const std::vector<FormatChoice> list = format_picker(s).list;
            std::string why;
            const FormatChoice *f = selectable_format(list, c, &why);
            if (!f) {
                if (!why.empty()) push_notice(why);
                return;
            }
            const FormatKey key = f->key;
            post([this, key] {
                if (!port_.set_format_choice(key.format, key.preset)) notice_from_job("The format cannot be changed now");
            });
        } else if (id == "timer") {
            if (c < 0 || c > 3) return;
            const int secs = kTimerChoices[c];
            post([this, secs] { port_.set_record_timer(secs); });
        } else if (id == "rewfwd") {
            const ScanMode m = c == 1 ? ScanMode::Jump : ScanMode::Fast;
            post([this, m] { port_.set_scan_mode(m); });
        } else if (id == "folder") {
            for (const ui::SettingRow &r : vm_.settings.rows)
                if (r.id == id && c >= 0 && c < static_cast<int>(r.choices.size())) {
                    const std::string f = r.choices[static_cast<size_t>(c)];
                    post([this, f] {
                        if (!port_.set_folder(f)) notice_from_job("The folder cannot be changed now");
                    });
                }
        } else if (id == "split") {
            const bool on = c == 0;
            post([this, on] { port_.set_auto_split(on); });
        } else if (id == "sync") {
            if (c < 0 || c > 3) return;
            const int secs = kSyncChoices[c];
            post([this, secs] { port_.set_sync_interval(secs); });
        } else if (id == "output") {
            /* the picker's entry to the controller's device string (the snapshot lists both in the same order) */
            const Snapshot s = have_snap_ ? snap_ : Snapshot();
            if (c < 0 || c >= static_cast<int>(s.card.output_devices.size())) return;
            const std::string dev = s.card.output_devices[static_cast<size_t>(c)];
            post([this, dev, c] {
                if (!port_.select_output(dev)) notice_from_job("The output cannot be changed now");
            });
        }
        /* "channels": what the card records; nothing to set */
    };
    a.setting_toggle = [this](const std::string &id, bool on) {
        if (id == "agc") post([this, on] { port_.set_auto_gain(on); });
        else if (id == "mute") post([this, on] { port_.set_mute(on); });
        else if (id == "lowcut") post([this, on] { port_.set_low_cut(on); });
        else if (id == "limiter") post([this, on] { port_.set_limiter(on); });
        else if (id == "monitor") post([this, on] { port_.set_monitor(on); });
    };
    a.setting_slider = [this](const std::string &id, int pos) {
        if (id == "monitor_level") { /* the slider counts steps of 5 % */
            const int pct = std::clamp(pos, 0, 20) * 5;
            post([this, pct] { port_.set_monitor_level(pct); });
            return;
        }
        if (id != "level") return;
        post([this, pos] {
            const CardSnap c = port_.snapshot().card;
            if (c.has_level) port_.set_level_db(std::clamp(c.level_min_db + pos, c.level_min_db, c.level_max_db));
        });
    };
    a.setting_text = [this](const std::string &id, const std::string &typed) {
        if (id != "prefix") return;
        const std::string text = trim_prefix(typed);
        const PrefixProblem pp = prefix_problem(text);
        if (pp != PrefixProblem::Ok) {
            push_notice(std::string("Prefix not changed: ") + prefix_problem_text(pp));
            return;
        }
        post([this, text] {
            if (!port_.set_prefix(text)) notice_from_job("Prefix not changed: use letters, digits, space, - and _ (1 to 24)");
        });
    };

    a.setting_number = [this](const std::string &id, int n) {
        if (id != "takenum" || n < 1) return;
        post([this, n] {
            uint64_t landed = 0;
            const int r = port_.set_next_take(static_cast<uint64_t>(n), landed);
            if (r == -EBUSY) {
                notice_from_job("The take number cannot be changed now");
                return;
            }
            if (r < 0) {
                notice_from_job("Take number not changed");
                return;
            }
            if (landed != static_cast<uint64_t>(n))
                notice_from_job("Take " + take_number_text(static_cast<uint64_t>(n)) + " is in use: next is " +
                                take_number_text(landed));
            const uint64_t next = port_.next_take_number();
            std::lock_guard<std::mutex> g(inbox_.mu);
            inbox_.have_next = true;
            inbox_.next_take = next;
        });
    };

    a.list_select = [this](int i) {
        const std::string name = take_at(i);
        if (name.empty()) return;
        local_.list_sel = name; /* the highlight only: what is loaded changes with LOAD alone */
    };
    a.list_activate = [this](int i) {
        const std::string name = take_at(i);
        if (name.empty()) return;
        list_preview_ = true;
        post([this, name] {
            const Snapshot s = port_.snapshot();
            if (recording_family(s.deck.state)) return;
            if (playing_family(s.deck.state) && s.take.playing_name == name) port_.key_event(KeyEvent::Stop); /* stop and rewind */
            else port_.preview_take(name); /* start, or switch to this take */
        });
    };
    a.list_load = [this](int i) {
        const std::string name = take_at(i);
        if (name.empty() || (have_vm_ && !vm_.list.can_load)) return;
        list_preview_ = false; /* the load stops the preview itself */
        local_.list_sel = name;
        post([this, name] {
            if (!port_.load(name)) notice_from_job("The take could not be loaded");
        });
    };
    a.list_rename = [this](int i, const std::string &typed) {
        const std::string name = take_at(i);
        const std::string prefix = trim_prefix(typed); /* spaces around the name never count */
        if (name.empty() || prefix == vm_.list.takes[static_cast<size_t>(i)].prefix) return;
        post([this, name, prefix] {
            const int r = port_.rename_take(name, prefix);
            if (r < 0) notice_from_job("Not renamed: " + error_text(r));
        });
    };
    a.list_trim = [this](int i) {
        const std::string name = take_at(i);
        if (name.empty()) return;
        local_.tap_selected = false;
        local_.save_shown = false;
        screen_ = ui::Screen::Trim; /* the UI shows the page right after this call */
        if (snap_.trim.save_state == 1) { /* a save still runs: opening another take would cancel it */
            pending_open_ = name;
            return;
        }
        pending_close_ = false;
        post([this, name] {
            const Snapshot s = port_.snapshot();
            if (recording_family(s.deck.state)) {
                notice_from_job("Stop the recording to trim a take");
            } else {
                if (playing_family(s.deck.state)) port_.key_event(KeyEvent::Stop);
                if (port_.open_trim(name)) return;
            }
            std::lock_guard<std::mutex> g(inbox_.mu);
            inbox_.have_nav = true;
            inbox_.nav = ui::Screen::List;
        });
    };
    a.list_delete = [this](int i) {
        const std::string name = take_at(i);
        if (name.empty()) return;
        local_.confirm = LocalState::Confirm::Delete;
        local_.confirm_take = name;
    };

    a.trim_zoom = [this](int dir) {
        if (dir == 0) return;
        const double min_s = opt_.min_window_s;
        post([this, dir, min_s] {
            const TrimSnap t = port_.snapshot().trim;
            if (!t.open) return;
            port_.set_zoom(std::clamp(t.zoom + dir, 0, zoom_limit(t, min_s)));
        });
    };
    a.trim_nudge = [this](std::int64_t delta_ms) {
        local_.tap_selected = false;
        post([this, delta_ms] {
            const TrimSnap t = port_.snapshot().trim;
            if (t.open) port_.move_position(ms_to_frames(delta_ms, t.rate));
        });
    };
    a.trim_scrub = [this](std::int64_t marker_ms) {
        local_.tap_selected = false;
        scrub_frame_.store(std::max<int64_t>(0, ms_to_frames(marker_ms, snap_.trim.rate)));
        if (scrub_posted_.exchange(true)) return; /* the queued job sends the latest position */
        post([this] {
            scrub_posted_.store(false);
            port_.set_position(static_cast<uint64_t>(scrub_frame_.load()));
        });
    };
    a.trim_set_in = [this] {
        local_.tap_selected = false;
        post([this] { port_.set_in(); });
    };
    a.trim_set_out = [this] {
        local_.tap_selected = false;
        post([this] { port_.set_out(); });
    };
    a.trim_split = [this] {
        local_.tap_selected = false;
        post([this] { port_.add_split(); });
    };
    a.trim_remove_split = [this](int n) {
        local_.tap_selected = false;
        post([this, n] { port_.remove_split(n); });
    };
    a.trim_select_split = [this](int n) {
        local_.tap_selected = n >= 0;
        if (n >= 0) post([this, n] { port_.select_split(n); });
    };
    a.trim_play = [this] {
        post([this] { port_.preview(!port_.snapshot().trim.previewing); });
    };
    a.trim_save = [this] {
        const TrimSnap &t = snap_.trim;
        if (!t.open || t.out_frame <= t.in_frame || t.save_state == 1) return;
        std::vector<uint64_t> cuts = {t.in_frame};
        for (uint64_t s : t.splits) cuts.push_back(s);
        cuts.push_back(t.out_frame);
        names_lengths_.clear();
        for (size_t i = 0; i + 1 < cuts.size(); ++i) names_lengths_.push_back(frames_to_ms(cuts[i + 1] - cuts[i], t.rate));
        const unsigned token = ++names_token_;
        const std::string take = t.take;
        const unsigned count = static_cast<unsigned>(names_lengths_.size());
        post([this, take, count, token] {
            std::vector<std::string> names = port_.edit_names(take, count);
            std::lock_guard<std::mutex> g(inbox_.mu);
            inbox_.have_names = true;
            inbox_.names_token = token;
            inbox_.names = std::move(names);
        });
    };
    a.save_choose = [this](ui::SaveMode m) {
        switch (m) {
        case ui::SaveMode::NewFiles:
            local_.save_shown = false;
            post([this] {
                if (port_.save(SaveKind::NewFiles)) save_accepted_.store(true);
                else notice_from_job("The save could not start");
            });
            break;
        case ui::SaveMode::Replace:
            local_.confirm = LocalState::Confirm::Replace;
            break;
        case ui::SaveMode::Cancel:
            local_.save_shown = false;
            break;
        }
    };
    a.confirm_answer = [this](bool yes) {
        const LocalState::Confirm what = local_.confirm;
        const std::string take = local_.confirm_take;
        local_.confirm = LocalState::Confirm::None;
        local_.confirm_take.clear();
        if (!yes) return;
        if (what == LocalState::Confirm::Replace) {
            local_.save_shown = false;
            post([this] {
                if (port_.save(SaveKind::ReplaceOriginal)) save_accepted_.store(true);
                else notice_from_job("The save could not start");
            });
        } else if (what == LocalState::Confirm::Delete) {
            post([this, take] {
                const int r = port_.delete_take(take);
                if (r < 0) notice_from_job("Not deleted: " + error_text(r));
            });
        }
    };
    return a;
}

void Binder::drain_inbox()
{
    std::vector<std::string> notices, names;
    bool done = false, have_next = false, have_names = false, have_nav = false;
    uint64_t next = 0;
    unsigned token = 0;
    ui::Screen nav = ui::Screen::List;
    {
        std::lock_guard<std::mutex> g(inbox_.mu);
        notices.swap(inbox_.notices);
        std::swap(done, inbox_.notices_done);
        std::swap(have_next, inbox_.have_next);
        next = inbox_.next_take;
        std::swap(have_names, inbox_.have_names);
        token = inbox_.names_token;
        names.swap(inbox_.names);
        std::swap(have_nav, inbox_.have_nav);
        nav = inbox_.nav;
    }
    for (const std::string &n : notices) push_notice(n);
    if (done) notice_job_ = false;
    if (have_next) local_.next_take = next;
    if (have_names && token == names_token_ && snap_.trim.open && screen_ == ui::Screen::Trim) {
        local_.save_files.clear();
        for (size_t i = 0; i < names_lengths_.size(); ++i) {
            ui::SaveFile f;
            f.name = i < names.size() ? names[i] : std::string();
            f.length_ms = names_lengths_[i];
            local_.save_files.push_back(f);
        }
        local_.save_shown = true;
    }
    if (have_nav) {
        nav_pending_ = true;
        nav_ = nav;
    }
}

void Binder::update_peaks()
{
    const TrimSnap &t = snap_.trim;
    if (!t.open || t.peaks_state != 2 || t.frames == 0) {
        window_.clear();
        overview_.clear();
        peaks_key_.clear();
        overview_key_.clear();
        return;
    }
    const std::string base = t.take + "|" + std::to_string(t.frames);
    if (overview_key_ != base || overview_.empty()) {
        overview_ = port_.peak_range(0, t.frames, opt_.overview_bars);
        overview_key_ = overview_.empty() ? std::string() : base;
    }
    const uint64_t vis = std::max<uint64_t>(1, t.visible_frames);
    const int64_t start = static_cast<int64_t>(t.position) - static_cast<int64_t>(vis / 2);
    const std::string key = base + "|" + std::to_string(start) + "|" + std::to_string(vis);
    if (peaks_key_ != key || window_.empty()) {
        window_ = port_.peak_range(start, vis, opt_.window_bars);
        peaks_key_ = window_.empty() ? std::string() : key;
    }
}

bool Binder::tick(int64_t now_mono_ms, int64_t now_wall_s)
{
    now_ms_ = now_mono_ms;
    drain_inbox();
    /* a save the controller accepted since the last tick: read before the snapshot, so the snapshot is not older than it */
    if (save_accepted_.exchange(false)) save_wait_ = true;
    snap_ = port_.snapshot();
    have_snap_ = true;

    if (!have_rows_ || snap_.takes_revision != rows_rev_) {
        rows_ = port_.takes();
        rows_rev_ = snap_.takes_revision;
        have_rows_ = true;
        post([this] {
            const uint64_t n = port_.next_take_number();
            std::lock_guard<std::mutex> g(inbox_.mu);
            inbox_.have_next = true;
            inbox_.next_take = n;
        });
    }

    /* the controller's one-time notices: taken and cleared by one job, shown here */
    if (!snap_.notices.empty() && !notice_job_ && snap_.started) {
        notice_job_ = true;
        post([this] {
            const std::vector<Notice> n = port_.snapshot().notices;
            port_.ack_notices();
            std::lock_guard<std::mutex> g(inbox_.mu);
            for (const Notice &x : n) inbox_.notices.push_back(x.text);
            inbox_.notices_done = true;
        });
    }

    /* the trim page: a finished save, a close or an open waiting for it */
    const int ss = snap_.trim.open ? snap_.trim.save_state : 0;
    /* a save that is over: seen running and then done, or (a small take saves between two ticks) accepted and now over */
    const bool over = (ss == 2 || ss == 3) && ((ss != last_save_state_ && last_save_state_ == 1) || save_wait_);
    if (ss == 1 || ss == 0 || ss == 4 || over) save_wait_ = false;
    if (ss != last_save_state_ || over) {
        if (over && ss == 2) {
            const std::vector<std::string> &outs = snap_.trim.save_outputs;
            const size_t n = outs.size();
            if (snap_.trim.save_replaced) {
                /* the take the page shows is gone: back to the list, on the first result */
                push_notice(n == 1 ? std::string("Saved, original replaced")
                                   : "Saved " + std::to_string(n) + " files, original removed");
                if (screen_ == ui::Screen::Trim) {
                    nav_pending_ = true;
                    nav_ = ui::Screen::List;
                    select_after_nav_ = outs.empty() ? std::string() : outs.front();
                }
            } else {
                push_notice("Saved " + std::to_string(n) + (n == 1 ? " new file" : " new files"));
            }
        } else if (over && ss == 3) {
            push_notice("Save failed: " + snap_.trim.save_message);
        }
        last_save_state_ = ss;
    }
    if (pending_close_ && (!snap_.trim.open || snap_.trim.save_state != 1)) {
        pending_close_ = false;
        if (snap_.trim.open && screen_ != ui::Screen::Trim) post([this] { port_.close_trim(); });
    }
    if (!pending_open_.empty() && snap_.trim.save_state != 1) {
        const std::string name = pending_open_;
        pending_open_.clear();
        post([this, name] {
            if (!port_.open_trim(name)) {
                std::lock_guard<std::mutex> g(inbox_.mu);
                inbox_.have_nav = true;
                inbox_.nav = ui::Screen::List;
            }
        });
    }
    update_peaks();

    /* the shown notice: each for notice_ms, then the next */
    if (!local_.notice.empty() && now_ms_ - notice_since_ >= opt_.notice_ms) local_.notice.clear();
    if (local_.notice.empty() && !notices_.empty()) {
        local_.notice = notices_.front();
        notices_.pop_front();
        notice_since_ = now_ms_;
    }
    if (local_.confirm == LocalState::Confirm::Replace && !snap_.trim.open) local_.confirm = LocalState::Confirm::None;
    if (!snap_.trim.open && local_.save_shown && screen_ != ui::Screen::Trim) local_.save_shown = false;

    MapInput in;
    in.snap = &snap_;
    in.rows = &rows_;
    in.window = &window_;
    in.overview = &overview_;
    in.local = &local_;
    in.now_wall_s = now_wall_s;
    in.utc = opt_.utc;
    ui::ViewModel vm = map_view(in);
    const bool changed = !have_vm_ || !same(vm, vm_);
    if (changed) vm_ = std::move(vm);
    have_vm_ = true;
    return changed;
}

void Binder::leave()
{
    if (left_) return;
    left_ = true;
    worker_.finish();
    port_.app_leave();
    port_.shutdown();
}

} // namespace bind
} // namespace cassette
