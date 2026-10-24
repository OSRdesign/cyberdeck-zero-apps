// Report 036 (Mesh Hop: managing a repeater from the contact list), slice S1: the decoders of LOGIN_SUCCESS / LOGIN_FAIL, the remote
// queue of Client (SEND_LOGIN 26, LOGOUT 29, the remote wait as a timer, one request at a time), the session table and the
// PasswordStore (meshcore-cli's <key>.pass). Included by test_core.cpp (FakePeer, Ready, run(), tmpdir()).

#include "passwords.hpp"

#include <dirent.h>

static void test_remote_protocol()
{
    PubKey k{};
    for (int i = 0; i < 32; ++i) k[static_cast<size_t>(i)] = static_cast<uint8_t>(0x50 + i);
    // SEND_LOGIN: the full key then the password bytes; a blank (guest) login is the key alone
    const Bytes login = build_send_login(k, "s3cret");
    CHECK(login.size() == 1 + 32 + 6 && login[0] == 26 && login[1] == 0x50 && login[32] == 0x6F && std::string(login.begin() + 33, login.end()) == "s3cret");
    CHECK(build_send_login(k, "").size() == 33);
    const Bytes logout = build_logout(k);
    CHECK(logout.size() == 33 && logout[0] == 29 && logout[32] == 0x6F);
    // LOGIN_SUCCESS, the 14 byte form of companion v1.10.0+: admin byte, prefix, the remote clock, its ACL byte, its firmware level
    Bytes ok = {0x85, 1, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55};
    put32(ok, 1790000500);
    ok.push_back(3);
    ok.push_back(2);
    auto p = parse_packet(ok);
    const auto *s = p ? std::get_if<LoginSuccess>(&*p) : nullptr;
    CHECK(s && s->permissions == 1 && s->prefix[0] == 0x50 && s->prefix[5] == 0x55 && s->has_clock && s->clock == 1790000500 && s->has_acl && s->acl == 3 &&
          s->has_fw_level && s->fw_level == 2);
    // the legacy 8 byte form: no clock, no ACL byte, no level
    p = parse_packet({0x85, 0, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55});
    s = p ? std::get_if<LoginSuccess>(&*p) : nullptr;
    CHECK(s && s->permissions == 0 && !s->has_clock && !s->has_acl && !s->has_fw_level);
    // a 12 byte one (a clock, no ACL byte yet)
    Bytes mid = {0x85, 1, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55};
    put32(mid, 77);
    p = parse_packet(mid);
    s = p ? std::get_if<LoginSuccess>(&*p) : nullptr;
    CHECK(s && s->has_clock && s->clock == 77 && !s->has_acl && !s->has_fw_level);
    CHECK(!parse_packet({0x85, 1, 0x50}));                                  // too short to be matched to a login
    p = parse_packet({0x86, 0, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55});
    const auto *f = p ? std::get_if<LoginFail>(&*p) : nullptr;
    CHECK(f && f->prefix[0] == 0x50 && f->prefix[5] == 0x55);
    CHECK(!parse_packet({0x86, 0}));
    // the wait after SENT: 1.25 x est_timeout (meshcore_py est_timeout / 800 s), bounded; 30 s when SENT gives none
    CHECK(Client::remote_wait_ms(0) == 30000);
    CHECK(Client::remote_wait_ms(1600) == 2000);
    CHECK(Client::remote_wait_ms(100) == 2000);
    CHECK(Client::remote_wait_ms(16000) == 20000);
    CHECK(Client::remote_wait_ms(500000) == 120000);
}

static PubKey contact_key(const Model &m, const std::string &name)
{
    for (const ContactRec *r : m.contacts_sorted())
        if (r->c.name == name) return r->c.key;
    return PubKey{};
}

static size_t cmd_count(const FakePeer &p, uint8_t code)
{
    size_t k = 0;
    for (const Bytes &c : p.commands) if (!c.empty() && c[0] == code) ++k;
    return k;
}

