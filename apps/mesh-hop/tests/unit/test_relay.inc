// Report 033 (Mesh Hop CLI), phases P1 and P3: a CLI reply never becomes a chat message; the loopback relay a meshcore-cli
// session uses to share the node (Client::start_relay, RelayServer). Included by test_core.cpp (FakePeer, Ready, run()).
// The console side is a real TCP client on 127.0.0.1 written here, framed like meshcore_py's tcp_cx.py.

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/* A contact message V3 with any text type (2 = signed: 4 signature bytes before the text, as the firmware queues it). */
static Bytes make_dm_v3_typed(uint8_t key_seed, const std::string &text, uint8_t txt_type, uint32_t ts)
{
    Bytes b = {resp::kContactMsgV3, 0x14, 0, 0};
    for (int i = 0; i < 6; ++i) b.push_back(static_cast<uint8_t>(key_seed + i));
    b.push_back(2);
    b.push_back(txt_type);
    put32(b, ts);
    if (txt_type == txttype::kSignedPlain) for (uint8_t s : {0xDE, 0xAD, 0xBE, 0xEF}) b.push_back(s);
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

static void test_cli_reply_not_a_message()
{
    Ready r;
    run(*r.client, 500);
    const size_t before = r.model.message_count();
    const int unread = r.model.unread_total();
    // a repeater's answer to a remote CLI command (text type 1), V3 and the old frame: nothing in Chats, no unread
    r.peer.messages.push_back(make_dm_v3_typed(0x50, "> Hilltop repeater", txttype::kCliData, 1790000100));
    Bytes old = {resp::kContactMsg};
    for (int i = 0; i < 6; ++i) old.push_back(static_cast<uint8_t>(0x50 + i));
    old.push_back(0xFF);
    old.push_back(txttype::kCliData);
    put32(old, 1790000101);
    const std::string ver = "v1.15.0 (Build: 19-Apr-2026)";
    old.insert(old.end(), ver.begin(), ver.end());
    r.peer.messages.push_back(old);
    r.peer.push({resp::kMessagesWaiting});
    run(*r.client, 500);
    CHECK(r.peer.messages.empty());                                   // both were pulled off the node
    CHECK(r.model.message_count() == before && r.model.unread_total() == unread);
    for (const Message &m : r.model.all_messages()) CHECK(m.text.find("Hilltop") == std::string::npos && m.text != ver);
    // a signed message (type 2, a room server's post) and a plain one stay messages; the signature is not part of the text
    r.peer.messages.push_back(make_dm_v3_typed(0x50, "room post", txttype::kSignedPlain, 1790000102));
    r.peer.messages.push_back(make_dm_v3_typed(0x10, "plain hello", txttype::kPlain, 1790000103));
    r.peer.push({resp::kMessagesWaiting});
    run(*r.client, 500);
    CHECK(r.model.message_count() == before + 2 && r.model.unread_total() == unread + 2);
    CHECK(r.model.all_messages().back().text == "plain hello");
    CHECK(r.model.all_messages()[r.model.all_messages().size() - 2].text == "room post");
}

/* ------------------------------------------------------------------ the relay */

/* The console: what meshcore_py's TCPConnection does on the wire ('<' frames out, '>' frames in). */
struct Console {
    int fd = -1;
    FrameParser parser;
    std::vector<Bytes> got;
    bool eof = false;

    bool connect_to(int port)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));   // as asyncio does (meshcore_py): no Nagle delay
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) { ::close(fd); fd = -1; return false; }
        return true;
    }
    void send(const Bytes &payload, size_t junk = 0)
    {
        Bytes f(junk, 'x');                                            // bytes that are not a frame (skipped by the relay)
        f.push_back(0x3C);
        f.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
        f.push_back(static_cast<uint8_t>(payload.size() >> 8));
        f.insert(f.end(), payload.begin(), payload.end());
        // one byte at a time for the first frame bytes: the relay must put a frame together from any chunking
        if (::send(fd, f.data(), 2, MSG_NOSIGNAL) < 0 || ::send(fd, f.data() + 2, f.size() - 2, MSG_NOSIGNAL) < 0) eof = true;
    }
    void drain()
    {
        uint8_t buf[2048];
        for (;;) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) { parser.feed(buf, static_cast<size_t>(n), got); continue; }
            if (n == 0) eof = true;
            break;
        }
    }
    size_t count(uint8_t code) const
    {
        size_t k = 0;
        for (const Bytes &f : got) if (!f.empty() && f[0] == code) ++k;
        return k;
    }
    void close() { if (fd >= 0) ::close(fd); fd = -1; }
    ~Console() { close(); }
};

