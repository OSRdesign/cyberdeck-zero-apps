// Phase 2 (0.2.0) tests of the UI-independent logic, included by test_ui.cpp: the waiting Yes, the pick list, contact selection and groups,
// contact details, Nearby rows, the message search, the statistics view, the auto-add filter, the Settings layout of the new rows, the
// new choices and the Back order.

static std::string hexkey(uint8_t id) { return to_hex(make_contact(id, "x", 1, 0).key); }

static void test_p2_confirm_and_list()
{
    // a Yes that waits 3 s: 3, 3, 2, 1, then 0
    CHECK(confirm_wait_left(1000, 1000, 3000) == 3 && confirm_wait_left(1000, 1001, 3000) == 3 && confirm_wait_left(1000, 2000, 3000) == 2);
    CHECK(confirm_wait_left(1000, 3999, 3000) == 1 && confirm_wait_left(1000, 4000, 3000) == 0 && confirm_wait_left(1000, 90000, 3000) == 0);
    CHECK(confirm_wait_left(1000, 500, 3000) == 3 && confirm_wait_left(1000, 1000, 0) == 0);                  // a clock that went back waits in full; no delay: ready
    CHECK(confirm_yes_text("Yes, erase", 3) == "Yes, erase (3)" && confirm_yes_text("Yes, erase", 0) == "Yes, erase");

    int sel = 0;
    CHECK(list_key(ke(Key::Down), sel, 4) == ListResult::Moved && sel == 1 && list_key(ke(Key::PageDown), sel, 4) == ListResult::Moved && sel == 3);
    CHECK(list_key(ke(Key::Down), sel, 4) == ListResult::None && sel == 3 && list_key(ke(Key::Home), sel, 4) == ListResult::Moved && sel == 0);
    CHECK(list_key(ke(Key::End), sel, 4) == ListResult::Moved && sel == 3 && list_key(ke(Key::Up), sel, 4) == ListResult::Moved && sel == 2);
    CHECK(list_key(ke(Key::Enter), sel, 4) == ListResult::Accept && list_key(ke(Key::Enter, true), sel, 4) == ListResult::None);
    CHECK(list_key(ke(Key::Esc), sel, 4) == ListResult::Cancel && list_key(ke(Key::Esc, true), sel, 4) == ListResult::None);
    CHECK(list_key(ke(Key::Esc), sel, 0) == ListResult::Cancel && list_key(ke(Key::Enter), sel, 0) == ListResult::None);
}

