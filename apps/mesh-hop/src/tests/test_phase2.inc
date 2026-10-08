// Phase 2 (0.2.0) core tests, included by test_core.cpp (it shares the fake board and the helpers defined there).
// Protocol frames and parsers, path hash size, contacts (bulk delete, Nearby, discover, manual add, auto-add), the board (reboot, factory reset,
// repeat, statistics), contact groups, the message archive, the advert schedule and the preset download schedule.

#include "archive.hpp"
#include "groups.hpp"

/* ---------------------------------------------------------------- helpers */

static Contact fake_contact(uint8_t seed, const std::string &name, uint8_t type, uint8_t path_len = 0xFF)
{
    return std::get<Contact>(*parse_packet(make_contact(resp::kContact, seed, name, type, 1790000000, path_len)));
}

static PubKey key_of(uint8_t seed)
{
    PubKey k{};
    for (int i = 0; i < 32; ++i) k[static_cast<size_t>(i)] = static_cast<uint8_t>(seed + i);
    return k;
}

/* The on-air ADVERT packet of the radio log: header (route type in the low bits, payload type 4), optional transport codes, path length byte,
 * path, key, timestamp, signature, appdata. */
static Bytes make_advert_packet(uint8_t seed, const std::string &name, uint8_t type, bool pos, int hops, int hash_size, uint8_t route = 1)
{
    Bytes p;
    p.push_back(static_cast<uint8_t>((4 << 2) | route));
    if (route == 0 || route == 3) put32(p, 0x11223344);
    p.push_back(static_cast<uint8_t>(((hash_size - 1) << 6) | hops));
    for (int i = 0; i < hops * hash_size; ++i) p.push_back(static_cast<uint8_t>(0x30 + i));
    const PubKey k = key_of(seed);
    p.insert(p.end(), k.begin(), k.end());
    put32(p, 1790000123);
    for (int i = 0; i < 64; ++i) p.push_back(0xEE);
    uint8_t flags = type | (name.empty() ? 0 : 0x80) | (pos ? 0x10 : 0);
    p.push_back(flags);
    if (pos) { put32(p, static_cast<uint32_t>(48870000)); put32(p, static_cast<uint32_t>(2360000)); }
    p.insert(p.end(), name.begin(), name.end());
    return p;
}

static Bytes make_log(const Bytes &packet, double snr, int rssi)
{
    Bytes b = {resp::kLogData, static_cast<uint8_t>(static_cast<int8_t>(snr * 4)), static_cast<uint8_t>(static_cast<int8_t>(rssi))};
    b.insert(b.end(), packet.begin(), packet.end());
    return b;
}

struct CountListener : Listener {
    int contacts = 0, groups = 0, prefs = 0, channels = 0;
    void message_added(const Message &) override {}
    void message_state(uint32_t, MsgState, const std::string &) override {}
    void contacts_changed() override { ++contacts; }
    void channels_changed() override { ++channels; }
    void read_changed() override {}
    void prefs_changed() override { ++prefs; }
    void groups_changed() override { ++groups; }
};

static Bytes last_cmd(const FakePeer &p, uint8_t code)
{
    for (auto it = p.commands.rbegin(); it != p.commands.rend(); ++it)
        if ((*it)[0] == code) return *it;
    return {};
}

/* ---------------------------------------------------------------- frames and parsers */

static void test_p2_protocol()
{
    PubKey k = key_of(0x40);
    CHECK(build_remove_contact(k).size() == 33 && build_remove_contact(k)[0] == 0x0F && build_remove_contact(k)[1] == 0x40 && build_remove_contact(k)[32] == 0x5F);
    CHECK(build_reboot() == hex("13") + Bytes{'r', 'e', 'b', 'o', 'o', 't'});
    CHECK(build_factory_reset() == hex("33") + Bytes{'r', 'e', 's', 'e', 't'});
    CHECK(build_get_stats(0) == hex("3800") && build_get_stats(1) == hex("3801") && build_get_stats(2) == hex("3802"));
    CHECK(build_set_other_params(true, 0x15, 2, 1) == hex("2601150201"));
    CHECK(build_set_other_params(false, 0, 0, 0, false) == hex("26000000"));
    CHECK(build_set_autoadd_config(0x1E) == hex("3a1e") && build_get_autoadd_config() == hex("3b") && build_get_allowed_repeat_freq() == hex("3c"));
    CHECK(build_set_path_hash_mode(0) == hex("3d0000") && build_set_path_hash_mode(1) == hex("3d0001") && build_set_path_hash_mode(2) == hex("3d0002"));
    CHECK(build_discover_request(kDiscoverAllTypes, 0x04030201, false) == hex("3780ff01020304"));
    CHECK(build_discover_request(0x04, 0x04030201, true) == hex("37810401020304"));     // prefix only, repeaters (bit 2)
    CHECK(build_set_radio(869.525, 250.0, 11, 5, 1) == hex("0b95440d0090d003000b0501") && build_set_radio(869.525, 250.0, 11, 5, 0) == hex("0b95440d0090d003000b0500"));
    CHECK(build_set_radio(869.525, 250.0, 11, 5) == hex("0b95440d0090d003000b05"));

    // ADD_UPDATE_CONTACT: 144 bytes in the order of meshcore_py update_contact
    Contact c = fake_contact(0x20, "Alice", 1, 0x41);
    c.out_path[0] = 0xAA; c.out_path[1] = 0xBB;
    c.last_advert = 0x01020304; c.lat = 48.5; c.lon = -2.25; c.flags = 1;
    const Bytes add = build_add_update_contact(c);
    CHECK(add.size() == 144 && add[0] == 9 && add[1] == 0x20 && add[33] == 1 && add[34] == 1 && add[35] == 0x41 && add[36] == 0xAA && add[37] == 0xBB);
    CHECK(add[100] == 'A' && add[104] == 'e' && add[105] == 0 && add[131] == 0);
    CHECK(add[132] == 0x04 && add[135] == 0x01);
    {   // latitude 48.5 and longitude -2.25 times 1e6, signed, little endian
        uint32_t lat = add[136] | (add[137] << 8) | (add[138] << 16) | (static_cast<uint32_t>(add[139]) << 24);
        uint32_t lon = add[140] | (add[141] << 8) | (add[142] << 16) | (static_cast<uint32_t>(add[143]) << 24);
        CHECK(static_cast<int32_t>(lat) == 48500000 && static_cast<int32_t>(lon) == -2250000);
    }
}

