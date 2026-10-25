// Report 036 (Mesh Hop: managing a repeater from the contact list), slice S2 "read-only view", the core: BINARY_REQ (50) and
// ANON_REQ (57) with their push BINARY_RESPONSE (0x8C) matched by tag; the parsers of the status (RepeaterStats / a room's
// ServerStats), a NEIGHBOURS page, OWNER_INFO (0x07) and the anonymous OWNER / BASIC answers; the client's request_status(),
// request_neighbours(), request_owner() on the one-at-a-time remote queue of slice S1; the neighbour names from the contacts.
// Included by test_core.cpp after test_remote.inc (FakePeer, Ready, run(), contact_key(), cmd_count(), last_remote_cmd()).
//
// The byte fixtures were built with Python's struct from the firmware's structs (simple_repeater MyMesh.h RepeaterStats,
// simple_room_server MyMesh.cpp ServerStats, MyMesh.cpp handleRequest) and read back with the bundled meshcore_py 2.3.17
// (parsing.py parse_status, reader.py's NEIGHBOURS loop, binary.py req_owner_sync); the values checked below are what it gave.
// Each one carries the AES zero padding the companion passes on (the reply is decrypted in 16 byte blocks).

static void test_remote_s2_protocol()
{
    PubKey k{};
    for (int i = 0; i < 32; ++i) k[static_cast<size_t>(i)] = static_cast<uint8_t>(0x50 + i);
    // ---- builders: 50 + key + the request type (+ its parameters); 57 + key + the anonymous type + the reply path
    Bytes b = build_status_request(k);
    CHECK(b.size() == 34 && b[0] == 50 && b[1] == 0x50 && b[32] == 0x6F && b[33] == 0x01);
    b = build_owner_info_request(k);
    CHECK(b.size() == 34 && b[0] == 50 && b[33] == 0x07);
    b = build_telemetry_request(k);
    CHECK(b.size() == 38 && b[0] == 50 && b[33] == 0x03 && b[34] == 0x00 && b[37] == 0x00);    // inverse permission mask 0: everything allowed
    b = build_neighbours_request(k, 255, 0x0123, NeighbourOrder::StrongestFirst, 4, 0xA1B2C3D4);
    CHECK(b.size() == 44 && b[0] == 50 && b[33] == 0x06 && b[34] == 0 && b[35] == 255 && b[36] == 0x23 && b[37] == 0x01 && b[38] == 2 &&
          b[39] == 4 && b[40] == 0xD4 && b[43] == 0xA1);                    // version 0, count, offset (LE), order, prefix length, a nonce
    b = build_binary_request(k, 0x05, {0, 0});
    CHECK(b.size() == 36 && b[33] == 0x05 && b[34] == 0 && b[35] == 0);
    b = build_anon_request(k, anontype::kOwner, {0x00});
    CHECK(b.size() == 35 && b[0] == 57 && b[1] == 0x50 && b[33] == 0x02 && b[34] == 0x00);
    // the reply path of an anonymous request: the contact's out path reversed hop by hop, behind its length byte (meshcore_py
    // encode_reply_path); flood (no route) or a bad length byte: none
    std::array<uint8_t, 64> path{};
    CHECK(anon_reply_path(0, path) && *anon_reply_path(0, path) == Bytes{0x00});
    path[0] = 0xA1; path[1] = 0xB2;
    CHECK(anon_reply_path(2, path) && *anon_reply_path(2, path) == (Bytes{0x02, 0xB2, 0xA1}));
    path[2] = 0xC3; path[3] = 0xD4;                                          // 2 hops of 2 byte hashes (mode 1): A1B2, C3D4
    CHECK(anon_reply_path(0x42, path) && *anon_reply_path(0x42, path) == (Bytes{0x42, 0xC3, 0xD4, 0xA1, 0xB2}));
    CHECK(!anon_reply_path(kPathFlood, path) && !anon_reply_path(0xC1, path));                    // hash size 4 is reserved
    CHECK(!anon_reply_path(0x3F | 0x80, path));                              // 63 hops of 3 bytes: more than the 64 byte path

    // ---- the push BINARY_RESPONSE (0x8C): code, a reserved byte, the tag of SENT, the reply
    auto p = parse_packet({0x8C, 0, 0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB});
    const auto *br = p ? std::get_if<BinaryResponse>(&*p) : nullptr;
    CHECK(br && br->tag == (std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0x44}) && br->data == (Bytes{0xAA, 0xBB}));
    p = parse_packet({0x8C, 0, 0x11, 0x22, 0x33, 0x44});
    br = p ? std::get_if<BinaryResponse>(&*p) : nullptr;
    CHECK(br && br->data.empty());
    CHECK(!parse_packet({0x8C, 0, 0x11, 0x22, 0x33}));                       // no full tag: cannot be matched

    // ---- STATUS of a repeater: RepeaterStats, 56 bytes (52 before repeater v1.12.0), + 4 bytes of padding
    const Bytes st = hex("1b10020090ffa9ff40e20100d21e0000100e000085f40300f40100002c01000070170000bc0200000500e7ff0b00de00201c00002100000000000000");
    CHECK(st.size() == 60);
    auto s = parse_repeater_status(st);
    CHECK(s && !s->room && s->battery_mv == 4123 && s->tx_queue == 2 && s->noise_floor == -112 && s->last_rssi == -87 && s->packets_rx == 123456 &&
          s->packets_tx == 7890 && s->airtime_tx_s == 3600 && s->uptime_s == 259205 && s->flood_tx == 500 && s->direct_tx == 300 && s->flood_rx == 6000 &&
          s->direct_rx == 700 && s->debug_flags == 5 && s->last_snr == -6.25 && s->direct_dups == 11 && s->flood_dups == 222 && s->has_airtime_rx &&
          s->airtime_rx_s == 7200 && s->has_rx_errors && s->rx_errors == 33);
    s = parse_repeater_status(Bytes(st.begin(), st.begin() + 52));           // the 52 byte layout: no receive error counter
    CHECK(s && s->has_airtime_rx && s->airtime_rx_s == 7200 && !s->has_rx_errors);
    CHECK(!parse_repeater_status(Bytes(st.begin(), st.begin() + 51)) && !parse_repeater_status({}));
    // a room server's ServerStats: the same first 48 bytes, then the posts it stored and pushed (no rx airtime, no rx errors)
    const Bytes room = hex("3c0f00008aff9bffe8030000c80000003c000000100e00000a000000140000001e0000002800000000001a0001000200110009000000000000000000");
    s = parse_repeater_status(room, true);
    CHECK(s && s->room && s->battery_mv == 3900 && s->noise_floor == -118 && s->last_rssi == -101 && s->packets_rx == 1000 && s->uptime_s == 3600 &&
          s->last_snr == 6.5 && s->direct_dups == 1 && s->flood_dups == 2 && s->posted == 17 && s->post_pushes == 9 && !s->has_airtime_rx && !s->has_rx_errors);

    // ---- a NEIGHBOURS page: total (int16), the entries in it (int16), then per entry the key prefix, heard N s ago (u32), SNR x 4 (int8)
    const Bytes nb = hex("14000300909192932d0000001630313233100e0000f6e0e1e2e3701101008000000000000000000000000000");
    auto pg = parse_neighbours(nb, 4, 0);
    CHECK(pg && pg->total == 20 && pg->offset == 0 && pg->entries.size() == 3 && pg->more);
    CHECK(pg && pg->entries[0].prefix == (Bytes{0x90, 0x91, 0x92, 0x93}) && pg->entries[0].heard_secs_ago == 45 && pg->entries[0].snr == 5.5);
    CHECK(pg && pg->entries[1].prefix == (Bytes{0x30, 0x31, 0x32, 0x33}) && pg->entries[1].heard_secs_ago == 3600 && pg->entries[1].snr == -2.5);
    CHECK(pg && pg->entries[2].heard_secs_ago == 70000 && pg->entries[2].snr == -32.0);
    pg = parse_neighbours(nb, 4, 17);                                        // the last page: 17 + 3 = 20
    CHECK(pg && pg->offset == 17 && !pg->more);
    CHECK(!parse_neighbours(Bytes(nb.begin(), nb.begin() + 4 + 2 * 9 + 8), 4, 0));   // says 3, holds 2 and a part
    CHECK(!parse_neighbours({0x14, 0x00}, 4, 0) && !parse_neighbours({0xFF, 0xFF, 0x00, 0x00}, 4, 0));   // too short, a negative total
    pg = parse_neighbours({0x00, 0x00, 0x00, 0x00}, 4, 0);                   // a repeater with no neighbour
    CHECK(pg && pg->total == 0 && pg->entries.empty() && !pg->more);
    pg = parse_neighbours({0x05, 0x00, 0x00, 0x00}, 4, 0);                   // none in this page although 5 exist: no "more" (no endless paging)
    CHECK(pg && pg->total == 5 && !pg->more);

    // ---- OWNER_INFO (0x07): "<firmware version>\n<name>\n<owner info>" (the owner info may hold new lines: '|' in `set owner.info`)
    const Bytes own = hex("76312e31382e300a48696c6c746f702072657065617465720a53696d206f776e65720a6b6579203132333400");
    auto o = parse_owner_info(own);
    CHECK(o && !o->anonymous && o->fw_version == "v1.18.0" && o->name == "Hilltop repeater" && o->owner_info == "Sim owner\nkey 1234" && !o->has_clock);
    std::string t = "v1.18.0\nNoOwner\n";
    o = parse_owner_info(Bytes(t.begin(), t.end()));
    CHECK(o && o->name == "NoOwner" && o->owner_info.empty());
    t = "just text";
    CHECK(!parse_owner_info(Bytes(t.begin(), t.end())) && !parse_owner_info({}) && !parse_owner_info({0, 0, 0}));
    // anonymous OWNER (0x02): the node's clock, then "<name>\n<owner info>"
    const Bytes anon = hex("5240b16a48696c6c746f702072657065617465720a53696d206f776e65720a6b657920313233340000000000");
    o = parse_anon_owner(anon);
    CHECK(o && o->anonymous && o->has_clock && o->clock == 1790001234 && o->name == "Hilltop repeater" && o->owner_info == "Sim owner\nkey 1234" &&
          o->fw_version.empty());
    CHECK(!parse_anon_owner({0x52, 0x40, 0xB1}));
    // anonymous BASIC (0x03): the clock and the feature byte (0x80 repeat off; bridge type in the low bits)
    auto ab = parse_anon_basic(hex("5240b16a8100000000000000"));
    CHECK(ab && ab->clock == 1790001234 && ab->features == 0x81 && ab->repeat_disabled() && ab->bridge() == 1);
    CHECK(!parse_anon_basic({0x52, 0x40, 0xB1, 0x6A}));

    // ---- the name of a neighbour from the contacts: the first match (most recently heard), "ambiguous" when two keys start alike
    Model m;
    Contact c1 = std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x90, "Ridge", 2, 1000)));
    Contact c2 = c1;
    c2.key[10] ^= 0xFF;                                                      // the same first 4 bytes, another key
    c2.name = "Ridge twin";
    Contact c3 = std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x30, "", 2, 1000)));   // no name yet
    m.upsert_contact(c1);
    m.upsert_contact(c2);
    m.upsert_contact(c3);
    m.heard(c2.key, 5000);                                                   // the twin was heard last
    const Bytes ridge_pre = {0x90, 0x91, 0x92, 0x93};
    auto nm = neighbour_name(m, ridge_pre);
    CHECK(nm.known && nm.ambiguous && nm.matches == 2 && nm.text == "Ridge twin" && nm.key_hex == to_hex(c2.key));
    m.heard(c1.key, 6000);
    nm = neighbour_name(m, ridge_pre);
    CHECK(nm.known && nm.ambiguous && nm.text == "Ridge");
    nm = neighbour_name(m, {0x30, 0x31, 0x32, 0x33});
    CHECK(nm.known && !nm.ambiguous && nm.matches == 1 && nm.text == "30313233");              // a contact without a name: its prefix
    nm = neighbour_name(m, {0xE0, 0xE1, 0xE2, 0xE3});
    CHECK(!nm.known && !nm.ambiguous && nm.matches == 0 && nm.text == "e0e1e2e3" && nm.key_hex.empty());
    CHECK(neighbour_name(m, {}).text.empty() && !neighbour_name(m, {}).known);
    CHECK(neighbour_name(m, Bytes(c1.key.begin(), c1.key.end())).text == "Ridge" && !neighbour_name(m, Bytes(c1.key.begin(), c1.key.end())).ambiguous);
}

