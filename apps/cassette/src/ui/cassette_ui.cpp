/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the root (see cassette_ui.hpp): the toolkit page, the pages, navigation, keys, the text boxes, the
 * scenes' layout check and the timing log.
 */
#include "cassette_ui.hpp"

#include "app.hpp"
#include "pages.hpp"
#include "paint.hpp"
#include "theme.hpp"
#include "ui_check.hpp"
#include "ui_format.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_widgets.hpp"
#include "keyboard_input.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* The narrow portrait shell (320x480: 40 px bar, Esc at the left edge, no title) is the toolkit's (launcher v0.9.4). */
#ifndef CP0_UI_NARROW_PORTRAIT_SIZE
#error "cassette needs the launcher toolkit at the pin of apps/_common/LAUNCHER_REF (v0.9.4 or later)"
#endif

namespace cassette {
namespace ui {

namespace {

void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char *format, ...)
{
    std::fputs("[cassette] ", stderr);
    va_list ap;
    va_start(ap, format);
    std::vfprintf(stderr, format, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

std::int64_t mono_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool env_on(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

const char *screen_title(Screen s)
{
    switch (s) {
    case Screen::Recorder: return "Cassette";
    case Screen::Settings: return "Settings";
    case Screen::List: return "Recordings";
    case Screen::Trim: return "Edit"; /* the trim and split page (its title) */
    }
    return "Cassette";
}

const char *screen_name(Screen s)
{
    switch (s) {
    case Screen::Recorder: return "recorder";
    case Screen::Settings: return "settings";
    case Screen::List: return "list";
    case Screen::Trim: return "trim";
    }
    return "?";
}

} // namespace

/* ------------------------------------------------------------------------------------------------ Frame, PageBase */

Fit Frame::fit(std::vector<double> seams_landscape, std::vector<double> seams_portrait, std::vector<Band> bands_landscape,
               std::vector<Band> bands_portrait) const
{
    return portrait ? make_fit(w, h, body_y, 320, 480, 40, std::move(seams_portrait), std::move(bands_portrait))
                    : make_fit(w, h, body_y, 480, 320, 40, std::move(seams_landscape), std::move(bands_landscape));
}

double Frame::scale() const
{
    return make_fit(w, h, body_y, portrait ? 320 : 480, portrait ? 480 : 320, 40, {}).s;
}

PageBase::PageBase(lv_obj_t *parent, const Frame &frame) : frame_(frame)
{
    root_ = lv_obj_create(parent);
    plain(root_);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, frame.w, frame.h);
}

void PageBase::set_shown(bool shown)
{
    shown_ = shown;
    show(root_, shown);
}

/* ------------------------------------------------------------------------------------------------ the root */

struct CassetteUi::Impl {
    lv_obj_t *screen = nullptr;
    UiActions act;
    Nav nav;
    Frame frame;
    cp0_ui_roller_page_t layout{};
    std::unique_ptr<cp0::ui::Page> page;
    lv_obj_t *surface = nullptr; /* takes the keys */
    std::unique_ptr<RecorderPage> rec;
    std::unique_ptr<SettingsPage> settings;
    std::unique_ptr<ListPage> list;
    std::unique_ptr<TrimPage> trim;
    std::unique_ptr<SaveBox> save;
    std::unique_ptr<ConfirmBox> confirm;
    ViewModel vm;
    bool have_vm = false;
    Screen cur = Screen::Recorder;
    std::string title;

    /* the text box (File prefix, RENAME) */
    std::unique_ptr<cp0::ui::Dialog> dialog;
    std::unique_ptr<cp0::ui::TextField> field;
    lv_obj_t *counter = nullptr; /* the "<used>/24" label under the entry (a child of the dialog's body) */
    std::function<void(const std::string &)> on_text;
    bool closing = false;
    bool held[static_cast<int>(KeyId::kCount)] = {}; /* deck keys held on the keyboard */

    /* scenes' check and timing */
    lv_timer_t *check_timer = nullptr;
    lv_obj_t *fail_label = nullptr;
    std::string last_report;
    bool timing = false;
    std::int64_t render_start = 0, timing_since = 0, render_sum = 0, render_max = 0;
    int render_n = 0;
    long swaps_since = 0;

    void build(AnimOptions anim);
    void go(Screen s);
    void back();
    void set_title(const std::string &t);
    void on_key(std::uint32_t k);
    void on_keyboard(const key_item &item);
    void open_text(const std::string &title, const std::string &label, const std::string &text,
                   std::function<void(const std::string &)> done);
    void close_text();
    void update_counter(const std::string &text);
    bool box_up() const { return (save && save->shown()) || (confirm && confirm->shown()); }
    PageBase *current() const
    {
        switch (cur) {
        case Screen::Settings: return settings.get();
        case Screen::List: return list.get();
        case Screen::Trim: return trim.get();
        default: return rec.get();
        }
    }
    void run_check();

    static void key_cb(lv_event_t *e);
    static void keyboard_cb(lv_event_t *e);
    static void check_cb(lv_timer_t *t);
    static void render_cb(lv_event_t *e);
};

void CassetteUi::Impl::build(AnimOptions anim)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &layout);
    page = std::make_unique<cp0::ui::Page>(screen, layout, cp0::ui::add_title, [this] { back(); });
    cp0::ui::StatusStrip::add(page->header());
    set_title("Cassette");
    frame.w = layout.w;
    frame.h = layout.h;
    frame.body_y = layout.body_y;
    frame.square = square_screen(layout.w, layout.h);
    frame.portrait = layout.h > layout.w && !frame.square;
    frame.target = m->tok[CP0_TOK_TARGET];
    frame.row = m->tok[CP0_TOK_ROW_H];
    frame.list_row = std::min(m->tok[CP0_TOK_ROW_H], m->tok[CP0_TOK_LIST_ROW_H]);