static void test_p2_parsers()
{
    // statistics: core (11), radio (14), packets (26 legacy, 30 with the error counter)
    Bytes core = {resp::kStats, 0, 0x7A, 0x0F};
    put32(core, 7300);
    core.insert(core.end(), {2, 0, 1});
    auto pc = parse_packet(core);
    CHECK(pc && std::holds_alternative<StatsReply>(*pc));
    const StatsReply &sc = std::get<StatsReply>(*pc);
    CHECK(sc.kind == StatsReply::Core && sc.core.battery_mv == 3962 && sc.core.uptime_secs == 7300 && sc.core.errors == 2 && sc.core.queue_len == 1);
    Bytes radio = {resp::kStats, 1, 0x92, 0xFF, 0xA1, static_cast<uint8_t>(-6)};
    put32(radio, 61);
    put32(radio, 940);
    auto pr = parse_packet(radio);
    CHECK(pr && std::get<StatsReply>(*pr).kind == StatsReply::Radio);
    const RadioStats &rs = std::get<StatsReply>(*pr).radio;
    CHECK(rs.noise_floor == -110 && rs.last_rssi == -95 && rs.last_snr == -1.5 && rs.tx_air_secs == 61 && rs.rx_air_secs == 940);
    Bytes packets = {resp::kStats, 2};
    for (uint32_t v : {500u, 120u, 14u, 106u, 310u, 190u}) put32(packets, v);
    auto pp = parse_packet(packets);
    CHECK(pp && std::get<StatsReply>(*pp).kind == StatsReply::Packets && std::get<StatsReply>(*pp).packets.recv == 500 && !std::get<StatsReply>(*pp).packets.has_errors);
    put32(packets, 9);
    auto pp2 = parse_packet(packets);
    CHECK(pp2 && std::get<StatsReply>(*pp2).packets.has_errors && std::get<StatsReply>(*pp2).packets.recv_errors == 9 && std::get<StatsReply>(*pp2).packets.direct_rx == 190);
    CHECK(!parse_packet(Bytes{resp::kStats}).has_value() && !parse_packet(Bytes{resp::kStats, 0, 1, 2}).has_value());
    CHECK(std::holds_alternative<Unknown>(*parse_packet(Bytes{resp::kStats, 9, 0, 0, 0})));       // an unknown sub type does not break the stream

    // auto-add config: the max_hops byte exists from companion v1.14
    auto a1 = parse_packet(hex("191e"));
    CHECK(a1 && std::get<AutoaddConfig>(*a1).config == 0x1E && !std::get<AutoaddConfig>(*a1).has_max_hops);
    auto a2 = parse_packet(hex("191e03"));
    CHECK(a2 && std::get<AutoaddConfig>(*a2).has_max_hops && std::get<AutoaddConfig>(*a2).max_hops == 3);
    CHECK(!parse_packet(hex("19")).has_value());

    // allowed repeat frequencies: pairs, a zero ends the list
    Bytes rf = {resp::kAllowedRepeatFreq};
    put32(rf, 433000); put32(rf, 434000); put32(rf, 863000); put32(rf, 870000); put32(rf, 0); put32(rf, 0); put32(rf, 915000); put32(rf, 928000);
    auto prf = parse_packet(rf);
    CHECK(prf && std::get<AllowedRepeatFreq>(*prf).ranges.size() == 2 && std::get<AllowedRepeatFreq>(*prf).ranges[1].lo_khz == 863000);
    CHECK(std::get<AllowedRepeatFreq>(*parse_packet(Bytes{resp::kAllowedRepeatFreq})).ranges.empty());
    CHECK(repeat_freq_allowed(std::get<AllowedRepeatFreq>(*prf).ranges, 869.525) && repeat_freq_allowed(std::get<AllowedRepeatFreq>(*prf).ranges, 433.65));
    CHECK(!repeat_freq_allowed(std::get<AllowedRepeatFreq>(*prf).ranges, 915.0) && !repeat_freq_allowed({}, 869.525));
    CHECK(repeat_freq_allowed({{869, 870}}, 869.525) && !repeat_freq_allowed({{869, 870}}, 915.0) && repeat_ranges_text({{869, 870}}) == "869.000-870.000 MHz");   // a firmware that answers in MHz
    CHECK(repeat_freq_allowed({{869000, 869000}}, 869.0) && !repeat_freq_allowed({{869000, 869000}}, 869.525));                                           // a single frequency range
    CHECK(repeat_ranges_text(std::get<AllowedRepeatFreq>(*prf).ranges) == "433.000-434.000, 863.000-870.000 MHz" && repeat_ranges_text({}) == "none");

    // control data: a DISCOVER_RESP with a 32 byte key, with an 8 byte prefix, and one that is not a discover answer
    Bytes cd = {resp::kControlData, 22, static_cast<uint8_t>(-70), 0, 0x92, static_cast<uint8_t>(-6), 1, 2, 3, 4};
    for (int i = 0; i < 32; ++i) cd.push_back(static_cast<uint8_t>(0x60 + i));
    auto pcd = parse_packet(cd);
    CHECK(pcd && std::holds_alternative<ControlData>(*pcd));
    const ControlData &d = std::get<ControlData>(*pcd);
    CHECK(d.discover && d.node_type == 2 && d.snr == 5.5 && d.rssi == -70 && d.snr_in == -1.5 && d.tag[0] == 1 && d.tag[3] == 4 && d.pubkey.size() == 32 && d.pubkey[0] == 0x60);
    cd.resize(10 + 12);                                       // 12 key bytes: only the 8 byte prefix is taken
    CHECK(std::get<ControlData>(*parse_packet(cd)).pubkey.size() == 8);
    Bytes other = {resp::kControlData, 0, 0, 0, 0x80, 1, 2, 3, 4, 5, 6, 7, 8};                       // a DISCOVER_REQ heard on the air
    CHECK(!std::get<ControlData>(*parse_packet(other)).discover);
    CHECK(!std::get<ControlData>(*parse_packet(Bytes{resp::kControlData, 0, 0, 0})).discover);
    CHECK(!parse_packet(Bytes{resp::kControlData, 0, 0}).has_value());
    Bytes shortd = {resp::kControlData, 0, 0, 0, 0x91, 0, 1, 2, 3, 4, 9};                           // too little key
    CHECK(!std::get<ControlData>(*parse_packet(shortd)).discover);

    // the radio log keeps the packet
    auto pl = parse_packet(make_log(Bytes{1, 2, 3}, 5.5, -80));
    CHECK(pl && std::get<LogData>(*pl).snr == 5.5 && std::get<LogData>(*pl).rssi == -80 && std::get<LogData>(*pl).raw == Bytes({1, 2, 3}));

    // ADVERT packets in the log: flood with 1 byte hashes, transport flood with 2 byte hashes, zero hops, no name, no position
    auto adv = [&](const Bytes &packet, double snr = 6.25) { return parse_heard_advert(std::get<LogData>(*parse_packet(make_log(packet, snr, -88)))); };
    auto h1 = adv(make_advert_packet(0x70, "Newbie", 1, true, 2, 1));
    CHECK(h1 && h1->key == key_of(0x70) && h1->type == 1 && h1->name == "Newbie" && h1->has_pos && h1->lat > 48.86 && h1->lat < 48.88 && h1->hops == 2 && h1->hash_size == 1);
    CHECK(h1->snr == 6.25 && h1->rssi == -88 && h1->timestamp == 1790000123);
    auto h2 = adv(make_advert_packet(0x71, "Ridge", 2, false, 3, 2, 0));
    CHECK(h2 && h2->type == 2 && !h2->has_pos && h2->hops == 3 && h2->hash_size == 2 && h2->name == "Ridge");
    auto h3 = adv(make_advert_packet(0x72, "Dock", 4, false, 0, 3, 3));
    CHECK(h3 && h3->hops == 0 && h3->hash_size == 3 && h3->type == 4);
    auto h4 = adv(make_advert_packet(0x73, "", 3, false, 1, 1));
    CHECK(h4 && h4->name.empty() && h4->type == 3);
    Bytes txt = make_advert_packet(0x74, "x", 1, false, 1, 1);
    txt[0] = static_cast<uint8_t>((2 << 2) | 1);                                                      // a text message, not an advert
    CHECK(!adv(txt));
    Bytes cut = make_advert_packet(0x75, "Cut", 1, false, 1, 1);
    cut.resize(40);
    CHECK(!adv(cut) && !adv(Bytes{}) && !adv(Bytes{0x11}));
    Bytes bad_hash = make_advert_packet(0x76, "Bad", 1, false, 1, 1);
    bad_hash[1] |= 0xC0;                                                                              // hash size code 3 is reserved
    CHECK(!adv(bad_hash));
    Bytes zero_pos = make_advert_packet(0x77, "Zero", 1, true, 1, 1);
    for (size_t i = zero_pos.size() - 4 - 8 - 4; i < zero_pos.size() - 4; ++i) zero_pos[i] = 0;       // latitude and longitude 0, 0 mean "no position"
    auto hz = adv(zero_pos);
    CHECK(hz && !hz->has_pos);

    // the path hash helpers
    CHECK(path_hash_bytes(0) == 1 && path_hash_bytes(1) == 2 && path_hash_bytes(2) == 3 && path_hash_bytes(3) == 0 && path_hash_bytes(-1) == 0);
    CHECK(std::string(path_hash_text(1)) == "2 bytes" && std::string(path_hash_text(0)) == "1 byte" && std::string(path_hash_text(-1)) == "unknown");
    CHECK(path_hash_mode(0x41) == 1 && path_hops(0x41) == 1 && path_hash_mode(0x82) == 2 && path_hops(0x8A) == 10);
}