static const Bytes *last_remote_cmd(const FakePeer &p, uint8_t code)
{
    for (size_t i = p.commands.size(); i-- > 0;)
        if (!p.commands[i].empty() && p.commands[i][0] == code) return &p.commands[i];
    return nullptr;
}

static bool logged_anywhere(const Ready &r, const std::string &needle)
{
    for (const std::string &l : r.log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

/* A second repeater with a known (direct) route, for two logins at once. */
static void add_second_repeater(FakePeer &p)
{
    p.contacts.push_back(std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x90, "Ridge", 2, 1790000000, 0))));
    p.admin_pw = "Adm1n-s3cret";
}

static void test_remote_login()
{
    using RE = Client::RemoteError;
    using Phase = Client::RemoteState::Phase;
    const std::string dir = tmpdir("remote");
    PasswordStore ps(dir);
    Ready r(add_second_repeater);
    Client &c = *r.client;
    c.set_password_store(&ps);
    const PubKey rk = contact_key(r.model, "Rptr"), ridge = contact_key(r.model, "Ridge");
    const std::string rhex = to_hex(rk);
    CHECK(rk != PubKey{} && ridge != PubKey{} && c.remote_sessions().empty() && !c.remote_busy());

    // ---- admin, remembered: one SEND_LOGIN with the key and the password, SENT, then the verdict
    r.peer.commands.clear();
    uint32_t id = c.remote_result().id;
    const uint32_t req = c.start_login(rk, "Adm1n-s3cret", false, true);
    CHECK(req != 0 && c.remote_busy() && c.remote_state().phase == Phase::Sending && c.remote_state().req == req && c.remote_state().key == rk);
    run(c, 200);
    const Client::RemoteResult &res = c.remote_result();
    CHECK(!c.remote_busy() && res.id == id + 1 && res.req == req && res.op == Client::RemoteOp::Login && res.key == rk && res.ok && res.error == RE::None);
    CHECK(res.role == Client::RemoteRole::Admin && res.has_acl && res.permissions == 3 && res.has_clock && res.clock == 1790000500 && res.deck_time == g_time &&
          res.has_fw_level && res.fw_level == 2 && res.sent && res.flood);
    CHECK(res.password == Client::PasswordSave::Saved && ps.read(rhex) && *ps.read(rhex) == "Adm1n-s3cret");
    const Bytes *wire = last_remote_cmd(r.peer, cmd::kSendLogin);
    CHECK(wire && wire->size() == 33 + 12 && std::equal(rk.begin(), rk.end(), wire->begin() + 1) && std::string(wire->begin() + 33, wire->end()) == "Adm1n-s3cret");
    const Client::RemoteSession *ses = c.remote_session(rk);
    CHECK(ses && ses->role == Client::RemoteRole::Admin && ses->clock == 1790000500 && ses->fw_level == 2 && c.remote_sessions().size() == 1);
    CHECK(!logged_anywhere(r, "Adm1n") && !logged_anywhere(r, "s3cret") && logged_anywhere(r, "remote login " + rhex.substr(0, 8) + ": admin"));

    // ---- a blank password from a key already in the node's list gives the role back (the firmware rule); nothing is written
    CHECK(ps.forget(rhex) && !ps.read(rhex));
    c.start_login(rk, "typed but ignored", true, true);
    run(c, 200);
    wire = last_remote_cmd(r.peer, cmd::kSendLogin);
    CHECK(wire && wire->size() == 33);                                      // guest: the key alone, no password bytes
    CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::Admin && c.remote_result().password == Client::PasswordSave::NotAsked && !ps.read(rhex));

    // ---- guest by a blank password (the node rebooted and forgot us; its guest password is the firmware's default, empty)
    r.peer.acl.clear();
    c.start_login(rk, "", false, true);
    run(c, 200);
    CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::Guest && c.remote_result().permissions == 0 && !ps.read(rhex));
    CHECK(c.remote_session(rk) && c.remote_session(rk)->role == Client::RemoteRole::Guest);

    // ---- success without Remember: nothing written
    c.start_login(rk, "Adm1n-s3cret", false, false);
    run(c, 200);
    CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::Admin && c.remote_result().password == Client::PasswordSave::NotAsked && !ps.read(rhex));

    // ---- a wrong password: SENT, then nothing at all (the firmware is silent): Timeout after 1.25 x est_timeout; nothing written, the
    //      session table keeps what it had (a wrong password does not touch the node's list)
    id = c.remote_result().id;
    c.start_login(rk, "wrong", false, true);
    run(c, 200);
    CHECK(c.remote_state().phase == Phase::Waiting && c.remote_state().flood && c.remote_state().est_timeout_ms == 1600 &&
          c.remote_state().deadline_ms > g_now + 1500 && c.remote_state().deadline_ms <= g_now + 2000);
    CHECK(c.queued() == 0);                                                   // the wait is a timer: no command is in flight
    run(c, 1600);
    CHECK(c.remote_busy());
    run(c, 400);
    CHECK(!c.remote_busy() && c.remote_result().id == id + 1 && !c.remote_result().ok && c.remote_result().error == RE::Timeout && c.remote_result().sent &&
          c.remote_result().waited_ms >= 2000 && c.remote_result().password == Client::PasswordSave::NotAsked && !ps.read(rhex));
    CHECK(c.remote_session(rk) && c.remote_session(rk)->role == Client::RemoteRole::Admin);

    // ---- the right password, but the answer is lost on the air: Timeout as well
    r.peer.login_mode = 1;
    c.start_login(rk, "Adm1n-s3cret", false, true);
    run(c, 2400);
    CHECK(c.remote_result().error == RE::Timeout && c.remote_result().sent && !ps.read(rhex));

    // ---- a late answer (the wait is over, nothing pending): ignored
    r.peer.login_mode = 2;
    r.peer.held_login.clear();
    c.start_login(ridge, "Adm1n-s3cret", false, false);
    run(c, 2400);
    CHECK(c.remote_result().error == RE::Timeout && !c.remote_result().flood && r.peer.held_login.size() == 1);    // Ridge has a direct route
    id = c.remote_result().id;
    r.peer.push(r.peer.held_login.back());
    run(c, 200);
    CHECK(c.remote_result().id == id && !c.remote_session(ridge) && logged_anywhere(r, "ignored: no login to that node is pending"));

    // ---- a second request cancels the first one; the first one's answer is then ignored
    r.peer.held_login.clear();
    const uint32_t first = c.start_login(rk, "Adm1n-s3cret", false, false);
    run(c, 100);
    CHECK(c.remote_state().phase == Phase::Waiting && c.remote_state().req == first);
    id = c.remote_result().id;
    const uint32_t second = c.start_login(ridge, "Adm1n-s3cret", false, false);
    CHECK(second != first && c.remote_result().id == id + 1 && c.remote_result().req == first && c.remote_result().error == RE::Cancelled &&
          c.remote_result().key == rk && c.remote_state().req == second);
    run(c, 100);
    CHECK(r.peer.held_login.size() == 2 && c.remote_state().phase == Phase::Waiting);
    r.peer.push(r.peer.held_login[0]);                                       // Rptr's answer: nobody waits for it any more
    run(c, 100);
    CHECK(c.remote_result().id == id + 1 && c.remote_busy());
    r.peer.push(r.peer.held_login[1]);                                       // Ridge's
    run(c, 100);
    CHECK(c.remote_result().id == id + 2 && c.remote_result().req == second && c.remote_result().ok && c.remote_session(ridge) &&
          c.remote_session(ridge)->role == Client::RemoteRole::Admin);

    // ---- a request still in the queue when it is cancelled never goes out
    r.peer.commands.clear();
    r.peer.hold_remove = true;                                               // a command that stays in flight
    CHECK(c.remove_contacts({contact_key(r.model, "Alice")}));
    run(c, 40);
    c.start_login(rk, "Adm1n-s3cret", false, false);
    CHECK(c.remote_state().phase == Phase::Sending);
    c.cancel_remote();
    CHECK(!c.remote_busy() && c.remote_result().error == RE::Cancelled && !c.remote_result().sent);
    c.cancel_bulk();
    r.peer.hold_remove = false;
    run(c, 6000);                                                            // the held REMOVE_CONTACT times out, the queue moves on
    CHECK(cmd_count(r.peer, cmd::kSendLogin) == 0);

    // ---- Chats go on during a wait: a direct message is sent and acknowledged, a message arrives
    r.peer.held_login.clear();
    c.start_login(rk, "Adm1n-s3cret", false, false);
    run(c, 100);
    CHECK(c.remote_state().phase == Phase::Waiting);
    const std::string alice = to_hex(contact_key(r.model, "Alice"));
    const size_t before = r.model.message_count();
    const uint32_t seq = c.send_direct(alice, "during the wait");
    r.peer.messages.push_back(make_chan_msg_v3(0, "Zed: still here"));
    r.peer.push({resp::kMessagesWaiting});
    run(c, 400);
    CHECK(seq != 0 && r.model.find_message(seq) && r.model.find_message(seq)->state == MsgState::Delivered);
    CHECK(r.model.message_count() == before + 2 && c.remote_busy() && c.remote_state().phase == Phase::Waiting);
    r.peer.push(r.peer.held_login.back());
    run(c, 100);
    CHECK(c.remote_result().ok && !c.remote_busy());

    // ---- logout: the session is forgotten at once, LOGOUT (29) with the key, the board's OK ends it
    CHECK(c.remote_session(rk));
    id = c.remote_result().id;
    const uint32_t lo = c.logout(rk);
    CHECK(lo != 0 && !c.remote_session(rk) && c.remote_session(ridge));
    run(c, 100);
    CHECK(c.remote_result().id == id + 1 && c.remote_result().op == Client::RemoteOp::Logout && c.remote_result().req == lo && c.remote_result().ok &&
          r.peer.logouts == 1);
    wire = last_remote_cmd(r.peer, cmd::kLogout);
    CHECK(wire && wire->size() == 33 && std::equal(rk.begin(), rk.end(), wire->begin() + 1));
    // a logout cancels a pending login to the same node (not one to another node)
    c.start_login(rk, "Adm1n-s3cret", false, false);
    run(c, 100);
    id = c.remote_result().id;
    c.logout(ridge);
    CHECK(c.remote_busy() && !c.remote_session(ridge));
    c.logout(rk);
    CHECK(!c.remote_busy() && c.remote_result().id == id + 1 && c.remote_result().error == RE::Cancelled);
    run(c, 100);
    CHECK(c.remote_result().op == Client::RemoteOp::Logout && c.remote_result().key == rk && c.remote_result().ok && c.remote_sessions().empty());

    // ---- refused at once: a password the firmware would cut (16 bytes), a NUL in it
    r.peer.commands.clear();
    CHECK(c.start_login(rk, std::string(16, 'x'), false, true) == 0 && c.remote_result().error == RE::BadPassword && c.remote_result().req == 0);
    CHECK(c.start_login(rk, std::string("ab\0cd", 5), false, true) == 0 && c.remote_result().error == RE::BadPassword);
    CHECK(c.start_login(rk, std::string(15, 'x'), false, false) != 0);       // 15 bytes is the most
    c.cancel_remote();
    run(c, 100);
    CHECK(cmd_count(r.peer, cmd::kSendLogin) == 1);

    // ---- the board's errors: not a contact, nothing could be sent, a firmware without the command
    PubKey stranger{};
    stranger.fill(0x77);
    c.start_login(stranger, "x", false, true);
    run(c, 100);
    CHECK(c.remote_result().error == RE::NotFound && c.remote_result().err_code == err::kNotFound && !c.remote_result().sent && !ps.read(to_hex(stranger)));
    r.peer.login_table_full = true;
    c.start_login(rk, "x", false, true);
    run(c, 100);
    CHECK(c.remote_result().error == RE::TableFull);
    r.peer.login_table_full = false;
    r.peer.login_supported = false;
    c.start_login(rk, "x", false, true);
    run(c, 100);
    CHECK(c.remote_result().error == RE::Unsupported && c.remote_result().err_code == err::kUnsupported);
    c.logout(rk);
    run(c, 100);
    CHECK(c.remote_result().op == Client::RemoteOp::Logout && !c.remote_result().ok && c.remote_result().error == RE::Unsupported);
    r.peer.login_supported = true;

    // ---- legacy nodes: an 8 byte success (the admin bit only), LOGIN_FAIL for a wrong password
    r.peer.login_mode = 3;
    r.peer.acl.clear();
    c.start_login(rk, "Adm1n-s3cret", false, false);
    run(c, 100);
    CHECK(c.remote_result().ok && c.remote_result().role == Client::RemoteRole::Admin && !c.remote_result().has_clock && !c.remote_result().has_fw_level &&
          !c.remote_result().has_acl && c.remote_result().permissions == 1);
    r.peer.login_mode = 4;
    c.start_login(rk, "wrong", false, true);
    run(c, 100);
    CHECK(!c.remote_result().ok && c.remote_result().error == RE::Refused && c.remote_result().sent && !ps.read(rhex));

    // ---- SENT without an estimate: the default wait (30 s)
    r.peer.login_mode = 2;
    r.peer.login_est = 0;
    c.start_login(rk, "Adm1n-s3cret", false, false);
    run(c, 100);
    CHECK(c.remote_state().phase == Phase::Waiting && c.remote_state().deadline_ms > g_now + 29000);
    run(c, 3000);
    CHECK(c.remote_busy());
    run(c, 27100);
    CHECK(!c.remote_busy() && c.remote_result().error == RE::Timeout);
    r.peer.login_est = 1600;
}