static void test_p2_selection_and_groups()
{
    set_time_source(fake_time);
    Model m;
    // heard: 0 never, 1 two days ago, 2 ten days ago, 3 forty days ago, 4 an hour ago, 5 never
    const uint32_t kDay = 86400;
    m.upsert_contact(make_contact(0x01, "Alice", advtype::kChat, 0));
    m.upsert_contact(make_contact(0x02, "Bob", advtype::kChat, g_time - 2 * kDay));
    m.upsert_contact(make_contact(0x03, "Carol", advtype::kRepeater, g_time - 10 * kDay));
    m.upsert_contact(make_contact(0x04, "Dave", advtype::kRoom, g_time - 40 * kDay));
    m.upsert_contact(make_contact(0x05, "Eve", advtype::kChat, g_time - 3600));
    m.upsert_contact(make_contact(0x06, "Frank", advtype::kSensor, 0));
    ContactView v;
    v.sort = SortKey::Name;
    ContactSnapshot snap = make_contact_snapshot(m, v);
    CHECK(snap.rows.size() == 6 && snap.rows[0].name == "Alice" && snap.rows[5].name == "Frank");

    ContactSelection sel;
    CHECK(sel.size() == 0 && sel.toggle(hexkey(1)) && sel.contains(hexkey(1)) && !sel.toggle(hexkey(1)) && !sel.contains(hexkey(1)));
    sel.select_shown(snap.rows);
    CHECK(sel.size() == 6);
    sel.clear();
    sel.select_stale(snap.rows, 7, g_time);                                  // never heard (Alice, Frank), 10 days (Carol), 40 days (Dave)
    CHECK(sel.size() == 4 && sel.contains(hexkey(1)) && sel.contains(hexkey(3)) && sel.contains(hexkey(4)) && sel.contains(hexkey(6)) && !sel.contains(hexkey(2)));
    sel.clear();
    sel.select_stale(snap.rows, 30, g_time);
    CHECK(sel.size() == 3 && !sel.contains(hexkey(3)));                      // Carol is only 10 days old
    sel.clear();
    sel.select_never_heard(snap.rows);
    CHECK(sel.size() == 2 && sel.contains(hexkey(1)) && sel.contains(hexkey(6)));
    sel.invert(snap.rows);
    CHECK(sel.size() == 4 && !sel.contains(hexkey(1)) && sel.contains(hexkey(5)));

    // only what is shown is acted on: a filter hides some marked rows, they stay marked but are not part of the delete
    sel.clear();
    sel.select_shown(snap.rows);
    ContactView rep = v;
    rep.filter.type = TypeFilter::Chat;
    ContactSnapshot chat_only = make_contact_snapshot(m, rep);
    CHECK(chat_only.rows.size() == 3);
    CHECK(sel.shown_selected(chat_only.rows).size() == 3 && sel.hidden_count(chat_only.rows) == 3);
    const BulkDeletePrompt p = bulk_delete_prompt(m, chat_only.rows, sel);
    CHECK(!p.empty && p.count == 3 && p.title == "Delete 3 contacts?" && p.body.find("Alice, Bob, Eve") == 0 && p.body.find("3 marked but hidden") != std::string::npos);
    CHECK(p.body.find("one by one") != std::string::npos && p.body.find("cannot be undone") != std::string::npos);
    sel.clear();
    CHECK(bulk_delete_prompt(m, snap.rows, sel).empty);
    sel.toggle(hexkey(2));
    const BulkDeletePrompt one = bulk_delete_prompt(m, snap.rows, sel);
    CHECK(one.title == "Delete 1 contact?" && one.body.find("Bob.") == 0);
    sel.select_shown(snap.rows);
    CHECK(bulk_delete_prompt(m, snap.rows, sel).body.find("and 3 more") != std::string::npos);
    std::set<std::string> alive = {hexkey(2)};
    sel.keep_only(alive);
    CHECK(sel.size() == 1 && sel.contains(hexkey(2)));

    const SelectMenu menu = select_menu_items();
    CHECK(menu.labels.size() == 6 && menu.actions.size() == 6 && menu.actions[0] == SelectAction::All && menu.actions[5] == SelectAction::Invert);

    // groups: the contacts table can show one group
    m.groups_mut().add_group("Family");
    m.groups_mut().add_group("Hikers");
    m.groups_mut().add_members("Family", {hexkey(1), hexkey(2), hexkey(9)});          // 9 is not a contact: ignored in the table
    m.groups_mut().add_members("Hikers", {hexkey(3)});
    ContactView g = v;
    g.group = "Family";
    CHECK(g != v && !(g == v));
    const ContactSnapshot fam = make_contact_snapshot(m, g);
    CHECK(fam.rows.size() == 2 && fam.rows[0].name == "Alice" && fam.rows[1].name == "Bob");
    g.filter.type = TypeFilter::Repeater;
    CHECK(make_contact_snapshot(m, g).rows.empty());                                  // group and type filter combine
    g.group = "No such group";
    g.filter.type = TypeFilter::All;
    CHECK(make_contact_snapshot(m, g).rows.empty());
    const std::vector<std::string> labels = group_filter_labels(m.groups());
    CHECK(labels.size() == 3 && labels[0] == "All contacts" && labels[1] == "Family (3)" && labels[2] == "Hikers (1)");
    CHECK(group_pick_labels(m.groups()) == std::vector<std::string>({"Family (3)", "Hikers (1)"}));

    // what the Group button offers: without groups only "new group"
    GroupMenu empty_menu = group_assign_menu(ContactGroups(), 2);
    CHECK(empty_menu.actions.size() == 1 && empty_menu.actions[0] == GroupAction::NewWith && empty_menu.labels[0] == "New group with 2 contacts");
    GroupMenu full = group_assign_menu(m.groups(), 1);
    CHECK(full.actions.size() == 4 && full.actions[0] == GroupAction::AddTo && full.actions[1] == GroupAction::RemoveFrom && full.actions[2] == GroupAction::NewWith &&
          full.actions[3] == GroupAction::RemoveAll && full.labels[0] == "Add 1 contact to a group");
}