    surface = lv_obj_create(page->root());
    plain(surface);
    lv_obj_set_pos(surface, 0, layout.body_y);
    lv_obj_set_size(surface, 1, 1);
    lv_obj_add_event_cb(surface, key_cb, LV_EVENT_KEY, this);
    if (LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(screen, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    if (lv_group_t *g = cdk::input_group()) {
        lv_group_add_obj(g, surface);
        lv_group_focus_obj(surface);
    }

    nav.go = [this](Screen s) { go(s); };
    nav.set_title = [this](const std::string &t) { set_title(t); };
    nav.rename = [this](int index) {
        if (index < 0 || index >= static_cast<int>(vm.list.takes.size())) return;
        const TakeRow &t = vm.list.takes[static_cast<std::size_t>(index)];
        open_text("Rename", "Name", t.prefix, [this, index](const std::string &text) {
            if (act.list_rename) act.list_rename(index, text);
        });
    };
    nav.edit_text = [this](const std::string &id, const std::string &label, const std::string &value) {
        open_text(label, label, value, [this, id](const std::string &text) {
            if (act.setting_text) act.setting_text(id, text);
        });
    };

    lv_obj_t *root = page->root();
    rec = std::make_unique<RecorderPage>(root, frame, act, nav, anim);
    settings = std::make_unique<SettingsPage>(root, frame, act, nav);
    list = std::make_unique<ListPage>(root, frame, act, nav);
    trim = std::make_unique<TrimPage>(root, frame, act, nav);
    save = std::make_unique<SaveBox>(root, frame, act);
    confirm = std::make_unique<ConfirmBox>(root, frame, act);
    settings->set_shown(false);
    list->set_shown(false);
    trim->set_shown(false);

    log_line("ui %dx%d%s%s, %.2f px/mm, touch target %d px, row %d px, list row %d px, animation %s at %d fps", frame.w,
             frame.h, frame.portrait ? " portrait" : frame.square ? " square" : "", cp0_ui_narrow_portrait(m) ? " (narrow shell)" : "",
             m->ppmm_x100 / 100.0, frame.target, frame.row, frame.list_row, anim.enabled ? "on" : "off", anim.fps);

    if (env_on("CASSETTE_LAYOUT_CHECK")) check_timer = lv_timer_create(check_cb, 250, this);
    timing = env_on("CASSETTE_TIMING");
    if (timing) {
        timing_since = mono_us();
        lv_display_add_event_cb(lv_obj_get_display(screen), render_cb, LV_EVENT_ALL, this);
    }
}

void CassetteUi::Impl::set_title(const std::string &t)
{
    if (t == title) return;
    title = t;
    page->set_title({t});
}

void CassetteUi::Impl::go(Screen s)
{
    if (dialog) close_text();
    cur = s;
    rec->set_shown_page(s == Screen::Recorder);
    settings->set_shown(s == Screen::Settings);
    list->set_shown(s == Screen::List);
    trim->set_shown(s == Screen::Trim);
    if (s == Screen::Settings) settings->reset_view();
    set_title(screen_title(s));
    log_line("page %s", screen_name(s));
    if (act.open) act.open(s);
}

void CassetteUi::Impl::back()
{
    /* Esc while the cassette is going in: the animation ends at once, then Esc acts as usual */
    if (rec && rec->window().loading()) rec->window().finish_load();
    if (dialog) {
        close_text();
        return;
    }
    if (confirm->shown()) {
        if (act.confirm_answer) act.confirm_answer(false);
        return;
    }
    if (save->shown()) {
        if (act.save_choose) act.save_choose(SaveMode::Cancel);
        return;
    }
    switch (cur) {
    case Screen::Recorder:
        if (vm.rec.transport == Transport::Countdown) {
            if (act.countdown_cancel) act.countdown_cancel();
        } else {
            cdk::request_quit(0);
        }
        break;
    case Screen::Settings:
        if (!settings->back()) go(Screen::Recorder);
        break;
    case Screen::List: go(Screen::Recorder); break;
    case Screen::Trim: go(Screen::List); break;
    }
}

void CassetteUi::Impl::on_key(std::uint32_t k)
{
    /* every key is handled from LV_EVENT_KEYBOARD (on_keyboard: it has the releases and the modifiers); the keypad's
     * LV_EVENT_KEY for the same key is ignored */
    (void)k;
}

/* The keyboard map (cassette_ui.hpp, docs/KEYS.md). */
void CassetteUi::Impl::on_keyboard(const key_item &item)
{
    if (dialog) return; /* the text box has the keys */
    /* the cassette is going in: every key is ignored but Esc */
    if (rec && rec->window().loading() && item.key_code != KEY_ESC && item.key_state != KBD_KEY_RELEASED) return;
    const bool press = item.key_state == KBD_KEY_PRESSED, repeat = item.key_state == KBD_KEY_REPEATED;
    const bool release = item.key_state == KBD_KEY_RELEASED;
    const std::uint32_t code = item.key_code;
    /* a character without a key code (the toolkit's on-screen keyboard, a scripted key) */
    char ch = 0;
    if (code == 0 && item.utf8[0] && !item.utf8[1]) {
        ch = item.utf8[0];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    auto is = [code, ch](std::uint32_t k, char c) { return code == k || (code == 0 && c && ch == c); };

    /* the deck keys: the press and the release reach the deck (holding REW / FWD scans), then a click */
    if (cur == Screen::Recorder && !box_up()) {
        int k = -1;
        if (is(KEY_COMMA, ',')) k = static_cast<int>(KeyId::Rew);
        else if (is(KEY_SPACE, ' ')) k = static_cast<int>(KeyId::PlayPause);
        else if (is(KEY_DOT, '.')) k = static_cast<int>(KeyId::Fwd);
        else if (is(KEY_X, 'x')) k = static_cast<int>(KeyId::Stop);
        else if (is(KEY_R, 'r')) k = static_cast<int>(KeyId::Rec);
        if (k >= 0) {
            if (press && !held[k]) {
                if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO | KBD_MOD_FN)) return;
                held[k] = true;
                rec->press_key(k, true);
                if (act.deck_key) act.deck_key(static_cast<KeyId>(k), KeyEdge::Down);
            } else if (release && held[k]) {
                held[k] = false;
                rec->press_key(k, false);
                if (act.deck_key) act.deck_key(static_cast<KeyId>(k), KeyEdge::Up);
                if (act.deck_key) act.deck_key(static_cast<KeyId>(k), KeyEdge::Click);
            }
            return;
        }
    }
    if (!press && !repeat) return;
    if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO | KBD_MOD_FN)) return;
    const bool shift = (item.mods & KBD_MOD_SHIFT) != 0;
    const bool esc = code == KEY_ESC, enter = code == KEY_ENTER || code == KEY_KPENTER;
    const bool up = code == KEY_UP, down = code == KEY_DOWN, left = code == KEY_LEFT, right = code == KEY_RIGHT;
    const bool tab = code == KEY_TAB || (code == 0 && ch == '\t');

