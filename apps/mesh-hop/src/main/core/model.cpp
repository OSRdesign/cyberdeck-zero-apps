/*
 * SPDX-License-Identifier: MIT
 */

#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace meshzero {

const char *msg_state_name(MsgState s)
{
    switch (s) {
    case MsgState::Received: return "received";
    case MsgState::Pending: return "sending";
    case MsgState::Sent: return "sent";
    case MsgState::Delivered: return "delivered";
    case MsgState::Failed: return "failed";
    case MsgState::NoAck: return "no ack";
    }
    return "";
}

KeyPrefix ContactRec::prefix() const
{
    KeyPrefix p{};
    std::copy_n(c.key.begin(), 6, p.begin());
    return p;
}

uint32_t ContactRec::last_heard() const
{
    if (heard_local) return heard_local;
    if (c.lastmod) return c.lastmod;
    return c.last_advert;
}

bool RadioSettings::same_radio(const RadioSettings &o) const
{
    return std::fabs(freq_mhz - o.freq_mhz) < 0.0005 && std::fabs(bw_khz - o.bw_khz) < 0.0005 && sf == o.sf &&
           cr == o.cr && tx_power == o.tx_power;
}

ChannelKind ChannelRec::kind() const
{
    if (has_secret) return classify_channel(name, secret);
    return !name.empty() && name[0] == '#' ? ChannelKind::Hashtag : name == "Public" ? ChannelKind::Public : ChannelKind::Private;
}

RetrySettings normalize_retry(RetrySettings r)
{
    r.attempts = std::clamp(r.attempts, 1, kMaxRetryAttempts);
    r.reset_after = std::clamp(r.reset_after, 0, r.attempts - 1);
    return r;
}

const std::vector<double> &lora_bandwidths()
{
    static const std::vector<double> v = {7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125.0, 250.0, 500.0};
    return v;
}

std::string validate_radio(const RadioSettings &s)
{
    if (s.freq_mhz < 137.0 || s.freq_mhz > 2500.0) return "Frequency must be 137 to 2500 MHz";
    bool bw_ok = false;
    for (double bw : lora_bandwidths())
        if (std::fabs(bw - s.bw_khz) < 0.05) bw_ok = true;
    if (!bw_ok) return "Bandwidth is not a LoRa value";
    if (s.sf < 5 || s.sf > 12) return "Spreading factor must be 5 to 12";
    if (s.cr < 5 || s.cr > 8) return "Coding rate must be 5 to 8";
    const int max_power = s.max_tx_power > 0 ? s.max_tx_power : 30;
    if (s.tx_power < -9 || s.tx_power > max_power) return "TX power out of range for this radio";
    return "";
}

std::string validate_name(const std::string &name)
{
    if (name.empty()) return "The name cannot be empty";
    if (name.size() > kMaxNameBytes) return "The name is limited to 31 bytes";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F) return "The name has a control character";
    return "";
}

/* ---------------------------------------------------------------- device */

void Model::set_self(const SelfInfo &s)
{
    select_board(s.key);            // another identity: the previous board's data goes, the new board's saved data comes (before self_ is set)
    self_ = s;
    ++revision_;
}

void Model::reset_board_scoped()
{
    contacts_.clear();
    channels_.clear();
    added_.clear();
    messages_.clear();
    read_.clear();
    muted_.clear();
    ignored_.clear();
    nearby_.clear();
    groups_ = ContactGroups();
    schedule_ = AdvertSchedule();
    stats_ = StatsSnapshot();
    batch_ = batch_dirty_ = false;
    next_seq_ = 1;
}

bool Model::select_board(const PubKey &key)
{
    const std::string hex = to_hex(key);
    if (hex == board_key_) return false;
    reset_board_scoped();
    board_key_ = hex;
    ++board_epoch_;
    ++revision_;
    if (listener_) listener_->board_changed(hex);
    ++revision_;
    return true;
}

void Model::forget_board_app_data()
{
    messages_.clear();
    read_.clear();
    muted_.clear();
    ignored_.clear();
    groups_ = ContactGroups();
    schedule_ = AdvertSchedule();
    ++revision_;
    if (listener_) listener_->board_data_forgotten();
}

void Model::set_device(const DeviceInfo &d)
{
    device_ = d;
    ++revision_;
}

void Model::set_battery(const Battery &b)
{
    battery_ = b;
    ++revision_;
}

