/*
 * SPDX-License-Identifier: MIT
 *
 * The Reticulum app, phase P2: Messages, Contacts, Heard recently, My address and Announce myself (see ret_app.hpp). The
 * conversation page itself is ret_convo.cpp. Every change goes to the bridge through the Link; nothing transmits unless
 * the user pressed Send, Retry or the announcement's confirmation (the three requests that carry the user's action).
 */
#include "ret_app.hpp"

#include "ret_ids.hpp"

#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace retui {

namespace {

using cp0::ui::ListRow;
using cp0::ui::RowAffordance;
using cp0::ui::ValueTone;
using ret::json::Value;

ValueTone vt(ret::Tone t)
{
    switch (t) {
    case ret::Tone::Ok: return ValueTone::Ok;
    case ret::Tone::Warn: return ValueTone::Warn;
    case ret::Tone::Bad: return ValueTone::Bad;
    case ret::Tone::Muted: return ValueTone::Muted;
    default: return ValueTone::Normal;
    }
}

double wall()
{
    return static_cast<double>(std::time(nullptr));
}

Value obj()
{
    return Value::object();
}

constexpr const char *kPasteDefault = "wl-paste -n 2>/dev/null || xclip -o -selection clipboard 2>/dev/null || xsel -ob 2>/dev/null";

} // namespace

/* ------------------------------------------------------------------ small answers */

const ret::Contact *App::current_contact() const
{
    for (const ret::Contact &c : contacts_)
        if (c.address == cur_addr_) return &c;
    return nullptr;
}

const ret::Conversation *App::current_conversation() const
{
    for (const ret::Conversation &c : convs_)
        if (c.address == cur_addr_) return &c;
    return nullptr;
}

std::string App::current_name() const
{
    if (const ret::Contact *c = current_contact()) return c->display();
    if (const ret::Conversation *c = current_conversation()) return c->display();
    for (const ret::Person &p : people_)
        if (p.address == cur_addr_) return p.display();
    return "Anonymous";
}

std::string App::footer_for(ret::View v) const
{
    switch (v) {
    case ret::View::Messages:
        return keep_known_ && !keep_running_ ? "You only get new messages while this app is open. Turn on Keep running in Settings."
                                              : std::string();
    case ret::View::Contacts:
        if (pick_) return "Choose who to write to.";
        return contacts_.empty() ? "A contact is a name and an address. Ask a friend for theirs, or add someone you heard."
                                 : "Tap a contact to open it. Nothing is sent until you send a message.";
    case ret::View::Contact: return "Removing is only on this deck.";
    case ret::View::AddContact: return "An address is 32 letters and digits, like 329a 4f1c 8be2 77d0 and so on. Spaces are fine.";
    case ret::View::Heard:  // vocab-ok: the owner's button word
        return "Tap a white dot to add that person.";
    case ret::View::Address:
        return "Until you announce, people who have not got your address will not see you.";  // vocab-ok: the owner's button word
    case ret::View::AnnounceBlocked: return "Nothing was sent. Your name and address stay on this deck.";
    case ret::View::ConvOptions: return "Deleting is only on this deck. Nothing is sent to the other side.";
    default: return std::string();
    }
}

/* ------------------------------------------------------------------ rows */

std::vector<ListRow> App::rows_contacts() const
{
    std::vector<ListRow> rows;
    const double now = wall();
    rows.push_back(ListRow::section("Your contacts"));
    int n = 0;
    for (const ret::Contact &c : contacts_) {
        const ret::State st = ret::contact_state(c, now);
        rows.push_back(ListRow::item(c.display(), st.word, vt(st.tone), kContactRow + n, RowAffordance::Open));
        ++n;
    }
    if (contacts_.empty()) rows.push_back(ListRow::item("No contacts yet", "", ValueTone::Muted, kNone, RowAffordance::Info));
    rows.push_back(ListRow::section("Add"));
    rows.push_back(ListRow::item("Add a contact", "", ValueTone::Normal, kAddContact, RowAffordance::Action));
    if (!pick_) {
        const std::string heard = !people_known_ || people_.empty() ? "nobody yet" : std::to_string(people_.size());
        rows.push_back(ListRow::item("Heard recently", heard, ValueTone::Muted, kHeard, RowAffordance::Open));
        rows.push_back(ListRow::section("Me"));
        rows.push_back(ListRow::item("My address", deck_.address.empty() ? "not available" : ret::short_address(deck_.address),
                                     vt(deck_.address.empty() ? ret::Tone::Muted : ret::Tone::Ok), kAddress, RowAffordance::Open));
    }
    return rows;
}