/* ---------------------------------------------------------------- the model: nearby nodes */

static void test_p2_model_nearby()
{
    set_time_source(fake_time);
    Model m;
    CountListener cl;
    m.set_listener(&cl);
    // an advert from a stranger: the log gives the key, the name, the position, the SNR and the hops
    HeardAdvert h;
    h.key = key_of(0x70); h.type = 1; h.name = "Newbie"; h.has_pos = true; h.lat = 48.87; h.lon = 2.36; h.hops = 2; h.hash_size = 2; h.snr = 4.5; h.rssi = -90;
    m.note_heard_advert(h, 1000);
    CHECK(m.nearby_count() == 1 && m.pending_count() == 0);
    const NearbyRec *n = m.find_nearby(to_hex(key_of(0x70)));
    CHECK(n && n->full_key && n->name == "Newbie" && n->type == 1 && n->has_snr && n->snr == 4.5 && n->hops == 2 && n->hash_size == 2 && n->first_heard == 1000);
    // heard again: the latest values win, what was learned stays
    HeardAdvert h2 = h;
    h2.name.clear(); h2.type = 0; h2.has_pos = false; h2.snr = -2.0; h2.hops = 0; h2.hash_size = 1;
    m.note_heard_advert(h2, 1100);
    n = m.find_nearby(to_hex(key_of(0x70)));
    CHECK(n->name == "Newbie" && n->type == 1 && n->has_pos && n->snr == -2.0 && n->hops == 0 && n->last_heard == 1100 && n->first_heard == 1000 && m.nearby_count() == 1);

    // a pending contact (manual add): the record is kept so that Add sends exactly it; a later contact with that key clears the flag
    Contact c = fake_contact(0x80, "Pend", 2);
    m.add_pending(c, 1200);
    CHECK(m.pending_count() == 1 && m.find_nearby(to_hex(c.key))->pending && m.find_nearby(to_hex(c.key))->has_contact && m.contact_count() == 0);
    auto sorted = m.nearby_sorted();
    CHECK(sorted.size() == 2 && sorted[0]->pending);                                                     // pending first
    m.set_ignored(to_hex(c.key), true);
    CHECK(m.pending_count() == 0 && m.is_ignored(to_hex(c.key)) && cl.prefs == 1);
    m.set_ignored(to_hex(c.key), true);
    CHECK(cl.prefs == 1);                                                                                // no change, no write
    m.set_ignored(to_hex(c.key), false);
    CHECK(m.pending_count() == 1);
    m.upsert_contact(c);
    CHECK(m.pending_count() == 0 && !m.find_nearby(to_hex(c.key))->pending && m.contact_count() == 1);

    // an 8 byte prefix from a discover answer: resolved against contacts, then against nodes already heard
    NearbyObs o;
    o.key_hex = to_hex(c.key.data(), 8);
    o.full_key = false;
    o.has_snr = true; o.snr = 7.0; o.discovered = true;
    m.note_nearby(o, 1300);
    CHECK(m.find_nearby(to_hex(c.key))->discovered && m.find_nearby(to_hex(c.key))->snr == 7.0 && m.find_nearby(to_hex(c.key))->hops == 0);
    NearbyObs o2;
    o2.key_hex = to_hex(key_of(0x70).data(), 8);
    o2.type = 1;
    m.note_nearby(o2, 1310);
    CHECK(m.nearby_count() == 2);                                                                        // joined the full entry of the stranger
    NearbyObs o3;
    o3.key_hex = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    o3.type = 2;
    m.note_nearby(o3, 1320);
    NearbyObs o4;
    o4.key_hex = "0011223344556677";
    o4.full_key = false;
    o4.has_snr = true; o4.snr = 1.0;
    m.note_nearby(o4, 1330);
    CHECK(m.nearby_count() == 3 && m.find_nearby("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")->has_snr);   // the prefix found its full key
    NearbyObs o5;
    o5.key_hex = "abcdef0123456789";
    o5.full_key = false;
    m.note_nearby(o5, 1340);
    CHECK(m.nearby_count() == 4 && !m.find_nearby("abcdef0123456789")->full_key);                         // an unresolved prefix is listed, not addable
    NearbyObs bad;
    bad.key_hex = "1234";
    m.note_nearby(bad, 1350);
    CHECK(m.nearby_count() == 4);                                                                        // a wrong length is dropped

    // the list is bounded: the one heard longest ago goes, a pending one stays while others exist
    Model big;
    for (size_t i = 0; i < kMaxNearby + 20; ++i) {
        HeardAdvert x;
        x.key = key_of(static_cast<uint8_t>(i));
        x.key[31] = static_cast<uint8_t>(i >> 8);
        x.key[30] = static_cast<uint8_t>(i);
        x.type = 1;
        big.note_heard_advert(x, 2000 + static_cast<uint32_t>(i));
        if (i == 3) big.add_pending(fake_contact(0xF0, "KeepMe", 1), 1);
    }
    CHECK(big.nearby_count() == kMaxNearby && big.pending_count() == 1);

    // the contact cache is written once per batch, not per contact
    Model b;
    CountListener bl;
    for (int i = 0; i < 6; ++i) b.upsert_contact(fake_contact(static_cast<uint8_t>(0x10 + i * 9), "N" + std::to_string(i), 1));
    b.set_listener(&bl);
    b.begin_contact_batch();
    for (int i = 0; i < 4; ++i) b.remove_contact(key_of(static_cast<uint8_t>(0x10 + i * 9)));
    CHECK(bl.contacts == 0 && b.contact_count() == 2);
    b.end_contact_batch();
    CHECK(bl.contacts == 1);
    b.begin_contact_batch();
    b.end_contact_batch();
    CHECK(bl.contacts == 1);                                                                             // nothing changed: nothing written

    // a factory reset empties what the board held, not the history, the groups or the preferences
    Model f;
    f.upsert_contact(fake_contact(0x10, "A", 1));
    ChannelRec ch;
    ch.idx = 1; ch.name = "#x"; ch.empty = false;
    f.set_channel(ch);
    f.add_outgoing("c:1", "hi", 1790000000);
    f.groups_mut().add_group("Crew");
    f.set_muted("c:1", true);
    f.clear_board_data();
    CHECK(f.contact_count() == 0 && f.channels().empty() && f.nearby_count() == 0 && f.message_count() == 1 && f.groups().has("Crew") && f.is_muted("c:1"));
}

/* ---------------------------------------------------------------- the client: contacts */