static void test_p2_contact_detail()
{
    set_time_source(fake_time);
    Model m;
    SelfInfo self;                                   // the board first: its identity decides whose data the model holds
    self.lat = 48.8566;
    self.lon = 2.3522;
    m.set_self(self);
    {
        Contact c = make_contact(0x21, "Hilltop repeater", advtype::kRepeater, g_time - 600, 49.12, 2.61, 0x42);     // 2 hops, 2-byte hashes
        for (int i = 0; i < 8; ++i) c.out_path[static_cast<size_t>(i)] = static_cast<uint8_t>(0xA0 + i);
        m.upsert_contact(c);
        Contact flood = make_contact(0x22, "Flooded", advtype::kChat, 0);
        m.upsert_contact(flood);
        Contact direct = make_contact(0x23, "Next door", advtype::kChat, g_time - 30, 0, 0, 0x00);
        m.upsert_contact(direct);
        Contact three = make_contact(0x24, "Three", advtype::kRoom, g_time - 30, 0, 0, 0x82);                        // 2 hops, 3-byte hashes
        for (int i = 0; i < 6; ++i) three.out_path[static_cast<size_t>(i)] = static_cast<uint8_t>(0x10 + i);
        m.upsert_contact(three);
    }
    m.groups_mut().add_group("Relays");
    m.groups_mut().add_group("Home");
    m.groups_mut().add_members("Relays", {hexkey(0x21)});
    m.groups_mut().add_members("Home", {hexkey(0x21)});

    ContactDetail d = build_contact_detail(m, hexkey(0x21));
    CHECK(d.found && !d.chat && d.name == "Hilltop repeater" && d.type == "Repeater" && d.title == "Hilltop repeater");
    CHECK(d.key1.size() == 32 && d.key2.size() == 32 && d.key1 + d.key2 == hexkey(0x21));
    CHECK(d.route == "2 hops (2-byte hashes): a0a1 > a2a3");
    CHECK(d.heard == "10 min ago" && d.last_advert == fmt_node_time(g_time - 600) && d.last_advert.size() == 16);
    CHECK(d.position == "49.12000, 2.61000" && d.distance.find("km") != std::string::npos && d.snr == "-");
    CHECK(d.groups == "Relays, Home");
    d = build_contact_detail(m, hexkey(0x22));
    CHECK(d.chat && d.route == "no route yet (flood)" && d.heard == "-" && d.last_advert == "-" && d.position == "not shared" && d.distance == "-" && d.groups == "none");
    CHECK(build_contact_detail(m, hexkey(0x23)).route == "direct (0 hops)");
    CHECK(build_contact_detail(m, hexkey(0x24)).route == "2 hops (3-byte hashes): 101112 > 131415");
    CHECK(!build_contact_detail(m, hexkey(0x99)).found);
    CHECK(fmt_node_time(5000) == "-" && fmt_node_time(0) == "-" && fmt_position(false, 1, 2) == "not shared" && fmt_position(true, -1.5, 2.25) == "-1.50000, 2.25000");
    std::array<uint8_t, 64> path{};
    CHECK(fmt_route(0xFF, path) == "no route yet (flood)" && fmt_route(0x01, path) == "1 hop (1-byte hashes): 00");
}

static void test_p2_nearby_rows()
{
    set_time_source(fake_time);
    Model m;
    m.upsert_contact(make_contact(0x31, "Known", advtype::kChat, g_time));
    NearbyObs o;
    o.key_hex = hexkey(0x31);                           // a contact heard again
    o.type = 1; o.name = "Known"; o.has_snr = true; o.snr = 6.5; o.hops = 0;
    m.note_nearby(o, g_time - 5);
    HeardAdvert h;
    h.key = make_contact(0x32, "x", 1, 0).key; h.type = 2; h.name = "Ridge"; h.hops = 2; h.hash_size = 2; h.snr = -3.25; h.rssi = -101;
    m.note_heard_advert(h, g_time - 60);
    m.add_pending(make_contact(0x33, "Waiting", advtype::kChat, 0), g_time - 120);
    NearbyObs nameless;
    nameless.key_hex = hexkey(0x34);
    nameless.type = 4; nameless.has_snr = true; nameless.snr = 1.0;
    m.note_nearby(nameless, g_time - 300);
    NearbyObs untyped;
    untyped.key_hex = hexkey(0x35);
    m.note_nearby(untyped, g_time - 400);
    NearbyObs partial;
    partial.key_hex = "0123456789abcdef";
    partial.full_key = false; partial.type = 3;
    m.note_nearby(partial, g_time - 500);

    auto rows = build_nearby_rows(m, false);
    CHECK(rows.size() == 6 && rows[0].state == NearbyState::Pending && rows[0].title == "Waiting");                  // pending first
    CHECK(rows[0].can_add && rows[0].type_name == "Chat" && rows[0].detail.find("2 min ago") != std::string::npos);
    CHECK(rows[1].key_hex == hexkey(0x31) && rows[1].state == NearbyState::Contact && !rows[1].can_add && rows[1].detail == "SNR 6.5 dB  direct  5 s ago");
    CHECK(rows[2].title == "Ridge" && rows[2].state == NearbyState::New && rows[2].can_add && rows[2].detail.find("2 hops (2-byte hashes)  1 min ago") != std::string::npos);
    CHECK(rows[3].title == "Node " + hexkey(0x34).substr(0, 6) && rows[3].type_name == "Sensor" && rows[3].can_add);
    CHECK(rows[4].type_name == "?" && !rows[4].can_add);                                                              // no type yet: cannot be added
    CHECK(!rows[5].can_add && rows[5].type_name == "Room");                                                           // only a prefix of the key
    // a known contact heard without a name in the observation: the row shows the contact's name and type
    m.upsert_contact(make_contact(0x36, "Hill", advtype::kRepeater, g_time));
    NearbyObs bare;
    bare.key_hex = hexkey(0x36);
    bare.discovered = true;
    m.note_nearby(bare, g_time - 2);
    bool found_hill = false;
    for (const NearbyRow &r : build_nearby_rows(m, false))
        if (r.key_hex == hexkey(0x36)) { found_hill = r.title == "Hill" && r.type_name == "Repeater" && r.state == NearbyState::Contact; }
    CHECK(found_hill);
    m.remove_nearby(hexkey(0x36));
    // ignore hides, the switch shows them again (as "ignored", with no Add)
    m.set_ignored(hexkey(0x34), true);
    m.set_ignored(hexkey(0x33), true);
    CHECK(build_nearby_rows(m, false).size() == 4 && ignored_nearby_count(m) == 2 && m.pending_count() == 0);
    rows = build_nearby_rows(m, true);
    CHECK(rows.size() == 6);
    int ignored = 0;
    for (const NearbyRow &r : rows)
        if (r.state == NearbyState::Ignored) { ++ignored; CHECK(!r.can_add); }
    CHECK(ignored == 2 && std::string(nearby_state_name(NearbyState::Pending)) == "waiting" && std::string(nearby_state_name(NearbyState::Ignored)) == "ignored");
    CHECK(fmt_signal(true, 5.5, true, -80) == "SNR 5.5 dB  RSSI -80" && fmt_signal(false, 0, true, -80) == "RSSI -80" && fmt_signal(false, 0, false, 0) == "no signal figure");
}