void Model::clear_device()
{
    self_.reset();
    device_.reset();
    battery_.reset();
    caps_ = BoardCaps();
    clock_ = ClockInfo();
    stats_ = StatsSnapshot();
    ++revision_;
}

const std::vector<int> &advert_interval_options()
{
    static const std::vector<int> v = {0, 1, 3, 6, 12};
    return v;
}

AdvertSchedule normalize_schedule(AdvertSchedule s)
{
    const auto &opts = advert_interval_options();
    if (std::find(opts.begin(), opts.end(), s.interval_hours) == opts.end()) s.interval_hours = 0;
    return s;
}

void Model::set_advert_schedule(AdvertSchedule s)
{
    s = normalize_schedule(s);
    if (s == schedule_) return;
    schedule_ = s;
    ++revision_;
    if (listener_) listener_->prefs_changed();
}

void Model::groups_changed()
{
    ++revision_;
    if (listener_) listener_->groups_changed();
}

void Model::set_stats(const StatsReply &s, uint32_t when)
{
    switch (s.kind) {
    case StatsReply::Core: stats_.core = s.core; break;
    case StatsReply::Radio: stats_.radio = s.radio; break;
    case StatsReply::Packets: stats_.packets = s.packets; break;
    }
    stats_.updated = when;
    ++revision_;
}

std::string Model::self_name() const
{
    return self_ ? self_->name : std::string();
}

RadioSettings Model::radio_settings() const
{
    RadioSettings r;
    if (!self_) return r;
    r.name = self_->name;
    r.freq_mhz = self_->freq_mhz();
    r.bw_khz = self_->bw_khz();
    r.sf = self_->sf;
    r.cr = self_->cr;
    r.tx_power = self_->tx_power;
    r.max_tx_power = self_->max_tx_power;
    return r;
}

/* ---------------------------------------------------------------- contacts */

void Model::replace_contacts(const std::vector<Contact> &list)
{
    std::map<std::string, ContactRec> next;
    for (const Contact &c : list) {
        ContactRec rec;
        const auto old = contacts_.find(to_hex(c.key));
        if (old != contacts_.end()) rec = old->second;      // keep what only the deck knows (heard time, SNR)
        rec.c = c;
        next[to_hex(c.key)] = rec;
    }
    contacts_.swap(next);
    ++revision_;
    refresh_pending();
    notify_contacts();
}

void Model::notify_contacts()
{
    if (batch_) {
        batch_dirty_ = true;
        return;
    }
    if (listener_) listener_->contacts_changed();
}

void Model::end_contact_batch()
{
    const bool dirty = batch_dirty_;
    batch_ = batch_dirty_ = false;
    if (dirty && listener_) listener_->contacts_changed();
}

void Model::upsert_contact(const Contact &c)
{
    ContactRec &rec = contacts_[to_hex(c.key)];
    rec.c = c;
    ++revision_;
    refresh_pending();
    notify_contacts();
}

void Model::remove_contact(const PubKey &key)
{
    if (contacts_.erase(to_hex(key)) == 0) return;
    ++revision_;
    notify_contacts();
}

void Model::heard(const PubKey &key, uint32_t when)
{
    const auto it = contacts_.find(to_hex(key));
    if (it == contacts_.end()) return;
    it->second.heard_local = when;
    ++revision_;
    notify_contacts();
}

void Model::note_snr(const KeyPrefix &prefix, double snr, uint32_t when)
{
    for (auto &kv : contacts_) {
        if (!std::equal(prefix.begin(), prefix.end(), kv.second.c.key.begin())) continue;
        kv.second.has_snr = true;
        kv.second.snr = snr;
        kv.second.heard_local = when;
        ++revision_;
        notify_contacts();
        return;
    }
}

/* ---------------------------------------------------------------- nearby nodes */

void Model::refresh_pending()
{
    for (auto &kv : nearby_)
        if (kv.second.pending && contacts_.count(kv.first)) kv.second.pending = false;
}