static void test_p2_bulk_delete()
{
    auto setup = [](FakePeer &p) {
        for (int i = 0; i < 6; ++i) p.contacts.push_back(fake_contact(static_cast<uint8_t>(0x90 + i * 11), "Node" + std::to_string(i), 1 + i % 4));
    };
    {   // one by one: the next command waits for the answer to the last
        Ready r(setup);
        CHECK(r.model.contact_count() == 8);
        std::vector<PubKey> keys;
        for (int i = 0; i < 4; ++i) keys.push_back(key_of(static_cast<uint8_t>(0x90 + i * 11)));
        r.peer.hold_remove = true;
        r.peer.commands.clear();
        CHECK(r.client->remove_contacts(keys));
        CHECK(!r.client->remove_contacts(keys) && r.client->bulk().active);                      // one job at a time
        run(*r.client, 300);
        CHECK(count_cmd(r.peer, cmd::kRemoveContact) == 1 && r.client->bulk().done == 0 && r.client->bulk().total == 4);
        r.peer.hold_remove = false;
        run(*r.client, 6000);                                                                    // the first one times out (5 s), the others go through
        CHECK(!r.client->bulk().active && r.client->bulk().done == 3 && r.client->bulk().failed == 1);
        CHECK(count_cmd(r.peer, cmd::kRemoveContact) == 4 && r.model.contact_count() == 5 && r.peer.contacts.size() == 5);
        CHECK(r.client->notice().text.find("Deleted 3 contacts, 1 failed") == 0 && !r.client->notice().ok);
        CHECK(r.client->bulk().id == 1);
    }
    {   // all good: one write of the contact cache, the groups lose the deleted members, a node already gone from the board counts as gone
        Ready r(setup);
        CountListener cl;
        r.model.set_listener(&cl);
        std::vector<PubKey> keys;
        std::vector<std::string> hexes;
        for (int i = 0; i < 4; ++i) {
            keys.push_back(key_of(static_cast<uint8_t>(0x90 + i * 11)));
            hexes.push_back(to_hex(keys.back()));
        }
        r.model.groups_mut().add_group("Crew");
        r.model.groups_mut().add_members("Crew", {hexes[0], hexes[3], to_hex(key_of(0x10))});
        r.peer.contacts.erase(r.peer.contacts.begin() + 3);                                      // keys[1] is already missing on the board (not found)
        r.peer.commands.clear();
        CHECK(r.client->remove_contacts(keys));
        run(*r.client, 500);
        CHECK(!r.client->bulk().active && r.client->bulk().done == 4 && r.client->bulk().failed == 0 && r.model.contact_count() == 4);
        CHECK(cl.contacts == 1 && cl.groups == 1);
        CHECK(r.model.groups().member_count("Crew") == 1 && r.model.groups().is_member("Crew", to_hex(key_of(0x10))));
        CHECK(r.client->notice().ok && r.client->notice().text == "Deleted 4 contacts");
        CHECK(!r.client->remove_contacts({}) && !r.client->notice().ok);
    }
    {   // stop: the one in flight finishes, the rest is not sent
        Ready r(setup);
        std::vector<PubKey> keys;
        for (int i = 0; i < 5; ++i) keys.push_back(key_of(static_cast<uint8_t>(0x90 + i * 11)));
        r.peer.commands.clear();
        CHECK(r.client->remove_contacts(keys));
        r.client->cancel_bulk();
        run(*r.client, 500);
        CHECK(!r.client->bulk().active && r.client->bulk().cancelled && r.client->bulk().done == 1 && count_cmd(r.peer, cmd::kRemoveContact) == 1);
        CHECK(r.model.contact_count() == 7 && r.client->notice().text.find("stopped") != std::string::npos);
    }
    {   // the board goes away in the middle: the job ends, nothing hangs
        Ready r(setup);
        std::vector<PubKey> keys;
        for (int i = 0; i < 5; ++i) keys.push_back(key_of(static_cast<uint8_t>(0x90 + i * 11)));
        r.peer.hold_remove = true;
        CHECK(r.client->remove_contacts(keys));
        run(*r.client, 100);
        r.peer.lost = true;
        run(*r.client, 100);
        CHECK(!r.client->bulk().active && r.client->bulk().cancelled && r.client->link() != Link::Ready);
        CHECK(r.client->notice().text.find("disconnected") != std::string::npos);
    }
    {   // not connected
        Ready r(nullptr, false);
        CHECK(!r.client->remove_contacts({key_of(1)}) && !r.client->notice().ok);
    }
}

static void test_p2_nearby_client()
{
    {   // automatic mode: a NEW_ADVERT is a contact the board already added
        Ready r;
        r.peer.push(make_contact(resp::kNewAdvert, 0xC0, "Auto", 1, 1790000050));
        run(*r.client, 100);
        CHECK(r.model.contact_count() == 3 && r.model.pending_count() == 0 && r.model.find_contact(to_hex(key_of(0xC0))));
    }
    {   // manual mode: the board did not keep it, it waits in Nearby
        Ready r([](FakePeer &p) { p.manual_add = true; });
        CHECK(r.model.self() && r.model.self()->manual_add_contacts);
        r.peer.push(make_contact(resp::kNewAdvert, 0xC0, "Pend", 2, 1790000050));
        run(*r.client, 100);
        CHECK(r.model.contact_count() == 2 && r.model.pending_count() == 1);
        const std::string kh = to_hex(key_of(0xC0));
        const NearbyRec *n = r.model.find_nearby(kh);
        CHECK(n && n->pending && n->name == "Pend" && n->type == 2);
        r.peer.commands.clear();
        CHECK(r.client->add_nearby(kh));
        run(*r.client, 300);
        CHECK(last_cmd(r.peer, cmd::kAddUpdateContact) == build_add_update_contact(r.model.find_nearby(kh)->contact));
        CHECK(r.model.contact_count() == 3 && r.model.pending_count() == 0 && r.peer.contacts.size() == 3 && r.client->notice().ok);
        CHECK(count_cmd(r.peer, cmd::kGetContactByKey) == 1);                                     // read back
        CHECK(!r.client->add_nearby(kh) && r.client->notice().text == "Already a contact");
        CHECK(!r.client->add_nearby(std::string(64, 'a')) && !r.client->notice().ok);            // not in the list
    }
    {   // a stranger from the radio log, added with the values the log gave; the table can be full
        Ready r;
        r.peer.push(make_log(make_advert_packet(0x70, "Newbie", 1, true, 2, 2), 5.5, -80));
        r.peer.push(make_log(make_advert_packet(0xA0, "Me", 1, false, 0, 1), 5.5, -80));          // our own key: ignored
        Bytes ack = {0x09, 0x00};
        r.peer.push(make_log(ack, 1.0, -100));                                                    // not an advert
        run(*r.client, 100);
        CHECK(r.model.nearby_count() == 1);
        const std::string kh = to_hex(key_of(0x70));
        CHECK(r.model.find_nearby(kh)->snr == 5.5 && r.model.find_nearby(kh)->hops == 2 && r.model.find_nearby(kh)->hash_size == 2 && !r.model.find_nearby(kh)->pending);
        r.peer.table_full = true;
        CHECK(r.client->add_nearby(kh));
        run(*r.client, 200);
        CHECK(r.model.contact_count() == 2 && r.client->notice().text.find("list is full") != std::string::npos && !r.client->notice().ok);
        r.peer.table_full = false;
        CHECK(r.client->add_nearby(kh));
        run(*r.client, 200);
        const Contact &sent = r.peer.contacts.back();
        CHECK(sent.name == "Newbie" && sent.type == 1 && sent.key == key_of(0x70) && sent.out_path_len == kPathFlood);
        CHECK(r.model.find_contact(kh) && r.model.find_contact(kh)->c.lat > 48.8);
    }
    {   // an ADVERT push of a known contact: it is heard now and listed in Nearby with its name and type
        Ready r;
        Bytes adv = {resp::kAdvertisement};
        const PubKey alice = key_of(0x10);
        adv.insert(adv.end(), alice.begin(), alice.end());
        r.peer.push(adv);
        run(*r.client, 100);
        const NearbyRec *n = r.model.find_nearby(to_hex(alice));
        CHECK(n && n->name == "Alice" && n->type == 1 && !n->pending && n->last_heard != 0 && !n->has_snr);
        r.peer.push(make_log(make_advert_packet(0x10, "Alice", 1, false, 0, 1), 7.25, -70));           // and the radio log gives its signal
        run(*r.client, 100);
        n = r.model.find_nearby(to_hex(alice));
        CHECK(n && n->has_snr && n->snr == 7.25 && n->hops == 0 && r.model.nearby_count() == 1);
    }
    {   // what cannot be added: an unknown type, a part of a key
        Ready r;
        NearbyObs o;
        o.key_hex = std::string(64, '1');
        r.model.note_nearby(o, 100);
        CHECK(!r.client->add_nearby(o.key_hex) && r.client->notice().text.find("type is not known") != std::string::npos);
        NearbyObs p;
        p.key_hex = "0123456789abcdef";
        p.full_key = false;
        p.type = 1;
        r.model.note_nearby(p, 100);
        CHECK(!r.client->add_nearby("0123456789abcdef") && r.client->notice().text.find("part of its key") != std::string::npos);
        NearbyObs q;
        q.key_hex = std::string(64, '2');
        q.type = 3;
        r.model.note_nearby(q, 100);                                                            // no name: a placeholder, the real one comes with its advert
        CHECK(r.client->add_nearby(q.key_hex));
        run(*r.client, 200);
        CHECK(r.peer.contacts.back().name == "Node 222222");
    }
}