/* Contacts for the requests: Alice (chat), Rptr (repeater, flood), Ridge (repeater, zero hop), Club (room, flood), Far (repeater, two
 * hops A1, B2); 20 neighbours behind each repeater: Ridge and Far first, then strangers. */
static void s2_setup(FakePeer &p)
{
    add_second_repeater(p);
    p.contacts.push_back(std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0xC0, "Club", 3, 1790000000))));
    Contact far = std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0xD0, "Far", 2, 1790000000, 2)));
    far.out_path[0] = 0xA1;
    far.out_path[1] = 0xB2;
    p.contacts.push_back(far);
    p.nb_table.push_back({0x90, 30, 22});                                   // Ridge: heard 30 s ago, 5.5 dB
    p.nb_table.push_back({0xD0, 400, -30});                                 // Far
    for (int i = 0; i < 18; ++i) p.nb_table.push_back({static_cast<uint8_t>(0xE0 + i), static_cast<uint32_t>(100 + i * 50), static_cast<int8_t>(-40 + i * 4)});
}

static void test_remote_requests()
{
    using RE = Client::RemoteError;
    using RO = Client::RemoteOp;
    using Phase = Client::RemoteState::Phase;
    Ready r(s2_setup);
    Client &c = *r.client;
    const PubKey rk = contact_key(r.model, "Rptr"), ridge = contact_key(r.model, "Ridge"), club = contact_key(r.model, "Club"),
                 far = contact_key(r.model, "Far");
    CHECK(rk != PubKey{} && ridge != PubKey{} && club != PubKey{} && far != PubKey{} && c.remote_sessions().empty());

    // ---- no session: status and neighbours are refused at once (nothing is sent: the node would not answer a stranger)
    r.peer.commands.clear();
    uint32_t id = c.remote_result().id;
    CHECK(c.request_status(rk) == 0 && c.remote_result().id == id + 1 && c.remote_result().op == RO::Status && c.remote_result().req == 0 &&
          c.remote_result().error == RE::NotLoggedIn && c.remote_result().key == rk && !c.remote_busy());
    CHECK(c.request_neighbours(rk) == 0 && c.remote_result().op == RO::Neighbours && c.remote_result().error == RE::NotLoggedIn);
    // a key that is not a contact: refused at once
    PubKey stranger{};
    stranger.fill(0x77);
    CHECK(c.request_status(stranger) == 0 && c.remote_result().error == RE::NotFound && c.remote_result().err_code == -1);
    CHECK(c.request_owner(stranger) == 0 && c.remote_result().error == RE::NotFound);
    // a chat node has none of this
    CHECK(c.request_status(contact_key(r.model, "Alice")) == 0 && c.remote_result().error == RE::Unsupported);
    CHECK(cmd_count(r.peer, cmd::kSendBinaryReq) == 0 && cmd_count(r.peer, cmd::kSendAnonReq) == 0);

    // ---- a guest session; STATUS: 50 + key + 0x01, SENT (the tag, flood, est_timeout), the push 0x8C with that tag
    c.start_login(rk, "", true);
    run(c, 100);
    CHECK(c.remote_session(rk) && c.remote_session(rk)->role == Client::RemoteRole::Guest);
    id = c.remote_result().id;
    const uint32_t sreq = c.request_status(rk);
    CHECK(sreq != 0 && c.remote_busy() && c.remote_state().op == RO::Status && c.remote_state().phase == Phase::Sending && c.remote_state().key == rk);
    run(c, 100);
    const Client::RemoteResult &res = c.remote_result();
    CHECK(!c.remote_busy() && res.id == id + 1 && res.req == sreq && res.op == RO::Status && res.ok && res.error == RE::None && res.sent && res.flood &&
          res.key == rk);
    CHECK(res.status.battery_mv == 4123 && res.status.noise_floor == -112 && res.status.last_snr == -6.25 && res.status.uptime_s == 259205 &&
          res.status.flood_dups == 222 && res.status.has_rx_errors && res.status.rx_errors == 33 && !res.status.room);
    const Bytes *wire = last_remote_cmd(r.peer, cmd::kSendBinaryReq);
    CHECK(wire && wire->size() == 34 && std::equal(rk.begin(), rk.end(), wire->begin() + 1) && (*wire)[33] == 0x01);
    CHECK(r.logged("remote status " + to_hex(rk).substr(0, 8)));

    // ---- the wait: 1.25 x est_timeout after SENT, as a timer (no command in flight); an answer lost on the air ends as Timeout
    r.peer.req_mode = 1;
    id = c.remote_result().id;
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_state().phase == Phase::Waiting && c.remote_state().flood && c.remote_state().est_timeout_ms == 1600 && c.queued() == 0 &&
          c.remote_state().deadline_ms <= g_now + 2000 && c.remote_state().deadline_ms > g_now + 1500);
    run(c, 1500);
    CHECK(c.remote_busy());
    run(c, 500);
    CHECK(!c.remote_busy() && c.remote_result().id == id + 1 && c.remote_result().error == RE::Timeout && c.remote_result().sent &&
          c.remote_result().op == RO::Status && c.remote_result().waited_ms >= 2000);
    CHECK(c.remote_session(rk));                                             // a timeout says nothing sure: the session stays

    // ---- a push with another tag is ignored; the right one ends the wait; the same push again (late) is ignored
    r.peer.req_mode = 2;
    r.peer.held_req.clear();
    id = c.remote_result().id;
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_state().phase == Phase::Waiting && r.peer.held_req.size() == 1);
    Bytes other = r.peer.held_req[0];
    other[5] ^= 0x01;                                                        // the last tag byte
    r.peer.push(other);
    run(c, 100);
    CHECK(c.remote_busy() && c.remote_result().id == id && r.logged("ignored"));
    r.peer.push(r.peer.held_req[0]);
    run(c, 100);
    CHECK(!c.remote_busy() && c.remote_result().id == id + 1 && c.remote_result().ok && c.remote_result().op == RO::Status);
    r.peer.push(r.peer.held_req[0]);
    run(c, 100);
    CHECK(c.remote_result().id == id + 1);
    // the answer after the wait is over: ignored
    r.peer.held_req.clear();
    c.request_status(rk);
    run(c, 2200);
    CHECK(c.remote_result().error == RE::Timeout);
    id = c.remote_result().id;
    r.peer.push(r.peer.held_req.back());
    run(c, 100);
    CHECK(c.remote_result().id == id && !c.remote_busy());

    // ---- one at a time: a new request cancels the pending one, whose answer is then ignored
    r.peer.held_req.clear();
    const uint32_t first = c.request_status(rk);
    run(c, 100);
    id = c.remote_result().id;
    const uint32_t second = c.request_neighbours(rk);
    CHECK(second != first && c.remote_result().id == id + 1 && c.remote_result().req == first && c.remote_result().error == RE::Cancelled &&
          c.remote_result().op == RO::Status && c.remote_result().sent);
    run(c, 100);
    CHECK(r.peer.held_req.size() == 2 && c.remote_state().req == second && c.remote_state().phase == Phase::Waiting);
    r.peer.push(r.peer.held_req[0]);
    run(c, 100);
    CHECK(c.remote_busy() && c.remote_result().id == id + 1);
    r.peer.push(r.peer.held_req[1]);
    run(c, 100);
    CHECK(c.remote_result().id == id + 2 && c.remote_result().req == second && c.remote_result().ok && c.remote_result().op == RO::Neighbours &&
          c.remote_result().neighbours.entries.size() == 14);
    // a login cancels a pending request too, and a request a pending login
    r.peer.held_req.clear();
    c.request_status(rk);
    run(c, 100);
    id = c.remote_result().id;
    c.start_login(ridge, "Adm1n-s3cret", false);
    CHECK(c.remote_result().id == id + 1 && c.remote_result().error == RE::Cancelled && c.remote_result().op == RO::Status);
    run(c, 100);
    CHECK(c.remote_session(ridge) && c.remote_session(ridge)->role == Client::RemoteRole::Admin);
    r.peer.req_mode = 0;

    // ---- cancel_remote(): Cancelled; a request still in the queue never goes out
    r.peer.commands.clear();
    r.peer.hold_remove = true;
    CHECK(c.remove_contacts({contact_key(r.model, "Alice")}));
    run(c, 40);
    c.request_status(rk);
    CHECK(c.remote_state().phase == Phase::Sending);
    c.cancel_remote();
    CHECK(!c.remote_busy() && c.remote_result().error == RE::Cancelled && !c.remote_result().sent && c.remote_result().op == RO::Status);
    c.cancel_bulk();
    r.peer.hold_remove = false;
    run(c, 6000);
    CHECK(cmd_count(r.peer, cmd::kSendBinaryReq) == 0);

    // ---- NEIGHBOURS, an admin session: pages of at most 14 entries (130 bytes of 4 byte prefixes), "more" while some remain
    c.start_login(rk, "Adm1n-s3cret", false);
    run(c, 100);
    CHECK(c.remote_session(rk) && c.remote_session(rk)->role == Client::RemoteRole::Admin);
    c.request_neighbours(rk);
    run(c, 100);
    const Client::RemoteResult &nr = c.remote_result();
    CHECK(nr.ok && nr.op == RO::Neighbours && nr.neighbours.total == 20 && nr.neighbours.offset == 0 && nr.neighbours.entries.size() == 14 &&
          nr.neighbours.more);
    CHECK(nr.neighbours.entries[0].prefix == (Bytes{0x90, 0x91, 0x92, 0x93}) && nr.neighbours.entries[0].heard_secs_ago == 30 &&
          nr.neighbours.entries[0].snr == 5.5);                             // newest first: Ridge, heard 30 s ago
    bool ascending = true;
    for (size_t i = 1; i < nr.neighbours.entries.size(); ++i) ascending = ascending && nr.neighbours.entries[i - 1].heard_secs_ago <= nr.neighbours.entries[i].heard_secs_ago;
    CHECK(ascending);
    auto nm = neighbour_name(r.model, nr.neighbours.entries[0].prefix);
    CHECK(nm.known && nm.text == "Ridge" && !nm.ambiguous);
    nm = neighbour_name(r.model, nr.neighbours.entries[1].prefix);
    CHECK(!nm.known && nm.text == "e0e1e2e3");
    wire = last_remote_cmd(r.peer, cmd::kSendBinaryReq);
    CHECK(wire && wire->size() == 44 && (*wire)[33] == 0x06 && (*wire)[34] == 0 && (*wire)[35] == 255 && (*wire)[36] == 0 && (*wire)[37] == 0 &&
          (*wire)[38] == 0 && (*wire)[39] == 4);
    c.request_neighbours(rk, 14);
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().neighbours.offset == 14 && c.remote_result().neighbours.entries.size() == 6 &&
          !c.remote_result().neighbours.more && c.remote_result().neighbours.total == 20);
    wire = last_remote_cmd(r.peer, cmd::kSendBinaryReq);
    CHECK(wire && (*wire)[36] == 14 && (*wire)[37] == 0);
    c.request_neighbours(rk, 0, 5, NeighbourOrder::StrongestFirst);
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().neighbours.entries.size() == 5 && c.remote_result().neighbours.more &&
          c.remote_result().neighbours.entries[0].snr == 7.0 && c.remote_result().neighbours.entries[1].snr == 6.0 &&
          c.remote_result().neighbours.entries[2].snr == 5.5);              // strongest first: two strangers, then Ridge
    wire = last_remote_cmd(r.peer, cmd::kSendBinaryReq);
    CHECK(wire && (*wire)[35] == 5 && (*wire)[38] == 2);

    // ---- OWNER with a session: OWNER_INFO 0x07 (it also gives the firmware version), even on a direct route
    c.request_owner(rk);
    run(c, 100);
    const Client::RemoteResult &orr = c.remote_result();
    CHECK(orr.ok && orr.op == RO::Owner && !orr.owner.anonymous && orr.owner.fw_version == "v1.18.0" && orr.owner.name == "Rptr" &&
          orr.owner.owner_info == "Sim owner\nkey 1234" && orr.has_fw_level && orr.fw_level == 2);
    wire = last_remote_cmd(r.peer, cmd::kSendBinaryReq);
    CHECK(wire && wire->size() == 34 && (*wire)[33] == 0x07);
    const size_t anon_before = cmd_count(r.peer, cmd::kSendAnonReq);
    c.request_owner(ridge);
    run(c, 100);
    CHECK(c.remote_result().ok && !c.remote_result().owner.anonymous && c.remote_result().owner.name == "Ridge" &&
          cmd_count(r.peer, cmd::kSendAnonReq) == anon_before);

    // ---- OWNER without a session: the anonymous request, only on a direct route (the repeater ignores it by flood)
    c.logout(ridge);
    run(c, 100);
    CHECK(!c.remote_session(ridge));
    c.request_owner(ridge);
    run(c, 100);
    const Client::RemoteResult &ar = c.remote_result();
    CHECK(ar.ok && ar.op == RO::Owner && ar.owner.anonymous && ar.owner.name == "Ridge" && ar.owner.owner_info == "Sim owner\nkey 1234" &&
          ar.owner.has_clock && ar.owner.clock == 1790001234 && ar.owner.fw_version.empty() && !ar.flood && !ar.has_fw_level);
    wire = last_remote_cmd(r.peer, cmd::kSendAnonReq);
    CHECK(wire && wire->size() == 35 && std::equal(ridge.begin(), ridge.end(), wire->begin() + 1) && (*wire)[33] == 0x02 && (*wire)[34] == 0x00);
    c.request_owner(far);                                                    // two hops: the reply path is the route reversed
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().owner.anonymous && c.remote_result().owner.name == "Far");
    wire = last_remote_cmd(r.peer, cmd::kSendAnonReq);
    CHECK(wire && wire->size() == 37 && (*wire)[34] == 0x02 && (*wire)[35] == 0xB2 && (*wire)[36] == 0xA1);
    r.peer.commands.clear();
    c.logout(rk);
    run(c, 100);
    CHECK(c.request_owner(rk) == 0 && c.remote_result().error == RE::NoDirectRoute && c.remote_result().op == RO::Owner);   // Rptr: flood
    CHECK(cmd_count(r.peer, cmd::kSendAnonReq) == 0 && cmd_count(r.peer, cmd::kSendBinaryReq) == 0);

    // ---- rooms: a status (ServerStats), no neighbours, no owner
    c.start_login(club, "", true);
    run(c, 100);
    CHECK(c.remote_session(club));
    c.request_status(club);
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().status.room && c.remote_result().status.posted == 17 && c.remote_result().status.post_pushes == 9 &&
          !c.remote_result().status.has_airtime_rx);
    CHECK(c.request_neighbours(club) == 0 && c.remote_result().error == RE::Unsupported);
    CHECK(c.request_owner(club) == 0 && c.remote_result().error == RE::Unsupported);

    // ---- a broken answer with the right tag: BadReply
    r.peer.status_len = 20;
    c.start_login(rk, "", true);
    run(c, 100);
    c.request_status(rk);
    run(c, 100);
    CHECK(!c.remote_result().ok && c.remote_result().error == RE::BadReply && c.remote_result().sent && !c.remote_busy());
    r.peer.status_len = 56;

    // ---- the board's errors: the node is not one of its contacts, its send queue is full, a firmware without the command
    Contact saved = r.peer.contacts[1];
    r.peer.contacts.erase(r.peer.contacts.begin() + 1);                    // Rptr still in the model, gone from the board
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_result().error == RE::NotFound && c.remote_result().err_code == err::kNotFound && !c.remote_result().sent);
    r.peer.contacts.insert(r.peer.contacts.begin() + 1, saved);
    r.peer.req_table_full = true;
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_result().error == RE::TableFull && c.remote_result().err_code == err::kTableFull);
    r.peer.req_table_full = false;
    r.peer.req_supported = false;
    c.request_neighbours(rk);
    run(c, 100);
    CHECK(c.remote_result().error == RE::Unsupported && c.remote_result().err_code == err::kUnsupported && c.remote_result().op == RO::Neighbours);
    c.request_owner(ridge);                                                  // anonymous (no session): 57 is unsupported as well
    run(c, 100);
    CHECK(c.remote_result().error == RE::Unsupported && c.remote_result().op == RO::Owner);
    r.peer.req_supported = true;

    // ---- Chats go on during a request's wait
    r.peer.req_mode = 2;
    r.peer.held_req.clear();
    c.request_status(rk);
    run(c, 100);
    const uint32_t seq = c.send_direct(to_hex(contact_key(r.model, "Alice")), "while the repeater thinks");
    run(c, 300);
    CHECK(seq != 0 && r.model.find_message(seq) && r.model.find_message(seq)->state == MsgState::Delivered && c.remote_busy());
    r.peer.push(r.peer.held_req.back());
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().op == RO::Status);
    r.peer.req_mode = 0;
}