void Model::note_nearby(const NearbyObs &in, uint32_t when)
{
    NearbyObs obs = in;
    if (obs.key_hex.size() != 64 && obs.key_hex.size() != 16) return;
    if (obs.key_hex.size() == 16) {                          // a prefix: resolve it against what is known
        obs.full_key = false;
        for (const auto &kv : contacts_)
            if (kv.first.compare(0, 16, obs.key_hex) == 0) {
                obs.key_hex = kv.first;
                obs.full_key = true;
                break;
            }
        if (!obs.full_key)
            for (const auto &kv : nearby_)
                if (kv.second.full_key && kv.first.compare(0, 16, obs.key_hex) == 0) {
                    obs.key_hex = kv.first;
                    obs.full_key = true;
                    break;
                }
    } else {
        obs.full_key = true;
    }
    auto it = nearby_.find(obs.key_hex);
    if (it == nearby_.end()) {
        if (nearby_.size() >= kMaxNearby) {                  // drop the one heard longest ago (never a pending one while others exist)
            auto victim = nearby_.end();
            for (auto j = nearby_.begin(); j != nearby_.end(); ++j) {
                if (victim == nearby_.end() || (victim->second.pending && !j->second.pending) ||
                    (victim->second.pending == j->second.pending && j->second.last_heard < victim->second.last_heard))
                    victim = j;
            }
            if (victim != nearby_.end()) nearby_.erase(victim);
        }
        NearbyRec r;
        r.key_hex = obs.key_hex;
        r.full_key = obs.full_key;
        if (r.full_key) from_hex(r.key_hex, r.key.data(), 32);
        r.first_heard = when;
        it = nearby_.emplace(obs.key_hex, r).first;
    }
    NearbyRec &r = it->second;
    if (obs.type != 0) r.type = obs.type;
    if (!obs.name.empty()) r.name = obs.name;
    if (obs.has_pos) {
        r.has_pos = true;
        r.lat = obs.lat;
        r.lon = obs.lon;
    }
    if (obs.has_snr) {
        r.has_snr = true;
        r.snr = obs.snr;
    }
    if (obs.has_rssi) {
        r.has_rssi = true;
        r.rssi = obs.rssi;
    }
    if (obs.hops >= 0) {
        r.hops = obs.hops;
        if (obs.hash_size > 0) r.hash_size = obs.hash_size;
    }
    if (obs.discovered) {
        r.discovered = true;
        r.hops = 0;
    }
    r.last_heard = when;                                    // no revision bump: an advert in the radio log must not rebuild the contact table; the Nearby panel redraws every second
}

void Model::note_heard_advert(const HeardAdvert &h, uint32_t when)
{
    NearbyObs o;
    o.key_hex = to_hex(h.key);
    o.type = h.type;
    o.name = h.name;
    o.has_pos = h.has_pos;
    o.lat = h.lat;
    o.lon = h.lon;
    o.has_snr = true;
    o.snr = h.snr;
    o.has_rssi = true;
    o.rssi = h.rssi;
    o.hops = h.hops;
    o.hash_size = h.hash_size;
    note_nearby(o, when);
}

void Model::add_pending(const Contact &c, uint32_t when)
{
    NearbyObs o;
    o.key_hex = to_hex(c.key);
    o.type = c.type;
    o.name = c.name;
    o.has_pos = !(c.lat == 0 && c.lon == 0);
    o.lat = c.lat;
    o.lon = c.lon;
    note_nearby(o, when);
    const auto it = nearby_.find(o.key_hex);
    if (it == nearby_.end()) return;
    it->second.pending = contacts_.count(o.key_hex) == 0;
    it->second.has_contact = true;
    it->second.contact = c;
}

const NearbyRec *Model::find_nearby(const std::string &key_hex) const
{
    const auto it = nearby_.find(key_hex);
    return it == nearby_.end() ? nullptr : &it->second;
}

std::vector<const NearbyRec *> Model::nearby_sorted() const
{
    std::vector<const NearbyRec *> v;
    v.reserve(nearby_.size());
    for (const auto &kv : nearby_) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const NearbyRec *a, const NearbyRec *b) {
        if (a->pending != b->pending) return a->pending;
        if (a->last_heard != b->last_heard) return a->last_heard > b->last_heard;
        return a->key_hex < b->key_hex;
    });
    return v;
}

size_t Model::pending_count() const
{
    size_t n = 0;
    for (const auto &kv : nearby_)
        if (kv.second.pending && !is_ignored(kv.first)) ++n;
    return n;
}

void Model::remove_nearby(const std::string &key_hex)
{
    if (nearby_.erase(key_hex)) ++revision_;
}

void Model::clear_nearby()
{
    if (nearby_.empty()) return;
    nearby_.clear();
    ++revision_;
}

void Model::set_ignored(const std::string &key_hex, bool ignored)
{
    if (ignored) {
        if (ignored_.count(key_hex)) return;
        if (ignored_.size() >= kMaxIgnored) ignored_.erase(ignored_.begin());
        ignored_.insert(key_hex);
    } else if (ignored_.erase(key_hex) == 0) {
        return;
    }
    ++revision_;
    if (listener_) listener_->prefs_changed();
}