static void test_p2_discover()
{
    {
        Ready r([](FakePeer &p) {
            p.neighbours = {{0x10, 1, 6.0, -70, false}, {0xD0, 2, -3.5, -100, false}, {0xE0, 4, 1.0, -90, true}};
        });
        r.peer.commands.clear();
        CHECK(r.client->start_discover());
        CHECK(!r.client->start_discover() && r.client->notice().text.find("already") != std::string::npos);
        run(*r.client, 300);
        const Bytes req = last_cmd(r.peer, cmd::kSendControlData);
        CHECK(req.size() == 7 && req[1] == 0x80 && req[2] == 0xFF);                               // full keys, every type
        CHECK(r.client->discover().active && r.client->discover().found == 3 && r.model.nearby_count() == 3 && r.logged("discover request: ok"));
        const NearbyRec *alice = r.model.find_nearby(to_hex(key_of(0x10)));
        CHECK(alice && alice->discovered && alice->snr == 6.0 && alice->rssi == -70 && alice->type == 1 && alice->hops == 0);
        CHECK(r.model.find_contact(to_hex(key_of(0x10)))->heard_local != 0);                       // a known contact is heard now
        const NearbyRec *rep = r.model.find_nearby(to_hex(key_of(0xD0)));
        CHECK(rep && rep->type == 2 && rep->snr == -3.5 && rep->full_key && !rep->pending);
        CHECK(r.model.find_nearby(to_hex(key_of(0xE0)).substr(0, 16)) && !r.model.find_nearby(to_hex(key_of(0xE0)).substr(0, 16))->full_key);
        // an answer with another tag is somebody else's scan
        Bytes cd = {resp::kControlData, 8, static_cast<uint8_t>(-70), 0, 0x91, 0, 9, 9, 9, 9};
        for (int i = 0; i < 32; ++i) cd.push_back(static_cast<uint8_t>(0x33 + i));
        r.peer.push(cd);
        run(*r.client, 100);
        CHECK(r.model.nearby_count() == 3);
        run(*r.client, 9000);
        CHECK(!r.client->discover().active && r.client->discover().finished_id == 1 && r.client->notice().text == "Scan finished: 3 nodes answered" && r.logged("discover: 3 nodes answered"));
        CHECK(r.client->start_discover(0x04));                                                    // repeaters only
        run(*r.client, 100);
        CHECK(last_cmd(r.peer, cmd::kSendControlData)[2] == 0x04);
    }
    {   // the firmware answers MSG_SENT instead of OK: still a scan
        Ready r([](FakePeer &p) { p.discover_mode = 1; p.neighbours = {{0xD0, 2, -3.5, -100, false}}; });
        CHECK(r.client->start_discover());
        run(*r.client, 300);
        CHECK(r.client->discover().active && r.model.nearby_count() == 1 && r.model.caps().discover == Cap::Yes);
        run(*r.client, 9000);
        CHECK(r.client->notice().text == "Scan finished: 1 node answered");
    }
    {   // nobody answers
        Ready r;
        CHECK(r.client->start_discover());
        run(*r.client, 9000);
        CHECK(r.client->notice().text == "Scan finished: no node answered");
    }
    {   // an old firmware: the notice, then the request is not even sent
        Ready r([](FakePeer &p) { p.discover_supported = false; });
        CHECK(r.client->start_discover());
        run(*r.client, 300);
        CHECK(!r.client->discover().active && r.model.caps().discover == Cap::No && r.client->notice().text.find("Firmware too old") == 0);
        r.peer.commands.clear();
        CHECK(!r.client->start_discover() && count_cmd(r.peer, cmd::kSendControlData) == 0);
        const FeatureState f = feature_state(Feature::Discover, &*r.model.device(), r.model.caps());
        CHECK(!f.available && f.notice.find("Discover nearby nodes") != std::string::npos);
    }
}

static void test_p2_autoadd_and_manual()
{
    {
        Ready r;
        CHECK(r.model.caps().autoadd == Cap::Yes && r.model.caps().autoadd_config.config == 0x02 && r.model.caps().autoadd_config.has_max_hops);
        CHECK(feature_state(Feature::AutoAdd, &*r.model.device(), r.model.caps()).available);
        r.peer.commands.clear();
        CHECK(r.client->set_autoadd_flags(0x0A));
        run(*r.client, 300);
        CHECK(last_cmd(r.peer, cmd::kSetAutoadd) == hex("3a0a") && r.peer.autoadd == 0x0A && r.model.caps().autoadd_config.config == 0x0A && r.client->notice().ok);
        // the manual add switch keeps the other flags of the board
        CHECK(!r.model.self()->manual_add_contacts);
        r.peer.commands.clear();
        CHECK(r.client->set_manual_add(true));
        run(*r.client, 300);
        CHECK(last_cmd(r.peer, cmd::kSetOtherParams) == hex("2601000000") && r.peer.manual_add);
        CHECK(r.model.self()->manual_add_contacts && r.client->notice().text.find("wait in Nearby") != std::string::npos);
        CHECK(r.client->set_manual_add(false));
        run(*r.client, 300);
        CHECK(!r.model.self()->manual_add_contacts && r.client->notice().text.find("by itself") != std::string::npos);
    }
    {   // a firmware that only knows the 4 byte SET_OTHER_PARAMS
        Ready r([](FakePeer &p) { p.other_params_old = true; });
        r.peer.commands.clear();
        CHECK(r.client->set_manual_add(true));
        run(*r.client, 400);
        CHECK(count_cmd(r.peer, cmd::kSetOtherParams) == 2 && r.peer.commands[0].size() == 5 && last_cmd(r.peer, cmd::kSetOtherParams).size() == 4);
        CHECK(r.peer.manual_add && r.model.self()->manual_add_contacts);
    }
    {   // too old for the auto-add filter
        Ready r([](FakePeer &p) { p.autoadd_supported = false; });
        CHECK(r.model.caps().autoadd == Cap::No);
        const FeatureState f = feature_state(Feature::AutoAdd, &*r.model.device(), r.model.caps());
        CHECK(!f.available && !f.hidden && f.notice.find("Auto-add contacts") != std::string::npos);
        r.peer.commands.clear();
        CHECK(!r.client->set_autoadd_flags(0x02) && count_cmd(r.peer, cmd::kSetAutoadd) == 0 && r.client->notice().text.find("Firmware too old") == 0);
    }
    {   // not connected: the feature is simply not shown
        Ready r(nullptr, false);
        CHECK(feature_state(Feature::AutoAdd, nullptr, r.model.caps()).hidden);
        CHECK(!r.client->set_manual_add(true));
    }
}

/* ---------------------------------------------------------------- the client: the board */

static void test_p2_board()
{
    {   // reboot: the command has no answer and must not count as a timeout; the link comes back
        Ready r;
        r.peer.commands.clear();
        CHECK(r.client->reboot());
        run(*r.client, 100);
        CHECK(last_cmd(r.peer, cmd::kReboot) == build_reboot() && r.peer.reboots == 1 && r.client->queued() == 0);
        run(*r.client, 12000);
        CHECK(r.client->link() == Link::Ready && r.model.contact_count() == 2);
    }
    {   // factory reset: the board answers OK and restarts with a new identity; the app shows the board that is there now (the detailed cases are in test_boards)
        Ready r;
        r.model.groups_mut().add_group("Crew");
        r.model.add_outgoing("c:0", "keep me", 1790000000);
        r.peer.commands.clear();
        CHECK(r.client->factory_reset());
        run(*r.client, 200);
        CHECK(last_cmd(r.peer, cmd::kFactoryReset) == build_factory_reset() && r.peer.resets == 1);
        CHECK(r.client->notice().ok && r.client->notice().text.find("Board reset") == 0 && r.client->reset_phase() == Client::ResetPhase::Waiting);
        run(*r.client, 12000);                                                                   // the board comes back empty, with new keys
        CHECK(r.client->link() == Link::Ready && r.model.contact_count() == 0 && r.peer.contacts.empty());
        CHECK(r.model.find_channel(0) && r.model.find_channel(0)->empty);
        CHECK(r.client->reset_phase() == Client::ResetPhase::Done && r.client->notice().text == "Board reset: new keys, no contacts or channels");
        CHECK(r.model.message_count() == 0 && !r.model.groups().has("Crew"));                    // another board: the data of the old identity is not shown
    }
    {   // a firmware that wants the word "reset" gets it
        Ready r([](FakePeer &p) { p.reset_needs_word = true; });
        CHECK(r.client->factory_reset());
        run(*r.client, 200);
        CHECK(r.peer.resets == 1);
    }
    {   // refused: nothing is forgotten
        Ready r([](FakePeer &p) { p.factory_ok = false; });
        CHECK(r.client->factory_reset());
        run(*r.client, 200);
        CHECK(r.peer.resets == 0 && r.model.contact_count() == 2 && !r.client->notice().ok && r.client->notice().text.find("Firmware too old") == 0);
    }
    {   // not connected
        Ready r(nullptr, false);
        CHECK(!r.client->reboot() && !r.client->factory_reset());
    }
}