std::vector<ListRow> App::rows_contact() const
{
    std::vector<ListRow> rows;
    const ret::Contact *c = current_contact();
    if (!c) return rows;
    rows.push_back(ListRow::section("Contact"));
    rows.push_back(ListRow::item("Name", c->name.empty() ? "not set" : c->name, c->name.empty() ? ValueTone::Muted : ValueTone::Normal,
                                 kNone, RowAffordance::Info));
    rows.push_back(ListRow::item("Address", ret::short_address(c->address), ValueTone::Normal, kNone, RowAffordance::Info));
    rows.push_back(ListRow::section("Actions"));
    rows.push_back(ListRow::item("Send a message", "", ValueTone::Normal, kSendMsg, RowAffordance::Open));
    rows.push_back(ListRow::item("Rename", "", ValueTone::Normal, kRenameContact, RowAffordance::Action));
    rows.push_back(ListRow::item("Remove contact", "", ValueTone::Normal, kRemoveContact, RowAffordance::Action));
    return rows;
}

std::vector<ListRow> App::rows_add_contact() const
{
    std::vector<ListRow> rows;
    bool valid = false;
    const std::string addr = ret::normalize_address(draft_caddr_, &valid);
    rows.push_back(ListRow::section("New contact"));
    rows.push_back(ListRow::item("Name", draft_cname_.empty() ? "optional" : draft_cname_,
                                 draft_cname_.empty() ? ValueTone::Muted : ValueTone::Normal, kContactName, RowAffordance::Edit));
    std::string av = "not set";
    ValueTone tone = ValueTone::Warn;
    if (!addr.empty()) {
        av = valid ? ret::short_address(addr) : std::to_string(ret::address_digits(addr)) + " of 32";
        tone = valid ? ValueTone::Ok : ValueTone::Warn;
    }
    rows.push_back(ListRow::item("Address", av, tone, kContactAddress, RowAffordance::Edit));
    rows.push_back(ListRow::item("Paste address", "", ValueTone::Normal, kPaste, RowAffordance::Action));
    rows.push_back(ListRow::section("Save"));
    if (valid) rows.push_back(ListRow::item("Save contact", "", ValueTone::Normal, kSaveContact, RowAffordance::Action));
    else rows.push_back(ListRow::item("Save contact", "needs an address", ValueTone::Muted, kNone, RowAffordance::Info));
    return rows;
}

std::vector<ListRow> App::rows_conv_options() const
{
    std::vector<ListRow> rows;
    rows.push_back(ListRow::section("Conversation"));
    const ret::Contact *c = current_contact();
    if (c) rows.push_back(ListRow::item("Contact", c->display(), ValueTone::Normal, kConvContact, RowAffordance::Open));
    else rows.push_back(ListRow::item("Add to contacts", "", ValueTone::Normal, kConvContact, RowAffordance::Action));
    bool failed = false;
    for (const ret::Message &m : msgs_)
        if (m.out() && m.state == "failed") failed = true;
    if (failed) rows.push_back(ListRow::item("Send the failed messages again", "", ValueTone::Normal, kRetryFailed, RowAffordance::Action));
    rows.push_back(ListRow::item("Delete conversation", "", ValueTone::Normal, kDeleteConv, RowAffordance::Action));
    return rows;
}