void Model::clear_board_data()
{
    contacts_.clear();
    channels_.clear();
    nearby_.clear();
    ++revision_;
    if (listener_) {
        listener_->contacts_changed();
        listener_->channels_changed();
    }
}

void Model::load_contact(const ContactRec &rec)
{
    contacts_[rec.key_hex()] = rec;
    ++revision_;
}

const ContactRec *Model::find_contact(const std::string &key_hex) const
{
    const auto it = contacts_.find(key_hex);
    return it == contacts_.end() ? nullptr : &it->second;
}

const ContactRec *Model::find_by_prefix(const KeyPrefix &prefix) const
{
    for (const auto &kv : contacts_)
        if (std::equal(prefix.begin(), prefix.end(), kv.second.c.key.begin())) return &kv.second;
    return nullptr;
}

std::vector<const ContactRec *> Model::contacts_sorted() const
{
    std::vector<const ContactRec *> v;
    v.reserve(contacts_.size());
    for (const auto &kv : contacts_) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const ContactRec *a, const ContactRec *b) {
        const uint32_t ha = a->last_heard(), hb = b->last_heard();
        if (ha != hb) return ha > hb;
        return a->c.name < b->c.name;
    });
    return v;
}

/* ---------------------------------------------------------------- channels */

void Model::set_channel(const ChannelRec &c)
{
    load_channel(c);
    if (listener_) listener_->channels_changed();
}

void Model::load_channel(const ChannelRec &in)
{
    ChannelRec c = in;
    c.app_added = !c.empty && added_.count(c.name) != 0;
    auto it = std::find_if(channels_.begin(), channels_.end(), [&](const ChannelRec &x) { return x.idx == c.idx; });
    if (it == channels_.end()) channels_.push_back(c);
    else *it = c;
    std::sort(channels_.begin(), channels_.end(), [](const ChannelRec &a, const ChannelRec &b) { return a.idx < b.idx; });
    ++revision_;
}

const ChannelRec *Model::find_channel(int idx) const
{
    for (const auto &c : channels_)
        if (c.idx == idx) return &c;
    return nullptr;
}

void Model::mark_added(const std::string &name)
{
    added_.insert(name);
    ++revision_;
    if (listener_) listener_->channels_changed();
}

void Model::unmark_added(const std::string &name)
{
    if (added_.erase(name) == 0) return;
    ++revision_;
    if (listener_) listener_->channels_changed();
}

int Model::reconcile_added()
{
    std::vector<std::string> drop;
    for (const std::string &n : added_) {
        const ChannelRec *c = find_channel_by_name(n);
        if (!c || !c->key_ok) drop.push_back(n);
    }
    for (const std::string &n : drop) added_.erase(n);
    if (!drop.empty()) {
        for (ChannelRec &c : channels_) c.app_added = !c.empty && added_.count(c.name) != 0;
        ++revision_;
        if (listener_) listener_->channels_changed();
    }
    return static_cast<int>(drop.size());
}

int Model::free_channel_slot() const
{
    for (const ChannelRec &c : channels_)
        if (c.idx >= 1 && c.empty) return c.idx;
    return -1;
}

const ChannelRec *Model::find_channel_by_name(const std::string &name) const
{
    for (const ChannelRec &c : channels_)
        if (!c.empty && c.name == name) return &c;
    return nullptr;
}

std::string Model::channel_title(int idx) const
{
    const ChannelRec *c = find_channel(idx);
    if (c && !c->name.empty()) return c->name;
    return "Channel " + std::to_string(idx);
}

/* ---------------------------------------------------------------- conversations */

std::string Model::conv_direct(const KeyPrefix &prefix)
{
    return "d:" + to_hex(prefix);
}

std::string Model::conv_channel(int idx)
{
    return "c:" + std::to_string(idx);
}