static void test_remote_relay_and_link()
{
    using RE = Client::RemoteError;
    Ready r(add_second_repeater);
    Client &c = *r.client;
    const PubKey rk = contact_key(r.model, "Rptr");
    r.peer.admin_pw = "pw";

    // ---- a CLI session: start_relay() cancels the pending wait, its late answer is ignored, nothing remote starts during the session
    c.start_login(rk, "pw", false, false);
    run(c, 100);
    CHECK(c.remote_session(rk));
    r.peer.login_mode = 2;
    r.peer.held_login.clear();
    c.start_login(rk, "pw", false, false);
    run(c, 100);
    CHECK(c.remote_state().phase == Client::RemoteState::Phase::Waiting);
    uint32_t id = c.remote_result().id;
    const int port = c.start_relay();
    CHECK(port > 0 && c.relay_active() && !c.remote_busy() && c.remote_result().id == id + 1 && c.remote_result().error == RE::Cancelled);
    r.peer.push(r.peer.held_login.back());                                   // the answer reaches the console; Mesh Hop ignores it
    run(c, 100);
    CHECK(c.remote_result().id == id + 1);
    r.peer.commands.clear();
    CHECK(c.start_login(rk, "pw", false, false) == 0 && c.remote_result().error == RE::Busy && c.remote_result().op == Client::RemoteOp::Login);
    CHECK(c.logout(rk) == 0 && c.remote_result().error == RE::Busy && c.remote_result().op == Client::RemoteOp::Logout);
    CHECK(c.remote_session(rk));                                             // a refused logout forgets nothing; a CLI session keeps the table
    c.stop_relay();
    run(c, 200);
    CHECK(!c.relay_active() && cmd_count(r.peer, cmd::kSendLogin) == 0 && c.remote_session(rk));

    // ---- a lost link: the wait ends (LinkLost) and the session table is cleared; it stays empty after the reconnect
    c.start_login(rk, "pw", false, false);
    run(c, 100);
    CHECK(c.remote_busy());
    id = c.remote_result().id;
    r.peer.lost = true;
    run(c, 100);
    CHECK(c.link() == Link::Searching && !c.remote_busy() && c.remote_result().id == id + 1 && c.remote_result().error == RE::LinkLost &&
          c.remote_result().sent && c.remote_sessions().empty());
    CHECK(c.start_login(rk, "pw", false, false) == 0 && c.remote_result().error == RE::NotConnected);
    // a login whose command was not answered yet when the link went
    r.peer.lost = false;
    run(c, 4000);
    CHECK(c.ready() && c.remote_sessions().empty());
    r.peer.silent = true;
    c.start_login(rk, "pw", false, false);
    run(c, 100);
    CHECK(c.remote_state().phase == Client::RemoteState::Phase::Sending);
    r.peer.lost = true;
    run(c, 100);
    CHECK(!c.remote_busy() && c.remote_result().error == RE::LinkLost && !c.remote_result().sent);
    r.peer.silent = false;
    r.peer.lost = false;
    run(c, 4000);
    // the board never confirms the send: Timeout with sent = false
    CHECK(c.ready());
    r.peer.silent = true;
    c.start_login(rk, "pw", false, false);
    run(c, 5200);
    CHECK(!c.remote_busy() && c.remote_result().error == RE::Timeout && !c.remote_result().sent);
    r.peer.silent = false;
}