std::vector<RichRow> App::rich_messages() const
{
    std::vector<RichRow> rows;
    const double now = wall();
    rows.push_back(RichRow::section("Conversations"));
    int n = 0;
    for (const ret::Conversation &c : convs_) {
        RichRow r;
        r.kind = RichRow::Kind::Two;
        r.label = c.display();
        r.time = ret::list_time(now, c.last_time);
        const ret::ConvLine l = ret::conversation_line(c);
        r.sub = l.text;
        r.sub_tone = vt(l.tone);
        r.bold = c.unread > 0;
        r.badge = c.unread;
        r.affordance = RowAffordance::Open;
        r.id = kConvRow + n;
        rows.push_back(r);
        ++n;
    }
    if (convs_.empty())
        rows.push_back(RichRow::item(convs_known_ ? "No conversations yet" : "Loading...", "", ValueTone::Muted, kNone,
                                     RowAffordance::Info));
    rows.push_back(RichRow::section("New"));
    rows.push_back(RichRow::item("New message", "", ValueTone::Normal, kNewMessage, RowAffordance::Action));
    return rows;
}

std::vector<RichRow> App::rich_heard() const
{
    std::vector<RichRow> rows;
    const double now = wall();
    rows.push_back(RichRow::section("Heard on your connections"));
    int n = 0;
    for (const ret::Person &p : people_) {
        RichRow r;
        r.kind = RichRow::Kind::Two;
        r.label = p.display();
        r.time = ret::ago_text(std::max(0.0, now - p.time));
        r.sub = ret::person_line(p);
        r.sub_tone = ValueTone::Muted;
        r.affordance = p.contact ? RowAffordance::Info : RowAffordance::Action;
        r.id = p.contact ? kNone : kPerson + n;
        rows.push_back(r);
        ++n;
    }
    if (people_.empty())
        rows.push_back(RichRow::item(people_known_ ? "Nobody yet" : "Loading...", "", ValueTone::Muted, kNone, RowAffordance::Info));
    rows.push_back(RichRow::item("Announce myself", "", ValueTone::Normal, kAnnounce, RowAffordance::Action));  // vocab-ok: the owner's button word
    return rows;
}

std::vector<RichRow> App::rich_address() const
{
    std::vector<RichRow> rows;
    rows.push_back(RichRow::section("Name"));
    rows.push_back(RichRow::item("Name", deck_.display_name.empty() ? "not set" : deck_.display_name,
                                 deck_.display_name.empty() ? ValueTone::Warn : ValueTone::Normal, kDisplayName, RowAffordance::Edit));
    rows.push_back(RichRow::section("Your address"));
    RichRow b;
    b.kind = RichRow::Kind::Block;
    b.label = deck_.address.empty() ? "not available" : ret::spaced_address(deck_.address);
    rows.push_back(b);
    rows.push_back(RichRow::item("Copy address", "", ValueTone::Normal, kCopyAddress, RowAffordance::Action));
    rows.push_back(RichRow::item("Announce myself", "", ValueTone::Normal, kAnnounce, RowAffordance::Action));  // vocab-ok: the owner's button word
    return rows;
}

std::vector<RichRow> App::rich_announce_blocked() const
{
    std::vector<RichRow> rows;
    RichRow p;
    p.kind = RichRow::Kind::Block;
    p.panel = true;
    p.label = "No connection is allowed to send. Switch one on in Connections.";
    rows.push_back(p);
    rows.push_back(RichRow::section("Go to"));
    rows.push_back(RichRow::item("Connections", ret::connections_value(ifaces_), ValueTone::Muted, kGoConnections, RowAffordance::Open));
    return rows;
}

/* ------------------------------------------------------------------ what a row does */

void App::activate_rich(std::intptr_t id)
{
    if (id == kNone) return;
    switch (nav_.current()) {
    case ret::View::Messages:
        if (id >= kConvRow && id < kConvRow + 1000) {
            const std::size_t idx = static_cast<std::size_t>(id - kConvRow);
            if (idx < convs_.size()) open_conversation(convs_[idx].address);
        } else if (id == kNewMessage) {
            open_contacts(true);
        }
        break;
    case ret::View::Heard: activate_heard(id); break;
    case ret::View::Address:
        if (id == kDisplayName)
            open_text(Edit::DisplayName, "Name", "Name", deck_.display_name, cp0::ui::TextType::Text, 4 * 64,
                      "The name others see next to your address. Saving it sends nothing.");
        else if (id == kCopyAddress) copy_address();
        else if (id == kAnnounce) start_announce();
        break;
    case ret::View::AnnounceBlocked:
        if (id == kGoConnections) {
            /* Connections is the page to open; this page is replaced by it (Esc goes back to My address) */
            nav_.back();
            if (!saved_sel_.empty()) saved_sel_.pop_back();
            open(ret::View::Connections);
        }
        break;
    default: break;
    }
}