static size_t count_cmd(const FakePeer &p, uint8_t code, size_t from = 0)
{
    size_t k = 0;
    for (size_t i = from; i < p.commands.size(); ++i) if (p.commands[i][0] == code) ++k;
    return k;
}

static const Bytes kCliAppStart = {cmd::kAppStart, 0x03, ' ', ' ', ' ', ' ', ' ', ' ', 'm', 'c', 'c', 'l', 'i'};   // meshcore_py send_appstart

static void test_relay_server()
{
    RelayServer s;
    std::vector<Bytes> frames;
    CHECK(s.poll(frames) == RelayServer::Io::Idle && !s.listening());
    const int port = s.open();
    CHECK(port > 0 && s.listening() && !s.connected() && s.port() == port);
    CHECK(s.poll(frames) == RelayServer::Io::Idle);
    Console c;
    CHECK(c.connect_to(port));
    CHECK(s.poll(frames) == RelayServer::Io::Ok && s.connected() && !s.listening());     // accepted: the listener is closed
    Console second;
    CHECK(!second.connect_to(port));                                                       // one client per session
    c.send({cmd::kDeviceQuery, 3}, 5);
    c.send(Bytes(250, 0x42));                                                              // longer than Mesh Hop's own frames: passed as it is
    CHECK(s.poll(frames) == RelayServer::Io::Ok && frames.size() == 2);
    CHECK(frames.size() == 2 && frames[0] == (Bytes{cmd::kDeviceQuery, 3}) && frames[1].size() == 250 && s.bad_bytes() == 5);
    s.send({resp::kOk});
    s.send(make_self_info("Deck Zero"));
    frames.clear();
    CHECK(s.poll(frames) == RelayServer::Io::Ok && frames.empty() && s.pending_out() == 0);
    c.drain();
    CHECK(c.got.size() == 2 && c.got[0] == (Bytes{resp::kOk}) && c.got[1] == make_self_info("Deck Zero"));
    c.close();
    CHECK(s.poll(frames) == RelayServer::Io::Gone && !s.connected());
    CHECK(s.poll(frames) == RelayServer::Io::Idle);
    s.close();
    CHECK(s.port() == 0);
}

