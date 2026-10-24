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
        CHECK(rows.size() == 3 && rows[0].kind == RepeaterRow::Section && rows[0].label == "ACCESS" && rows[1].label == "Not logged in" && rows[1].value.empty());
        CHECK(rows[2].kind == RepeaterRow::Note && rows[2].label == "Status, neighbours and owner: next version" && rows[2].tone == Tone::Muted);
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
        CHECK(rows.size() == 5 && rows[1].label == "Admin since " + fmt_clock(g_time) && rows[1].value == "password saved");
        CHECK(rows[2].label == "Clock at login" && rows[2].value == fmt_hms(g_time + 1) + "  \xC2\xB7  in step with the deck" && rows[2].tone == Tone::Muted);
        CHECK(rows[3].kind == RepeaterRow::Note && rows[3].label == logout_note(false) && rows[3].label.find("until setperm") != std::string::npos &&
              rows[3].label.find("until it reboots") != std::string::npos && rows[3].label.find("repeater") != std::string::npos);
        CHECK(rows[4].label == "Status, neighbours and owner: next version");
        ses.clock = g_time + 61;                                             // more than a minute ahead: Gold, and the warning
        rows = repeater_rows(v);
        CHECK(rows.size() == 6 && rows[2].tone == Tone::Gold && rows[2].value.find("1 min ahead") != std::string::npos);
        CHECK(rows[3].kind == RepeaterRow::Note && rows[3].tone == Tone::Gold && rows[3].label.find("more than a minute ahead") != std::string::npos);
        ses.clock = g_time + 60;                                             // a minute exactly: no warning
        CHECK(repeater_rows(v).size() == 5);
        ses.clock = g_time - 600;                                            // behind: no warning
        rows = repeater_rows(v);
        CHECK(rows.size() == 5 && rows[2].value.find("10 min behind") != std::string::npos && rows[2].tone == Tone::Muted);
        ses.has_clock = false;                                               // a legacy answer: no clock
        CHECK(repeater_rows(v)[2].value == "not sent (older firmware)");
        ses.role = Client::RemoteRole::ReadWrite;                            // a room: its roles, "room" in the notes
        v.room = true;
        v.password_saved = false;
        rows = repeater_rows(v);
        CHECK(rows[1].label == "Read-write since " + fmt_clock(g_time) && rows[1].value.empty() && rows[3].label.find("the room keeps") != std::string::npos);
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
        CHECK(more.size() == 3 && more[0].label == "Details" && more[1].label == "Open in CLI" && more[2].label == "Reset path");
        CHECK(more[0].action == RepeaterMore::Details && more[1].action == RepeaterMore::OpenCli && more[2].action == RepeaterMore::ResetPath);
        CHECK(more[0].aff == Aff::Open && more[1].aff == Aff::Open && more[2].aff == Aff::Action);
        more = repeater_more_items(true);
        CHECK(more.size() == 4 && more[3].label == "Forget password" && more[3].action == RepeaterMore::ForgetPassword && more[3].aff == Aff::Action &&
              more[2].label == "Reset path");
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
    }
}
