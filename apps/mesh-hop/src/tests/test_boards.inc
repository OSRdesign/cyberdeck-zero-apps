// Per-board data and the factory reset (0.2.2), included by test_core.cpp after test_phase2.inc (it shares the fake board and the helpers).
// The store keeps the history, read marks, mute flags, groups, ignored nodes and the advert schedule of each board in its own folder, chosen by the
// public key of SELF_INFO; the first start of the new layout moves the old files to the board that is connected; a swap while the app runs shows the
// other board's data; the factory reset is judged by the identity the board reports when it is back.

#include <dirent.h>

static bool exists_file(const std::string &f)
{
    struct stat st;
    return ::stat(f.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static IncomingMessage board_dm(uint8_t first, const char *text, uint32_t stamp)
{
    IncomingMessage dm;
    dm.prefix = {first, 2, 3, 4, 5, 6};
    dm.text = text;
    dm.sender_timestamp = stamp;
    return dm;
}

static void fill_board(Model &m, uint8_t seed, int unread_msgs)
{
    m.replace_contacts({fake_contact(seed, "Contact" + std::to_string(seed), 1)});
    ChannelRec ch;
    ch.idx = 0; ch.name = "Public"; ch.empty = false;
    m.set_channel(ch);
    for (int i = 0; i < unread_msgs; ++i) m.add_incoming(board_dm(seed, ("hello " + std::to_string(i)).c_str(), 100 + static_cast<uint32_t>(i)), g_time + static_cast<uint32_t>(i));
    m.add_outgoing("c:0", "mine on " + std::to_string(seed), g_time + 50);
}

/* ---------------------------------------------------------------- the store with two boards */

static void test_boards_store()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("boards");
    const PubKey ka = test_board_key(1), kb = test_board_key(2);
    {
        Model m;
        Store st(dir);
        std::vector<std::string> log;
        st.set_log([&](const std::string &s) { log.push_back(s); });
        CHECK(st.attach(m));
        CHECK(!m.board_known() && m.message_count() == 0 && st.board_dir().empty() && m.board_epoch() == 0);
        m.set_retry({4, 1});                                                  // about the deck: global, not per board

        CHECK(m.select_board(ka) && m.board_known() && m.board_epoch() == 1 && !st.board_dir().empty());
        CHECK(st.board_id() == to_hex(ka).substr(0, 12) && st.board_dir() == dir + "/boards/" + to_hex(ka).substr(0, 12));
        fill_board(m, 0x10, 3);
        m.add_incoming(board_dm(0x11, "other", 1), g_time + 60);
        m.mark_read("d:110203040506");                                        // one conversation read, the other has 3 unread
        m.groups_mut().add_group("Crew");
        m.groups_changed();
        m.set_ignored(to_hex(key_of(0x42)), true);
        m.set_advert_schedule({6, false});
        m.set_muted("c:0", true);
        const int unread_a = m.unread_total();
        const size_t msgs_a = m.message_count();
        CHECK(msgs_a == 5 && unread_a == 3 && m.unread("d:100203040506") == 3 && m.unread("d:110203040506") == 0);

        CHECK(!m.select_board(ka) && m.board_epoch() == 1);                   // the same board again: nothing is dropped or reloaded
        CHECK(m.message_count() == msgs_a);

        // plug the other board: its own (empty) data, nothing of the first
        CHECK(m.select_board(kb) && m.board_epoch() == 2);
        CHECK(m.message_count() == 0 && m.contact_count() == 0 && m.channels().empty() && m.conversations().empty() && m.unread_total() == 0);
        CHECK(m.muted().empty() && m.ignored().empty() && m.groups().groups().empty() && m.advert_schedule().interval_hours == 0 && m.next_seq() == 1);
        CHECK(m.retry().attempts == 4 && m.retry().reset_after == 1);         // the deck's retry rules stay
        fill_board(m, 0x50, 2);
        CHECK(m.message_count() == 3 && m.unread_total() == 2 && m.unread("d:500203040506") == 2);
        CHECK(m.contacts_sorted()[0]->c.name == "Contact80" && st.board_id() == to_hex(kb).substr(0, 12));
        m.set_muted("d:500203040506", true);
        CHECK(m.unread_total() == 0);

        // back to the first: everything restored, the unread totals follow the board
        CHECK(m.select_board(ka));
        CHECK(m.message_count() == msgs_a && m.contact_count() == 1 && m.contacts_sorted()[0]->c.name == "Contact16");
        CHECK(m.is_muted("c:0") && !m.is_muted("d:500203040506") && m.groups().has("Crew") && m.is_ignored(to_hex(key_of(0x42))));
        CHECK(m.advert_schedule().interval_hours == 6 && !m.advert_schedule().flood);
        CHECK(m.unread_total() == unread_a && m.unread("d:110203040506") == 0);          // the read marks came back too
        CHECK(m.find_channel(0) && m.find_channel(0)->name == "Public");
        // B's unread count when it comes back: the two incoming messages are unread but its contact is muted -> 0
        CHECK(m.select_board(kb) && m.message_count() == 3 && m.is_muted("d:500203040506") && m.unread_total() == 0);
        m.set_muted("d:500203040506", false);
        CHECK(m.unread_total() == 2);
        CHECK(exists_file(bdir_of(dir, 1) + "/messages.jsonl") && exists_file(bdir_of(dir, 2) + "/messages.jsonl"));
        CHECK(!exists_file(dir + "/messages.jsonl") && !exists_file(dir + "/read.txt"));        // nothing of a board in the shared folder
        CHECK(read_text(dir + "/prefs.txt") == "retry 4 1\n");                                   // only the deck preference there
        CHECK(read_text(bdir_of(dir, 1) + "/prefs.txt").find("retry") == std::string::npos);
        CHECK(st.other_boards().size() == 1 && st.other_boards()[0].id == to_hex(ka).substr(0, 12) && st.other_boards()[0].bytes > 0);
        bool said = false;
        for (const std::string &l : log) if (l.find("board " + to_hex(kb).substr(0, 12)) != std::string::npos) said = true;
        CHECK(said);
    }
    {   // a restart: no board yet = nothing shown; then each board gets its own data back
        Model m;
        Store st(dir);
        st.attach(m);
        CHECK(!m.board_known() && m.message_count() == 0 && m.contact_count() == 0 && m.conversations().empty() && m.unread_total() == 0);
        CHECK(m.retry().attempts == 4 && m.retry().reset_after == 1);
        CHECK(m.select_board(ka) && m.message_count() == 5 && m.is_muted("c:0") && m.groups().has("Crew") && m.advert_schedule().interval_hours == 6);
        CHECK(m.select_board(kb) && m.message_count() == 3 && m.unread_total() == 2);
        // writes go to the board that is current, not to the previous one
        m.add_outgoing("c:0", "only on B", g_time + 99);
        CHECK(m.select_board(ka) && m.message_count() == 5);
        CHECK(m.select_board(kb) && m.message_count() == 4);
    }
    {   // forget: this board's app data, then the other boards' folders
        Model m;
        Store st(dir);
        st.attach(m);
        m.select_board(kb);
        const size_t contacts = m.contact_count();
        m.forget_board_app_data();
        CHECK(m.message_count() == 0 && m.muted().empty() && m.groups().groups().empty() && m.unread_total() == 0);
        CHECK(m.contact_count() == contacts && contacts == 1 && m.find_channel(0) != nullptr);        // the board's own mirror stays
        CHECK(!exists_file(bdir_of(dir, 2) + "/messages.jsonl") && exists_file(bdir_of(dir, 2) + "/contacts.jsonl"));
        CHECK(st.other_boards().size() == 1);
        CHECK(st.forget_other_boards() == 1 && st.other_boards().empty() && !exists_file(bdir_of(dir, 1) + "/messages.jsonl"));
        CHECK(exists_file(bdir_of(dir, 2) + "/contacts.jsonl"));                                      // the current board is untouched
        Model m2;
        Store st2(dir);
        st2.attach(m2);
        CHECK(m2.select_board(ka) && m2.message_count() == 0);                                         // forgotten: starts empty
        CHECK(m2.select_board(kb) && m2.message_count() == 0 && m2.contact_count() == 1);
    }
}

/* ---------------------------------------------------------------- the move of the old layout */

static void test_boards_migration()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("migration");
    CHECK(std::system(("mkdir -p " + dir).c_str()) == 0);
    const std::string msg = "{\"i\":1,\"k\":\"c:0\",\"d\":0,\"t\":1790000000,\"s\":5,\"n\":\"Zed\",\"x\":\"old history\",\"st\":0,\"h\":-1}\n"
                            "{\"i\":2,\"k\":\"d:101010101010\",\"d\":0,\"t\":1790000100,\"s\":6,\"n\":\"Alice\",\"x\":\"second\",\"st\":0,\"h\":-1}\n";
    const std::string prefs = "mute c:0\nretry 2 1\nadvert 3 0\nignore " + to_hex(key_of(0x42)) + "\n";
    const Contact c = fake_contact(0x10, "Alice", 1);
    write_text(dir + "/messages.jsonl", msg);
    write_text(dir + "/read.txt", "d:101010101010 2\n");
    write_text(dir + "/channels.jsonl", "{\"idx\":0,\"nm\":\"Public\",\"em\":0}\n");
    write_text(dir + "/groups.jsonl", "{\"v\":1,\"n\":0}\n");
    write_text(dir + "/groups.bak", "{\"v\":1,\"n\":0}\n");
    write_text(dir + "/prefs.txt", prefs);
    write_text(dir + "/presets.jsonl", "keep me\n");
    write_text(dir + "/port", "/dev/ttyACM0\n");
    write_text(dir + "/contacts.jsonl", "{\"pk\":\"" + to_hex(c.key) + "\",\"ty\":1,\"fl\":0,\"pl\":255,\"pa\":\"" + std::string(128, '0') +
                                           "\",\"nm\":\"Alice\",\"la\":1790000000,\"lat\":0,\"lon\":0,\"lm\":0,\"hl\":0}\n");
    const std::string contacts_before = read_text(dir + "/contacts.jsonl");
    const PubKey ka = test_board_key(1), kb = test_board_key(2);
    {
        Model m;
        Store st(dir);
        std::vector<std::string> log;
        st.set_log([&](const std::string &s) { log.push_back(s); });
        st.attach(m);
        CHECK(m.message_count() == 0 && m.conversations().empty());                                  // before the first SELF_INFO: nothing
        CHECK(exists_file(dir + "/messages.jsonl") && m.retry().attempts == 2);                      // untouched until a board is known
        CHECK(m.select_board(ka));                                                                    // the board connected at the first start gets the old files
        CHECK(st.migrated_files() >= 7);
        CHECK(m.message_count() == 2 && m.is_muted("c:0") && m.advert_schedule().interval_hours == 3 && !m.advert_schedule().flood);
        CHECK(m.is_ignored(to_hex(key_of(0x42))) && m.unread("d:101010101010") == 0 && m.find_channel(0) && m.next_seq() == 3 && m.contact_count() == 1);
        CHECK(read_text(dir + "/contacts.jsonl.pre-boards.bak") == contacts_before && read_text(bdir_of(dir) + "/contacts.jsonl") == contacts_before);
        CHECK(m.retry().attempts == 2 && m.retry().reset_after == 1);
        // the working copies are in the board folder, the originals are kept as .bak, nothing is left behind to be moved twice
        CHECK(read_text(bdir_of(dir) + "/messages.jsonl") == msg && read_text(dir + "/messages.jsonl.pre-boards.bak") == msg);
        CHECK(read_text(bdir_of(dir) + "/read.txt") == "d:101010101010 2\n" && read_text(dir + "/read.txt.pre-boards.bak") == "d:101010101010 2\n");
        CHECK(!exists_file(dir + "/messages.jsonl") && !exists_file(dir + "/read.txt") && !exists_file(dir + "/channels.jsonl") && !exists_file(dir + "/groups.jsonl"));
        CHECK(read_text(dir + "/prefs.txt.pre-boards.bak") == prefs && read_text(dir + "/prefs.txt") == "retry 2 1\n");
        CHECK(read_text(bdir_of(dir) + "/prefs.txt").find("mute c:0\n") != std::string::npos && read_text(bdir_of(dir) + "/prefs.txt").find("retry") == std::string::npos);
        CHECK(exists_file(dir + "/presets.jsonl") && exists_file(dir + "/port"));                    // other global files are not touched
        bool said = false;
        for (const std::string &l : log) if (l.find("migrated") != std::string::npos) said = true;
        CHECK(said);
        CHECK(m.select_board(kb) && m.message_count() == 0 && !m.is_muted("c:0") && m.groups().groups().empty());     // the second board never sees the old history
        CHECK(m.retry().attempts == 2);
    }
    {   // the next start: nothing to move again, the history is still the first board's
        Model m;
        Store st(dir);
        st.attach(m);
        CHECK(m.select_board(kb) && m.message_count() == 0);
        CHECK(m.select_board(ka) && st.migrated_files() == 0 && m.message_count() == 2 && m.is_muted("c:0"));
    }
    {   // a board folder that already has a file keeps it; the old copy is only renamed (and still readable)
        const std::string d2 = tmpdir("migration2");
        CHECK(std::system(("mkdir -p " + bdir_of(d2)).c_str()) == 0);
        write_text(bdir_of(d2) + "/messages.jsonl", msg.substr(0, msg.find('\n') + 1));
        write_text(d2 + "/messages.jsonl", msg);
        Model m;
        Store st(d2);
        st.attach(m);
        m.select_board(ka);
        CHECK(m.message_count() == 1 && read_text(d2 + "/messages.jsonl.pre-boards.bak") == msg);
    }
}

