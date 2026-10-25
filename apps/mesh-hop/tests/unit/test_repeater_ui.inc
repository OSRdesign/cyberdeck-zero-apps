// 0.7.0 (report 036 slice S1, the regrouping of option B): Home and its page loop, the Tools rows, the Search of Contacts, what a tap
// on a contact opens and its Options menu, MY CONTACT, the Repeater page's rows and texts, the CLI's `to` line (src/ui/ui_repeater.*; 0.7.2: Reset path, More's order).
// Included by test_ui.cpp.

/* A contact whose key bytes count up from `seed`, as the simulator makes them (0x90: "909192...", the first Hilltop repeater). */
static Contact counting_contact(uint8_t seed, const char *name, uint8_t type, uint32_t heard, uint8_t path = kPathFlood)
{
    Contact c;
    for (int i = 0; i < 32; ++i) c.key[static_cast<size_t>(i)] = static_cast<uint8_t>(seed + i);
    c.type = type;
    c.name = name;
    c.last_advert = heard;
    c.lastmod = heard;
    c.out_path_len = path;
    return c;
}

static void test_repeater_ui()
{
    set_time_source(fake_time);

    // ---- Home: Chats, Contacts, Tools, Settings, Node; Esc from a page lands on its own row (the CLI's is Tools)
    {
        const auto &items = home_items();
        CHECK(items.size() == 5 && items[0] == HomeItem::Chats && items[1] == HomeItem::Contacts && items[2] == HomeItem::Tools &&
              items[3] == HomeItem::Settings && items[4] == HomeItem::Node);
        CHECK(std::string(home_item_name(HomeItem::Tools)) == "Tools" && std::string(home_item_name(HomeItem::Node)) == "Node");
        CHECK(home_index(HomeItem::Settings) == 3 && home_index(HomeItem::Node) == 4);
        CHECK(home_row_of(Tab::Chats) == 0 && home_row_of(Tab::Contacts) == 1 && home_row_of(Tab::Tools) == 2 && home_row_of(Tab::Cli) == 2 &&
              home_row_of(Tab::Settings) == 3);
        CHECK(tools_home_value(3) == "3 nearby waiting" && tools_home_value(0).empty());
        // Tab / Shift+Tab: Chats, Contacts, Tools, Settings in a loop
        CHECK(next_page(Tab::Chats, 1) == Tab::Contacts && next_page(Tab::Contacts, 1) == Tab::Tools && next_page(Tab::Tools, 1) == Tab::Settings &&
              next_page(Tab::Settings, 1) == Tab::Chats);
        CHECK(next_page(Tab::Chats, -1) == Tab::Settings && next_page(Tab::Settings, -1) == Tab::Tools && next_page(Tab::Tools, -1) == Tab::Contacts);
        CHECK(next_page(Tab::Cli, 1) == Tab::Settings && next_page(Tab::Cli, -1) == Tab::Contacts);       // the CLI counts as Tools
        Tab t = Tab::Chats;
        for (int i = 0; i < 4; ++i) t = next_page(t, 1);
        CHECK(t == Tab::Chats);                                                                           // four presses: a full turn
    }

    // ---- Tools: Nearby, Packet log, Statistics, CLI, each with its second line
    {
        ToolsContext c;
        std::vector<ToolRow> r = tools_rows(c);
        CHECK(r.size() == 4 && r[0].item == ToolItem::Nearby && r[1].item == ToolItem::PacketLog && r[2].item == ToolItem::Statistics && r[3].item == ToolItem::Cli);
        CHECK(r[0].title == "Nearby" && r[0].caption == "Scan asks the nodes in range" && r[0].tone == Tone::Muted);
        CHECK(r[1].caption == "capture off" && r[2].caption == "battery, noise floor, airtime" && r[3].title == "CLI");
        CHECK(r[3].caption == "meshcore-cli  \xC2\xB7  not connected");
        c.pending = 3;
        c.capturing = true;
        c.log_n = 12;
        c.connected = true;
        r = tools_rows(c);
        CHECK(r[0].caption == "3 waiting  \xC2\xB7  Scan asks the nodes in range" && r[0].tone == Tone::Gold);
        CHECK(r[1].caption == "capturing, 12 packets" && r[1].tone == Tone::Green && r[3].caption == "meshcore-cli, for admin work");
        c.capturing = false;
        c.log_n = 1;
        c.cli_needs_python = true;
        r = tools_rows(c);
        CHECK(r[1].caption == "stopped, 1 packet" && r[3].caption == "needs python3" && r[3].tone == Tone::Gold);
    }

    // ---- the Search: a part of the name in any case, or '<' and the start of the key
    {
        std::string hex;
        CHECK(search_key_prefix("<9A3f", &hex) && hex == "9a3f");
        CHECK(search_key_prefix("<", &hex) && hex.empty());
        CHECK(!search_key_prefix("<9g") && !search_key_prefix("9a3f") && !search_key_prefix(""));
        const std::string key = "909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
        CHECK(contact_matches_search("Hilltop repeater", key, "") && contact_matches_search("Hilltop repeater", key, "hill"));
        CHECK(contact_matches_search("Hilltop repeater", key, "TOP REP") && !contact_matches_search("Hilltop repeater", key, "hilly"));
        CHECK(contact_matches_search("Hilltop repeater", key, "<9091") && contact_matches_search("x", key, "<909192939495") &&
              !contact_matches_search("Hilltop repeater", key, "<9092"));
        CHECK(contact_matches_search("x", key, "<") && !contact_matches_search("Hilltop", key, "<" + key + "b0"));    // longer than the key: no
        CHECK(!contact_matches_search("Hilltop repeater", key, "9091") && contact_matches_search("909192", key, "9091"));   // no '<': the name
        CHECK(contact_matches_search("Old Mill  relay 2", key, "mill  relay") && !contact_matches_search("Old Mill  relay 2", key, "mill relay"));
        CHECK(contact_matches_search("a<9b", key, "<9b") == false && contact_matches_search("a<9g", key, "<9g"));         // not hex after '<': a name part
        ContactFilter f;
        f.search = "hill";
        CHECK(f.active());
        ContactView a, b;
        b.filter.search = "x";
        CHECK(a != b && !(a == b));
    }

    // ---- 0.7.2 (the To... picker is gone): the repeater is picked in Contacts (its Search) and Open in CLI types its own full key:
    // two of the same name, a hex-looking name, spaces in a name
    {
        Model m;
        m.upsert_contact(counting_contact(0x10, "Alice", advtype::kChat, g_time - 60, 1));
        m.upsert_contact(counting_contact(0x90, "Hilltop repeater", advtype::kRepeater, g_time - 120, 3));
        m.upsert_contact(counting_contact(0x30, "Hilltop repeater", advtype::kRepeater, g_time - 30, 0));
        m.upsert_contact(counting_contact(0x38, "909192", advtype::kRepeater, g_time - 300));
        m.upsert_contact(counting_contact(0x40, "Old Mill  relay 2", advtype::kRepeater, g_time - 400, 1));
        m.upsert_contact(counting_contact(0x48, "Club room", advtype::kRoom, g_time - 500));
        m.upsert_contact(counting_contact(0xF0, "Dock sensor", advtype::kSensor, g_time - 10));
        ContactView v;
        v.sort = SortKey::Name;
        v.filter.search = "hill";
        ContactSnapshot s = make_contact_snapshot(m, v);
        CHECK(s.rows.size() == 2 && s.rows[0].name == "Hilltop repeater" && s.rows[1].name == "Hilltop repeater");
        CHECK(s.rows[0].key_hex.substr(0, 2) == "30" && s.rows[1].key_hex.substr(0, 2) == "90");      // same name: by key, each its own row
        // the key the CLI types is the row's own: the same name never picks the other one
        CHECK(cli_to_line(s.key_at(1)) == "to " + s.rows[1].key_hex && cli_to_line(s.key_at(1)).size() == 3 + 64);
        CHECK(cli_to_line(s.key_at(0)) != cli_to_line(s.key_at(1)));
        CHECK(cli_to_shown(s.rows[1].name, s.key_at(1)) == "to Hilltop repeater <909192" && cli_to_shown(s.rows[0].name, s.key_at(0)) == "to Hilltop repeater <303132");
        // a hex-looking NAME is found by its name; '<' finds the KEY that starts with those digits (the first Hilltop)
        v.filter.search = "909192";
        s = make_contact_snapshot(m, v);
        CHECK(s.rows.size() == 1 && s.rows[0].name == "909192" && cli_to_line(s.key_at(0)).substr(0, 5) == "to 38");
        CHECK(cli_to_shown("909192", s.key_at(0)) == "to 909192 <38393a");
        v.filter.search = "<909192";
        s = make_contact_snapshot(m, v);
        CHECK(s.rows.size() == 1 && s.rows[0].name == "Hilltop repeater" && s.rows[0].key_hex.substr(0, 6) == "909192");
        v.filter.search = "mill  ";
        s = make_contact_snapshot(m, v);
        CHECK(s.rows.size() == 1 && cli_to_line(s.key_at(0)) == "to " + std::string("404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"));
        CHECK(cli_to_shown("Old Mill  relay 2", s.key_at(0)) == "to Old Mill  relay 2 <404142" && cli_to_shown("", s.key_at(0)) == "to <404142");
        CHECK(cli_to_shown("x", "abc").empty());
        // Contacts: the same Search over every type, with the type filter on top
        v.filter.search = "o";
        CHECK(make_contact_snapshot(m, v).rows.size() == 5);                                          // Hilltop x2, Old Mill, Club room, Dock sensor
        v.filter.type = TypeFilter::Room;
        CHECK(make_contact_snapshot(m, v).rows.size() == 1);
        // the Repeater + Room preset never comes in the loop of T (nor in the Filter dialog: All .. Sensor)
        CHECK(next_type_filter(TypeFilter::RepeaterRoom) == TypeFilter::All && next_type_filter(TypeFilter::Sensor) == TypeFilter::All);
        TypeFilter t = TypeFilter::All;
        for (int i = 0; i < 10; ++i) {
            t = next_type_filter(t);
            CHECK(t != TypeFilter::RepeaterRoom);
        }
        CHECK(cli_to_line("ABCDEF" + std::string(58, '0')) == "to abcdef" + std::string(58, '0') && cli_to_line("abc").empty() &&
              cli_to_line(std::string(63, 'a') + "g").empty());
        CHECK(cli_pick_hint().find("Open in CLI") != std::string::npos && cli_pick_hint().find("to <name>") != std::string::npos);
    }

    // ---- a tap on a contact: by its type; the Options menu: by its type
    {
        CHECK(contact_open_target(advtype::kRepeater) == ContactOpen::Repeater && contact_open_target(advtype::kRoom) == ContactOpen::Repeater);
        CHECK(contact_open_target(advtype::kChat) == ContactOpen::Chat && contact_open_target(advtype::kNone) == ContactOpen::Chat);
        CHECK(contact_open_target(advtype::kSensor) == ContactOpen::Details && contact_open_target(9) == ContactOpen::Details);
        ContactOptions o = contact_options("Hilltop repeater", advtype::kRepeater);
        CHECK(o.title == "Options for Hilltop repeater" && o.labels == std::vector<std::string>({"Manage", "Details", "Groups", "Delete"}));
        CHECK(o.actions[0] == ContactAction::Manage && o.actions[3] == ContactAction::Delete && o.affs[0] == Aff::Open && o.affs[3] == Aff::Action);
        CHECK(contact_options("Club room", advtype::kRoom).labels[0] == "Manage");
        o = contact_options("Alice", advtype::kChat);
        CHECK(o.labels == std::vector<std::string>({"Message", "Details", "Groups", "Delete"}) && o.actions[0] == ContactAction::Message);
        o = contact_options("Dock sensor", advtype::kSensor);
        CHECK(o.labels == std::vector<std::string>({"Details", "Groups", "Delete"}) && o.actions[0] == ContactAction::Details);
        CHECK(o.affs.size() == o.labels.size() && o.actions.size() == o.labels.size());
        CHECK(contact_options("A very long repeater name for the box", advtype::kRepeater).title == "Options for A very long repeater nam...");
    }

    // ---- MY CONTACT: the key on its row, and whole
    {
        const std::string key = "A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF";
        CHECK(key_short_grouped(key) == "a0a1 a2a3 \xE2\x80\xA6 bebf");
        CHECK(key_grouped_lines(key) == "a0a1 a2a3 a4a5 a6a7 a8a9 aaab acad aeaf\nb0b1 b2b3 b4b5 b6b7 b8b9 babb bcbd bebf");
        CHECK(key_short_grouped("a0a1").empty() && key_grouped_lines(std::string(64, 'z')).empty());
    }

    // ---- the Repeater page: the line under the title, the ACCESS rows, the clock, the notes
    {
        CHECK(repeater_route_text(-1) == "flood" && repeater_route_text(0) == "direct" && repeater_route_text(2) == "direct, 2 hops" &&
              repeater_route_text(1) == "direct, 1 hop");
        CHECK(repeater_status_line(advtype::kRepeater, 2, g_time - 180) == "Repeater  \xC2\xB7  direct, 2 hops  \xC2\xB7  heard 3 min ago");
        CHECK(repeater_status_line(advtype::kRoom, -1, 0) == "Room  \xC2\xB7  flood  \xC2\xB7  never heard");
        CHECK(repeater_status_line(advtype::kRepeater, 0, g_time - 5) == "Repeater  \xC2\xB7  direct  \xC2\xB7  heard < 1 min ago");
        CHECK(std::string(remote_role_name(Client::RemoteRole::Admin)) == "Admin" && std::string(remote_role_name(Client::RemoteRole::ReadOnly)) == "Read-only" &&
              std::string(remote_role_name(Client::RemoteRole::ReadWrite)) == "Read-write" && std::string(remote_role_name(Client::RemoteRole::Guest)) == "Guest");
        CHECK(clock_skew_text(0) == "in step with the deck" && clock_skew_text(1) == "in step with the deck" && clock_skew_text(5) == "5 s ahead" &&
              clock_skew_text(-180) == "3 min behind" && clock_skew_text(7200) == "2 h ahead");

        RepeaterView v;
        std::vector<RepeaterRow> rows = repeater_rows(v);                     // not logged in, nothing saved
        CHECK(rows.size() == 6 && rows[0].kind == RepeaterRow::Section && rows[0].label == "ACCESS" && rows[1].label == "Not logged in" && rows[1].value.empty());
        CHECK(rows[2].kind == RepeaterRow::Section && rows[2].label == "READ");                   // 0.8.0: the grey "next version" row is gone
        for (const RepeaterRow &x : rows) CHECK(x.label.find("next version") == std::string::npos);
        v.password_saved = true;
        rows = repeater_rows(v);
        CHECK(rows[1].value == "password saved");
        Client::RemoteSession ses;
        ses.role = Client::RemoteRole::Admin;
        ses.since = g_time;
        ses.has_clock = true;
        ses.deck_time = g_time;
        ses.clock = g_time + 1;
        v.session = &ses;
        rows = repeater_rows(v);                                              // admin, the clock in step: the clock row, the note of Log out
        CHECK(rows.size() == 8 && rows[1].label == "Admin since " + fmt_clock(g_time) && rows[1].value == "password saved");
        CHECK(rows[2].label == "Clock at login" && rows[2].value == fmt_hms(g_time + 1) + "  \xC2\xB7  in step with the deck" && rows[2].tone == Tone::Muted);
        CHECK(rows[3].kind == RepeaterRow::Note && rows[3].label == logout_note(false) && rows[3].label.find("until setperm") != std::string::npos &&
              rows[3].label.find("until it reboots") != std::string::npos && rows[3].label.find("repeater") != std::string::npos);
        CHECK(rows[4].kind == RepeaterRow::Section && rows[4].label == "READ" && rows[5].label == "Status" && rows[5].value == "not read yet");
        ses.clock = g_time + 61;                                             // more than a minute ahead: Gold, and the warning
        rows = repeater_rows(v);
        CHECK(rows.size() == 9 && rows[2].tone == Tone::Gold && rows[2].value.find("1 min ahead") != std::string::npos);
        CHECK(rows[3].kind == RepeaterRow::Note && rows[3].tone == Tone::Gold && rows[3].label.find("more than a minute ahead") != std::string::npos);
        ses.clock = g_time + 60;                                             // a minute exactly: no warning
        CHECK(repeater_rows(v).size() == 8);
        ses.clock = g_time - 600;                                            // behind: no warning
        rows = repeater_rows(v);
        CHECK(rows.size() == 8 && rows[2].value.find("10 min behind") != std::string::npos && rows[2].tone == Tone::Muted);
        ses.has_clock = false;                                               // a legacy answer: no clock
        CHECK(repeater_rows(v)[2].value == "not sent (older firmware)");
        ses.role = Client::RemoteRole::ReadWrite;                            // a room: its roles, "room" in the notes
        v.room = true;
        v.password_saved = false;
        rows = repeater_rows(v);
        CHECK(rows[1].label == "Read-write since " + fmt_clock(g_time) && rows[1].value.empty() && rows[3].label.find("the room keeps") != std::string::npos);
        CHECK(rows.size() == 6 && rows[4].label == "READ" && rows.back().label == "Status");                 // a room: Status only
        ses.role = Client::RemoteRole::None;                                 // 0.8.0: a legacy answer with no role
        CHECK(repeater_rows(v)[1].label == "Logged in since " + fmt_clock(g_time));
    }

    // ---- the login box: the route note, the wait, the results
    {
        CHECK(login_route_note(-1) == "No known route: the login is sent to the whole mesh" && login_route_note(0).empty() && login_route_note(3).empty());
        // 0.7.2: the route line under the box's title (the page's line is under the box), Reset path only with a route
        CHECK(login_route_line(2) == "Route: direct, 2 hops" && login_route_line(1) == "Route: direct, 1 hop" && login_route_line(0) == "Route: direct" &&
              login_route_line(-1) == "Route: flood: the login goes to the whole mesh");   // one line, in place of the note
        CHECK(login_offers_reset(0) && login_offers_reset(3) && !login_offers_reset(-1));
        CHECK(login_route_line(2).find("No known route") == std::string::npos);
        // Reset path's answers
        Client::PathResetResult pr;
        CHECK(path_reset_text(pr, "Hilltop").empty());                                       // no result yet
        pr.id = 1;
        pr.ok = true;
        CHECK(path_reset_text(pr, "Hilltop") == "Path reset: the next message goes by flood");
        pr.ok = false;
        using PE = Client::PathResetError;
        pr.error = PE::NotFound;
        CHECK(path_reset_text(pr, "Hilltop repeater") == "Hilltop repeater is not in the node's contacts.");
        pr.error = PE::Busy;
        CHECK(path_reset_text(pr, "Hilltop").find("busy") != std::string::npos);
        pr.error = PE::Failed;
        pr.err_code = 6;
        CHECK(path_reset_text(pr, "Hilltop") == "The path was not reset (error 6).");
        for (PE e : {PE::NotConnected, PE::Timeout, PE::LinkLost}) {
            pr.error = e;
            CHECK(!path_reset_text(pr, "Hilltop").empty());
        }
        // More: Details, Open in CLI, Reset path, Forget password (only when one is saved) last
        std::vector<RepeaterMoreItem> more = repeater_more_items(false);
        CHECK(more.size() == 4 && more[0].label == "Details" && more[1].label == "Open in CLI" && more[2].label == "Reset path" && more[3].label == "Refresh all");
        CHECK(more[0].action == RepeaterMore::Details && more[1].action == RepeaterMore::OpenCli && more[2].action == RepeaterMore::ResetPath &&
              more[3].action == RepeaterMore::RefreshAll);
        CHECK(more[0].aff == Aff::Open && more[1].aff == Aff::Open && more[2].aff == Aff::Action && more[3].aff == Aff::Action);
        more = repeater_more_items(true);                                    // 0.8.1: Refresh all before Forget password, which stays last
        CHECK(more.size() == 5 && more[4].label == "Forget password" && more[4].action == RepeaterMore::ForgetPassword && more[4].aff == Aff::Action &&
              more[2].label == "Reset path" && more[3].label == "Refresh all");
        more = repeater_more_items(true, true);                              // while a read runs it stops it
        CHECK(more.size() == 5 && more[3].label == "Stop reading" && more[3].action == RepeaterMore::RefreshAll);
        Client::RemoteState st;
        CHECK(remote_wait_text(st, 1000).empty());
        st.phase = Client::RemoteState::Phase::Sending;
        CHECK(remote_wait_text(st, 1000) == "Sending\xE2\x80\xA6");
        st.phase = Client::RemoteState::Phase::Waiting;
        st.deadline_ms = 13000;
        CHECK(remote_wait_text(st, 1000) == "Waiting\xE2\x80\xA6 up to 12 s" && remote_wait_text(st, 12500) == "Waiting\xE2\x80\xA6 up to 1 s" &&
              remote_wait_text(st, 20000) == "Waiting\xE2\x80\xA6 up to 1 s" && remote_wait_text(st, 10900) == "Waiting\xE2\x80\xA6 up to 3 s");
        Client::RemoteResult r;
        using E = Client::RemoteError;
        CHECK(login_error_text(r, "Hilltop").empty());
        r.error = E::Timeout;
        CHECK(login_error_text(r, "Hilltop repeater") == "No answer. Wrong password, or Hilltop repeater is out of reach.");
        r.error = E::NotFound;
        CHECK(login_error_text(r, "Hilltop") == "Hilltop is not in the node's contacts.");
        r.error = E::TableFull;
        CHECK(login_error_text(r, "Hilltop") == "The node is busy: try again in a moment.");
        r.error = E::Unsupported;
        CHECK(login_error_text(r, "Hilltop").find("firmware is too old") != std::string::npos);
        r.error = E::Busy;
        CHECK(login_error_text(r, "Hilltop") == "The CLI has the node: leave the CLI first.");
        r.error = E::Failed;
        r.err_code = 6;
        CHECK(login_error_text(r, "Hilltop") == "The node refused the login (error 6).");
        r.error = E::Refused;
        CHECK(login_error_text(r, "Hilltop") == "Hilltop refused the password.");
        r.error = E::BadPassword;
        CHECK(login_error_text(r, "Hilltop") == "The password is too long: 15 bytes at most.");
        for (E e : {E::NotConnected, E::Cancelled, E::LinkLost}) {
            r.error = e;
            CHECK(!login_error_text(r, "Hilltop").empty());
        }
        Client::RemoteResult ok;
        ok.ok = true;
        ok.role = Client::RemoteRole::Admin;
        ok.password = Client::PasswordSave::Saved;
        CHECK(login_success_text(ok, "Hilltop repeater") == "Logged in to Hilltop repeater as admin, password saved");
        ok.role = Client::RemoteRole::Guest;
        ok.password = Client::PasswordSave::NotAsked;
        CHECK(login_success_text(ok, "Hilltop repeater") == "Logged in to Hilltop repeater as guest");
        ok.password = Client::PasswordSave::Failed;
        CHECK(login_success_text(ok, "X") == "Logged in to X as guest: the password could not be saved");
        ok.role = Client::RemoteRole::ReadOnly;
        ok.password = Client::PasswordSave::NotAsked;
        CHECK(login_success_text(ok, "Club room") == "Logged in to Club room as read-only");
        ok.role = Client::RemoteRole::None;                                  // 0.8.0: the role is not known (a legacy answer): no guess
        CHECK(login_success_text(ok, "Old relay") == "Logged in to Old relay");
        ok.password = Client::PasswordSave::Saved;
        CHECK(login_success_text(ok, "Old relay") == "Logged in to Old relay, password saved");
    }
}