    if (esc) {
        if (press) back();
        return;
    }
    /* the boxes: Left / Right / Tab move between the buttons, Enter chooses, Esc cancels */
    if (box_up()) {
        if (!press && !(left || right)) return;
        const int dir = left ? -1 : (right || tab) ? 1 : 0;
        if (confirm->shown()) {
            if (dir) confirm->key_move(dir);
            else if (enter) confirm->key_choose();
        } else {
            if (dir) save->key_move(dir);
            else if (enter) save->key_choose();
        }
        return;
    }
    switch (cur) {
    case Screen::Recorder:
        if (press && is(KEY_L, 'l')) go(Screen::List);
        else if (press && is(KEY_S, 's')) go(Screen::Settings);
        break;
    case Screen::Settings:
        if (up) settings->on_key(LV_KEY_UP);
        else if (down) settings->on_key(LV_KEY_DOWN);
        else if (enter && press) settings->on_key(LV_KEY_ENTER);
        else if (right) settings->on_right();
        else if (left) settings->on_left_right(-1);
        else if (press && is(KEY_L, 'l')) go(Screen::List);
        break;
    case Screen::List:
        if (up) list->on_key(LV_KEY_UP);
        else if (down) list->on_key(LV_KEY_DOWN);
        else if (!press) break;
        else if (enter) list->activate();
        else if (is(KEY_L, 'l')) list->key(0);
        else if (is(KEY_N, 'n')) list->key(1);
        else if (is(KEY_T, 't')) list->key(2);
        else if (code == KEY_DELETE) list->key(3);
        else if (is(KEY_S, 's')) go(Screen::Settings);
        break;
    case Screen::Trim:
        if (left || right) trim->nudge(left ? -1 : 1, shift);
        else if (!press) break;
        else if (up) trim->press(TrimPage::ZoomIn);
        else if (down) trim->press(TrimPage::ZoomOut);
        else if (is(KEY_I, 'i')) trim->press(TrimPage::SetIn);
        else if (is(KEY_O, 'o')) trim->press(TrimPage::SetOut);
        else if (enter) trim->press(TrimPage::Split);
        else if (tab) trim->press(TrimPage::NextFlag);
        else if (is(KEY_SPACE, ' ')) trim->press(TrimPage::Play);
        else if (is(KEY_S, 's')) trim->press(TrimPage::Save);
        else if (is(KEY_F, 'f')) trim->press(TrimPage::ToggleFit);
        break;
    }
}