/* Older repeaters (their FIRMWARE_VER_LEVEL in the login answer): level 1 (v1.9.0-v1.11.0) has neighbours but no OWNER_INFO, no
 * anonymous requests and no receive error counter; level 0 (before v1.9.0) has no neighbours either; a legacy login answer gives no level. */
static void test_remote_old_repeaters()
{
    using RE = Client::RemoteError;
    Ready r(s2_setup);
    Client &c = *r.client;
    const PubKey rk = contact_key(r.model, "Rptr");
    r.peer.remote_level = 1;
    c.start_login(rk, "", true);
    run(c, 100);
    CHECK(c.remote_session(rk) && c.remote_session(rk)->has_fw_level && c.remote_session(rk)->fw_level == 1);
    r.peer.commands.clear();
    CHECK(c.request_owner(rk) == 0 && c.remote_result().error == RE::Unsupported && cmd_count(r.peer, cmd::kSendBinaryReq) == 0);
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_result().ok && !c.remote_result().status.has_rx_errors && c.remote_result().status.has_airtime_rx);   // padding, not a counter
    c.request_neighbours(rk);                                                // level 1 answers a guest (no admin check in v1.9.0-v1.11.0)
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().neighbours.total == 20);
    // report 036's claim (the official app): older repeaters give neighbours to admins only. Such a node is silent for a guest: Timeout
    r.peer.neighbours_admin_only = true;
    c.request_neighbours(rk);
    run(c, 2200);
    CHECK(c.remote_result().error == RE::Timeout && c.remote_result().op == Client::RemoteOp::Neighbours);
    c.start_login(rk, "Adm1n-s3cret", false);
    run(c, 100);
    c.request_neighbours(rk);
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().neighbours.entries.size() == 14);
    r.peer.neighbours_admin_only = false;
    // level 0: no neighbours, no owner info
    r.peer.remote_level = 0;
    c.start_login(rk, "", true);
    run(c, 100);
    CHECK(c.remote_session(rk) && c.remote_session(rk)->fw_level == 0);
    CHECK(c.request_neighbours(rk) == 0 && c.remote_result().error == RE::Unsupported);
    CHECK(c.request_owner(rk) == 0 && c.remote_result().error == RE::Unsupported);
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_result().ok);
    // a legacy login answer (a companion before v1.10.0: no level): everything is tried, an old node then simply does not answer
    r.peer.remote_level = 2;
    r.peer.login_mode = 3;
    c.start_login(rk, "Adm1n-s3cret", false);
    run(c, 100);
    CHECK(c.remote_session(rk) && !c.remote_session(rk)->has_fw_level);
    c.request_owner(rk);
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().owner.fw_version == "v1.18.0" && !c.remote_result().has_fw_level);
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().status.has_rx_errors);
    r.peer.login_mode = 0;
}