/* ------------------------------------------------------------------ 0.8.0: the read-only view (report 036 slice S2) */

static const RepeaterRow *rep_row(const std::vector<RepeaterRow> &rows, const std::string &id)
{
    for (const RepeaterRow &r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

static std::vector<std::string> rep_ids(const std::vector<RepeaterRow> &rows, RepSection s)
{
    std::vector<std::string> v;
    for (const RepeaterRow &r : rows)
        if (r.section == s && r.kind != RepeaterRow::Section) v.push_back(r.id);
    return v;
}

static Client::RemoteResult rep_result(Client::RemoteOp op, uint32_t req, Client::RemoteError e = Client::RemoteError::None)
{
    Client::RemoteResult r;
    r.op = op;
    r.req = req;
    r.ok = e == Client::RemoteError::None;
    r.error = e;
    return r;
}

/* A page of `n` neighbours from `offset` out of `total`: prefixes seed+0..3 (seed counts up from 0xE0), heard 10 s apart. */
static NeighboursPage rep_page(uint16_t offset, int n, int total, uint8_t seed0 = 0xE0)
{
    NeighboursPage pg;
    pg.offset = offset;
    pg.total = total;
    for (int i = 0; i < n; ++i) {
        NeighbourEntry e;
        const uint8_t seed = static_cast<uint8_t>(seed0 + offset + i);
        e.prefix = {seed, static_cast<uint8_t>(seed + 1), static_cast<uint8_t>(seed + 2), static_cast<uint8_t>(seed + 3)};
        e.heard_secs_ago = static_cast<uint32_t>(10 * (offset + i) + 5);
        e.snr = 7.0 - 0.5 * (offset + i);
        pg.entries.push_back(e);
    }
    pg.more = offset + n < total && n > 0;
    return pg;
}

static void test_repeater_view()
{
    set_time_source(fake_time);
    using E = Client::RemoteError;
    using RO = Client::RemoteOp;

    // ---- the values, rounded for the rows
    {
        CHECK(fmt_volts(4123) == "4.12 V" && fmt_volts(4996) == "5.00 V" && fmt_volts(3705) == "3.71 V" && fmt_volts(0) == "0.00 V" && fmt_volts(65535) == "65.54 V");
        CHECK(fmt_rep_uptime(45) == "45 s" && fmt_rep_uptime(60) == "1 m" && fmt_rep_uptime(4 * 3600 + 12 * 60 + 5) == "4 h 12 m" && fmt_rep_uptime(259205) == "3 d 0 h 0 m" &&
              fmt_rep_uptime(3 * 86400 + 4 * 3600 + 12 * 60) == "3 d 4 h 12 m" && fmt_rep_uptime(0) == "0 s");
        CHECK(fmt_rep_airtime(40) == "40 s" && fmt_rep_airtime(725) == "12 m" && fmt_rep_airtime(3600) == "1 h 0 m" && fmt_rep_airtime(52 * 3600 + 180) == "52 h 3 m");
        CHECK(fmt_dbm(-112) == "-112 dBm" && fmt_dbm(0) == "0 dBm" && fmt_db(6.5) == "6.5 dB" && fmt_db(-6.25) == "-6.3 dB" && fmt_db(0) == "0.0 dB");
        CHECK(fmt_snr1(7.0) == "7.0" && fmt_snr1(-12.5) == "-12.5" && fmt_snr1(-0.25) == "-0.3" && fmt_snr1(-0.04) == "0.0" && fmt_snr1(5.25) == "5.3");
        CHECK(fmt_ago(12) == "12 s ago" && fmt_ago(300) == "5 min ago" && fmt_ago(3 * 3600) == "3 h ago" && fmt_ago(2 * 86400) == "2 d ago" && fmt_ago(0) == "0 s ago");
        CHECK(neighbour_value(7.0, 12) == "SNR 7.0  \xC2\xB7  12 s ago" && neighbour_value(-3.25, 400) == "SNR -3.3  \xC2\xB7  6 min ago");
    }

    // ---- the names of the neighbours: a contact's name, the prefix when no contact (or no name) matches, ambiguous prefixes
    {
        Model m;
        m.upsert_contact(counting_contact(0x90, "Ridge", advtype::kRepeater, g_time - 60, 0));
        Contact twin = counting_contact(0x90, "Ridge twin", advtype::kRepeater, g_time - 30, 0);
        twin.key[20] ^= 0xFF;                                                // the same first 4 bytes, another key
        m.upsert_contact(twin);
        m.upsert_contact(counting_contact(0x30, "", advtype::kRepeater, g_time - 600));     // a contact with no name yet
        m.upsert_contact(counting_contact(0x40, "Old Mill", advtype::kRepeater, g_time - 600));
        const Bytes ridge = {0x90, 0x91, 0x92, 0x93}, noname = {0x30, 0x31, 0x32, 0x33}, mill = {0x40, 0x41, 0x42, 0x43}, stranger = {0xE0, 0xE1, 0xE2, 0xE3};
        CHECK(neighbour_label(neighbour_name(m, mill), mill) == "Old Mill");
        CHECK(neighbour_label(neighbour_name(m, stranger), stranger) == "<e0e1e2e3");
        CHECK(neighbour_label(neighbour_name(m, noname), noname) == "<30313233");
        CHECK(neighbour_label(neighbour_name(m, ridge), ridge) == "Ridge twin (ambiguous 2)");          // the most recently heard first
        CHECK(neighbour_label(NeighbourName{}, ridge) == "<90919293");                                    // no model: the prefix
    }

    // ---- the errors: the notice / note text, the header state, Try again
    {
        CHECK(read_error_text(E::Timeout, -1, "Hilltop repeater") == "No answer. Hilltop repeater may be out of reach, or its session ended (a reboot): log in again.");
        CHECK(read_error_text(E::Timeout, -1, "Hilltop repeater", true) == "No answer. Hilltop repeater may be out of reach.");
        CHECK(read_error_text(E::NotLoggedIn, -1, "X") == "Log in first" && read_error_text(E::Unsupported, -1, "X") == "Firmware too old for this");
        CHECK(read_error_text(E::NoDirectRoute, -1, "X") == "No direct route: log in to read the owner" && read_error_text(E::BadReply, -1, "X") == "Unreadable answer");
        CHECK(read_error_text(E::Busy, -1, "X") == "The CLI holds the node" && read_error_text(E::NotFound, -1, "Hilltop") == "Hilltop is not in the node's contacts.");
        CHECK(read_error_text(E::Failed, 6, "X") == "The node refused the request (error 6)." && read_error_text(E::Failed, -1, "X") == "The node refused the request.");
        CHECK(read_error_text(E::TableFull, -1, "X") == "The node is busy: try again in a moment." && read_error_text(E::LinkLost, -1, "X") == "The link to the node was lost." &&
              read_error_text(E::NotConnected, -1, "X") == "Not connected to a node.");
        CHECK(read_error_text(E::None, -1, "X").empty() && read_error_text(E::Cancelled, -1, "X").empty());
        CHECK(read_error_text(E::Timeout, -1, "").find("the node may be out of reach") != std::string::npos);
        CHECK(read_error_state(E::Timeout) == "no answer" && read_error_state(E::NotLoggedIn) == "needs login" && read_error_state(E::Unsupported) == "firmware too old" &&
              read_error_state(E::NoDirectRoute) == "direct route only" && read_error_state(E::BadReply) == "unreadable answer" && read_error_state(E::LinkLost) == "not read");
        CHECK(!RepeaterReads::stops_chain(E::Timeout) && !RepeaterReads::stops_chain(E::Unsupported) && !RepeaterReads::stops_chain(E::NoDirectRoute) &&
              !RepeaterReads::stops_chain(E::BadReply) && !RepeaterReads::stops_chain(E::Failed));
        for (E e : {E::NotLoggedIn, E::Busy, E::NotConnected, E::LinkLost, E::NotFound, E::TableFull, E::Cancelled}) CHECK(RepeaterReads::stops_chain(e));
        Client::RemoteState st;
        CHECK(section_wait_text(st, 0).empty());
        st.phase = Client::RemoteState::Phase::Sending;
        CHECK(section_wait_text(st, 0) == "sending\xE2\x80\xA6");
        st.phase = Client::RemoteState::Phase::Waiting;
        st.deadline_ms = 13000;
        CHECK(section_wait_text(st, 1000) == "waiting\xE2\x80\xA6 up to 12 s" && section_wait_text(st, 14000) == "waiting\xE2\x80\xA6 up to 1 s");
        CHECK(std::string(rep_section_name(RepSection::Neighbours)) == "Neighbours" && std::string(rep_section_name(RepSection::None)).empty());
    }

    // ---- Refresh all: Status, Neighbours (page 0), Owner, one after the other; a room: Status; not logged in: Owner if direct
    {
        RepeaterReads rd;
        RepeaterReads::Next n;
        CHECK(!rd.running() && !rd.pop(n));
        CHECK(rd.refresh_all(true, false, false) && rd.running() && rd.queued() == 3);
        CHECK(!rd.refresh_all(true, false, false) && !rd.refresh(RepSection::Status) && rd.queued() == 3);      // one chain at a time
        CHECK(rd.pop(n) && n.section == RepSection::Status && rd.in_flight() == RepSection::Status && !rd.sent() && !rd.pop(n));
        rd.sent_as(41);
        CHECK(rd.sent() && rd.req() == 41);
        CHECK(rd.on_result(rep_result(RO::Status, 40), g_time, "H").empty() && rd.in_flight() == RepSection::Status);   // another request: ignored
        CHECK(rd.on_result(rep_result(RO::Neighbours, 41), g_time, "H").empty() && rd.in_flight() == RepSection::Status);
        Client::RemoteResult ok = rep_result(RO::Status, 41);
        ok.status.battery_mv = 4120;
        CHECK(rd.on_result(ok, g_time, "H").empty() && rd.in_flight() == RepSection::None && rd.section(RepSection::Status).has &&
              rd.section(RepSection::Status).read_at == g_time && rd.status.battery_mv == 4120);
        // Neighbours: no answer: marked, the chain goes on
        CHECK(rd.pop(n) && n.section == RepSection::Neighbours && n.offset == 0);
        rd.sent_as(42);
        CHECK(rd.on_result(rep_result(RO::Neighbours, 42, E::Timeout), g_time, "Hilltop") ==
              "No answer. Hilltop may be out of reach, or its session ended (a reboot): log in again.");
        CHECK(rd.section(RepSection::Neighbours).error == E::Timeout && !rd.section(RepSection::Neighbours).has && rd.queued() == 1);
        CHECK(rd.pop(n) && n.section == RepSection::Owner);
        rd.sent_as(43);
        Client::RemoteResult own = rep_result(RO::Owner, 43);
        own.owner.name = "Hilltop";
        own.owner.fw_version = "v1.18.0";
        CHECK(rd.on_result(own, g_time + 5, "Hilltop").empty() && !rd.running() && rd.owner.fw_version == "v1.18.0" && rd.section(RepSection::Owner).read_at == g_time + 5);
        // a hard failure stops the chain: the sections still queued keep what they had
        CHECK(rd.refresh_all(true, false, false) && rd.pop(n));
        rd.sent_as(44);
        CHECK(rd.on_result(rep_result(RO::Status, 44, E::LinkLost), g_time, "H") == "The link to the node was lost." && !rd.running() && rd.queued() == 0);
        CHECK(rd.section(RepSection::Status).error == E::LinkLost && rd.section(RepSection::Status).has && rd.section(RepSection::Neighbours).error == E::Timeout &&
              rd.section(RepSection::Owner).error == E::None && rd.section(RepSection::Owner).has);
        // the login lost (refused at once, req 0): stops too
        CHECK(rd.refresh_all(true, false, false) && rd.pop(n));
        CHECK(rd.on_result(rep_result(RO::Status, 0, E::NotLoggedIn), g_time, "H") == "Log in first" && !rd.running());
        // soft failures one after the other: firmware too old (refused at once), then the owner too, the chain runs to its end
        CHECK(rd.refresh_all(true, false, false) && rd.pop(n));
        rd.sent_as(45);
        CHECK(rd.on_result(rep_result(RO::Status, 45), g_time, "H").empty());
        CHECK(rd.pop(n) && rd.on_result(rep_result(RO::Neighbours, 0, E::Unsupported), g_time, "H") == "Firmware too old for this" && rd.queued() == 1);
        CHECK(rd.pop(n) && rd.on_result(rep_result(RO::Owner, 0, E::Unsupported), g_time, "H") == "Firmware too old for this" && !rd.running());
        CHECK(rd.section(RepSection::Status).error == E::None && rd.section(RepSection::Owner).error == E::Unsupported && rd.section(RepSection::Owner).has);
        // Cancel: the queue goes, the request in flight ends Cancelled: the section stays as it was
        CHECK(rd.refresh_all(true, false, false) && rd.pop(n));
        rd.sent_as(46);
        rd.cancel();
        CHECK(rd.running() && rd.queued() == 0 && rd.in_flight() == RepSection::Status);
        CHECK(rd.on_result(rep_result(RO::Status, 46, E::Cancelled), g_time + 99, "H").empty() && !rd.running() &&
              rd.section(RepSection::Status).error == E::None && rd.section(RepSection::Status).read_at == g_time);
        // a Cancelled result also ends a chain that was not cancelled here (a login, the CLI)
        CHECK(rd.refresh_all(true, false, false) && rd.pop(n));
        rd.sent_as(47);
        CHECK(rd.on_result(rep_result(RO::Status, 47, E::Cancelled), g_time, "H").empty() && !rd.running());
        // a room: Status only; not logged in: the owner alone on a direct route, nothing on a flood route
        CHECK(rd.refresh_all(true, true, false) && rd.queued() == 1 && rd.pop(n) && n.section == RepSection::Status);
        rd.on_result(rep_result(RO::Status, 0, E::Busy), g_time, "H");
        CHECK(!rd.running() && rd.section(RepSection::Status).error == E::Busy);
        CHECK(!rd.refresh_all(false, false, false) && !rd.running());
        CHECK(!rd.refresh_all(false, true, true) && !rd.running());
        CHECK(rd.refresh_all(false, false, true) && rd.queued() == 1 && rd.pop(n) && n.section == RepSection::Owner);
        Client::RemoteResult an = rep_result(RO::Owner, 0, E::NoDirectRoute);
        CHECK(rd.on_result(an, g_time, "H") == "No direct route: log in to read the owner" && rd.section(RepSection::Owner).anonymous);
        // one section alone
        CHECK(rd.refresh(RepSection::Owner) && rd.queued() == 1 && rd.pop(n) && n.section == RepSection::Owner && !rd.refresh(RepSection::Status));
        rd.on_result(rep_result(RO::Owner, 0, E::Busy), g_time, "H");
        CHECK(!rd.refresh(RepSection::None));
        rd.clear();
        CHECK(!rd.running() && !rd.section(RepSection::Status).has && rd.section(RepSection::Owner).error == E::None && rd.neighbours.empty());
    }

    // ---- the neighbours' pages: page 0, then More (from offset + entries) until none remains; newest first, one row per prefix
    {
        RepeaterReads rd;
        RepeaterReads::Next n;
        CHECK(!rd.more_neighbours());                                         // nothing read yet
        CHECK(rd.refresh(RepSection::Neighbours) && rd.pop(n) && n.offset == 0);
        rd.sent_as(7);
        Client::RemoteResult pg = rep_result(RO::Neighbours, 7);
        pg.neighbours = rep_page(0, 14, 22);
        CHECK(rd.on_result(pg, g_time, "H").empty() && rd.neighbours.size() == 14 && rd.nb_total == 22 && rd.nb_more && rd.nb_next == 14);
        CHECK(rd.neighbours[0].heard_at == g_time - 5 && rd.neighbours[13].heard_at == g_time - 135);
        CHECK(rd.more_neighbours() && rd.pop(n) && n.section == RepSection::Neighbours && n.offset == 14);
        rd.sent_as(8);
        pg = rep_result(RO::Neighbours, 8, E::Timeout);                        // the next page: no answer; More asks that page again
        CHECK(!rd.on_result(pg, g_time + 30, "H").empty() && rd.section(RepSection::Neighbours).offset == 14 && rd.neighbours.size() == 14);
        CHECK(rd.more_neighbours() && rd.pop(n) && n.offset == 14);                     // More asks that page again
        rd.sent_as(9);
        pg = rep_result(RO::Neighbours, 9);
        pg.neighbours = rep_page(14, 8, 22);
        pg.neighbours.entries[0].prefix = rd.neighbours[3].prefix;            // the list moved: a neighbour of page 0 again, heard earlier
        pg.neighbours.entries[0].heard_secs_ago = 1000;
        CHECK(rd.on_result(pg, g_time + 40, "H").empty() && rd.section(RepSection::Neighbours).error == E::None && !rd.nb_more && rd.nb_total == 22);
        CHECK(rd.neighbours.size() == 21 && !rd.more_neighbours());           // 14 + 8, one of them twice: kept once (the newer)
        bool newest_first = true;
        for (size_t i = 1; i < rd.neighbours.size(); ++i) newest_first = newest_first && rd.neighbours[i - 1].heard_at >= rd.neighbours[i].heard_at;
        CHECK(newest_first && rd.neighbours[3].heard_at == g_time - 35);
        // Refresh of the section: page 0 again, a new list
        CHECK(rd.refresh(RepSection::Neighbours) && rd.pop(n) && n.offset == 0);
        pg = rep_result(RO::Neighbours, 0);
        pg.neighbours = rep_page(0, 3, 3, 0x50);
        CHECK(rd.on_result(pg, g_time + 60, "H").empty() && rd.neighbours.size() == 3 && !rd.nb_more && rd.nb_next == 3);
    }

    // ---- 0.8.1: a sub-page opened reads its section by itself ONCE (a session, never tried since the last clear); Refresh asks again
    {
        RepeaterReads rd;
        RepeaterReads::Next n;
        CHECK(!rd.auto_read(RepSection::Neighbours, false) && !rd.running());                        // no session: nothing by itself
        CHECK(rd.auto_read(RepSection::Neighbours, true) && rd.busy(RepSection::Neighbours) && !rd.busy(RepSection::Status));
        CHECK(!rd.auto_read(RepSection::Owner, true) && rd.queued() == 1);                           // nothing while a read runs
        CHECK(rd.pop(n) && n.section == RepSection::Neighbours && rd.section(RepSection::Neighbours).tried && rd.busy(RepSection::Neighbours));
        rd.on_result(rep_result(RO::Neighbours, 0, E::Timeout), g_time, "H");
        CHECK(!rd.running() && !rd.busy(RepSection::Neighbours));
        CHECK(!rd.auto_read(RepSection::Neighbours, true) && !rd.running());                         // failed once: no automatic retry
        CHECK(rd.refresh(RepSection::Neighbours) && rd.pop(n));                                      // Refresh does ask again
        rd.on_result(rep_result(RO::Neighbours, 0), g_time, "H");
        CHECK(!rd.auto_read(RepSection::Neighbours, true));                                          // read: not again by itself
        // the status read after the login counts: opening its page shows it at once, nothing asked
        CHECK(rd.refresh(RepSection::Status) && rd.pop(n));
        rd.on_result(rep_result(RO::Status, 0), g_time, "H");
        CHECK(!rd.auto_read(RepSection::Status, true) && !rd.running());
        // a refusal at once counts as tried too (firmware too old: the page says so, nothing is asked again)
        CHECK(rd.auto_read(RepSection::Owner, true) && rd.pop(n));
        rd.on_result(rep_result(RO::Owner, 0, E::Unsupported), g_time, "H");
        CHECK(!rd.auto_read(RepSection::Owner, true));
        // a new login (clear()): each one once again
        rd.clear();
        CHECK(!rd.section(RepSection::Owner).tried && rd.auto_read(RepSection::Owner, true));
        CHECK(!rd.auto_read(RepSection::None, true));
    }

    // ---- the hub (0.8.1): ACCESS, then READ with a row per section that opens its screen, its summary at the right; and each
    // section's own rows and line under the header
    {
        Model m;
        m.upsert_contact(counting_contact(0x90, "Hilltop repeater", advtype::kRepeater, g_time - 60, 0));
        m.upsert_contact(counting_contact(0xE2, "Ridge", advtype::kRepeater, g_time - 600, 1));
        RepeaterReads rd;
        Client::RemoteSession ses;
        ses.role = Client::RemoteRole::Guest;
        ses.since = g_time;
        Client::RemoteState remote;
        RepeaterView v;
        v.reads = &rd;
        v.model = &m;
        v.remote = &remote;
        v.now = g_time;
        v.now_ms = 1000;
        v.name = "Hilltop repeater";
        v.hops = -1;
        RepeaterReads::Next n;
        CHECK(rep_sections(false) == std::vector<RepSection>({RepSection::Status, RepSection::Neighbours, RepSection::Owner}) &&
              rep_sections(true) == std::vector<RepSection>({RepSection::Status}));
        // not logged in, flood: every section needs a login (the owner: or a direct route)
        std::vector<RepeaterRow> rows = repeater_rows(v);
        CHECK(rows.size() == 6 && rows[2].kind == RepeaterRow::Section && rows[2].label == "READ" && rows[2].id == "read");
        const RepeaterRow *ost = rep_row(rows, "open.st"), *onb = rep_row(rows, "open.nb"), *oown = rep_row(rows, "open.own");
        CHECK(ost && onb && oown && ost->label == "Status" && onb->label == "Neighbours" && oown->label == "Owner");
        CHECK(ost->aff == Aff::Open && ost->action == RepAction::Open && ost->section == RepSection::Status && ost->selectable() && oown->section == RepSection::Owner);
        CHECK(ost->value == "needs login" && onb->value == "needs login" && oown->value == "needs login" && ost->tone == Tone::Muted);
        CHECK(!rep_row(rows, "access.role")->selectable() && !rep_row(rows, "read")->selectable());
        CHECK(rep_section_line(v, RepSection::Status).text == "Log in first: Esc, then Log in" && rep_section_line(v, RepSection::Owner).text == "Log in, or get a direct route");
        rows = repeater_section_rows(v, RepSection::Status);
        CHECK(rows.size() == 1 && rows[0].id == "st.none" && rows[0].label == "Log in to read" && rows[0].selectable());
        rows = repeater_section_rows(v, RepSection::Owner);
        CHECK(rows.size() == 1 && rows[0].id == "own.login" && rows[0].label == "Log in to read" && rows[0].value == "or a direct route" && rows[0].aff == Aff::Info);
        CHECK(repeater_section_rows(v, RepSection::None).empty());
        // not logged in, a direct route (0x81 brought one, or zero hop): the owner can be read without a login
        v.hops = 1;
        CHECK(rep_summary(v, RepSection::Owner).text == "not read yet" && rep_summary(v, RepSection::Status).text == "needs login");
        CHECK(rep_section_line(v, RepSection::Owner).text == "not read yet  \xC2\xB7  tap Refresh");
        rows = repeater_section_rows(v, RepSection::Owner);
        CHECK(rows.size() == 1 && rows[0].id == "own.read" && rows[0].label == "Read owner (direct route)" && rows[0].aff == Aff::Action &&
              rows[0].action == RepAction::ReadOwner && rows[0].selectable());
        // the anonymous request waits: the hub's summary and the page's line say so
        CHECK(rd.refresh(RepSection::Owner) && rd.pop(n));
        CHECK(rep_summary(v, RepSection::Owner).text == "sending\xE2\x80\xA6" && rep_section_line(v, RepSection::Owner).text == "sending\xE2\x80\xA6");
        rd.sent_as(3);
        remote.phase = Client::RemoteState::Phase::Waiting;
        remote.req = 3;
        remote.deadline_ms = 13000;
        CHECK(rep_summary(v, RepSection::Owner).text == "waiting\xE2\x80\xA6 up to 12 s" && rep_section_line(v, RepSection::Owner).text == "waiting\xE2\x80\xA6 up to 12 s");
        CHECK(rep_section_state(v, RepSection::Owner).text == "waiting\xE2\x80\xA6 up to 12 s");
        // ... without an answer: the summary "no answer" (Gold), the line and the first row the whole text, the limit line, Read owner again
        Client::RemoteResult anon = rep_result(RO::Owner, 3, E::Timeout);
        anon.anonymous = true;
        anon.sent = true;
        rd.on_result(anon, g_time, v.name);
        remote = Client::RemoteState();
        CHECK(rep_summary(v, RepSection::Owner).text == "no answer" && rep_summary(v, RepSection::Owner).tone == Tone::Gold);
        CHECK(rep_section_line(v, RepSection::Owner).text == "No answer. Hilltop repeater may be out of reach." && rep_section_line(v, RepSection::Owner).tone == Tone::Gold);
        rows = repeater_section_rows(v, RepSection::Owner);
        CHECK(rows.size() == 3 && rows[0].id == "own.err" && rows[0].kind == RepeaterRow::Note && rows[0].tone == Tone::Gold &&
              rows[0].label == "No answer. Hilltop repeater may be out of reach." && rows[1].id == "own.read" && rows[2].id == "own.limit");
        CHECK(rows[2].label.find("4 times in 3 minutes, for everyone") != std::string::npos && !rows[2].selectable() && !rows[0].selectable());
        CHECK(rep_row(repeater_rows(v), "open.own")->value == "no answer" && rep_row(repeater_rows(v), "open.own")->tone == Tone::Gold);
        // ... answered: Name and the owner info (no firmware in the anonymous answer); the summary: the owner info's first line
        CHECK(rd.refresh(RepSection::Owner) && rd.pop(n));
        anon = rep_result(RO::Owner, 0);
        anon.owner.anonymous = true;
        anon.owner.name = "Hilltop repeater";
        anon.owner.owner_info = "Olivier\nAsk on #test";
        rd.on_result(anon, g_time, v.name);
        CHECK(rep_summary(v, RepSection::Owner).text == "Olivier" && rep_summary(v, RepSection::Owner).tone == Tone::Muted);
        CHECK(rep_section_line(v, RepSection::Owner).text == "read " + fmt_clock(g_time) + "  \xC2\xB7  tap Refresh to read again");
        rows = repeater_section_rows(v, RepSection::Owner);
        CHECK(rep_ids(rows, RepSection::Owner) == std::vector<std::string>({"own.name", "own.info", "own.read"}) && rep_row(rows, "own.name")->value == "Hilltop repeater");
        CHECK(rep_row(rows, "own.info")->kind == RepeaterRow::Note && rep_row(rows, "own.info")->label == "Olivier\nAsk on #test" && rep_row(rows, "own.info")->tone == Tone::Normal);
        // the route back to flood (Reset path): Read owner goes, the old answer stays
        v.hops = -1;
        rows = repeater_section_rows(v, RepSection::Owner);
        CHECK(!rep_row(rows, "own.read") && rep_row(rows, "own.login") && rep_row(rows, "own.name"));
        // logged in, nothing read (a new login: the status comes by itself; the others "not read yet")
        rd.clear();
        v.session = &ses;
        rows = repeater_rows(v);
        CHECK(rep_row(rows, "open.st")->value == "not read yet" && rep_row(rows, "open.nb")->value == "not read yet" && rep_row(rows, "open.own")->value == "not read yet");
        CHECK(rows.size() == 8 && rows[4].label == "READ");                                                    // ACCESS: role, clock, the note of Log out
        rows = repeater_section_rows(v, RepSection::Neighbours);
        CHECK(rows.size() == 1 && rows[0].id == "nb.none" && rows[0].label == "Not read yet");
        CHECK(rep_section_line(v, RepSection::Status).text == "not read yet  \xC2\xB7  tap Refresh");
        // the status read: its summary on the hub, one row per value on its page, the pairs on one row
        RepeaterStatus s;
        s.battery_mv = 4123;
        s.tx_queue = 2;
        s.noise_floor = -112;
        s.last_rssi = -87;
        s.last_snr = -6.25;
        s.packets_rx = 123456;
        s.packets_tx = 7890;
        s.airtime_tx_s = 3600;
        s.uptime_s = 3 * 86400 + 4 * 3600 + 12 * 60;
        s.flood_tx = 500;
        s.direct_tx = 300;
        s.flood_rx = 6000;
        s.direct_rx = 700;
        s.direct_dups = 11;
        s.flood_dups = 222;
        s.has_airtime_rx = true;
        s.airtime_rx_s = 7200 + 125;
        s.has_rx_errors = true;
        s.rx_errors = 33;
        CHECK(rd.refresh(RepSection::Status) && rd.pop(n));
        Client::RemoteResult sr = rep_result(RO::Status, 0);
        sr.status = s;
        rd.on_result(sr, g_time, v.name);
        CHECK(rep_summary(v, RepSection::Status).text == "4.12 V  \xC2\xB7  up 3 d 4 h" && rep_row(repeater_rows(v), "open.st")->value == "4.12 V  \xC2\xB7  up 3 d 4 h");
        CHECK(rep_section_line(v, RepSection::Status).text == "read " + fmt_clock(g_time) + "  \xC2\xB7  tap Refresh to read again" &&
              rep_section_line(v, RepSection::Status).tone == Tone::Muted && rep_section_state(v, RepSection::Status).text == "read " + fmt_clock(g_time));
        rows = repeater_section_rows(v, RepSection::Status);
        CHECK(rep_ids(rows, RepSection::Status) == std::vector<std::string>({"st.battery", "st.uptime", "st.noise", "st.signal", "st.packets", "st.rxsplit", "st.txsplit",
                                                                            "st.airtime", "st.dups", "st.rxerr", "st.txq"}));
        CHECK(rep_row(rows, "st.battery")->label == "Battery" && rep_row(rows, "st.battery")->value == "4.12 V");
        CHECK(rep_row(rows, "st.uptime")->value == "3 d 4 h 12 m" && rep_row(rows, "st.noise")->value == "-112 dBm");
        CHECK(rep_row(rows, "st.signal")->label == "Last RSSI / SNR" && rep_row(rows, "st.signal")->value == "-87 dBm / -6.3 dB");
        CHECK(rep_row(rows, "st.packets")->label == "Packets received / sent" && rep_row(rows, "st.packets")->value == "123456 / 7890");
        CHECK(rep_row(rows, "st.rxsplit")->label == "Received flood / direct" && rep_row(rows, "st.rxsplit")->value == "6000 / 700");
        CHECK(rep_row(rows, "st.txsplit")->label == "Sent flood / direct" && rep_row(rows, "st.txsplit")->value == "500 / 300");
        CHECK(rep_row(rows, "st.airtime")->label == "Airtime TX / RX" && rep_row(rows, "st.airtime")->value == "1 h 0 m / 2 h 2 m");
        CHECK(rep_row(rows, "st.dups")->label == "Duplicates flood / direct" && rep_row(rows, "st.dups")->value == "222 / 11");
        CHECK(rep_row(rows, "st.rxerr")->label == "RX errors" && rep_row(rows, "st.rxerr")->value == "33" && rep_row(rows, "st.txq")->value == "2");
        for (const RepeaterRow &r : rows) CHECK(r.selectable() && r.aff == Aff::Info && r.action == RepAction::None);
        // a repeater before v1.12.0: no RX errors row; a room: TX airtime only, its posts; its hub has Status only
        s.has_rx_errors = false;
        sr.status = s;
        CHECK(rd.refresh(RepSection::Status) && rd.pop(n));
        rd.on_result(sr, g_time, v.name);
        CHECK(!rep_row(repeater_section_rows(v, RepSection::Status), "st.rxerr"));
        s.room = true;
        s.has_airtime_rx = false;
        s.posted = 17;
        s.post_pushes = 9;
        sr.status = s;
        CHECK(rd.refresh(RepSection::Status) && rd.pop(n));
        rd.on_result(sr, g_time, v.name);
        v.room = true;
        rows = repeater_section_rows(v, RepSection::Status);
        CHECK(rep_row(rows, "st.airtime")->label == "Airtime TX" && rep_row(rows, "st.airtime")->value == "1 h 0 m" && rep_row(rows, "st.posts")->value == "17 / 9");
        rows = repeater_rows(v);
        CHECK(rep_row(rows, "open.st") && !rep_row(rows, "open.nb") && !rep_row(rows, "open.own") && rows.back().id == "open.st");
        v.room = false;
        // the status without an answer after a good read: the old values stay, the error first, the summary says it
        CHECK(rd.refresh(RepSection::Status) && rd.pop(n));
        rd.on_result(rep_result(RO::Status, 0, E::Timeout), g_time + 600, v.name);
        CHECK(rep_summary(v, RepSection::Status).text == "no answer" && rep_summary(v, RepSection::Status).tone == Tone::Gold);
        CHECK(rep_section_line(v, RepSection::Status).text == read_error_text(E::Timeout, -1, "Hilltop repeater") &&
              rep_section_state(v, RepSection::Status).text == "no answer, read " + fmt_clock(g_time));
        rows = repeater_section_rows(v, RepSection::Status);
        const std::vector<std::string> st_ids = rep_ids(rows, RepSection::Status);
        CHECK(st_ids.size() >= 2 && st_ids[0] == "st.err" && st_ids[1] == "st.battery" && !rep_row(rows, "st.err")->selectable());
        CHECK(rep_row(rows, "st.err")->label == "No answer. Hilltop repeater may be out of reach, or its session ended (a reboot): log in again.");
        // while it is read again: the line waits, the error note goes
        CHECK(rd.refresh(RepSection::Status) && rd.pop(n));
        CHECK(rep_section_line(v, RepSection::Status).text == "sending\xE2\x80\xA6" && !rep_row(repeater_section_rows(v, RepSection::Status), "st.err"));
        rd.on_result(rep_result(RO::Status, 0, E::Cancelled), g_time, v.name);
        // the neighbours: known names, a prefix, More with what is shown of the total; the hub: how many, the newest
        CHECK(rd.refresh(RepSection::Neighbours) && rd.pop(n));
        Client::RemoteResult nr = rep_result(RO::Neighbours, 0);
        nr.neighbours = rep_page(0, 14, 22, 0x8E);                            // 8e..., 8f..., 90 91 92 93 (Hilltop), ...
        nr.neighbours.entries[3].prefix = {0xE2, 0xE3, 0xE4, 0xE5};           // Ridge
        rd.on_result(nr, g_time, v.name);
        v.now = g_time + 7;
        CHECK(rep_summary(v, RepSection::Neighbours).text == "22 heard  \xC2\xB7  newest 12 s ago");
        rows = repeater_section_rows(v, RepSection::Neighbours);
        const std::vector<std::string> nb_ids = rep_ids(rows, RepSection::Neighbours);
        CHECK(nb_ids.size() == 15 && nb_ids.back() == "nb.more");
        CHECK(rep_row(rows, "nb.8e8f9091") && rep_row(rows, "nb.8e8f9091")->label == "<8e8f9091" && rep_row(rows, "nb.8e8f9091")->value == "SNR 7.0  \xC2\xB7  12 s ago");
        CHECK(rep_row(rows, "nb.90919293") && rep_row(rows, "nb.90919293")->label == "Hilltop repeater" && rep_row(rows, "nb.e2e3e4e5")->label == "Ridge");
        CHECK(rep_row(rows, "nb.e2e3e4e5")->value == "SNR 5.5  \xC2\xB7  42 s ago");
        const RepeaterRow *more = rep_row(rows, "nb.more");
        CHECK(more && more->label == "More neighbours" && more->value == "14 of 22 shown" && more->aff == Aff::Action && more->action == RepAction::MoreNeighbours);
        // the next page without an answer: the error first, the list and More (it asks that page again) stay
        CHECK(rd.more_neighbours() && rd.pop(n) && n.offset == 14);
        rd.on_result(rep_result(RO::Neighbours, 0, E::Timeout), g_time, v.name);
        rows = repeater_section_rows(v, RepSection::Neighbours);
        CHECK(rows.front().id == "nb.err" && rep_row(rows, "nb.more") && rep_ids(rows, RepSection::Neighbours).size() == 16);
        CHECK(rd.more_neighbours() && rd.pop(n) && n.offset == 14);
        nr = rep_result(RO::Neighbours, 0);
        nr.neighbours = rep_page(14, 8, 22, 0x8E);
        rd.on_result(nr, g_time, v.name);
        rows = repeater_section_rows(v, RepSection::Neighbours);
        CHECK(rep_ids(rows, RepSection::Neighbours).size() == 22 && !rep_row(rows, "nb.more") && !rep_row(rows, "nb.err"));
        // an empty list
        CHECK(rd.refresh(RepSection::Neighbours) && rd.pop(n));
        nr = rep_result(RO::Neighbours, 0);
        nr.neighbours = NeighboursPage();
        rd.on_result(nr, g_time, v.name);
        CHECK(rep_ids(repeater_section_rows(v, RepSection::Neighbours), RepSection::Neighbours) == std::vector<std::string>({"nb.empty"}) &&
              rep_summary(v, RepSection::Neighbours).text == "none heard");
        // the owner, logged in: Name, Firmware, the owner info; the summary: owner info and version ("v" dropped); none set: the name
        CHECK(rd.refresh(RepSection::Owner) && rd.pop(n));
        Client::RemoteResult orr = rep_result(RO::Owner, 0);
        orr.owner.name = "Hilltop repeater";
        orr.owner.fw_version = "v1.12.0";
        orr.owner.owner_info = "Olivier";
        rd.on_result(orr, g_time, v.name);
        CHECK(rep_summary(v, RepSection::Owner).text == "Olivier  \xC2\xB7  fw 1.12.0");
        rows = repeater_section_rows(v, RepSection::Owner);
        CHECK(rep_ids(rows, RepSection::Owner) == std::vector<std::string>({"own.name", "own.fw", "own.info"}) && rep_row(rows, "own.fw")->value == "v1.12.0" &&
              rep_row(rows, "own.info")->kind == RepeaterRow::Note);
        CHECK(rd.refresh(RepSection::Owner) && rd.pop(n));
        orr.owner.owner_info.clear();
        rd.on_result(orr, g_time, v.name);
        CHECK(rep_summary(v, RepSection::Owner).text == "Hilltop repeater  \xC2\xB7  fw 1.12.0" && rep_row(repeater_section_rows(v, RepSection::Owner), "own.info")->value == "none set");
        CHECK(rd.refresh(RepSection::Owner) && rd.pop(n));
        rd.on_result(rep_result(RO::Owner, 0, E::Unsupported), g_time, v.name);
        CHECK(rep_summary(v, RepSection::Owner).text == "firmware too old" && rep_section_line(v, RepSection::Owner).text == "Firmware too old for this");
        CHECK(repeater_section_rows(v, RepSection::Owner).front().label == "Firmware too old for this");
        // every id is unique; the hub's rows: only READ's open rows can be selected
        rows = repeater_rows(v);
        std::vector<std::string> all;
        for (const RepeaterRow &r : rows) all.push_back(r.id);
        std::vector<std::string> sorted = all;
        std::sort(sorted.begin(), sorted.end());
        CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end() && std::find(all.begin(), all.end(), "") == all.end());
        for (const RepeaterRow &r : rows) CHECK(r.selectable() == (r.action == RepAction::Open));
        // a read refused at once (the CLI holds the node), nothing read before: the error alone, no "Not read yet"
        rd.clear();
        CHECK(rd.refresh(RepSection::Status) && rd.pop(n));
        rd.on_result(rep_result(RO::Status, 0, E::Busy), g_time, v.name);
        rows = repeater_section_rows(v, RepSection::Status);
        CHECK(rows.size() == 1 && rows[0].label == "The CLI holds the node" && rep_summary(v, RepSection::Status).text == "not read");
    }

    // ---- the back rule (0.8.1): Esc on a section's screen goes back to the hub; on the hub it closes the Repeater page
    {
        NavState s;
        s.tab = Tab::Contacts;
        s.repeater_open = true;
        CHECK(back_action(s) == BackAction::CloseRepeater);
        s.repeater_section = true;
        CHECK(back_action(s) == BackAction::CloseRepeaterSection);
        s.popup_open = true;
        CHECK(back_action(s) == BackAction::ClosePopup);
        s.popup_open = false;
        s.detail_open = true;                                                // (Details opens from the hub's More only)
        CHECK(back_action(s) == BackAction::CloseDetail);
    }
}