static void test_p2_path_hash_and_repeat()
{
    {   // the user's real board reports mode 1 (2 byte hashes); the setting writes 3D 00 mode and reads the board back
        Ready r;
        CHECK(r.model.device()->path_hash_mode == 1 && feature_state(Feature::PathHashMode, &*r.model.device(), r.model.caps()).available);
        CHECK(r.logged("board: Seeed Xiao-nrf52 v1.15.0-dee3e26, firmware level 11, path hash mode 1, repeat off, manual add off"));       // what the Controller reads in mesh-hop.log
        CHECK(r.logged("repeat: allowed frequency ranges as the board sent them (read as kHz): 433000-434000, 863000-870000") && r.logged("autoadd: config 0x02, max hops 0"));
        r.peer.commands.clear();
        CHECK(r.client->set_path_hash_mode(2));
        run(*r.client, 300);
        CHECK(last_cmd(r.peer, cmd::kSetPathHashMode) == hex("3d0002") && r.peer.path_mode == 2 && r.logged("path hash mode 2: ok"));
        CHECK(r.model.device()->path_hash_mode == 2 && r.client->notice().ok && r.client->notice().text == "Path hash size set to 3 bytes");
        CHECK(r.client->set_path_hash_mode(0));
        run(*r.client, 300);
        CHECK(r.model.device()->path_hash_mode == 0 && r.client->notice().text == "Path hash size set to 1 byte");
        r.peer.commands.clear();
        CHECK(!r.client->set_path_hash_mode(3) && !r.client->set_path_hash_mode(-1) && count_cmd(r.peer, cmd::kSetPathHashMode) == 0);
    }
    {   // firmware level 9: no path hash mode (needs level 10): the row says so, nothing is sent
        Ready r([](FakePeer &p) { p.fw_level = 9; });
        CHECK(r.model.device()->path_hash_mode == -1 && r.model.device()->has_repeat);
        const FeatureState f = feature_state(Feature::PathHashMode, &*r.model.device(), r.model.caps());
        CHECK(!f.available && f.notice.find("Firmware too old") == 0 && f.notice.find("level 10") != std::string::npos);
        r.peer.commands.clear();
        CHECK(!r.client->set_path_hash_mode(1) && count_cmd(r.peer, cmd::kSetPathHashMode) == 0 && r.client->notice().text.find("level 10") != std::string::npos);
    }
    {   // repeat: allowed on the frequencies the board lists, the trailing byte of SET_RADIO
        Ready r;
        const RepeatState rs = repeat_state(&*r.model.device(), r.model.caps(), 869.525);
        CHECK(r.model.caps().repeat_freqs == Cap::Yes && r.model.caps().repeat_ranges.size() == 2);
        CHECK(rs.shown && rs.available && !rs.on && rs.note.empty());
        r.peer.commands.clear();
        CHECK(r.client->set_client_repeat(true));
        run(*r.client, 400);
        CHECK(last_cmd(r.peer, cmd::kSetRadio) == hex("0b95440d0090d003000b0501") && r.peer.repeat);
        CHECK(r.model.device()->repeat && repeat_state(&*r.model.device(), r.model.caps(), 869.525).on && r.client->notice().ok);
        CHECK(r.client->set_client_repeat(false));
        run(*r.client, 400);
        CHECK(!r.peer.repeat && !r.model.device()->repeat);
    }
    {   // a frequency the board does not allow for repeat: the row is off with the reason, nothing is sent
        Ready r([](FakePeer &p) { p.self.freq_khz = 915000; });
        const RepeatState rs = repeat_state(&*r.model.device(), r.model.caps(), r.model.self()->freq_mhz());
        CHECK(rs.shown && !rs.available && rs.note.find("Not allowed on this frequency") == 0 && rs.note.find("433.000-434.000") != std::string::npos);
        r.peer.commands.clear();
        CHECK(!r.client->set_client_repeat(true) && count_cmd(r.peer, cmd::kSetRadio) == 0);
    }
    {   // firmware level 8: repeat does not exist
        Ready r([](FakePeer &p) { p.fw_level = 8; });
        const RepeatState rs = repeat_state(&*r.model.device(), r.model.caps(), 869.525);
        CHECK(rs.shown && !rs.available && rs.note.find("Firmware too old") == 0 && rs.note.find("level 9") != std::string::npos);
        CHECK(r.model.caps().repeat_freqs == Cap::Unknown && count_cmd(r.peer, cmd::kGetAllowedRepeatFreq) == 0);
        CHECK(!repeat_state(nullptr, BoardCaps(), 869.5).shown);
    }
}

static void test_p2_stats()
{
    {
        Ready r;
        CHECK(feature_state(Feature::Stats, &*r.model.device(), r.model.caps()).available);
        r.peer.commands.clear();
        CHECK(r.client->refresh_stats() && r.client->stats_busy() && !r.client->refresh_stats());
        run(*r.client, 400);
        CHECK(!r.client->stats_busy() && count_cmd(r.peer, cmd::kGetStats) == 3);
        CHECK(r.peer.commands[0] == hex("3800") && r.peer.commands[1] == hex("3801") && r.peer.commands[2] == hex("3802"));
        const StatsSnapshot &s = r.model.stats();
        CHECK(s.core && s.core->battery_mv == 3962 && s.core->uptime_secs == 7300 && s.core->errors == 2 && s.core->queue_len == 1);
        CHECK(s.radio && s.radio->noise_floor == -110 && s.radio->last_rssi == -95 && s.radio->last_snr == 5.5 && s.radio->tx_air_secs == 61 && s.radio->rx_air_secs == 940);
        CHECK(s.packets && s.packets->recv == 500 && s.packets->sent == 120 && s.packets->flood_tx == 14 && s.packets->direct_tx == 106 && s.packets->flood_rx == 310 &&
              s.packets->direct_rx == 190 && s.packets->has_errors && s.packets->recv_errors == 9);
        CHECK(s.updated != 0 && r.model.caps().stats == Cap::Yes);
        CHECK(r.client->refresh_stats());                                                         // again: allowed once the last set is done
        run(*r.client, 400);
        CHECK(count_cmd(r.peer, cmd::kGetStats) == 6);
    }
    {   // a board without statistics: off with the notice, no more requests
        Ready r([](FakePeer &p) { p.stats_supported = false; });
        r.peer.commands.clear();
        CHECK(r.client->refresh_stats());
        run(*r.client, 400);
        CHECK(!r.client->stats_busy() && r.model.caps().stats == Cap::No && !r.model.stats().core);
        const FeatureState f = feature_state(Feature::Stats, &*r.model.device(), r.model.caps());
        CHECK(!f.available && !f.hidden && f.notice.find("Statistics") != std::string::npos);
        r.peer.commands.clear();
        CHECK(!r.client->refresh_stats() && count_cmd(r.peer, cmd::kGetStats) == 0);
    }
}