static void test_remote_requests_relay_and_link()
{
    using RE = Client::RemoteError;
    Ready r(s2_setup);
    Client &c = *r.client;
    const PubKey rk = contact_key(r.model, "Rptr"), ridge = contact_key(r.model, "Ridge");
    c.start_login(rk, "", true);
    run(c, 100);
    // ---- a CLI session: start_relay() cancels the pending request; requests are refused while it runs
    r.peer.req_mode = 2;
    r.peer.held_req.clear();
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_state().phase == Client::RemoteState::Phase::Waiting);
    uint32_t id = c.remote_result().id;
    CHECK(c.start_relay() > 0 && !c.remote_busy() && c.remote_result().id == id + 1 && c.remote_result().error == RE::Cancelled &&
          c.remote_result().op == Client::RemoteOp::Status);
    r.peer.push(r.peer.held_req.back());                                     // its answer reaches the console; Mesh Hop ignores it
    run(c, 100);
    CHECK(c.remote_result().id == id + 1);
    r.peer.commands.clear();
    CHECK(c.request_status(rk) == 0 && c.remote_result().error == RE::Busy && c.remote_result().op == Client::RemoteOp::Status);
    CHECK(c.request_neighbours(rk) == 0 && c.remote_result().error == RE::Busy);
    CHECK(c.request_owner(ridge) == 0 && c.remote_result().error == RE::Busy && c.remote_result().op == Client::RemoteOp::Owner);
    c.stop_relay();
    run(c, 200);
    CHECK(!c.relay_active() && cmd_count(r.peer, cmd::kSendBinaryReq) == 0 && cmd_count(r.peer, cmd::kSendAnonReq) == 0 && c.remote_session(rk));
    // ---- a lost link: the wait ends (LinkLost), the sessions go; after the reconnect a request needs a new login
    c.request_status(rk);
    run(c, 100);
    CHECK(c.remote_busy());
    id = c.remote_result().id;
    r.peer.lost = true;
    run(c, 100);
    CHECK(c.link() == Link::Searching && !c.remote_busy() && c.remote_result().id == id + 1 && c.remote_result().error == RE::LinkLost &&
          c.remote_result().sent && c.remote_result().op == Client::RemoteOp::Status);
    CHECK(c.request_status(rk) == 0 && c.remote_result().error == RE::NotConnected);
    r.peer.lost = false;
    run(c, 4000);
    CHECK(c.ready() && c.request_status(rk) == 0 && c.remote_result().error == RE::NotLoggedIn);
    // the board never confirms the send: Timeout with sent = false (the command's own 5 s)
    r.peer.req_mode = 0;
    c.start_login(rk, "", true);
    run(c, 100);
    r.peer.silent = true;
    c.request_status(rk);
    run(c, 5200);
    CHECK(!c.remote_busy() && c.remote_result().error == RE::Timeout && !c.remote_result().sent);
    r.peer.silent = false;
    run(c, 4000);
    // not connected at all
    Ready off(nullptr, false);
    CHECK(off.client->request_status(rk) == 0 && off.client->remote_result().error == RE::NotConnected && off.client->remote_result().id == 1);
}