void App::activate_heard(std::intptr_t id)
{
    if (id == kAnnounce) {
        start_announce();
        return;
    }
    if (id >= kPerson && id < kPerson + 1000) {
        const std::size_t idx = static_cast<std::size_t>(id - kPerson);
        if (idx < people_.size() && !people_[idx].contact) add_person(people_[idx]);
    }
}

void App::activate_contacts(std::intptr_t id)
{
    if (id >= kContactRow && id < kContactRow + 1000) {
        const std::size_t idx = static_cast<std::size_t>(id - kContactRow);
        if (idx >= contacts_.size()) return;
        if (pick_) {
            const std::string address = contacts_[idx].address;
            /* the conversation takes this page's place: Esc goes back to Messages */
            nav_.back();
            if (!saved_sel_.empty()) saved_sel_.pop_back();
            pick_ = false;
            open_conversation(address);
        } else {
            open_contact(contacts_[idx].address);
        }
    } else if (id == kAddContact) {
        begin_add_contact();
    } else if (id == kHeard) {
        request_people();
        open(ret::View::Heard);
    } else if (id == kAddress) {
        if (deck_.address.empty()) notice("No address yet", "The service has not given this deck an address.");
        else open_address();
    }
}

void App::activate_contact(std::intptr_t id)
{
    const ret::Contact *c = current_contact();
    if (!c) return;
    if (id == kSendMsg) {
        /* a conversation already open under this page: go back to it instead of stacking a second one */
        if (nav_.has(ret::View::Conversation)) {
            while (nav_.current() != ret::View::Conversation) {
                nav_.back();
                if (!saved_sel_.empty()) saved_sel_.pop_back();
            }
            render();
            return;
        }
        open_conversation(c->address);
    } else if (id == kRenameContact) {
        open_text(Edit::RenameContact, "Rename", "Name", c->name, cp0::ui::TextType::Text, 4 * 64,
                  "How this contact is shown on this deck. Nothing is sent.");
    } else if (id == kRemoveContact) {
        confirm_remove_contact();
    }
}

void App::activate_add_contact(std::intptr_t id)
{
    if (id == kContactName) {
        open_text(Edit::ContactName, "Name", "Name", draft_cname_, cp0::ui::TextType::Text, 4 * 64,
                  "Optional. How this contact is shown on this deck.");
    } else if (id == kContactAddress) {
        const std::string hint = std::to_string(ret::address_digits(draft_caddr_)) +
                                 " of 32 characters so far. Type the address your contact gave you, or paste it.";
        open_text(Edit::ContactAddress, "Address", "Address", draft_caddr_, cp0::ui::TextType::Text, 256, hint);
    } else if (id == kPaste) {
        paste_address();
    } else if (id == kSaveContact) {
        save_contact();
    }
}

void App::activate_conv_options(std::intptr_t id)
{
    if (id == kConvContact) {
        if (current_contact()) {
            open_contact(cur_addr_);
        } else {
            add_person([&] {
                ret::Person p;
                p.address = cur_addr_;
                p.name = current_name() == "Anonymous" ? std::string() : current_name();
                return p;
            }());
        }
    } else if (id == kRetryFailed) {
        retry_all_failed();
    } else if (id == kDeleteConv) {
        confirm_delete_conversation();
    }
}

/* ------------------------------------------------------------------ opening pages */

void App::open_contacts(bool pick)
{
    pick_ = pick;
    request_contacts();
    request_people();
    open(ret::View::Contacts);
}

void App::open_contact(const std::string &address)
{
    cur_addr_ = address;
    open(ret::View::Contact);
}

void App::open_conversation(const std::string &address)
{
    cur_addr_ = address;
    msgs_.clear();
    msgs_more_ = false;
    send_blocked_ = false;
    push_view(ret::View::Conversation);
    render(); /* build_conversation(): the title, the bubbles from the history, the compose row */
}

void App::open_conv_options()
{
    open(ret::View::ConvOptions);
}