static void test_p2_search_ui()
{
    set_time_source(fake_time);
    Model m;
    ChannelRec ch;
    ch.idx = 0; ch.name = "Public"; ch.empty = false;
    m.set_channel(ch);
    m.upsert_contact(make_contact(0x10, "Alice", advtype::kChat, 0));
    IncomingMessage cm;
    cm.channel = true;
    cm.channel_idx = 0;
    cm.text = "Zed: the quick brown fox jumps over the lazy dog every single morning without fail";
    cm.sender_timestamp = 1;
    m.add_incoming(cm, g_time - 2 * 86400);
    IncomingMessage dm;
    for (int i = 0; i < 6; ++i) dm.prefix[static_cast<size_t>(i)] = 0x10;
    dm.text = "see you at the lake";
    dm.sender_timestamp = 2;
    m.add_incoming(dm, g_time - 600);
    m.add_outgoing("d:101010101010", "ok lake it is", g_time - 300);

    SearchState st;
    CHECK(st.query(g_time).empty() && st.date_label() == "Any date" && st.conv_label(m) == "All chats");
    size_t total = 0;
    auto rows = build_search_rows(m, st, &total, g_time);
    CHECK(rows.size() == 3 && total == 3 && rows[0].snippet == "ok lake it is" && rows[0].outgoing && rows[0].conv == "d:101010101010");
    CHECK(rows[0].head.find("Alice   You") != std::string::npos);
    CHECK(rows[2].head.find("Public   Zed") != std::string::npos && rows[2].head.size() > 11 && rows[2].head[2] == '-');                  // an older day shows the date
    st.text = "lake";
    rows = build_search_rows(m, st, &total, g_time);
    CHECK(rows.size() == 2 && total == 2);
    st.conv = "d:101010101010";
    st.dir = SearchDir::Received;
    rows = build_search_rows(m, st, nullptr, g_time);
    CHECK(rows.size() == 1 && rows[0].snippet == "see you at the lake" && st.conv_label(m) == "Alice");
    st.cycle_dir();
    CHECK(st.dir == SearchDir::Sent);
    st.cycle_dir();
    CHECK(st.dir == SearchDir::Any);
    st.cycle_dir();
    CHECK(st.dir == SearchDir::Received);
    st.dir = SearchDir::Any;
    st.conv.clear();
    st.text.clear();
    st.date = DatePreset::Day;
    CHECK(st.date_label() == "Last 24 h" && build_search_rows(m, st, nullptr, g_time).size() == 2);                 // the 2 day old one is out
    st.date = DatePreset::Custom;
    st.from = g_time - 3 * 86400;
    st.to = g_time - 86400;
    CHECK(build_search_rows(m, st, nullptr, g_time).size() == 1 && st.date_label().find(" to ") != std::string::npos);

    // the snippet: a long text is cut around the match, with the ends marked
    const std::string text = "Zed: the quick brown fox jumps over the lazy dog every single morning without fail";
    CHECK(make_snippet("short", "x", 56) == "short" && make_snippet("line one\nline two", "", 56) == "line one line two");
    const std::string s = make_snippet(text, "morning", 30);
    CHECK(utf8_length(s) <= 33 && s.find("morning") != std::string::npos && s.rfind("...", 0) == 0);
    const std::string s2 = make_snippet(text, "quick", 30);
    CHECK(s2.find("quick") != std::string::npos && s2.size() > 3 && s2.find("...") == s2.size() - 3 && s2.rfind("...", 0) != 0);
    CHECK(make_snippet(text, "absent", 20) == truncate_ellipsis(text, 20));
    const std::string acc = make_snippet("caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9 caf\xC3\xA9", "caf\xC3\xA9", 12);
    for (size_t i = 0; i < acc.size(); ++i) CHECK(!((static_cast<unsigned char>(acc[i]) & 0xC0) == 0x80 && (i == 0)));    // never starts inside a character

    // when
    CHECK(fmt_when(g_time - 300, g_time) == fmt_clock(g_time - 300) && fmt_when(5, g_time) == "--:--");
    CHECK(fmt_when(g_time - 5 * 86400, g_time).size() == 11 && fmt_when(g_time - 5 * 86400, g_time)[2] == '-');
    const auto convs = search_conversations(m);
    CHECK(convs.size() == 2 && convs[0].first == "c:0" && convs[0].second == "Public" && convs[1].first == "d:101010101010" && convs[1].second == "Alice");
}