/* ---------------------------------------------------------------- a swap while the client runs, and the factory reset */

struct BoardRig {
    FakePeer peer;
    Model model;
    Store store;
    std::unique_ptr<Client> client;
    std::vector<std::string> log;
    explicit BoardRig(const std::string &dir, const std::function<void(FakePeer &)> &setup = nullptr) : store(dir)
    {
        set_time_source(fake_time);
        set_time_offset(0);
        if (setup) setup(peer);
        store.attach(model);
        store.set_log([this](const std::string &s) { log.push_back(s); });
        client = std::make_unique<Client>(peer, model);
        client->set_deck_clock([] { return DeckClock::Synced; }, [] {});
        client->set_log([this](const std::string &s) { log.push_back(s); });
    }
    bool logged(const std::string &needle) const
    {
        for (const std::string &l : log)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

static void test_boards_swap_running()
{
    const std::string dir = tmpdir("swap");
    BoardRig r(dir);
    CHECK(!r.model.board_known());
    run(*r.client, 4500);
    CHECK(r.client->link() == Link::Ready && r.model.board_known() && r.model.board_epoch() == 1);
    const std::string key_a = r.model.board_key();
    CHECK(key_a == to_hex(r.model.self()->key) && r.model.contact_count() == 2);
    r.model.add_outgoing("c:0", "hello from A", 1790000000);
    r.model.add_incoming(board_dm(0x10, "A says hi", 7), 1790000001);
    r.model.groups_mut().add_group("Crew");
    r.model.groups_changed();
    r.model.set_muted("c:0", true);
    CHECK(r.model.message_count() == 2 && r.model.unread_total() == 1);

    // unplug A, plug B (another public key, its own contacts)
    r.peer.plugged = false;
    r.peer.lost = true;
    run(*r.client, 1500);
    CHECK(r.client->link() == Link::Searching && r.model.board_key() == key_a && r.model.message_count() == 2);       // unplugged: A's data is still what is shown
    r.peer.key_seed = 0xB0;
    r.peer.self.name = "Board B";
    r.peer.contacts.clear();
    r.peer.contacts.push_back(std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x70, "Bea", 1, 1790000000))));
    r.peer.plugged = true;
    run(*r.client, 6000);
    CHECK(r.client->link() == Link::Ready && r.model.board_epoch() == 2 && r.model.board_key() != key_a);
    CHECK(r.model.message_count() == 0 && r.model.unread_total() == 0 && r.model.conversations().size() == 2);          // only the board's two channels
    CHECK(r.model.contact_count() == 1 && r.model.contacts_sorted()[0]->c.name == "Bea" && !r.model.is_muted("c:0") && r.model.groups().groups().empty());
    CHECK(r.model.self_name() == "Board B");
    r.model.add_incoming(board_dm(0x70, "B says hi", 8), 1790000002);
    CHECK(r.model.unread_total() == 1);

    // back to A
    r.peer.plugged = false;
    r.peer.lost = true;
    run(*r.client, 1500);
    r.peer.key_seed = 0xA0;
    r.peer.self.name = "Deck Zero";
    r.peer.contacts.clear();
    r.peer.contacts.push_back(std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x10, "Alice", 1, 1790000000))));
    r.peer.plugged = true;
    run(*r.client, 6000);
    CHECK(r.client->link() == Link::Ready && r.model.board_key() == key_a && r.model.board_epoch() == 3);
    CHECK(r.model.message_count() == 2 && r.model.groups().has("Crew") && r.model.is_muted("c:0") && r.model.unread_total() == 1);
    CHECK(r.model.find_by_prefix({0x10, 0x11, 0x12, 0x13, 0x14, 0x15}) != nullptr);
    CHECK(r.logged("board " + key_a.substr(0, 12)));
}