void App::begin_add_contact()
{
    draft_cname_.clear();
    draft_caddr_.clear();
    open(ret::View::AddContact);
}

/* ------------------------------------------------------------------ changes */

void App::save_contact()
{
    if (busy_) return;
    bool valid = false;
    const std::string addr = ret::normalize_address(draft_caddr_, &valid);
    if (!valid) {
        notice("Not an address yet", "An address is 32 letters and digits. Spaces are fine.");
        return;
    }
    Value p = obj();
    p.set("address", Value::string(addr));
    if (!draft_cname_.empty()) p.set("name", Value::string(draft_cname_));
    busy_ = true;
    link_->request("contacts.add", p, 10.0, [this, addr](const ret::Response &r) {
        busy_ = false;
        if (!r.ok) {
            if (r.transport && r.code == "exists") notice("Already a contact", "That address is already in your contacts.");
            else request_failed(r, "Could not save the contact");
            return;
        }
        std::fprintf(stderr, "[reticulum] contact added\n");
        request_contacts();
        request_people();
        /* back to the Contacts list, with the new one in it */
        if (nav_.current() == ret::View::AddContact) {
            nav_.back();
            if (!saved_sel_.empty()) saved_sel_.pop_back();
            render();
        }
    });
}

void App::add_person(const ret::Person &p)
{
    if (busy_) return;
    Value params = obj();
    params.set("address", Value::string(p.address));
    busy_ = true;
    const std::string address = p.address;
    link_->request("contacts.add", params, 10.0, [this, address](const ret::Response &r) {
        busy_ = false;
        if (!r.ok && !(r.transport && r.code == "exists")) {
            request_failed(r, "Could not add the contact");
            return;
        }
        for (ret::Person &q : people_)
            if (q.address == address) q.contact = true;
        std::fprintf(stderr, "[reticulum] contact added from the heard list\n");
        request_contacts();
        request_people();
        refresh_view();
    });
}

void App::confirm_remove_contact()
{
    const ret::Contact *c = current_contact();
    if (!c) return;
    const std::string address = c->address, name = c->display();
    confirm("Remove " + name + "?",
            "This removes " + name + " and your messages with " + name + " from this deck. It happens only on this deck, "
            "nothing is sent to the other side. You can add " + name + " again with the address.",
            "Remove", [this, address] {
                busy_ = true;
                Value p = obj();
                p.set("address", Value::string(address));
                p.set("keep_conversation", Value::boolean(false));
                link_->request("contacts.remove", p, 10.0, [this](const ret::Response &r) {
                    busy_ = false;
                    if (!r.ok) {
                        request_failed(r, "Could not remove the contact");
                        return;
                    }
                    std::fprintf(stderr, "[reticulum] contact removed\n");
                    request_contacts();
                    request_convs();
                    request_people();
                    /* the contact's page and any conversation under it are gone: back to the list that held it */
                    while (nav_.depth() > 0 && (nav_.current() == ret::View::Contact || nav_.current() == ret::View::Conversation ||
                                                 nav_.current() == ret::View::ConvOptions)) {
                        nav_.back();
                        if (!saved_sel_.empty()) saved_sel_.pop_back();
                    }
                    render();
                });
            });
}

void App::confirm_delete_conversation()
{
    const std::string address = cur_addr_, name = current_name();
    const bool contact = current_contact() != nullptr;
    confirm("Delete conversation?",
            "This deletes all messages with " + name + " from this deck. It happens only on this deck, nothing is sent to the "
            "other side." + (contact ? " " + name + " stays in your contacts." : std::string()),
            "Delete", [this, address] {
                busy_ = true;
                Value p = obj();
                p.set("address", Value::string(address));
                link_->request("messages.delete_conversation", p, 10.0, [this](const ret::Response &r) {
                    busy_ = false;
                    if (!r.ok) {
                        request_failed(r, "Could not delete the conversation");
                        return;
                    }
                    std::fprintf(stderr, "[reticulum] conversation deleted\n");
                    msgs_.clear();
                    draft_text_.clear();
                    request_convs();
                    leave_deleted_conversation();
                });
            });
}