static void test_p2_stats_and_autoadd()
{
    set_time_source(fake_time);
    StatsSnapshot s;
    StatsView v = build_stats_view(s, g_time);
    CHECK(!v.any && v.left.empty() && v.right.empty() && v.updated == "Not read yet");
    s.core = CoreStats{3962, 7300, 2, 1};
    s.radio = RadioStats{-110, -95, 5.5, 61, 940};
    s.packets = PacketStats{500, 120, 14, 106, 310, 190, true, 9};
    s.updated = g_time - 4;
    v = build_stats_view(s, g_time);
    CHECK(v.any && v.left.size() == 9 && v.right.size() == 7 && v.updated == "Updated 4 s ago");
    CHECK(v.left[0].label == "Battery" && v.left[0].value == "3.96 V" && v.left[1].value == "2 h 01 min" && v.left[2].value == "2" && v.left[3].value == "1");
    CHECK(v.left[4].value == "-110 dBm" && v.left[5].value == "-95 dBm" && v.left[6].value == "5.5 dB" && v.left[7].label == "TX airtime" && v.left[7].value == "61 s (0.84 %)" && v.left[8].value == "940 s");
    CHECK(v.right[0].label == "Received" && v.right[0].value == "500" && v.right[6].label == "Receive errors" && v.right[6].value == "9");
    s.packets->has_errors = false;
    CHECK(build_stats_view(s, g_time).right[6].value == "not reported");
    s.core.reset();
    v = build_stats_view(s, g_time);
    CHECK(v.left.size() == 5 && v.left[3].value == "61 s");                       // no uptime: no duty figure
    CHECK(fmt_uptime(45) == "45 s" && fmt_uptime(59) == "59 s" && fmt_uptime(60) == "1 min" && fmt_uptime(3599) == "59 min" && fmt_uptime(3600) == "1 h 00 min" &&
          fmt_uptime(7300) == "2 h 01 min" && fmt_uptime(86400) == "1 d 0 h" && fmt_uptime(3 * 86400 + 4 * 3600) == "3 d 4 h");

    AutoaddConfig c;
    c.config = kAutoaddChat | kAutoaddSensor;
    auto items = autoadd_items(c);
    CHECK(items.size() == 5 && items[0].label == "Chat nodes" && items[0].on && !items[1].on && !items[2].on && items[3].on && !items[4].on && items[4].flag == kAutoaddOverwrite);
    CHECK(autoadd_summary(c) == "Chat, Sensor");
    CHECK(autoadd_toggled(c.config, kAutoaddRepeater) == (kAutoaddChat | kAutoaddSensor | kAutoaddRepeater) && autoadd_toggled(c.config, kAutoaddChat) == kAutoaddSensor);
    c.config = 0;
    CHECK(autoadd_summary(c) == "none");
    c.config = kAutoaddOverwrite | kAutoaddRoom;
    CHECK(autoadd_summary(c) == "Room + replace oldest");
}