static void test_relay()
{
    {   // refused without a node, and a second session while one runs
        Ready off(nullptr, false);
        CHECK(off.client->start_relay() == 0 && !off.client->relay_active() && off.client->notice().text == "Not connected to a radio");
    }
    Ready r;
    Client &c = *r.client;
    run(c, 500);
    CHECK(c.queued() == 0);
    const std::string alice = to_hex(r.model.contacts_sorted()[0]->c.key);
    // a direct message waiting for its ACK when the session starts (its retry timer must stop with the session)
    r.peer.drop_acks = true;
    const uint32_t dm = c.send_direct(alice, "before the console");
    run(c, 200);
    CHECK(r.model.find_message(dm)->state == MsgState::Sent);
    const size_t contacts = r.model.contact_count();

    const int port = c.start_relay();
    CHECK(port > 0 && c.relay_active() && c.relay_ready() && c.relay_port() == port && c.relay_state() == Client::RelayState::Passthrough);
    CHECK(c.start_relay() == 0 && c.notice().text.find("already") != std::string::npos);
    const size_t mark = r.peer.commands.size();
    // Mesh Hop's own sends wait: a chat typed now, the 60 s battery poll, the sync after MESSAGES_WAITING
    const uint32_t held = c.send_direct(alice, "typed during the session");
    CHECK(held != 0 && c.queued() == 1);
    const size_t msgs = r.model.message_count();
    const int unread = r.model.unread_total();

    Console con;
    CHECK(con.connect_to(port));
    run(c, 40);
    CHECK(c.relay_client_connected());
    // what meshcore-cli sends at connect, answered by the node itself (no faked answers); the node renamed by the console
    r.peer.self.name = "Renamed by CLI";
    con.send(kCliAppStart);
    run(c, 40);
    con.send({cmd::kDeviceQuery, 3});
    run(c, 40);
    con.drain();
    CHECK(con.got.size() == 2 && con.count(resp::kSelfInfo) == 1 && con.count(resp::kDeviceInfo) == 1);
    CHECK(r.model.self()->name == "Deck Zero");                       // the console's answers are not applied
    // its contact list (the node now has one contact): not applied either
    r.peer.contacts.pop_back();
    Bytes since = {cmd::kGetContacts};
    put32(since, 1790000000);
    con.send(since);
    con.send({cmd::kSetFloodScope, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    con.send(Bytes{0x7E} + Bytes(220, 7));                            // a command Mesh Hop does not know, longer than its own frames
    run(c, 60);
    con.drain();
    CHECK(con.count(resp::kContactStart) == 1 && con.count(resp::kContactEnd) == 1 && con.count(0x88) >= 1);
    CHECK(r.model.contact_count() == contacts);
    CHECK(count_cmd(r.peer, 0x7E, mark) == 1 && r.peer.commands.back().size() == 221);

    // messages: the node tickles, the CONSOLE fetches, Mesh Hop reads along (Chats keep working; the CLI reply stays out)
    r.peer.messages.push_back(make_dm_v3_typed(0x10, "hello during the session", txttype::kPlain, 1790000200));
    r.peer.messages.push_back(make_dm_v3_typed(0x50, "> Hilltop", txttype::kCliData, 1790000201));
    r.peer.push({resp::kMessagesWaiting});
    run(c, 100);
    con.drain();
    CHECK(con.count(resp::kMessagesWaiting) == 1);
    CHECK(count_cmd(r.peer, cmd::kSyncNext, mark) == 0);              // Mesh Hop did not fetch
    for (int i = 0; i < 3; ++i) { con.send({cmd::kSyncNext}); run(c, 40); }
    con.drain();
    CHECK(con.count(resp::kContactMsgV3) == 2 && con.count(resp::kNoMoreMsgs) == 1);
    CHECK(r.model.message_count() == msgs + 1 && r.model.unread_total() == unread + 1);
    CHECK(r.model.all_messages().back().text == "hello during the session");
    // pushes act on the model: the ACK of the message sent before the session; an advert does not make Mesh Hop ask for the contact
    r.peer.push({resp::kAck, 0xA0, 0xB0, 0xC0, 0xD0, 5, 0, 0, 0});
    PubKey k{};
    for (int i = 0; i < 32; ++i) k[static_cast<size_t>(i)] = static_cast<uint8_t>(0x10 + i);
    r.peer.push(Bytes{resp::kAdvertisement} + Bytes(k.begin(), k.end()));
    run(c, 100);
    con.drain();
    CHECK(r.model.find_message(dm)->state == MsgState::Delivered);
    CHECK(con.count(resp::kAck) == 1 && con.count(resp::kAdvertisement) == 1);
    CHECK(count_cmd(r.peer, cmd::kGetContactByKey, mark) == 0 && count_cmd(r.peer, cmd::kSendTxt, mark) == 0 && count_cmd(r.peer, cmd::kBattery, mark) == 0);
    run(c, 61000);                                                     // a long session: still nothing of Mesh Hop's own
    CHECK(r.peer.commands.size() == mark + 8 && c.relay_active() && c.queued() == 1);

    // the page leaves: the console is cut off, the node's late answer to it is not taken for Mesh Hop's, then Mesh Hop resumes
    const uint32_t ended = c.relay_sessions_ended();
    c.stop_relay();
    CHECK(c.relay_state() == Client::RelayState::Settling && !c.relay_client_connected());
    con.drain();
    CHECK(con.eof);
    r.peer.push({resp::kError, err::kNotFound});                      // the answer to the console's last command, late
    run(c, 300);
    CHECK(c.relay_state() == Client::RelayState::Settling && count_cmd(r.peer, cmd::kSendTxt, mark) == 0);
    run(c, 1000);
    CHECK(!c.relay_active() && c.relay_sessions_ended() == ended + 1 && c.relay_end_reason() == "closed by Mesh Hop");
    CHECK(c.relay_frames_to_node() == 8 && c.relay_frames_to_console() >= 12);
    run(c, 1000);
    CHECK(r.model.find_message(held)->state == MsgState::Sent);       // the held chat went out (no ACK: drop_acks), not failed by the late error
    // the node read again: the scope the console set is reset, the protocol level, self info, contacts, channels
    size_t scope_resets = 0;
    for (size_t i = mark; i < r.peer.commands.size(); ++i)
        if (r.peer.commands[i] == build_reset_flood_scope()) ++scope_resets;
    CHECK(scope_resets == 1);
    CHECK(count_cmd(r.peer, cmd::kDeviceQuery, mark) == 2 && count_cmd(r.peer, cmd::kAppStart, mark) == 2 && count_cmd(r.peer, cmd::kGetContacts, mark) == 2);
    CHECK(count_cmd(r.peer, cmd::kGetChannel, mark) >= 8 && count_cmd(r.peer, cmd::kSyncNext, mark) >= 1);
    CHECK(r.model.self()->name == "Renamed by CLI" && r.model.contact_count() == contacts - 1);
    CHECK(r.logged("console relay: session ended (closed by Mesh Hop)"));
    for (const std::string &l : r.log) CHECK(l.find("hello during the session") == std::string::npos && l.find("Hilltop") == std::string::npos);
    // the retry timer of a message sent before the session started again where it stopped: no retry at once
    const uint32_t dm2 = c.send_direct(alice, "waits for an ack");
    run(c, 200);
    const size_t m2 = r.peer.commands.size();
    CHECK(c.start_relay() > 0);
    run(c, 8000);
    c.stop_relay();
    run(c, 100);                                                       // nothing reached the node: no settling, no re-read
    CHECK(!c.relay_active() && c.relay_end_reason() == "closed by Mesh Hop" && count_cmd(r.peer, cmd::kDeviceQuery, m2) == 0);
    CHECK(r.model.find_message(dm2)->state == MsgState::Sent && count_cmd(r.peer, cmd::kSendTxt, m2) == 0);
    auto resent = [&] {
        size_t k = 0;
        const std::string t = "waits for an ack";
        for (size_t i = m2; i < r.peer.commands.size(); ++i)
            if (r.peer.commands[i][0] == cmd::kSendTxt && std::string(r.peer.commands[i].begin() + 13, r.peer.commands[i].end()) == t) ++k;
        return k;
    };
    run(c, 2000);
    CHECK(resent() == 0);                                              // about 2.8 s of its 3 s were left when the session started
    run(c, 1500);
    CHECK(resent() == 1);                                              // its retry
    r.peer.drop_acks = false;

    {   // a command in flight when the session starts: its answer stays Mesh Hop's, the console's first frame waits (Draining)
        Ready d;
        run(*d.client, 500);
        CHECK(d.client->refresh_contacts());                           // written at once; the answer is read at the next poll
        const int p = d.client->start_relay();
        CHECK(p > 0 && d.client->relay_state() == Client::RelayState::Draining && !d.client->relay_ready());
        Console k2;
        CHECK(k2.connect_to(p));
        k2.send(kCliAppStart);
        const size_t before = d.peer.commands.size();
        run(*d.client, 20);
        CHECK(d.client->relay_ready());
        run(*d.client, 40);
        k2.drain();
        CHECK(k2.got.size() == 1 && k2.count(resp::kSelfInfo) == 1);   // no contact frame of Mesh Hop's command reached the console
        CHECK(d.peer.commands.size() == before + 1 && d.peer.commands.back() == kCliAppStart);
        // the console quits by itself
        k2.close();
        run(*d.client, 100);
        CHECK(d.client->relay_state() == Client::RelayState::Settling);
        run(*d.client, 1000);
        CHECK(!d.client->relay_active() && d.client->relay_end_reason() == "the console closed its connection");
        CHECK(count_cmd(d.peer, cmd::kSetFloodScope, before) == 0);     // no scope set: none reset
    }
    {   // the console never connects
        Ready n;
        run(*n.client, 500);
        CHECK(n.client->start_relay() > 0);
        run(*n.client, Client::kRelayAcceptTimeoutMs + 500, 100);
        CHECK(!n.client->relay_active() && n.client->relay_end_reason() == "the console did not connect");
    }
    {   // over Bluetooth LE (BleTransport over the fake BlueZ with the scripted companion behind, frames cut in 20 byte notifications):
        // each console command is one bare GATT write, the node's frames come back whole
        set_time_source(fake_time);
        Model m;
        auto owned = std::make_unique<PeerBle>();
        PeerBle &be = *owned;
        be.mtu = 23;
        be.cut_at = 20;
        be.add_device(kAddrA, "MeshCore-OSR T1000", true);
        bt::BleTransport t(std::move(owned), kAddrA, "", [] { return g_now; });
        Client bc(t, m);
        bc.set_deck_clock([] { return DeckClock::Synced; }, [] {});
        auto tick = [&](int n) {
            for (int i = 0; i < n; ++i) {
                g_now += 20;
                t.poll();
                bc.poll(g_now);
            }
        };
        for (int i = 0; i < 1500 && bc.link() != Link::Ready; ++i) tick(1);
        tick(150);
        CHECK(bc.link() == Link::Ready && bc.queued() == 0 && m.contact_count() == 2);
        const int p = bc.start_relay();
        Console kb;
        CHECK(p > 0 && kb.connect_to(p));
        tick(2);
        const size_t w0 = be.writes.size();
        kb.send(kCliAppStart);
        tick(5);
        kb.send({cmd::kGetContacts});
        tick(10);
        kb.drain();
        CHECK(kb.count(resp::kSelfInfo) == 1 && kb.count(resp::kContactStart) == 1 && kb.count(resp::kContact) == 2 && kb.count(resp::kContactEnd) == 1);
        CHECK(be.writes.size() == w0 + 2 && be.writes[w0] == kCliAppStart && be.writes[w0 + 1] == Bytes{cmd::kGetContacts});
        kb.close();
        tick(60);
        CHECK(!bc.relay_active() && bc.relay_end_reason() == "the console closed its connection" && bc.link() == Link::Ready);
        CHECK(be.writes.size() > w0 + 4 && be.writes[w0 + 2] == build_device_query());           // Mesh Hop reads the node again
        t.shutdown();
    }
    {   // the link to the node is lost during a session: it ends at once; the scope the console set is reset after the reconnect
        Ready l;
        run(*l.client, 500);
        const int p = l.client->start_relay();
        Console k3;
        CHECK(p > 0 && k3.connect_to(p));
        run(*l.client, 40);
        k3.send({cmd::kSetFloodScope, 0, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9});
        run(*l.client, 40);
        l.peer.lost = true;
        run(*l.client, 100);
        CHECK(l.client->link() == Link::Searching && !l.client->relay_active() && l.client->relay_end_reason() == "the link to the node was lost");
        k3.drain();
        CHECK(k3.eof);
        l.peer.lost = false;
        const size_t before = l.peer.commands.size();
        run(*l.client, 4500);
        CHECK(l.client->ready() && count_cmd(l.peer, cmd::kSetFloodScope, before) == 1);
    }
}