/* 0.8.0, the follow-ups of the core for the Repeater page: PATH_UPDATED (0x81) reads the contact back; the role of a node too old to
 * send its ACL byte; a NEIGHBOURS page after the first waits 25 s at least (meshcore_py fetch_all_neighbours). */
static void test_remote_s2_followups()
{
    using RE = Client::RemoteError;
    using Phase = Client::RemoteState::Phase;
    // ---- PATH_UPDATED: the companion learnt a route (a flood exchange brought one back); the contact is read back, so its route and
    // the anonymous owner request (direct only) follow
    {
        Ready r([](FakePeer &p) {
            s2_setup(p);
            p.contact_by_key = true;
        });
        Client &c = *r.client;
        const PubKey rk = contact_key(r.model, "Rptr");
        CHECK(r.model.find_contact(to_hex(rk)) && r.model.find_contact(to_hex(rk))->c.out_path_len == kPathFlood);
        CHECK(c.request_owner(rk) == 0 && c.remote_result().error == RE::NoDirectRoute);       // flood: refused at once
        for (Contact &x : r.peer.contacts)
            if (x.key == rk) {
                x.out_path_len = 1;
                x.out_path[0] = 0xA1;
            }
        r.peer.commands.clear();
        Bytes pu = {resp::kPathUpdate};
        pu.insert(pu.end(), rk.begin(), rk.end());
        r.peer.silent = true;                                                // the read waits for its answer
        r.peer.push(pu);
        r.peer.push(pu);                                                     // twice before the answer: one read
        run(c, 40);
        CHECK(c.path_lookups_pending() == 1 && cmd_count(r.peer, cmd::kGetContactByKey) == 1);
        r.peer.silent = false;
        run(c, 5200);                                                        // (that one times out: the board was silent)
        CHECK(c.path_lookups_pending() == 0 && r.logged("was not read back"));
        r.peer.push(pu);
        run(c, 100);
        CHECK(cmd_count(r.peer, cmd::kGetContactByKey) == 2 && c.path_lookups_pending() == 0 && r.logged("path updated " + to_hex(rk.data(), 4)));
        CHECK(r.model.find_contact(to_hex(rk))->c.out_path_len == 1 && r.model.find_contact(to_hex(rk))->c.out_path[0] == 0xA1);
        const uint32_t req = c.request_owner(rk);                            // direct now: the anonymous request goes
        run(c, 100);
        CHECK(req != 0 && c.remote_result().req == req && c.remote_result().ok && c.remote_result().owner.anonymous && c.remote_result().owner.name == "Rptr");
        const Bytes *wire = last_remote_cmd(r.peer, cmd::kSendAnonReq);
        CHECK(wire && wire->size() == 36 && (*wire)[34] == 0x01 && (*wire)[35] == 0xA1);   // the reply path: one hop back over A1
        // a failed anonymous request says it was one (the page's note about the repeater's limit)
        r.peer.req_mode = 1;
        c.request_owner(rk);
        run(c, 2200);
        CHECK(c.remote_result().error == RE::Timeout && c.remote_result().anonymous && c.remote_result().op == Client::RemoteOp::Owner);
        r.peer.req_mode = 0;
        // a key that is not a contact: nothing asked
        PubKey stranger{};
        stranger.fill(0x77);
        Bytes ps = {resp::kPathUpdate};
        ps.insert(ps.end(), stranger.begin(), stranger.end());
        r.peer.push(ps);
        run(c, 100);
        CHECK(cmd_count(r.peer, cmd::kGetContactByKey) == 2);
        // during a CLI session: nothing asked (the console sees the push and reads what it wants)
        CHECK(c.start_relay() > 0);
        run(c, 100);
        r.peer.push(pu);
        run(c, 100);
        CHECK(c.path_lookups_pending() == 0);
        c.stop_relay();
        run(c, 5000);
        CHECK(!c.relay_active() && cmd_count(r.peer, cmd::kGetContactByKey) == 2);
        // the link goes while a read waits: nothing stays pending
        r.peer.silent = true;
        r.peer.push(pu);
        run(c, 60);
        CHECK(c.path_lookups_pending() == 1);
        r.peer.lost = true;
        run(c, 100);
        CHECK(c.path_lookups_pending() == 0);
        r.peer.silent = false;
        r.peer.lost = false;
        run(c, 4000);
        CHECK(c.ready());
    }

    // ---- the role of a login: a node before v1.9.0 (level 0) has no ACL byte in its reply (a repeater: a reserved 0), so an admin
    // came out as guest in 0.7; byte 1 (is_admin) says it. A legacy OK answer carries no role: None (the page says Logged in)
    {
        Ready r(s2_setup);
        Client &c = *r.client;
        const PubKey rk = contact_key(r.model, "Rptr"), ridge = contact_key(r.model, "Ridge"), club = contact_key(r.model, "Club");
        r.peer.remote_level = 0;
        c.start_login(rk, "Adm1n-s3cret", false);
        run(c, 100);
        CHECK(c.remote_result().ok && c.remote_result().has_acl && c.remote_result().permissions == 0 && c.remote_result().has_fw_level &&
              c.remote_result().fw_level == 0);
        CHECK(c.remote_result().role == Client::RemoteRole::Admin && c.remote_session(rk) && c.remote_session(rk)->role == Client::RemoteRole::Admin);
        c.start_login(ridge, "", true);                                      // a guest of an old repeater: guest
        run(c, 100);
        CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::Guest);
        c.start_login(club, "", true);                                       // an old room: byte 1 = 0 is its GUEST (the room password: may post)
        run(c, 100);
        CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::ReadWrite);
        // level 1+ (v1.9.0+): the ACL byte counts
        r.peer.remote_level = 1;
        c.start_login(ridge, "", true);
        run(c, 100);
        CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::Guest && c.remote_result().permissions == 0);
        r.peer.acl.clear();
        c.start_login(ridge, "Adm1n-s3cret", false);
        run(c, 100);
        CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::Admin && c.remote_result().permissions == 3);
        // the legacy 8 byte answer from a companion of level 11: no role (a guest gives byte 1 = 0, like the legacy OK)
        r.peer.login_mode = 3;
        r.peer.acl.clear();
        c.start_login(rk, "", true);
        run(c, 100);
        CHECK(c.remote_result().ok && !c.remote_result().has_acl && c.remote_result().role == Client::RemoteRole::None &&
              c.remote_session(rk) && c.remote_session(rk)->role == Client::RemoteRole::None && r.logged("role unknown"));
        c.start_login(rk, "Adm1n-s3cret", false);                            // the admin bit is still believed
        run(c, 100);
        CHECK(c.remote_result().role == Client::RemoteRole::Admin);
        // a session of unknown role reads the status like any other
        r.peer.acl[to_hex(rk)] = 0;
        c.start_login(rk, "", true);
        run(c, 100);
        r.peer.login_mode = 0;
        r.peer.remote_level = 2;
        CHECK(c.remote_session(rk) && c.remote_session(rk)->role == Client::RemoteRole::None);
        c.request_status(rk);
        run(c, 100);
        CHECK(c.remote_result().ok && c.remote_result().op == Client::RemoteOp::Status);
    }

    // ---- NEIGHBOURS pages after the first: 25 s at least (the first page and the other requests: 1.25 x est_timeout)
    {
        Ready r(s2_setup);
        Client &c = *r.client;
        const PubKey rk = contact_key(r.model, "Rptr");
        c.start_login(rk, "", true);
        run(c, 100);
        r.peer.req_mode = 1;                                                 // the answers are lost: the waits run out
        c.request_neighbours(rk, 0);
        run(c, 100);
        CHECK(c.remote_state().phase == Phase::Waiting && c.remote_state().deadline_ms <= g_now + 2000 && c.remote_state().deadline_ms > g_now + 1800);
        run(c, 2000);
        CHECK(!c.remote_busy() && c.remote_result().error == RE::Timeout);
        c.request_neighbours(rk, 14);
        run(c, 100);
        CHECK(c.remote_state().phase == Phase::Waiting && c.remote_state().nb_offset == 14 && c.remote_state().deadline_ms <= g_now + 25000 &&
              c.remote_state().deadline_ms > g_now + 24800);
        CHECK(r.logged("waiting up to 25000 ms"));
        run(c, 20000);
        CHECK(c.remote_busy());
        run(c, 5000);
        CHECK(!c.remote_busy() && c.remote_result().error == RE::Timeout && c.remote_result().waited_ms >= 25000 && c.remote_result().waited_ms < 26000);
        c.request_status(rk);                                                // a status: the usual wait
        run(c, 100);
        CHECK(c.remote_state().deadline_ms <= g_now + 2000);
        c.cancel_remote();
        r.peer.req_est = 40000;                                              // a longer estimate wins over the 25 s: 50 s
        c.request_neighbours(rk, 14);
        run(c, 100);
        CHECK(c.remote_state().deadline_ms <= g_now + 50000 && c.remote_state().deadline_ms > g_now + 49800);
        c.cancel_remote();
        r.peer.req_est = 400000;                                             // never more than kRemoteMaxWaitMs
        c.request_neighbours(rk, 14);
        run(c, 100);
        CHECK(c.remote_state().deadline_ms <= g_now + Client::kRemoteMaxWaitMs && c.remote_state().deadline_ms > g_now + Client::kRemoteMaxWaitMs - 200);
        c.cancel_remote();
        r.peer.req_est = 1600;
        r.peer.req_mode = 0;
        c.request_neighbours(rk, 14);                                        // answered: the page as before
        run(c, 100);
        CHECK(c.remote_result().ok && c.remote_result().neighbours.offset == 14 && c.remote_result().neighbours.entries.size() == 6);
    }
}