static void test_passwords()
{
    const std::string dir = tmpdir("passwords");
    PasswordStore ps(dir + "/data");                                     // the data folder does not exist yet: save() makes it
    const std::string key = "AABBCCDDEEFF00112233445566778899aabbccddeeff00112233445566778899";
    const std::string low = "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899";
    // keys: 64 hex digits, named in lower case like meshcore_py's public_key
    CHECK(PasswordStore::normalize_key(key) == low && PasswordStore::normalize_key(low) == low);
    CHECK(PasswordStore::normalize_key(low.substr(1)).empty() && PasswordStore::normalize_key(low + "0").empty() &&
          PasswordStore::normalize_key("zz" + low.substr(2)).empty() && PasswordStore::normalize_key("").empty());
    CHECK(ps.dir() == dir + "/data/cli/.config/meshcore" && ps.path(key) == ps.dir() + "/" + low + ".pass" && ps.path("abc").empty());
    CHECK(!ps.read(key) && !ps.remembered(key));
    CHECK(ps.save("abc", "pw") == PasswordStore::SaveResult::BadKey && !ps.read("abc"));
    // nothing is written for a blank password or one meshcore-cli would read back differently
    CHECK(ps.save(key, "") == PasswordStore::SaveResult::Blank);
    CHECK(ps.save(key, " pw") == PasswordStore::SaveResult::NotStorable && ps.save(key, "pw ") == PasswordStore::SaveResult::NotStorable &&
          ps.save(key, "p\nw") == PasswordStore::SaveResult::NotStorable && ps.save(key, std::string("p\0w", 3)) == PasswordStore::SaveResult::NotStorable);
    struct stat st;
    CHECK(::stat(ps.dir().c_str(), &st) != 0);                             // none of the refusals made a folder
    // round trip: the file holds the password alone, no line end (meshcore-cli's f.write(password)), mode 0600 in 0700 folders
    CHECK(ps.save(key, "Pa ss-w0rd") == PasswordStore::SaveResult::Saved);
    CHECK(ps.read(low) && *ps.read(low) == "Pa ss-w0rd" && ps.remembered(key));
    CHECK(read_text(ps.path(key)) == "Pa ss-w0rd");
    CHECK(::stat(ps.path(key).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    for (const std::string &d : {dir + "/data/cli", dir + "/data/cli/.config", ps.dir()})
        CHECK(::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
    // a folder made wider by hand is set back to 0700 by the next save (as mccli does at every start)
    CHECK(::chmod(ps.dir().c_str(), 0755) == 0);
    CHECK(ps.save(key, "second") == PasswordStore::SaveResult::Saved && *ps.read(key) == "second");
    CHECK(::stat(ps.dir().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    // no temporary file is left behind
    int entries = 0;
    if (DIR *d = ::opendir(ps.dir().c_str())) {
        while (const dirent *e = ::readdir(d))
            if (std::string(e->d_name) != "." && std::string(e->d_name) != "..") ++entries;
        ::closedir(d);
    }
    CHECK(entries == 1);
    // read like meshcore-cli (readline().strip()): the first line without the spaces around it; a blank first line is no password
    write_text(ps.path(key), "  typed in the CLI \nsecond line\n");
    CHECK(ps.read(key) && *ps.read(key) == "typed in the CLI");
    write_text(ps.path(key), "\nlater\n");
    CHECK(!ps.read(key));
    // forget: the file goes (meshcore-cli's fp); forgetting twice is fine; a bad key is not
    CHECK(ps.save(key, "third") == PasswordStore::SaveResult::Saved);
    CHECK(ps.forget(low) && !ps.read(key) && ::stat(ps.path(key).c_str(), &st) != 0);
    CHECK(ps.forget(key) && !ps.forget("nope"));
    // a data folder that cannot be made: an I/O error, nothing written
    write_text(dir + "/afile", "x");
    PasswordStore bad(dir + "/afile/data");
    CHECK(bad.save(key, "pw") == PasswordStore::SaveResult::IoError && !bad.read(key));
}

/* 0.7.2: Reset path (RESET_PATH 13 with the contact's key): OK -> the model's route is flood at once and the record is read back;
 * not found, busy (a reset on its way, a CLI session), not connected, no answer. */
static void test_reset_path()
{
    using PE = Client::PathResetError;
    Ready r(add_second_repeater);
    Client &c = *r.client;
    const PubKey ridge = contact_key(r.model, "Ridge");
    const std::string hex = to_hex(ridge);
    CHECK(ridge != PubKey{} && r.model.find_contact(hex) && r.model.find_contact(hex)->c.out_path_len == 0);   // direct
    r.peer.commands.clear();
    const uint32_t id = c.path_reset_result().id;
    CHECK(c.reset_path(ridge) && c.path_reset_busy());
    CHECK(!c.reset_path(ridge) && c.path_reset_result().id == id + 1 && c.path_reset_result().error == PE::Busy);   // one at a time
    run(c, 300);
    const Client::PathResetResult &res = c.path_reset_result();
    CHECK(!c.path_reset_busy() && res.id == id + 2 && res.ok && res.error == PE::None && res.key == ridge);
    const Bytes *wire = last_remote_cmd(r.peer, cmd::kResetPath);
    CHECK(wire && wire->size() == 33 && std::equal(ridge.begin(), ridge.end(), wire->begin() + 1));
    CHECK(cmd_count(r.peer, cmd::kGetContactByKey) == 1);                                 // read back
    CHECK(r.model.find_contact(hex)->c.out_path_len == kPathFlood && path_hops(r.model.find_contact(hex)->c.out_path_len) < 0);
    CHECK(r.logged("reset path " + hex.substr(0, 8)));
    // a key the board does not hold: not found, the model untouched
    PubKey stranger{};
    stranger.fill(0x77);
    CHECK(c.reset_path(stranger));
    run(c, 300);
    CHECK(!c.path_reset_result().ok && c.path_reset_result().error == PE::NotFound && c.path_reset_result().err_code == err::kNotFound);
    // no answer: Timeout after the command's 5 s
    r.peer.reset_path_silent = true;
    CHECK(c.reset_path(ridge));
    run(c, 4000);
    CHECK(c.path_reset_busy());
    run(c, 1500);
    CHECK(!c.path_reset_busy() && c.path_reset_result().error == PE::Timeout);
    r.peer.reset_path_silent = false;
    // a CLI session holds the node: refused at once
    CHECK(c.start_relay() > 0);
    CHECK(!c.reset_path(ridge) && c.path_reset_result().error == PE::Busy);
    c.stop_relay();
    run(c, 5000);
    // not connected: refused at once
    Ready off(nullptr, false);
    CHECK(!off.client->reset_path(ridge) && off.client->path_reset_result().error == PE::NotConnected && off.client->path_reset_result().id == 1);
}