/* The conversation (and its options) are gone: back to the page under them. */
void App::leave_deleted_conversation()
{
    if (!nav_.has(ret::View::Conversation)) return;
    close_dialog();
    while (nav_.depth() > 0 && nav_.current() != ret::View::Messages && nav_.current() != ret::View::Contacts &&
           nav_.current() != ret::View::Contact) {
        nav_.back();
        if (!saved_sel_.empty()) saved_sel_.pop_back();
        if (nav_.current() == ret::View::Home) break;
    }
    render();
}

/* ------------------------------------------------------------------ My address and Announce myself */

void App::copy_address()
{
    if (deck_.address.empty()) return;
    clip_ = deck_.address;
    /* the system clipboard, where this machine has one (best effort: a Wayland or X session on a desktop board) */
    cdk::async_run::Spec spec;
    spec.argv = {"sh", "-c", "printf %s " + deck_.address + " | (wl-copy || xclip -selection clipboard || xsel -ib) 2>/dev/null"};
    spec.timeout_ms = 3000;
    spec.on_done = [](const cdk::async_run::Result &) {};
    cdk::async_run::run(std::move(spec));
    notice("Address copied", "Your address is copied, where this deck has a clipboard:\n\n" + ret::spaced_address(deck_.address));
}

void App::paste_address()
{
    const char *over = std::getenv("RETICULUM_PASTE_CMD"); /* a test hook, never set on a deck */
    cdk::async_run::Spec spec;
    spec.argv = {"sh", "-c", over ? over : kPasteDefault};
    spec.timeout_ms = 3000;
    auto out = std::make_shared<std::string>();
    spec.on_line = [out](const std::string &line) {
        if (out->size() < 512) *out += line + " ";
    };
    spec.on_done = [this, out](const cdk::async_run::Result &) {
        bool valid = false;
        const std::string addr = ret::normalize_address(*out, &valid);
        if (valid) {
            draft_caddr_ = addr;
            refresh_view();
        } else {
            notice("Nothing to paste", "There is no address on the clipboard of this deck. Type it instead: it is 32 letters and digits.");
        }
    };
    cdk::async_run::run(std::move(spec));
}

void App::start_announce()  // vocab-ok: the owner's button word
{
    if (busy_) return;
    if (deck_.display_name.empty()) {
        notice("Set a name first", "Others see your name next to your address. Choose a name, then try again.");
        return;
    }
    if (ret::nothing_may_send(ifaces_)) {
        open(ret::View::AnnounceBlocked);
        return;
    }
    confirm_announce();
}

void App::confirm_announce()  // vocab-ok: the owner's button word
{
    const std::string via = ret::sending_names(ifaces_);
    confirm("Announce myself?",  // vocab-ok: the owner's words
            "This sends your name (" + deck_.display_name + ") and your address to everyone who can hear the connections you "
            "allowed to send: " + via + ". It transmits right now and cannot be taken back. People who hear it can add you "
            "to their contacts.",
            "Announce", [this] { do_announce(); });  // vocab-ok: the owner's button word
}

/* Only the confirmation of the dialog above gets here: the one request that makes this deck announce itself. */
void App::do_announce()
{
    busy_ = true;
    link_->request(
        "announce", obj(), 20.0,  // vocab-ok: the bridge's verb
        [this](const ret::Response &r) {
            busy_ = false;
            if (r.ok) {
                std::fprintf(stderr, "[reticulum] sent by the user\n");
                notice("Sent", "Your name and address were sent.");
            } else if (r.transport && r.code == "no_transmit") {
                open(ret::View::AnnounceBlocked);
            } else {
                request_failed(r, "Could not send it");
            }
        },
        /*user_action=*/true);
}

void App::set_alerts(bool on)
{
    if (busy_) return;
    busy_ = true;
    Value p = obj();
    p.set("alerts", Value::boolean(on));
    link_->request("settings.set", p, 8.0, [this](const ret::Response &r) {
        busy_ = false;
        if (r.ok) {
            deck_.alerts = r.result["alerts"].as_bool(false);
            std::fprintf(stderr, "[reticulum] alerts %s\n", deck_.alerts ? "on" : "off");
            refresh_view();
        } else {
            request_failed(r, "Could not change it");
        }
    });
}

