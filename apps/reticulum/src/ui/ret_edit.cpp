/*
 * SPDX-License-Identifier: MIT
 *
 * The Reticulum app: changing things (switches, the options of a connection, the new-connection form, the name) and the
 * dialogs (see ret_app.hpp). Every change goes to the bridge through the Link; a refusal is shown in plain words.
 */
#include "ret_app.hpp"

#include "ret_ids.hpp"

#include "app.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_list.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdio>

namespace retui {

namespace {

constexpr const char *kTyped = "\x01typed"; /* the choice that opens the text editor instead */
constexpr uint32_t kKeyTab = 15;            /* KEY_TAB: cp0's keyboard hands an object the evdev code of Tab as it is */
constexpr int kLabelColumnPct = 22;        /* form labels beside their fields, as the template's */

using ret::json::Value;

Value obj()
{
    return Value::object();
}

} // namespace

/* ------------------------------------------------------------------ switches */

void App::toggle_enabled()
{
    const ret::Iface *i = current_iface();
    if (!i || busy_) return;
    const bool want = !i->enabled;
    if (want && !ret::has_listen_choice(*i)) {
        /* Wi-Fi / LAN has no listen-only state: it sends discovery packets from the moment it is on. Ask first. */
        switch_on_asking();
        return;
    }
    busy_ = true;
    set_top(want ? "connection: switching on..." : "connection: switching off...", ret::Tone::Warn);
    Value p = obj();
    p.set("name", Value::string(i->name));
    p.set("enabled", Value::boolean(want));
    link_->request("iface.enable", p, 25.0, [this, want](const ret::Response &r) {
        busy_ = false;
        if (r.ok) std::fprintf(stderr, "[reticulum] connection switched %s\n", want ? "on" : "off");
        else request_failed(r, want ? "Could not switch it on" : "Could not switch it off");
        request_ifaces();
    });
}

/* Only the confirmation of the dialog gets here for a kind with no listen-only state: the request that lets the deck send. */
void App::switch_on_asking()
{
    const ret::Iface *i = current_iface();
    if (!i) return;
    const std::string title = "Switch on " + ret::conn_title(*i) + "?";
    confirm(title, ret::switch_on_question(*i), "Switch on", [this] {
        const ret::Iface *c = current_iface();
        if (!c || busy_) return;
        busy_ = true;
        set_top("connection: switching on...", ret::Tone::Warn);
        Value p = obj();
        p.set("name", Value::string(c->name));
        p.set("enabled", Value::boolean(true));
        p.set("allow_sending", Value::boolean(true));
        link_->request(
            "iface.enable", p, 25.0,
            [this](const ret::Response &r) {
                busy_ = false;
                if (r.ok) std::fprintf(stderr, "[reticulum] connection switched on, sending allowed by the user\n");
                else request_failed(r, "Could not switch it on");
                request_ifaces();
            },
            /*user_action=*/true);
    });
}

/* The service tells, once, what it changed while the app was not looking: a Wi-Fi / LAN connection that an older version
 * left on and listening only is switched off, because that kind cannot only listen. */
void App::request_notices()
{
    link_->request("notices.take", obj(), 5.0, [this](const ret::Response &r) {
        if (!r.ok || svc_ != Svc::Up) return; /* an older service does not know the request */
        std::string names;
        int n = 0;
        for (const Value &v : r.result["notices"].items()) {
            if (v["kind"].as_string() != "listen_only_removed") continue;
            ret::Iface shown;
            shown.name = v["name"].as_string();
            shown.type = v["type"].as_string();
            if (!names.empty()) names += ", ";
            names += ret::conn_title(shown);
            ++n;
        }
        if (n == 0) return;
        std::fprintf(stderr, "[reticulum] %d connection(s) switched off: they could only listen, which their kind cannot do\n", n);
        notice(n == 1 ? names + " was switched off" : "Connections were switched off",
               (n == 1 ? names + " was" : names + " were") +
                   " set to listen only. This kind of connection cannot only listen: while it is on it makes this deck known to "
                   "others. It is off now and nothing is sent. Switch it on again when you want it; it may send then.");
        request_ifaces();
    });
}

void App::toggle_sending()
{
    const ret::Iface *i = current_iface();
    if (!i || busy_) return;
    if (!i->outgoing) {
        const bool radio = ret::kind_info(i->type).section.compare(0, 5, "Radio") == 0;
        std::string text = "While this connection is on, this deck may send: it can make itself known and answer others on it.";
        if (radio) text += " On a radio, the rules for sending are yours to check: frequency, power and time on air.";
        confirm("Allow sending?", text, "Allow sending", [this] { allow_sending(); });
        return;
    }
    busy_ = true;
    set_top("connection: listen only...", ret::Tone::Warn);
    Value p = obj();
    p.set("name", Value::string(i->name));
    p.set("outgoing", Value::boolean(false));
    link_->request("iface.outgoing", p, 25.0, [this](const ret::Response &r) {
        busy_ = false;
        if (r.ok) std::fprintf(stderr, "[reticulum] sending not allowed (listen only)\n");
        else request_failed(r, "Could not change it");
        request_ifaces();
    });
}

/* Only the confirmation of the dialog gets here: the one request that lets the deck transmit. */
void App::allow_sending()
{
    const ret::Iface *i = current_iface();
    if (!i || busy_) return;
    busy_ = true;
    set_top("connection: allowing sending...", ret::Tone::Warn);
    Value p = obj();
    p.set("name", Value::string(i->name));
    p.set("outgoing", Value::boolean(true));
    link_->request(
        "iface.outgoing", p, 25.0,
        [this](const ret::Response &r) {
            busy_ = false;
            if (r.ok) std::fprintf(stderr, "[reticulum] sending allowed by the user\n");
            else request_failed(r, "Could not allow sending");
            request_ifaces();
        },
        /*user_action=*/true);
}

void App::confirm_remove()
{
    const ret::Iface *i = current_iface();
    if (!i) return;
    const std::string name = i->name;
    confirm("Remove this connection?",
            ret::conn_title(*i) + " is taken out of this deck's settings. You can add it again later.", "Remove",
            [this, name] {
                busy_ = true;
                Value p = obj();
                p.set("name", Value::string(name));
                link_->request("iface.remove", p, 20.0, [this](const ret::Response &r) {
                    busy_ = false;
                    if (r.ok) std::fprintf(stderr, "[reticulum] connection removed\n");
                    else request_failed(r, "Could not remove it");
                    request_ifaces(); /* a removed connection's page closes itself when the list comes back */
                });
            });
}

/* ------------------------------------------------------------------ the new-connection form */

void App::begin_form(const std::string &type)
{
    const ret::TypeDef *t = catalog_.find(type);
    if (!t || !t->supported) {
        notice("Not available", t && !t->reason.empty() ? ret::plainify(t->reason) : "This kind cannot be added here.");
        return;
    }
    drafting_ = true;
    draft_type_ = type;
    draft_name_ = ret::suggest_name(type, ifaces_);
    draft_vals_.clear();
    form_opts_ = t->options;
    form_type_n_ = form_opts_.size();
    open(ret::View::AddForm);
}

void App::save_draft()
{
    if (busy_) return;
    Value opts = obj();
    for (const ret::OptionDef &def : form_opts_) {
        const auto it = draft_vals_.find(def.name);
        if (it != draft_vals_.end() && !it->second.empty()) opts.set(def.name, Value::string(it->second));
    }
    Value p = obj();
    p.set("name", Value::string(draft_name_));
    p.set("type", Value::string(draft_type_));
    p.set("options", opts);
    busy_ = true;
    const std::string name = draft_name_;
    link_->request("iface.add", p, 20.0, [this, name](const ret::Response &r) {
        busy_ = false;
        if (!r.ok) {
            request_failed(r, "Could not save it");
            return;
        }
        std::fprintf(stderr, "[reticulum] connection added, switched off\n");
        request_ifaces([this, name] {
            /* the new connection's own page, with Connections under it */
            list_.reset();
            nav_.reset();
            saved_sel_.clear();
            drafting_ = false;
            nav_.open(ret::View::Connections);
            saved_sel_.push_back(0);
            open_connection(name);
        });
    });
}

/* ------------------------------------------------------------------ editing one option */

void App::edit_option(const ret::OptionDef &def)
{
    edit_def_ = def;
    edit_title_ = ret::option_label(def.name);
    edit_secret_ = def.secret;
    if (def.kind == ret::OptKind::Bool || !def.choices.empty() || def.device) {
        open_choice(def);
        return;
    }
    std::string value;
    bool present = false;
    if (drafting_) {
        const auto it = draft_vals_.find(def.name);
        if (it != draft_vals_.end()) {
            value = it->second;
            present = !value.empty();
        }
    } else if (const ret::Iface *i = current_iface()) {
        value = i->value(def.name, &present);
    }
    const cp0::ui::TextType type = def.secret ? cp0::ui::TextType::Password
                                   : (def.kind == ret::OptKind::Int || def.kind == ret::OptKind::Float)
                                       ? cp0::ui::TextType::Number
                                       : cp0::ui::TextType::Text;
    open_text(Edit::Option, edit_title_, edit_title_, ret::option_edit_text(def, present ? &value : nullptr), type, 256,
              ret::option_hint(def));
}

void App::open_choice(const ret::OptionDef &def)
{
    std::string cur;
    bool present = false;
    if (drafting_) {
        const auto it = draft_vals_.find(def.name);
        if (it != draft_vals_.end()) {
            cur = it->second;
            present = !cur.empty();
        }
    } else if (const ret::Iface *i = current_iface()) {
        cur = i->value(def.name, &present);
    }
    if (def.kind == ret::OptKind::Bool) cur = ret::bool_text(cur);
    choices_.clear();
    if (def.device) {
        for (const ret::PortChoice &p : ret::list_ports()) choices_.push_back({p.label, p.path, p.path == cur, false});
        if (choices_.empty()) choices_.push_back({"No port found: plug the device in", "", false, true});
        choices_.push_back({"Type a path...", kTyped, false, false});
    } else if (def.kind == ret::OptKind::Bool) {
        choices_.push_back({"Yes", "yes", cur == "yes", false});
        choices_.push_back({"No", "no", cur == "no", false});
    } else {
        for (const std::string &c : def.choices) choices_.push_back({ret::value_label(def.name, c), c, c == cur, false});
    }
    if (!def.required && present) choices_.push_back({def.has_default ? "Clear (use the default)" : "Clear", "", false, false});
    edit_ = Edit::Option;
    push_view(ret::View::Choice);
    render();
}

void App::apply_choice(const Choice &c)
{
    if (c.value == kTyped) {
        /* swap this list for the text editor: one level, so Esc comes back to the form */
        const int sel = saved_sel_.empty() ? 0 : saved_sel_.back();
        if (!saved_sel_.empty()) saved_sel_.pop_back();
        nav_.back();
        std::string cur;
        bool present = false;
        if (drafting_) {
            const auto it = draft_vals_.find(edit_def_.name);
            if (it != draft_vals_.end()) {
                cur = it->second;
                present = !cur.empty();
            }
        } else if (const ret::Iface *i = current_iface()) {
            cur = i->value(edit_def_.name, &present);
        }
        open_text(Edit::Option, edit_title_, edit_title_, cur, cp0::ui::TextType::Text, 256,
                  "The path of the device, for example /dev/ttyACM0.");
        if (!saved_sel_.empty()) saved_sel_.back() = sel;
        return;
    }
    apply_option(c.value, c.value.empty());
}

void App::apply_option(const std::string &value, bool clear)
{
    if (drafting_) {
        if (clear || value.empty()) draft_vals_.erase(edit_def_.name);
        else draft_vals_[edit_def_.name] = value;
        back();
        return;
    }
    const ret::Iface *i = current_iface();
    if (!i || busy_) return;
    busy_ = true;
    Value opts = obj();
    opts.set(edit_def_.name, clear ? Value() : Value::string(value));
    Value p = obj();
    p.set("name", Value::string(i->name));
    p.set("options", opts);
    const bool text_page = nav_.current() == ret::View::Option;
    link_->request("iface.edit", p, 25.0, [this, text_page](const ret::Response &r) {
        busy_ = false;
        if (r.ok) {
            std::fprintf(stderr, "[reticulum] option changed\n"); /* never the value: it may be a secret */
            request_ifaces([this] {
                if (nav_.current() == ret::View::Option || nav_.current() == ret::View::Choice) back();
                else refresh_view();
            });
            return;
        }
        if (text_page && r.transport) text_error(ret::plain_error(r));
        else request_failed(r, "Could not change it");
    });
}

/* ------------------------------------------------------------------ the text page */

void App::open_text(Edit what, const std::string &title, const std::string &label, const std::string &initial,
                    cp0::ui::TextType type, std::size_t max_bytes, const std::string &hint)
{
    edit_ = what;
    edit_title_ = title;
    field_error_ = false;
    push_view(what == Edit::Option ? ret::View::Option : ret::View::Name);
    set_title();
    clear_view();

    cp0::ui::TextFieldSpec spec;
    spec.label = label;
    spec.type = type;
    spec.text = initial;
    spec.max_bytes = max_bytes;
    spec.label_w = layout_.roller_w * kLabelColumnPct / 100;
    if (const lv_font_t *font = cp0_fonts().get("Montserrat-Medium.ttf",
                                                 static_cast<uint16_t>(cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY]))) {
        lv_point_t size{};
        lv_text_get_size(&size, spec.label.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        spec.label_w = std::max(spec.label_w, static_cast<int>(size.x) + layout_.gap);
    }
    cp0::ui::TextFieldEvents events;
    events.submit = [this](const std::string &text) { post([this, text] { submit_text(text); }); };
    events.changed = [this](const std::string &text) {
        /* the address page counts the digits as they are typed */
        if (edit_ == Edit::ContactAddress && form_status_) {
            /* after a refusal (Enter on a short address) the red line stays until the next key: then the count is back and
             * follows every character (board QA of 0.3.2, D6: it kept "3 so far" while 31 were typed) */
            if (field_error_) {
                field_error_ = false;
                lv_obj_set_style_text_color(form_status_, lv_color_hex(CP0_UI_LIST_SECTION_RGB), 0);
            }
            const std::string line = std::to_string(ret::address_digits(text)) +
                                     " of 32 characters so far. Type the address your contact gave you, or paste it.";
            lv_label_set_text(form_status_, line.c_str());
        }
    };
    events.back = [this] { post([this] { back(); }); };
    field_ = std::make_unique<cp0::ui::TextField>(page_->body(), layout_, std::move(spec), std::move(events));

    form_status_ = lv_label_create(page_->body());
    const lv_font_t *cap = cp0_ui_font_px(layout_.caption_px);
    lv_label_set_long_mode(form_status_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(form_status_, cap, 0);
    lv_obj_set_width(form_status_, layout_.roller_w);
    lv_obj_set_height(form_status_, 2 * lv_font_get_line_height(cap));
    lv_obj_set_style_pad_top(page_->body(), 0, 0);
    lv_label_set_text(form_status_, hint.c_str());
    lv_obj_set_style_text_color(form_status_, lv_color_hex(CP0_UI_LIST_SECTION_RGB), 0);
    focus(field_->focus_object()); /* focus starts editing: the on-screen keyboard shows when the rule allows it */
}

void App::text_error(const std::string &message)
{
    if (!form_status_ || !field_) return;
    lv_label_set_text(form_status_, message.c_str());
    field_error_ = true;
    lv_obj_set_style_text_color(form_status_, lv_color_hex(CP0_UI_LIST_BAD_RGB), 0);
    lv_obj_t *body = page_->body();
    lv_obj_update_layout(body);
    lv_obj_scroll_to_view(form_status_, LV_ANIM_OFF);
    lv_obj_scroll_to_view(field_->row(), LV_ANIM_OFF);
    if (!field_->editing()) field_->begin_edit();
}

void App::submit_text(const std::string &raw)
{
    if (!field_ || busy_) return;
    const std::string text = ret::trim_text(raw);
    switch (edit_) {
    case Edit::Option:
        if (text.empty()) {
            if (edit_def_.required) text_error("Enter a value, or press Esc to leave it as it is.");
            else apply_option("", true);
        } else {
            apply_option(text, false);
        }
        break;
    case Edit::DraftName:
        if (text.empty()) {
            text_error("Give it a name.");
            return;
        }
        draft_name_ = text;
        back();
        break;
    case Edit::ContactName:
        draft_cname_ = text;
        back();
        break;
    case Edit::ContactAddress: {
        if (text.empty()) {
            draft_caddr_.clear();
            back();
            break;
        }
        bool valid = false;
        const std::string addr = ret::normalize_address(text, &valid);
        if (!valid) {
            text_error("An address is 32 letters and digits (" + std::to_string(ret::address_digits(text)) + " so far). Spaces are fine.");
            break;
        }
        draft_caddr_ = addr;
        back();
        break;
    }
    case Edit::RenameContact: {
        busy_ = true;
        Value p = obj();
        p.set("address", Value::string(cur_addr_));
        p.set("name", Value::string(text));
        link_->request("contacts.rename", p, 10.0, [this](const ret::Response &r) {
            busy_ = false;
            if (r.ok) {
                std::fprintf(stderr, "[reticulum] contact renamed\n");
                request_contacts();
                request_convs();
                if (nav_.current() == ret::View::Name) back();
            } else if (r.transport) {
                text_error(ret::plain_error(r));
            } else {
                request_failed(r, "Could not rename it");
            }
        });
        break;
    }
    case Edit::DisplayName: {
        busy_ = true;
        Value p = obj();
        p.set("name", Value::string(text));
        link_->request("identity.name", p, 10.0, [this](const ret::Response &r) {
            busy_ = false;
            if (r.ok) {
                deck_.display_name = r.result["display_name"].as_string();
                std::fprintf(stderr, "[reticulum] name %s\n", deck_.display_name.empty() ? "cleared" : "set");
                if (nav_.current() == ret::View::Name) back();
            } else if (r.transport) {
                text_error(ret::plain_error(r));
            } else {
                request_failed(r, "Could not save the name");
            }
        });
        break;
    }
    default: break;
    }
}

void App::request_failed(const ret::Response &r, const std::string &title)
{
    if (!r.transport && (r.code == "no_service" || r.code == "lost")) {
        on_link_lost(r);
        return;
    }
    notice(title, ret::plain_error(r));
}

/* ------------------------------------------------------------------ dialogs */

/* The dialog's own input group and its timer go (also when the app ends with a question open); the keyboard is the page's again. */
void App::release_dialog_keys()
{
    dialog_buttons_.clear();
    if (dialog_timer_) {
        lv_timer_delete(dialog_timer_);
        dialog_timer_ = nullptr;
    }
    if (dialog_group_) {
        if (dialog_indev_) lv_indev_set_group(dialog_indev_, saved_group_);
        lv_group_delete(dialog_group_);
        dialog_group_ = nullptr;
    }
    dialog_indev_ = nullptr;
    saved_group_ = nullptr;
}

void App::close_dialog()
{
    if (!dialog_) return;
    release_dialog_keys();
    dialog_.reset();
    if (field_) focus(field_->focus_object());
    else if (list_) focus(list_->focus_object());
    else if (rich_) focus(rich_->focus_object());
    else if (convo_.built && convo_.msgs) focus(convo_.msgs);
    else if (text_) focus(text_->focus_object());
}

/* The dialog toolkit leaves the buttons to touch and gives Enter to the primary one. A question that deletes something
 * or lets the deck send must not be answered yes by a stray Enter (QA of 0.3.0, F4), so here the buttons are in the input
 * group with a visible focus ring: the focus starts on `first` (Cancel for a question), Left / Up / Shift-Tab and
 * Right / Down / Tab move between the buttons (the arrows stop at the ends), Enter presses the focused one, Esc cancels. */
void App::arm_dialog(std::size_t first)
{
    dialog_buttons_.clear();
    if (!dialog_) return;
    lv_obj_t *box = dialog_->focus_object();
    for (uint32_t k = 0; k < lv_obj_get_child_count(box); ++k) {
        lv_obj_t *child = lv_obj_get_child(box, static_cast<int32_t>(k));
        if (lv_obj_check_type(child, &lv_button_class)) dialog_buttons_.push_back(child);
    }
    /* The dialog is modal for the keys: its buttons are the only members of a group of their own while it is open, so that
     * LVGL's own Next / Previous keys go round the buttons and never to the page behind it. */
    lv_group_t *group = lv_group_create(); // the dialog's own
    saved_group_ = cdk::input_group();
    for (lv_indev_t *dev = lv_indev_get_next(nullptr); dev; dev = lv_indev_get_next(dev))
        if (lv_indev_get_type(dev) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_group(dev) == saved_group_) dialog_indev_ = dev;
    if (dialog_indev_) {
        dialog_group_ = group;
    } else { /* the keyboard device is not found: the buttons join the page's group, as before */
        lv_group_delete(group);
        group = saved_group_;
        saved_group_ = nullptr;
    }
    for (lv_obj_t *b : dialog_buttons_) {
        /* the ring on every focus state: the theme draws its own, thinner one for a key focus on the main (blue) button */
        for (const lv_style_selector_t sel : {static_cast<lv_style_selector_t>(LV_STATE_FOCUSED),
                                              static_cast<lv_style_selector_t>(LV_STATE_FOCUS_KEY),
                                              static_cast<lv_style_selector_t>(LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY)}) {
            lv_obj_set_style_outline_width(b, 3, sel);
            lv_obj_set_style_outline_pad(b, 1, sel);
            lv_obj_set_style_outline_color(b, lv_color_hex(0xF0B400), sel);
            lv_obj_set_style_outline_opa(b, LV_OPA_COVER, sel);
        }
        if (group) lv_group_add_obj(group, b);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *ev) {
                auto *self = static_cast<App *>(lv_event_get_user_data(ev));
                lv_obj_t *cur = lv_event_get_current_target_obj(ev);
                lv_event_stop_processing(ev);
                const uint32_t key = lv_event_get_key(ev);
                const std::vector<lv_obj_t *> &btns = self->dialog_buttons_;
                std::size_t at = 0;
                for (std::size_t n = 0; n < btns.size(); ++n)
                    if (btns[n] == cur) at = n;
                /* The arrows stop at the ends: Left from Cancel stays on Cancel (it must never land on the main button,
                 * where the Enter that follows would confirm). Tab goes round. */
                auto go = [&](std::ptrdiff_t step, bool wrap) {
                    if (btns.size() < 2) return;
                    const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(btns.size());
                    std::ptrdiff_t to = static_cast<std::ptrdiff_t>(at) + step;
                    if (wrap) to = (to + n) % n;
                    else to = std::max<std::ptrdiff_t>(0, std::min<std::ptrdiff_t>(n - 1, to));
                    lv_group_focus_obj(btns[static_cast<std::size_t>(to)]);
                };
                switch (key) {
                case LV_KEY_LEFT:
                case LV_KEY_UP: go(-1, false); break;
                case LV_KEY_RIGHT:
                case LV_KEY_DOWN: go(1, false); break;
                case kKeyTab: go(1, true); break; /* Tab goes round (Shift-Tab arrives as the same key) */
                case LV_KEY_ENTER: lv_obj_send_event(cur, LV_EVENT_CLICKED, nullptr); break;
                case LV_KEY_ESC: self->post([self] { if (self->dialog_) self->dialog_->cancel(); }); break;
                default: break; /* a letter typed with a question open does nothing */
                }
            },
            LV_EVENT_KEY, this);
    }
    dialog_first_ = first;
    /* The Enter that opened the dialog is usually still down: its release would land on the button that has the focus now
     * (a click on Cancel closes the question at once, on the main button it would confirm). So the buttons take the
     * keyboard only when the key is up; until then the keys still go to the page under the dialog, which ignores them
     * while a question is open. */
    auto engage = [](lv_timer_t *t) {
        auto *self = static_cast<App *>(lv_timer_get_user_data(t));
        const bool up = self->dialog_indev_ ? lv_indev_get_state(self->dialog_indev_) == LV_INDEV_STATE_RELEASED
                                            : lv_tick_elaps(self->dialog_opened_tick_) > 150;
        if (!up) return;
        self->dialog_timer_ = nullptr;
        lv_timer_delete(t);
        if (!self->dialog_ || self->dialog_buttons_.empty()) return;
        if (self->dialog_group_ && self->dialog_indev_) lv_indev_set_group(self->dialog_indev_, self->dialog_group_);
        lv_group_focus_obj(self->dialog_buttons_[std::min(self->dialog_first_, self->dialog_buttons_.size() - 1)]);
    };
    dialog_opened_tick_ = lv_tick_get();
    dialog_timer_ = lv_timer_create(engage, 20, this);
}

void App::confirm(const std::string &title, const std::string &text, const std::string &yes, std::function<void()> on_yes)
{
    close_dialog();
    dialog_done_ = false;
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] {
                                if (dialog_done_) return;
                                dialog_done_ = true;
                                post([this] { close_dialog(); });
                            }});
    auto fn = std::make_shared<std::function<void()>>(std::move(on_yes));
    spec.buttons.push_back({yes, cp0::ui::ButtonRole::Primary, [this, fn] {
                                if (dialog_done_) return;
                                dialog_done_ = true;
                                post([this, fn] {
                                    close_dialog();
                                    if (*fn) (*fn)();
                                });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    if (!text.empty()) dialog_->add_text(text);
    dialog_->relayout();
    arm_dialog(0); /* the focus starts on Cancel: Enter alone never confirms */
}

void App::notice(const std::string &title, const std::string &text)
{
    close_dialog();
    dialog_done_ = false;
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({"OK", cp0::ui::ButtonRole::Primary, [this] {
                                if (dialog_done_) return;
                                dialog_done_ = true;
                                post([this] { close_dialog(); });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    if (!text.empty()) dialog_->add_text(text);
    dialog_->relayout();
    arm_dialog(0);
}

} // namespace retui