uint32_t Model::add_incoming(const IncomingMessage &m, uint32_t local_time)
{
    Message msg;
    msg.dir = Dir::In;
    msg.state = MsgState::Received;
    msg.ts = local_time;
    msg.sender_ts = m.sender_timestamp;
    msg.has_snr = m.has_snr;
    msg.snr = m.snr;
    msg.hops = path_hops(m.path_len);
    msg.text = m.text;
    if (m.channel) {
        msg.conv = conv_channel(m.channel_idx);
        // The firmware puts the sender's name in front of a channel message: "Name: text".
        const size_t colon = m.text.find(": ");
        if (colon != std::string::npos && colon > 0 && colon <= 40) {
            msg.sender = m.text.substr(0, colon);
            msg.text = m.text.substr(colon + 2);
        } else {
            msg.sender = "?";
        }
    } else {
        msg.conv = conv_direct(m.prefix);
        const ContactRec *c = find_by_prefix(m.prefix);
        msg.sender = c && !c->c.name.empty() ? c->c.name : to_hex(m.prefix);
    }
    // the same message twice (a re-sync after a reconnect): same sender clock and text within the last minutes
    int looked = 0;
    for (auto it = messages_.rbegin(); it != messages_.rend() && looked < 60; ++it, ++looked) {
        if (it->conv == msg.conv && it->dir == Dir::In && it->sender_ts == msg.sender_ts && it->text == msg.text &&
            local_time - it->ts < 600)
            return 0;
    }
    msg.seq = next_seq_++;
    messages_.push_back(msg);
    if (!m.channel && m.has_snr) note_snr(m.prefix, m.snr, local_time);
    else if (!m.channel) {
        const ContactRec *c = find_by_prefix(m.prefix);
        if (c) heard(c->c.key, local_time);
    }
    trim(msg.conv);
    ++revision_;
    if (listener_) listener_->message_added(msg);
    return msg.seq;
}

uint32_t Model::add_outgoing(const std::string &conv, const std::string &text, uint32_t local_time, int hops)
{
    Message msg;
    msg.seq = next_seq_++;
    msg.conv = conv;
    msg.dir = Dir::Out;
    msg.state = MsgState::Pending;
    msg.ts = local_time;
    msg.sender_ts = local_time;
    msg.sender = "You";
    msg.text = text;
    msg.hops = hops;
    messages_.push_back(msg);
    trim(conv);
    ++revision_;
    if (listener_) listener_->message_added(msg);
    return msg.seq;
}

void Model::load_message(const Message &m)
{
    messages_.push_back(m);
    if (m.seq >= next_seq_) next_seq_ = m.seq + 1;
    trim(m.conv);
    ++revision_;
}

void Model::set_state(uint32_t seq, MsgState state, const std::string &note)
{
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->seq != seq) continue;
        if (it->state == state && it->note == note) return;
        it->state = state;
        it->note = note;
        ++revision_;
        if (listener_) listener_->message_state(seq, state, note);
        return;
    }
}

void Model::set_heard_back(uint32_t seq, int count, bool final)
{
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->seq != seq) continue;
        if (it->heard_back != count) {
            it->heard_back = count;
            ++revision_;
        }
        if (final && listener_) listener_->message_heard_back(seq, count);
        return;
    }
}

const Message *Model::find_message(uint32_t seq) const
{
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
        if (it->seq == seq) return &*it;
    return nullptr;
}

std::vector<const Message *> Model::messages(const std::string &conv) const
{
    std::vector<const Message *> v;
    for (const Message &m : messages_)
        if (m.conv == conv) v.push_back(&m);
    return v;
}

std::string Model::conv_title(const std::string &conv) const
{
    if (conv.rfind("c:", 0) == 0) return channel_title(std::atoi(conv.c_str() + 2));
    KeyPrefix p{};
    if (conv.size() == 14 && from_hex(conv.substr(2), p.data(), 6)) {
        const ContactRec *c = find_by_prefix(p);
        if (c && !c->c.name.empty()) return c->c.name;
    }
    return conv.size() > 2 ? conv.substr(2) : conv;
}

int Model::unread(const std::string &conv) const
{
    return is_muted(conv) ? 0 : unread_raw(conv);
}

int Model::unread_raw(const std::string &conv) const
{
    const auto r = read_.find(conv);
    const uint32_t mark = r == read_.end() ? 0 : r->second;
    int n = 0;
    for (const Message &m : messages_)
        if (m.conv == conv && m.dir == Dir::In && m.seq > mark) ++n;
    return n;
}

int Model::unread_total() const
{
    int n = 0;
    for (const Message &m : messages_) {
        if (m.dir != Dir::In || is_muted(m.conv)) continue;
        const auto r = read_.find(m.conv);
        if (m.seq > (r == read_.end() ? 0 : r->second)) ++n;
    }
    return n;
}

void Model::mark_read(const std::string &conv)
{
    uint32_t top = 0;
    for (const Message &m : messages_)
        if (m.conv == conv && m.dir == Dir::In) top = std::max(top, m.seq);
    const auto r = read_.find(conv);
    if ((r == read_.end() ? 0 : r->second) >= top) return;
    read_[conv] = top;
    ++revision_;
    if (listener_) listener_->read_changed();
}