static void test_p2_advert_schedule()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("sched");
    {
        Ready r;
        CHECK(r.model.advert_schedule().interval_hours == 0 && r.model.advert_schedule().flood);       // default: off
        r.peer.commands.clear();
        run(*r.client, 7200000, 1000);
        CHECK(count_cmd(r.peer, cmd::kSendAdvert) == 0);                                              // off: never
        r.model.set_advert_schedule({1, true});
        run(*r.client, 1000, 500);
        r.peer.commands.clear();
        run(*r.client, 3500000, 500);
        CHECK(count_cmd(r.peer, cmd::kSendAdvert) == 0);                                              // not yet: 58 minutes
        run(*r.client, 200000, 500);
        CHECK(count_cmd(r.peer, cmd::kSendAdvert) == 1 && last_cmd(r.peer, cmd::kSendAdvert) == hex("0701"));
        CHECK(r.client->notice().text == "Scheduled advert sent (flood)" && r.logged("scheduled advert (flood): ok"));
        run(*r.client, 3600000, 500);
        CHECK(count_cmd(r.peer, cmd::kSendAdvert) == 2);                                              // every hour
        // zero-hop, and a change restarts the interval
        r.model.set_advert_schedule({3, false});
        r.peer.commands.clear();
        run(*r.client, 3 * 3600000 - 600000, 1000);
        CHECK(count_cmd(r.peer, cmd::kSendAdvert) == 0);
        run(*r.client, 700000, 1000);
        CHECK(count_cmd(r.peer, cmd::kSendAdvert) == 1 && last_cmd(r.peer, cmd::kSendAdvert) == hex("07"));
        r.model.set_advert_schedule({0, false});
        r.peer.commands.clear();
        run(*r.client, 4 * 3600000, 1000);
        CHECK(count_cmd(r.peer, cmd::kSendAdvert) == 0);
        // only the listed intervals exist
        r.model.set_advert_schedule({5, true});
        CHECK(r.model.advert_schedule().interval_hours == 0);
        CHECK(advert_interval_options() == std::vector<int>({0, 1, 3, 6, 12}));
    }
    {   // kept between runs; so are the ignored nodes
        Model m;
        Store st(dir);
        attach_b(st, m);
        m.set_advert_schedule({6, false});
        m.set_ignored(to_hex(key_of(0x42)), true);
    }
    {
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(m.advert_schedule().interval_hours == 6 && !m.advert_schedule().flood && m.is_ignored(to_hex(key_of(0x42))));
        const std::string prefs = read_text(bdir_of(dir) + "/prefs.txt");
        CHECK(prefs.find("advert 6 0\n") != std::string::npos && prefs.find("ignore " + to_hex(key_of(0x42)) + "\n") != std::string::npos);
    }
}

/* ---------------------------------------------------------------- groups, archive */

static void test_p2_groups()
{
    CHECK(ContactGroups::validate_name("Friends").empty() && !ContactGroups::validate_name("").empty() && !ContactGroups::validate_name(" x").empty() &&
          !ContactGroups::validate_name("x ").empty() && !ContactGroups::validate_name(std::string(25, 'a')).empty() && ContactGroups::validate_name(std::string(24, 'a')).empty());
    CHECK(!ContactGroups::validate_name("a\tb").empty() && ContactGroups::validate_name("caf\xC3\xA9 \xC3\xA0 la plage").empty());
    ContactGroups g;
    const std::string k1 = to_hex(key_of(1)), k2 = to_hex(key_of(2)), k3 = to_hex(key_of(3));
    CHECK(g.add_group("Friends").empty() && g.add_group("Hikers").empty() && !g.add_group("Friends").empty() && g.groups().size() == 2);
    CHECK(g.add_members("Friends", {k1, k2, k1, "zz", std::string(64, 'G')}) == 2 && g.member_count("Friends") == 2);
    CHECK(g.add_members("Hikers", {k2, k3}) == 2 && g.add_members("Nope", {k1}) == 0);
    CHECK(g.is_member("Friends", k1) && !g.is_member("Hikers", k1) && g.groups_of(k2) == std::vector<std::string>({"Friends", "Hikers"}));
    CHECK(g.members("Hikers") == std::set<std::string>({k2, k3}));
    CHECK(g.remove_members("Friends", {k1}) == 1 && g.member_count("Friends") == 1);
    CHECK(g.remove_everywhere({k2}) == 2 && g.member_count("Friends") == 0 && g.member_count("Hikers") == 1);
    CHECK(g.rename_group("Hikers", "Walkers").empty() && g.has("Walkers") && !g.has("Hikers") && !g.rename_group("Walkers", "Friends").empty() && !g.rename_group("Nope", "X").empty());
    CHECK(g.delete_group("Friends").empty() && !g.delete_group("Friends").empty() && g.groups().size() == 1);
    for (int i = 0; i < 20; ++i) g.add_group("G" + std::to_string(i));
    CHECK(g.groups().size() == ContactGroups::kMaxGroups && !g.add_group("one more").empty());

    // text form: names with quotes, commas, accents survive; a damaged line is skipped
    ContactGroups h;
    h.add_group("Caf\xC3\xA9 \"Nord\", 2");
    h.add_group("Empty");
    h.add_members("Caf\xC3\xA9 \"Nord\", 2", {k1, k3});
    ContactGroups back;
    CHECK(back.parse(h.serialize()) == 2 && back.is_member("Caf\xC3\xA9 \"Nord\", 2", k3) && back.has("Empty") && back.member_count("Empty") == 0);
    ContactGroups damaged;
    CHECK(damaged.parse("{\"g\":\"Ok\",\"k\":\"" + k1 + "\"}\nnot json\n{\"g\":\"\",\"k\":\"\"}\n{\"g\":\"Ok\",\"k\":\"\"}\n{\"g\":\"Two\",\"k\":\"" + k1 + "," + k1 + ",bad\"}\n") == 2);
    CHECK(damaged.member_count("Ok") == 1 && damaged.member_count("Two") == 1);
}

static void test_p2_groups_store()
{
    const std::string dir = tmpdir("groups");
    const std::string k1 = to_hex(key_of(1)), k2 = to_hex(key_of(2));
    {   // every change writes groups.jsonl; the version before it stays as groups.bak
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(!st.groups_restored() && m.groups().empty());
        m.groups_mut().add_group("Crew");
        m.groups_mut().add_members("Crew", {k1});
        m.groups_changed();
        CHECK(read_text(bdir_of(dir) + "/groups.jsonl").find("{\"v\":1,\"n\":1}\n") == 0);
        m.groups_mut().add_members("Crew", {k2});
        m.groups_changed();
        CHECK(read_text(bdir_of(dir) + "/groups.jsonl").find(k2) != std::string::npos && read_text(bdir_of(dir) + "/groups.bak").find(k2) == std::string::npos && read_text(bdir_of(dir) + "/groups.bak").find(k1) != std::string::npos);
    }
    {   // a restart reads the main file
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(m.groups().member_count("Crew") == 2 && !st.groups_restored());
    }
    {   // the main file is damaged (cut short): the backup is used and the app can say so
        write_text(bdir_of(dir) + "/groups.jsonl", "{\"v\":1,\"n\":1}\n{\"g\":\"Cr");
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(st.groups_restored() && m.groups().has("Crew") && m.groups().member_count("Crew") == 1);
    }
    {   // missing main file: the backup too
        std::remove((bdir_of(dir) + "/groups.jsonl").c_str());
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(st.groups_restored() && m.groups().has("Crew"));
        m.groups_mut().delete_group("Crew");
        m.groups_changed();                                    // a deliberate delete of the last group: an empty file with its header
    }
    {   // an empty list the user chose is trusted: the old backup does not bring the group back
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(!st.groups_restored() && m.groups().empty());
        CHECK(read_text(bdir_of(dir) + "/groups.jsonl") == "{\"v\":1,\"n\":0}\n");
    }
    {   // a file from an older run with no header counts as damaged; nothing at all is simply empty
        const std::string d2 = tmpdir("groups2");
        Model m;
        Store st(d2);
        attach_b(st, m);
        CHECK(m.groups().empty() && !st.groups_restored());
    }
}