static void test_factory_reset_cases()
{
    {   // the real thing: OK, a restart, another identity. The frame and the answer are in the log, the old identity keeps its folder.
        const std::string dir = tmpdir("reset1");
        BoardRig r(dir);
        run(*r.client, 4500);
        const std::string key_a = r.model.board_key();
        r.model.add_outgoing("c:0", "history of the old identity", 1790000000);
        r.model.groups_mut().add_group("Crew");
        r.model.groups_changed();
        r.peer.commands.clear();
        CHECK(r.client->factory_reset());
        run(*r.client, 200);
        CHECK(last_cmd(r.peer, cmd::kFactoryReset) == Bytes({0x33, 'r', 'e', 's', 'e', 't'}) && r.peer.resets == 1);
        CHECK(r.logged("factory reset requested: frame 337265736574 (6 bytes), on the wire 3c0600337265736574"));
        CHECK(r.logged("answered with code 0x00, first bytes 00"));
        CHECK(r.client->reset_phase() == Client::ResetPhase::Waiting && r.client->notice().ok && r.client->notice().text.find("Board reset") == 0);
        run(*r.client, 12000);
        CHECK(r.client->link() == Link::Ready && r.client->reset_phase() == Client::ResetPhase::Done);
        CHECK(r.client->notice().ok && r.client->notice().text == "Board reset: new keys, no contacts or channels");
        CHECK(r.logged("a NEW identity (public key"));
        CHECK(r.model.board_key() != key_a && r.model.contact_count() == 0 && r.model.message_count() == 0 && !r.model.groups().has("Crew"));
        CHECK(r.model.self_name() == "MeshCore" && r.model.find_channel(0) && r.model.find_channel(0)->empty);
        CHECK(exists_file(dir + "/boards/" + key_a.substr(0, 12) + "/messages.jsonl"));                                              // the old identity's history is kept
        CHECK(r.store.board_id() == r.model.board_key().substr(0, 12) && r.store.other_boards().size() == 1);
        // the phase ends by itself
        run(*r.client, 130000, 100);
        CHECK(r.client->reset_phase() == Client::ResetPhase::None);
    }
    {   // the board formats and restarts without ever sending OK (the link drops first): still "Board reset", never "failed"
        const std::string dir = tmpdir("reset2");
        BoardRig r(dir, [](FakePeer &p) { p.reset_answers = false; });
        run(*r.client, 4500);
        CHECK(r.client->factory_reset());
        run(*r.client, 400);
        CHECK(r.logged("the link went away while waiting") && r.client->notice().ok && r.client->notice().text.find("Board reset") == 0);
        CHECK(r.client->reset_phase() == Client::ResetPhase::Waiting);
        run(*r.client, 12000);
        CHECK(r.client->reset_phase() == Client::ResetPhase::Done && r.client->link() == Link::Ready && r.model.self_name() == "MeshCore");
    }
    {   // no answer and no restart within the wait: logged, the notice is not "done"
        const std::string dir = tmpdir("reset3");
        BoardRig r(dir, [](FakePeer &p) { p.reset_answers = false; p.drop_on_reboot = false; });
        run(*r.client, 4500);
        CHECK(r.client->factory_reset());
        run(*r.client, 61000, 100);
        CHECK(r.logged("no answer within 60 s") && !r.client->notice().ok);
    }
    {   // the board restarts with the same keys: the reset did not happen, and the app says so
        const std::string dir = tmpdir("reset4");
        BoardRig r(dir, [](FakePeer &p) { p.reset_new_key = false; });
        run(*r.client, 4500);
        const std::string key_a = r.model.board_key();
        r.model.add_outgoing("c:0", "still here", 1790000000);
        CHECK(r.client->factory_reset());
        run(*r.client, 12000);
        CHECK(r.client->reset_phase() == Client::ResetPhase::KeptKeys && !r.client->notice().ok && r.client->notice().text == "Restarted with the same keys: not reset");
        CHECK(r.model.board_key() == key_a && r.model.message_count() == 1);
    }
    {   // refused: the code of the answer is logged, nothing else happens
        const std::string dir = tmpdir("reset5");
        BoardRig r(dir, [](FakePeer &p) { p.factory_ok = false; });
        run(*r.client, 4500);
        CHECK(r.client->factory_reset());
        run(*r.client, 300);
        CHECK(r.logged("refused: the board answered with code 0x01, first bytes 0101") && r.client->reset_phase() == Client::ResetPhase::None);
        CHECK(!r.client->notice().ok && r.model.contact_count() == 2);
    }
}

static void test_boards()
{
    test_boards_store();
    test_boards_migration();
    test_boards_swap_running();
    test_factory_reset_cases();
}