/* ------------------------------------------------------------------ data from the bridge */

void App::request_contacts()
{
    contacts_busy_ = true;
    contacts_dirty_ = false;
    link_->request("contacts.list", obj(), 8.0, [this](const ret::Response &r) {
        contacts_busy_ = false;
        if (svc_ != Svc::Up) return;
        if (r.ok && ret::parse_contacts(r.result, contacts_)) contacts_known_ = true;
        chat_changed();
    });
}

void App::request_convs()
{
    convs_busy_ = true;
    convs_dirty_ = false;
    last_convs_ms_ = now_ms();
    link_->request("messages.conversations", obj(), 8.0, [this](const ret::Response &r) {
        convs_busy_ = false;
        if (svc_ != Svc::Up) return;
        int total = 0;
        if (r.ok && ret::parse_conversations(r.result, convs_, &total)) {
            convs_known_ = true;
            deck_.unread = total;
            unread_seen_ = total;
        }
        chat_changed();
    });
}

void App::request_people()
{
    Value p = obj();
    p.set("limit", Value::number(200));
    link_->request("heard.list", p, 8.0, [this](const ret::Response &r) {
        if (svc_ != Svc::Up) return;
        if (r.ok && ret::parse_people(r.result, people_)) people_known_ = true;
        chat_changed();
    });
}

void App::request_limits()
{
    link_->request("messages.limits", obj(), 5.0, [this](const ret::Response &r) {
        if (r.ok) ret::parse_limits(r.result, limits_);
        if (svc_ == Svc::Up && nav_.current() == ret::View::Conversation) update_compose();
    });
}

void App::request_all_chat()
{
    if (!limits_.loaded) request_limits();
    request_contacts();
    request_convs();
    request_people();
}

/* The data under the visible page changed: the page follows in place. */
void App::chat_changed()
{
    refresh_view();
}

void App::mark_read(const std::string &address)
{
    Value p = obj();
    p.set("address", Value::string(address));
    link_->request("messages.mark_read", p, 8.0, [this](const ret::Response &r) {
        if (r.ok) {
            deck_.unread = static_cast<int>(r.result["unread_total"].as_int(deck_.unread));
            unread_seen_ = deck_.unread;
            convs_dirty_ = true;
        }
    });
}

void App::handle_chat_event(const ret::json::Value &ev)
{
    const std::string name = ev["event"].as_string();
    const Value &d = ev["data"];
    if (name == "message") {
        ret::Message m;
        if (!ret::parse_message(d, m)) return;
        deck_.unread = static_cast<int>(d["unread_total"].as_int(deck_.unread));
        unread_seen_ = deck_.unread;
        convs_dirty_ = true;
        if (m.address == cur_addr_ && nav_.has(ret::View::Conversation)) {
            ret::merge_message(msgs_, m);
            if (nav_.current() == ret::View::Conversation) {
                if (!m.out() && m.unread) mark_read(m.address);
                show_messages();
                update_compose();
            }
        }
    } else if (name == "message_state") {
        const std::int64_t id = d["id"].as_int(0);
        if (ret::apply_state(msgs_, id, d["state"].as_string(), d["detail"].as_string(),
                             d["delivered_at"].as_number(0)) &&
            nav_.current() == ret::View::Conversation) {
            show_messages();
            update_compose();
        }
        convs_dirty_ = true;
    } else if (name == "unread") {
        deck_.unread = static_cast<int>(d["unread_total"].as_int(deck_.unread));
        unread_seen_ = deck_.unread;
        convs_dirty_ = true;
    } else if (name == "conversation_deleted") {
        convs_dirty_ = true;
        if (d["address"].as_string() == cur_addr_ && nav_.has(ret::View::Conversation)) {
            msgs_.clear();
            leave_deleted_conversation();
        }
    } else if (name == "contacts_changed") {
        contacts_dirty_ = true;
        convs_dirty_ = true;
    } else if (name == "heard") {
        ret::Person p;
        if (ret::parse_person(d, p)) {
            ret::merge_person(people_, p);
            people_known_ = true;
            if (nav_.current() == ret::View::Heard) refresh_view();
        }
    }
}

} // namespace retui