static void test_p2_archive()
{
    set_time_source(fake_time);
    const uint32_t kNow = 1790000000, kDay = 86400;
    Model m;
    ChannelRec ch;
    ch.idx = 0; ch.name = "Public"; ch.empty = false;
    m.set_channel(ch);
    m.upsert_contact(fake_contact(0x10, "Alice", 1));
    auto chan = [&](const char *text, uint32_t stamp, uint32_t ts) {
        IncomingMessage cm;
        cm.channel = true;
        cm.channel_idx = 0;
        cm.text = text;
        cm.sender_timestamp = stamp;
        return m.add_incoming(cm, ts);
    };
    auto direct = [&](const char *text, uint32_t stamp, uint32_t ts) {
        IncomingMessage dm;
        for (int i = 0; i < 6; ++i) dm.prefix[static_cast<size_t>(i)] = static_cast<uint8_t>(0x10 + i);
        dm.text = text;
        dm.sender_timestamp = stamp;
        return m.add_incoming(dm, ts);
    };
    chan("Zed: Hello mesh", 1, kNow - 50 * kDay);
    chan("Yan: weather is nice", 2, kNow - 5 * kDay);
    chan("Zed: caf\xC3\xA9 tonight?", 3, kNow - 1 * kDay);
    direct("meet at the Hill", 4, kNow - 3 * kDay);
    direct("clock unknown", 5, 100);
    const uint32_t out1 = m.add_outgoing("d:101112131415", "ok, see you at the hill", kNow - 3 * kDay + 60);
    const uint32_t out2 = m.add_outgoing("c:0", "hello everyone", kNow - 2 * kDay);
    CHECK(out1 != 0 && out2 != 0 && m.message_count() == 7);

    SearchQuery q;
    CHECK(q.empty() && search_messages(m, q).size() == 7);
    size_t total = 0;
    auto hits = search_messages(m, q, 3, &total);
    CHECK(hits.size() == 3 && total == 7 && hits[0].seq > hits[1].seq);                           // newest first, limited, the real total reported
    q.text = "HELLO";                                                                              // case-insensitive
    hits = search_messages(m, q);
    CHECK(hits.size() == 2 && hits[0].outgoing && hits[1].text == "Hello mesh" && hits[1].sender == "Zed");
    q.text = "caf\xC3\xA9";
    CHECK(search_messages(m, q).size() == 1);
    q.text = "CAF\xC3\x89";                                                                         // an accented capital is not folded: exact bytes
    CHECK(search_messages(m, q).empty());
    q.text = "zed";                                                                                // the sender's name matches too
    CHECK(search_messages(m, q).size() == 2);
    q.text = "hill";
    hits = search_messages(m, q);
    CHECK(hits.size() == 2 && hits[0].title == "Alice" && hits[0].conv == "d:101112131415");
    q.conv = "c:0";
    CHECK(search_messages(m, q).empty());
    q = SearchQuery();
    q.conv = "c:0";
    CHECK(search_messages(m, q).size() == 4);
    q.dir = SearchDir::Sent;
    hits = search_messages(m, q);
    CHECK(hits.size() == 1 && hits[0].outgoing && hits[0].sender == "You");
    q.dir = SearchDir::Received;
    CHECK(search_messages(m, q).size() == 3);
    // dates: both ends inclusive; a message whose time is unknown never passes a range
    q = SearchQuery();
    q.from = kNow - 5 * kDay;
    CHECK(search_messages(m, q).size() == 5);
    q.to = kNow - 3 * kDay;
    CHECK(search_messages(m, q).size() == 2);                                                      // the weather line (exact start) and "meet at the Hill" (exact end)
    q.from = q.to;
    CHECK(search_messages(m, q).size() == 1);                                                      // "meet at the Hill" at exactly kNow - 3 days
    q = SearchQuery();
    q.to = kNow;
    CHECK(search_messages(m, q).size() == 6);                                                      // 7 minus the unknown time
    uint32_t from = 1, to = 1;
    date_preset_range(DatePreset::Week, kNow, from, to);
    CHECK(from == kNow - 7 * kDay && to == 0);
    date_preset_range(DatePreset::Any, kNow, from, to);
    CHECK(from == 0 && to == 0);
    date_preset_range(DatePreset::Day, 100, from, to);
    CHECK(from == 0 && to == 0);                                                                   // no clock, no range
    CHECK(std::string(date_preset_name(DatePreset::Quarter)) == "Last 90 days" && std::string(search_dir_name(SearchDir::Sent)) == "Sent");

    CHECK(parse_date_range("2026-09-01", from, to).empty() && to - from == 86399);
    CHECK(parse_date_range("2026-09-01 2026-09-30", from, to).empty() && to - from == 29 * 86400 + 86399);
    CHECK(!parse_date_range("", from, to).empty() && !parse_date_range("2026-09-30 2026-09-01", from, to).empty() && !parse_date_range("2026-13-01", from, to).empty() &&
          !parse_date_range("1 2 3", from, to).empty() && !parse_date_range("2026-02-31", from, to).empty() && !parse_date_range("yesterday", from, to).empty());
    CHECK(parse_date_range("2026-09-01 2026-09-30", from, to).empty() && fmt_date_range(from, to) == "2026-09-01 to 2026-09-30" && fmt_date_range(0, 0) == "Any date");
    CHECK(parse_date_range("2026-09-05", from, to).empty() && fmt_date_range(from, to) == "2026-09-05");
    CHECK(text_contains_nocase("Hello", "") && text_contains_nocase("Hello", "ELL") && !text_contains_nocase("Hi", "Hello"));
}

/* ---------------------------------------------------------------- the preset download schedule (the retry of 0.2.0) */

static void test_p2_preset_schedule()
{
    int asked = 0;
    auto online = [&] { ++asked; return true; };
    auto offline = [&] { ++asked; return false; };
    {   // 4 s after the start, then one retry 60 s after a failure, then no more
        PresetSchedule s(1000);
        CHECK(!s.due(2000, online) && asked == 0);                                                // too early: the network is not even asked
        CHECK(s.due(5000, online));
        s.started();
        CHECK(!s.due(5100, online) && s.attempts() == 1);                                         // running
        s.failed(10000, true);
        CHECK(!s.due(69999, online) && !s.finished());
        CHECK(s.due(70000, online));
        s.started();
        s.failed(75000, true);
        CHECK(s.finished() && !s.due(500000, online) && s.attempts() == 2);                       // at most two attempts per launch
    }
    {   // a success ends it
        PresetSchedule s(0);
        CHECK(s.due(5000, online));
        s.started();
        s.succeeded();
        CHECK(s.finished() && !s.due(900000, online));
    }
    {   // nothing to retry when curl is missing
        PresetSchedule s(0);
        CHECK(s.due(5000, online));
        s.started();
        s.failed(5001, false);
        CHECK(s.finished() && !s.due(900000, online));
    }
    {   // offline at the start: checked every 30 s, started when the deck comes online
        asked = 0;
        PresetSchedule s(0);
        CHECK(!s.due(4000, offline) && asked == 1);
        CHECK(!s.due(20000, online) && asked == 1);                                               // 30 s have not passed since the last look
        CHECK(!s.due(33999, online) && asked == 1);
        CHECK(s.due(34000, online) && asked == 2);
    }
    {   // the retry is for the time the deck is online: offline at 60 s, it is looked at again every 30 s
        asked = 0;
        PresetSchedule s(0);
        CHECK(s.due(4000, online));
        s.started();
        s.failed(10000, true);
        CHECK(!s.due(70000, offline) && asked == 2);
        CHECK(!s.due(90000, online) && asked == 2);
        CHECK(s.due(100000, online) && asked == 3);
    }
}

static void test_phase2()
{
    test_p2_protocol();
    test_p2_parsers();
    test_p2_model_nearby();
    test_p2_bulk_delete();
    test_p2_nearby_client();
    test_p2_discover();
    test_p2_autoadd_and_manual();
    test_p2_board();
    test_p2_path_hash_and_repeat();
    test_p2_stats();
    test_p2_advert_schedule();
    test_p2_groups();
    test_p2_groups_store();
    test_p2_archive();
    test_p2_preset_schedule();
}