std::vector<ConvSummary> Model::conversations() const
{
    std::map<std::string, ConvSummary> by_key;
    for (const Message &m : messages_) {
        ConvSummary &s = by_key[m.conv];
        s.key = m.conv;
        s.last_ts = m.ts;
        s.last_text = m.dir == Dir::Out ? "You: " + m.text : m.text;
    }
    for (const ChannelRec &c : channels_) {
        if (c.empty) continue;
        ConvSummary &s = by_key[conv_channel(c.idx)];
        s.key = conv_channel(c.idx);
    }
    std::vector<ConvSummary> chans, directs;
    for (auto &kv : by_key) {
        ConvSummary &s = kv.second;
        s.channel = s.key.rfind("c:", 0) == 0;
        s.channel_idx = s.channel ? std::atoi(s.key.c_str() + 2) : -1;
        s.title = conv_title(s.key);
        s.unread = unread(s.key);
        s.muted = is_muted(s.key);
        (s.channel ? chans : directs).push_back(s);
    }
    std::sort(chans.begin(), chans.end(),
              [](const ConvSummary &a, const ConvSummary &b) { return a.channel_idx < b.channel_idx; });
    std::sort(directs.begin(), directs.end(), [](const ConvSummary &a, const ConvSummary &b) {
        if (a.last_ts != b.last_ts) return a.last_ts > b.last_ts;
        return a.title < b.title;
    });
    chans.insert(chans.end(), directs.begin(), directs.end());
    return chans;
}

/* ---------------------------------------------------------------- deleting history */

size_t Model::message_count_in(const std::string &conv) const
{
    size_t n = 0;
    for (const Message &m : messages_)
        if (m.conv == conv) ++n;
    return n;
}

size_t Model::delete_conversation(const std::string &conv)
{
    const size_t before = messages_.size();
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&](const Message &m) { return m.conv == conv; }), messages_.end());
    const size_t removed = before - messages_.size();
    const bool had_mark = read_.erase(conv) != 0;
    if (removed || had_mark) ++revision_;
    if (listener_) {
        if (removed) listener_->messages_removed();
        if (had_mark) listener_->read_changed();
    }
    return removed;
}

size_t Model::delete_all_messages()
{
    const size_t removed = messages_.size();
    const bool had_marks = !read_.empty();
    messages_.clear();
    read_.clear();
    if (removed || had_marks) ++revision_;
    if (listener_) {
        if (removed) listener_->messages_removed();
        if (had_marks) listener_->read_changed();
    }
    return removed;
}

size_t Model::count_older_than(uint32_t cutoff) const
{
    size_t n = 0;
    for (const Message &m : messages_)
        if (m.ts >= kMinPlausibleTime && m.ts < cutoff) ++n;
    return n;
}

size_t Model::delete_older_than(uint32_t cutoff)
{
    const size_t before = messages_.size();
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&](const Message &m) { return m.ts >= kMinPlausibleTime && m.ts < cutoff; }),
                    messages_.end());
    const size_t removed = before - messages_.size();
    if (removed) {
        ++revision_;
        if (listener_) listener_->messages_removed();
    }
    return removed;
}

void Model::set_muted(const std::string &conv, bool muted)
{
    const bool was = is_muted(conv);
    if (muted == was) return;
    if (muted) muted_.insert(conv);
    else muted_.erase(conv);
    ++revision_;
    if (listener_) listener_->prefs_changed();
}

void Model::set_retry(RetrySettings r)
{
    r = normalize_retry(r);
    if (r.attempts == retry_.attempts && r.reset_after == retry_.reset_after) return;
    retry_ = r;
    ++revision_;
    if (listener_) listener_->prefs_changed();
}

void Model::trim(const std::string &conv)
{
    size_t in_conv = 0;
    for (const Message &m : messages_)
        if (m.conv == conv) ++in_conv;
    while (in_conv > kMaxPerConversation) {
        const auto it = std::find_if(messages_.begin(), messages_.end(), [&](const Message &m) { return m.conv == conv; });
        if (it == messages_.end()) break;
        messages_.erase(it);
        --in_conv;
    }
    if (messages_.size() > kMaxMessages) messages_.erase(messages_.begin(), messages_.begin() + static_cast<long>(messages_.size() - kMaxMessages));
}

} // namespace meshzero