void CassetteUi::Impl::open_text(const std::string &title, const std::string &label, const std::string &text,
                                 std::function<void(const std::string &)> done)
{
    if (dialog) close_text();
    on_text = std::move(done);
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page.get();
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { close_text(); }});
    spec.buttons.push_back({"Save", cp0::ui::ButtonRole::Primary, [this] {
                                const std::string t = field ? field->text() : std::string();
                                auto cb = on_text;
                                close_text();
                                if (cb) cb(t);
                            }});
    dialog = std::make_unique<cp0::ui::Dialog>(page->root(), page->layout(), spec);
    cp0::ui::TextFieldSpec fs;
    fs.label = label;
    fs.text = text.size() > static_cast<std::size_t>(kNameLimit) ? text.substr(0, kNameLimit) : text;
    fs.max_bytes = kNameLimit; /* the entry refuses input beyond the limit; the counter shows how much is left */
    cp0::ui::TextFieldEvents fe;
    fe.changed = [this](const std::string &t) { update_counter(t); };
    fe.submit = [this](const std::string &t) {
        auto cb = on_text;
        close_text();
        if (cb) cb(t);
    };
    fe.escape = [this] { close_text(); };
    fe.back = [this] { close_text(); };
    field = std::make_unique<cp0::ui::TextField>(dialog->body(), dialog->field_layout(), fs, fe);
    {
        const int px = std::max(12, static_cast<int>(std::lround(12 * frame.scale())));
        counter = ui::label(dialog->body(), Face::Medium, px, col::kHint, 0, 0, dialog->field_layout().roller_w,
                            LV_TEXT_ALIGN_RIGHT);
        lv_obj_remove_flag(counter, LV_OBJ_FLAG_CLICKABLE);
        update_counter(fs.text);
    }
    dialog->relayout();
    if (lv_group_t *g = cdk::input_group()) {
        lv_group_add_obj(g, field->focus_object());
        lv_group_focus_obj(field->focus_object());
    }
    field->begin_edit();
    /* the page goes while the box is up: nothing of it may lie under the toolkit's keyboard (the box's backdrop only
     * dims it) */
    show(current()->root(), false);
    log_line("text box: %s", title.c_str());
}