static void test_p2_settings_and_choices()
{
    SettingsContext ctx;
    ctx.connected = true;
    ctx.have_self = true;
    ctx.channel_admin.available = true;
    ctx.path_hash.available = true;
    ctx.stats.available = true;
    ctx.autoadd.available = true;
    ctx.repeat.shown = true;
    ctx.repeat.available = true;
    auto kinds_of = [](const std::vector<SRowSpec> &rows) {
        std::vector<SRowKind> k;
        for (const SRowSpec &r : rows) k.push_back(r.kind);
        return k;
    };
    auto has = [&](const std::vector<SRowSpec> &rows, SRowKind k) {
        const auto v = kinds_of(rows);
        return std::find(v.begin(), v.end(), k) != v.end();
    };
    auto pos = [&](const std::vector<SRowSpec> &rows, SRowKind k) {
        const auto v = kinds_of(rows);
        return static_cast<int>(std::find(v.begin(), v.end(), k) - v.begin());
    };
    auto rows = settings_layout(ctx);
    CHECK(has(rows, SRowKind::Position) && has(rows, SRowKind::Stats) && has(rows, SRowKind::PathHash) && has(rows, SRowKind::Repeat) && has(rows, SRowKind::AdvertEvery) &&
          has(rows, SRowKind::AdvertKind) && has(rows, SRowKind::ManualAdd) && has(rows, SRowKind::AutoAddTypes) && has(rows, SRowKind::Reboot) && has(rows, SRowKind::FactoryReset));
    CHECK(!has(rows, SRowKind::PathHashNotice) && !has(rows, SRowKind::RepeatNotice) && !has(rows, SRowKind::StatsNotice) && !has(rows, SRowKind::AutoAddNotice));
    CHECK(pos(rows, SRowKind::Tx) < pos(rows, SRowKind::PathHash) && pos(rows, SRowKind::PathHash) < pos(rows, SRowKind::Repeat) && pos(rows, SRowKind::Repeat) < pos(rows, SRowKind::RetryAttempts));
    CHECK(pos(rows, SRowKind::AdvertEvery) + 1 == pos(rows, SRowKind::AdvertKind) && pos(rows, SRowKind::ManualAdd) + 1 == pos(rows, SRowKind::AutoAddTypes));
    CHECK(pos(rows, SRowKind::HistoryNote) < pos(rows, SRowKind::Reboot) && pos(rows, SRowKind::Reboot) + 1 == pos(rows, SRowKind::FactoryReset) && pos(rows, SRowKind::FactoryReset) < pos(rows, SRowKind::Undo));
    CHECK(rows.back().kind == SRowKind::Save && rows[rows.size() - 2].kind == SRowKind::Undo);                     // Undo and Save stay the last two rows
    int infos4 = 0;
    for (const SRowSpec &r : rows)
        if (r.kind == SRowKind::Info && r.arg == 4) ++infos4;
    CHECK(infos4 == 0);                                                                                            // the path hash size is only in the Radio section (the row with the popup)

    // the firmware lacks the features: notices instead of rows (path hash needs level 10, repeat level 9)
    ctx.path_hash.available = false;
    ctx.path_hash.notice = "Firmware too old for this feature: Path hash size (needs firmware level 10, the board has 9)";
    ctx.stats.available = false;
    ctx.stats.notice = "Firmware too old for this feature: Statistics";
    ctx.autoadd.available = false;
    ctx.autoadd.notice = "Firmware too old for this feature: Auto-add contacts";
    ctx.repeat.available = false;
    ctx.repeat.note = "Not allowed on this frequency";
    rows = settings_layout(ctx);
    CHECK(!has(rows, SRowKind::PathHash) && has(rows, SRowKind::PathHashNotice) && !has(rows, SRowKind::Stats) && has(rows, SRowKind::StatsNotice) &&
          !has(rows, SRowKind::AutoAddTypes) && has(rows, SRowKind::AutoAddNotice) && !has(rows, SRowKind::Repeat) && has(rows, SRowKind::RepeatNotice) && has(rows, SRowKind::ManualAdd));
    infos4 = 0;
    for (const SRowSpec &r : rows)
        if (r.kind == SRowKind::Info && r.arg == 4) ++infos4;
    CHECK(infos4 == 0);
    for (SRowKind k : {SRowKind::PathHashNotice, SRowKind::StatsNotice, SRowKind::RepeatNotice, SRowKind::AutoAddNotice}) CHECK(!settings_row_selectable(k));
    for (SRowKind k : {SRowKind::PathHash, SRowKind::Stats, SRowKind::Repeat, SRowKind::AdvertEvery, SRowKind::AdvertKind, SRowKind::ManualAdd, SRowKind::AutoAddTypes, SRowKind::Reboot,
                       SRowKind::FactoryReset, SRowKind::Position}) CHECK(settings_row_selectable(k));
    // hidden features leave no row at all; not connected: no board rows, but the advert schedule is local and stays
    ctx.path_hash = FeatureState();
    ctx.path_hash.hidden = true;
    ctx.stats = FeatureState();
    ctx.stats.hidden = true;
    ctx.repeat = RepeatState();
    ctx.connected = false;
    ctx.have_self = false;
    rows = settings_layout(ctx);
    CHECK(!has(rows, SRowKind::PathHash) && !has(rows, SRowKind::PathHashNotice) && !has(rows, SRowKind::Stats) && !has(rows, SRowKind::StatsNotice) && !has(rows, SRowKind::Repeat) &&
          !has(rows, SRowKind::RepeatNotice) && !has(rows, SRowKind::ManualAdd) && !has(rows, SRowKind::Reboot) && !has(rows, SRowKind::FactoryReset) && !has(rows, SRowKind::Position));
    CHECK(has(rows, SRowKind::AdvertEvery) && has(rows, SRowKind::AdvertKind) && rows.back().kind == SRowKind::Save);
    for (int s = 0; s < 10; ++s) CHECK(std::string(settings_section_title(s)).size() > 3);
    CHECK(std::string(settings_section_title(7)) == "SCHEDULED ADVERT" && std::string(settings_section_title(10)).empty() && std::string(settings_section_title(5)) == "RADIO CHANGES");

    // the path hash choice: 1 / 2 / 3 bytes, starts on the board's mode, one line of explanation
    ChoiceExtra extra;
    extra.path_hash_mode = 1;
    Choice c = make_choice(ChoiceField::PathHash, RadioSettings(), RetrySettings(), &extra);
    CHECK(c.title == "Path hash size" && c.labels == std::vector<std::string>({"1 byte", "2 bytes", "3 bytes"}) && c.index == 1 && c.label() == "2 bytes" && c.position() == "2 / 3");
    CHECK(c.note.find("fewer collisions") != std::string::npos && c.note.find("less room") != std::string::npos && c.note.find('\n') == std::string::npos);
    CHECK(choice_key(ke(Key::Right), c) == ChoiceResult::Moved && c.index == 2 && choice_key(ke(Key::Right), c) == ChoiceResult::None && path_hash_mode_from_index(c.index) == 2);
    CHECK(path_hash_mode_from_index(0) == 0 && path_hash_mode_from_index(3) == -1 && path_hash_mode_from_index(-1) == -1);
    extra.path_hash_mode = -1;                                                                                     // unknown: starts on the first
    CHECK(make_choice(ChoiceField::PathHash, RadioSettings(), RetrySettings(), &extra).index == 0);

    // the scheduled advert: off, 1, 3, 6, 12 h; flood or zero-hop; default off
    extra.schedule = AdvertSchedule();
    c = make_choice(ChoiceField::AdvertEvery, RadioSettings(), RetrySettings(), &extra);
    CHECK(c.labels == std::vector<std::string>({"Off", "Every 1 h", "Every 3 h", "Every 6 h", "Every 12 h"}) && c.index == 0 && c.note.find("Default: off") != std::string::npos);
    extra.schedule.interval_hours = 6;
    extra.schedule.flood = false;
    CHECK(make_choice(ChoiceField::AdvertEvery, RadioSettings(), RetrySettings(), &extra).index == 3);
    Choice kind = make_choice(ChoiceField::AdvertKind, RadioSettings(), RetrySettings(), &extra);
    CHECK(kind.labels == std::vector<std::string>({"Flood", "Zero-hop"}) && kind.index == 1);
    AdvertSchedule s = apply_advert_choice(ChoiceField::AdvertEvery, 4, extra.schedule);
    CHECK(s.interval_hours == 12 && !s.flood);
    s = apply_advert_choice(ChoiceField::AdvertKind, 0, s);
    CHECK(s.interval_hours == 12 && s.flood);
    CHECK(apply_advert_choice(ChoiceField::AdvertEvery, 0, s).interval_hours == 0 && apply_advert_choice(ChoiceField::AdvertEvery, 99, s).interval_hours == 12);
    // the radio apply does not touch the new fields
    RadioSettings rs;
    rs.freq_mhz = 869.525;
    RetrySettings rt;
    apply_choice(ChoiceField::PathHash, 2, rs, rt);
    CHECK(rs.freq_mhz == 869.525 && rt.attempts == 3);

    // Back closes the newest layer first
    NavState n;
    n.tab = Tab::Contacts;
    n.select_mode = true;
    CHECK(back_action(n) == BackAction::ExitSelect);
    n.nearby_open = true;
    CHECK(back_action(n) == BackAction::CloseNearby);
    n.detail_open = true;
    CHECK(back_action(n) == BackAction::CloseDetail);
    n.stats_open = true;
    CHECK(back_action(n) == BackAction::CloseStats);
    n.search_open = true;
    CHECK(back_action(n) == BackAction::CloseSearch);
    n.editor_open = true;
    CHECK(back_action(n) == BackAction::CancelEditor);
    n.popup_open = true;
    CHECK(back_action(n) == BackAction::ClosePopup);
    NavState top;
    top.tab = Tab::Contacts;
    CHECK(back_action(top) == BackAction::ExitHint);                                                              // a short Esc never quits
}