void CassetteUi::Impl::update_counter(const std::string &text)
{
    if (!counter) return;
    const CountTone tone = name_count_tone(text.size());
    set_text(counter, name_count_text(text.size()));
    lv_obj_set_style_text_color(counter, lv_color_hex(name_count_color(tone)), 0);
}

void CassetteUi::Impl::close_text()
{
    if (!dialog || closing) return;
    closing = true;
    counter = nullptr; /* it goes with the dialog */
    /* never delete the box inside its own event: hand the objects to the next LVGL cycle */
    auto *pending = new std::pair<std::unique_ptr<cp0::ui::TextField>, std::unique_ptr<cp0::ui::Dialog>>(std::move(field),
                                                                                                       std::move(dialog));
    if (pending->second) lv_obj_add_flag(pending->second->root(), LV_OBJ_FLAG_HIDDEN);
    if (pending->first && pending->first->editing()) pending->first->end_edit();
    lv_async_call(
        [](void *p) {
            delete static_cast<std::pair<std::unique_ptr<cp0::ui::TextField>, std::unique_ptr<cp0::ui::Dialog>> *>(p);
        },
        pending);
    on_text = nullptr;
    show(current()->root(), true);
    if (cdk::input_group()) lv_group_focus_obj(surface);
    closing = false;
}

void CassetteUi::Impl::key_cb(lv_event_t *e)
{
    static_cast<Impl *>(lv_event_get_user_data(e))->on_key(lv_event_get_key(e));
}

void CassetteUi::Impl::keyboard_cb(lv_event_t *e)
{
    auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
    const auto *item = static_cast<const key_item *>(lv_event_get_param(e));
    if (item) self->on_keyboard(*item);
}

void CassetteUi::Impl::check_cb(lv_timer_t *t)
{
    static_cast<Impl *>(lv_timer_get_user_data(t))->run_check();
}