static void test_phase2_ui()
{
    test_p2_confirm_and_list();
    test_p2_selection_and_groups();
    test_p2_contact_detail();
    test_p2_nearby_rows();
    test_p2_search_ui();
    test_p2_stats_and_autoadd();
    test_p2_settings_and_choices();
}

/* ---------------------------------------------------------------- 0.2.2: the data kept per board */

static void test_boards_ui()
{
    CHECK(fmt_bytes(0) == "0 B" && fmt_bytes(1023) == "1023 B" && fmt_bytes(1024) == "1 KB" && fmt_bytes(1536) == "2 KB" && fmt_bytes(1048576) == "1.0 MB" && fmt_bytes(1572864) == "1.5 MB");
    CHECK(fmt_boards(1) == "1 board" && fmt_boards(0) == "0 boards" && fmt_boards(3) == "3 boards");

    Model m;
    m.add_outgoing("c:0", "a", g_time);
    m.add_outgoing("c:0", "b", g_time);
    const DeletePrompt fb = forget_board_prompt(m, "a1b2c3d4e5f6");
    CHECK(fb.title == "Forget this board's data?" && fb.count == 2 && fb.body.find("a1b2c3d4e5f6") != std::string::npos && fb.body.find("2 messages") != std::string::npos &&
          fb.body.find("cannot be undone") != std::string::npos && fb.body.find("other boards keep their data") != std::string::npos);
    CHECK(forget_others_prompt(0, 0).empty);
    const DeletePrompt fo = forget_others_prompt(2, 3000);
    CHECK(!fo.empty && fo.count == 2 && fo.body.find("2 boards that are not connected (3 KB)") != std::string::npos && fo.title == "Forget other boards' data?");
    CHECK(forget_others_prompt(1, 10).body.find("1 board that is not connected (10 B)") != std::string::npos);
    CHECK(delete_all_prompt(m).body.find("of this board") != std::string::npos);

    // the Settings layout: the forget rows sit in the History section, only when there is something to forget
    SettingsContext ctx;
    ctx.connected = true;
    ctx.have_self = true;
    auto has = [](const std::vector<SRowSpec> &rows, SRowKind k) {
        for (const SRowSpec &r : rows)
            if (r.kind == k) return true;
        return false;
    };
    auto pos = [](const std::vector<SRowSpec> &rows, SRowKind k) {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == k) return static_cast<int>(i);
        return -1;
    };
    auto rows = settings_layout(ctx);
    CHECK(!has(rows, SRowKind::HistoryForget) && !has(rows, SRowKind::HistoryOthers));
    ctx.board_known = true;
    rows = settings_layout(ctx);
    CHECK(has(rows, SRowKind::HistoryForget) && !has(rows, SRowKind::HistoryOthers) && pos(rows, SRowKind::HistoryNote) + 1 == pos(rows, SRowKind::HistoryForget));
    ctx.other_boards = 2;
    rows = settings_layout(ctx);
    CHECK(pos(rows, SRowKind::HistoryForget) + 1 == pos(rows, SRowKind::HistoryOthers) && pos(rows, SRowKind::HistoryOthers) < pos(rows, SRowKind::Reboot) &&
          pos(rows, SRowKind::HistoryOthers) < pos(rows, SRowKind::Undo));
    CHECK(settings_row_selectable(SRowKind::HistoryForget) && settings_row_selectable(SRowKind::HistoryOthers));
    CHECK(rows.back().kind == SRowKind::Save && rows[rows.size() - 2].kind == SRowKind::Undo);
    int info4 = 0;
    for (const SRowSpec &r : rows)
        if (r.kind == SRowKind::Info && r.arg == 4) ++info4;
    CHECK(info4 == 0);

    // a model that learns its board: the UI sees the epoch change once
    Model b;
    CHECK(!b.board_known() && b.board_epoch() == 0);
    SelfInfo s1;
    s1.key[0] = 1;
    b.set_self(s1);
    CHECK(b.board_known() && b.board_epoch() == 1 && b.board_key() == to_hex(s1.key));
    b.set_self(s1);
    CHECK(b.board_epoch() == 1);
    SelfInfo s2;
    s2.key[0] = 2;
    b.add_outgoing("c:0", "of board 1", g_time);
    b.set_self(s2);
    CHECK(b.board_epoch() == 2 && b.message_count() == 0);
}