void CassetteUi::Impl::run_check()
{
    lv_obj_t *root = rec->root();
    if (confirm->shown()) root = confirm->root();
    else if (save->shown()) root = save->root();
    else if (cur == Screen::Settings) root = settings->root();
    else if (cur == Screen::List) root = list->root();
    else if (cur == Screen::Trim) root = trim->root();
    CheckStats st;
    /* the two header-side buttons drawn 40 x 44 at 480 wide: the recorder's Recordings and Settings buttons */
    std::vector<lv_obj_t *> narrow;
    if (cur == Screen::Recorder && !frame.portrait) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(rec->root()); ++i) {
            lv_obj_t *c = lv_obj_get_child(rec->root(), static_cast<int32_t>(i));
            if (lv_obj_check_type(c, &lv_button_class) && lv_obj_get_width(c) < lv_obj_get_height(c)) narrow.push_back(c);
        }
    }
    const std::vector<std::string> fails = check_layout(root, frame.w, frame.h, narrow, &st);
    std::string report = std::string(screen_name(cur)) + (box_up() ? "+box" : "") + ": ";
    if (fails.empty()) {
        report += "PASS, " + std::to_string(st.labels) + " labels, " + std::to_string(st.targets) + " targets (smallest " +
                  std::to_string(st.targets ? st.min_target : 0) + " px)";
    } else {
        report += "FAIL";
        for (const std::string &f : fails) report += "\n  " + f;
    }
    if (report != last_report) {
        last_report = report;
        log_line("layout check %s", report.c_str());
    }
    if (!fails.empty() && !fail_label) {
        fail_label = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(fail_label, lv_color_hex(0xFF2020), 0);
        lv_obj_set_style_bg_color(fail_label, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(fail_label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(fail_label, cp0_ui_font_px(14), 0);
        lv_label_set_text(fail_label, "LAYOUT FAIL");
        lv_obj_set_pos(fail_label, 0, 0);
    }
    show(fail_label, !fails.empty());
}

void CassetteUi::Impl::render_cb(lv_event_t *e)
{
    auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        self->render_start = mono_us();
    } else if (code == LV_EVENT_REFR_READY && self->render_start > 0) {
        const std::int64_t now = mono_us(), d = now - self->render_start;
        self->render_start = 0;
        self->render_sum += d;
        self->render_max = std::max(self->render_max, d);
        ++self->render_n;
        if (now - self->timing_since >= 5000000) {
            const long swaps = self->rec->window().frames_swapped();
            log_line("timing: %d refreshes in %.1f s, render avg %.3f ms, max %.3f ms, hub frames %ld", self->render_n,
                     static_cast<double>(now - self->timing_since) / 1e6,
                     self->render_n ? static_cast<double>(self->render_sum) / self->render_n / 1000.0 : 0.0,
                     static_cast<double>(self->render_max) / 1000.0, swaps - self->swaps_since);
            self->swaps_since = swaps;
            self->timing_since = now;
            self->render_sum = self->render_max = 0;
            self->render_n = 0;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ CassetteUi */

CassetteUi::CassetteUi(lv_obj_t *screen, UiActions actions, AnimOptions anim) : d_(std::make_unique<Impl>())
{
    d_->screen = screen;
    d_->act = std::move(actions);
    d_->build(anim);
}

CassetteUi::~CassetteUi()
{
    if (!d_) return;
    if (d_->check_timer) lv_timer_delete(d_->check_timer);
    if (d_->timing && lv_obj_is_valid(d_->screen))
        lv_display_remove_event_cb_with_user_data(lv_obj_get_display(d_->screen), Impl::render_cb, d_.get());
    if (LV_EVENT_KEYBOARD != 0 && lv_obj_is_valid(d_->screen))
        lv_obj_remove_event_cb_with_user_data(d_->screen, Impl::keyboard_cb, d_.get());
    d_->field.reset();
    d_->dialog.reset();
    d_->confirm.reset();
    d_->save.reset();
    d_->trim.reset();
    d_->list.reset();
    d_->settings.reset();
    d_->rec.reset();
    d_->page.reset();
}

void CassetteUi::update(const ViewModel &vm)
{
    Impl &d = *d_;
    d.vm = vm;
    d.rec->update(d.vm.rec, d.vm.notice); /* the member copy: the page keeps a pointer to it for the landing */
    d.settings->set_notice(vm.notice);
    d.list->set_notice(vm.notice);
    d.trim->set_notice(vm.notice);
    d.settings->update(vm.settings);
    d.list->update(vm.list);
    d.trim->update(vm.trim);
    d.save->update(vm.save, vm.trim);
    d.confirm->update(vm.confirm);
    d.have_vm = true;
}

void CassetteUi::go(Screen s)
{
    d_->go(s);
}

Screen CassetteUi::screen() const
{
    return d_->cur;
}

double CassetteUi::trim_bars_across() const
{
    return d_->trim->bars_across();
}

} // namespace ui
} // namespace cassette
