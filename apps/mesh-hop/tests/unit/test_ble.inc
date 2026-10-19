// Bluetooth LE link (src/core/ble): framing, the BleLink state machine over FakeBle, BleTransport, and the Client over
// Bluetooth against the scripted companion (FakePeer). Included by test_core.cpp (its CHECK, FakePeer and helpers).

#include "ble/ble_framing.hpp"
#include "ble/ble_link.hpp"
#include "ble/ble_transport.hpp"
#include "ble/fake_ble.hpp"

namespace bt = meshzero::ble;

static const char *kAddrA = "EC:EB:45:39:28:2D";      // the deck's scan of 2026-10-19 saw these two MeshCore boards
static const char *kAddrB = "D7:C5:07:5B:55:EB";

static Bytes seq_bytes(size_t n, uint8_t first)
{
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(first + i);
    return b;
}

/* cut a frame as the nRF52 Bluefruit library does when the MTU is small: MTU - 3 bytes per notification */
static std::vector<Bytes> cut(const Bytes &f, size_t chunk)
{
    std::vector<Bytes> out;
    for (size_t i = 0; i < f.size(); i += chunk)
        out.emplace_back(f.begin() + static_cast<long>(i), f.begin() + static_cast<long>(std::min(f.size(), i + chunk)));
    return out;
}

static void test_ble_framing()
{
    // the receive side keeps the Client's serial parser: '>' + u16 length + payload
    const Bytes contact = make_contact(resp::kContact, 0x30, "Alice", 1, 1790000000);
    const Bytes w = bt::wrap_from_device(contact);
    CHECK(w.size() == contact.size() + 3 && w[0] == 0x3E && w[1] == (contact.size() & 0xFF) && w[2] == 0);
    FrameParser p;
    std::vector<Bytes> got;
    p.feed(w.data(), w.size(), got);
    CHECK(got.size() == 1 && got[0] == contact);

    // the send side: the Client's '<' frames, in any chunking, back to payloads; junk and impossible lengths skipped
    const Bytes q = build_device_query(), a = build_app_start("mccli");
    Bytes wire = {0x00, 0x3C, 0x00, 0x00};                    // junk, then a zero-length header
    for (const Bytes &f : {encode_frame(q), encode_frame(a)}) wire.insert(wire.end(), f.begin(), f.end());
    bt::ToDeviceUnframer u;
    std::vector<Bytes> sent;
    for (uint8_t b : wire) u.feed(&b, 1, sent);
    CHECK(sent.size() == 2 && sent[0] == q && sent[1] == a);
    CHECK(u.bad() >= 2 && u.pending() == 0);

    // one frame is one write, whatever the MTU: no cutting by the app (the firmware reads each write as a frame)
    CHECK(bt::plan_write(20, 23) == bt::WritePlan::Single);
    CHECK(bt::plan_write(21, 23) == bt::WritePlan::Long);
    CHECK(bt::plan_write(172, 176) == bt::WritePlan::Single);  // esp32: setMTU(MAX_FRAME_SIZE)
    CHECK(bt::plan_write(174, 176) == bt::WritePlan::Long);
    CHECK(bt::plan_write(176, 247) == bt::WritePlan::Single);  // nrf52: 247
    CHECK(bt::plan_write(177, 517) == bt::WritePlan::TooBig);
    CHECK(bt::plan_write(0, 247) == bt::WritePlan::TooBig);
    CHECK(bt::plan_write(20, 0) == bt::WritePlan::Single);     // no MTU known: the ATT default 23

    // MTU 247: every notification is a frame at once (no frame of the firmware can be cut)
    {
        bt::NotifyAssembler n(247);
        CHECK(!n.reassembles());
        std::vector<Bytes> f;
        const Bytes big = seq_bytes(176, 1), small = {0x83};
        n.feed(big.data(), big.size(), 0, f);
        n.feed(small.data(), small.size(), 0, f);
        CHECK(f.size() == 2 && f[0] == big && f[1] == small && n.held() == 0);
    }
    // MTU 23: a 148 byte contact in 20 byte pieces comes back whole
    {
        bt::NotifyAssembler n(23);
        CHECK(n.reassembles() && n.chunk() == 20);
        std::vector<Bytes> f;
        const std::vector<Bytes> parts = cut(contact, 20);
        CHECK(parts.size() == 8);
        for (const Bytes &x : parts) n.feed(x.data(), x.size(), 100, f);
        CHECK(f.size() == 1 && f[0] == contact && n.joined() == 1);
        // two short frames stay two frames
        const Bytes s1 = {0x00}, s2 = seq_bytes(12, 9);
        n.feed(s1.data(), s1.size(), 200, f);
        n.feed(s2.data(), s2.size(), 200, f);
        CHECK(f.size() == 3 && f[1] == s1 && f[2] == s2);
        // a frame of exactly two pieces ends when no third one comes within the hold time
        const Bytes forty = seq_bytes(40, 50);
        for (const Bytes &x : cut(forty, 20)) n.feed(x.data(), x.size(), 300, f);
        CHECK(f.size() == 3 && n.held() == 40);
        n.tick(349, f);
        CHECK(f.size() == 3);
        n.tick(350, f);
        CHECK(f.size() == 4 && f[3] == forty && n.held() == 0);
        // never longer than the firmware's longest frame: 9 full pieces make a 160 byte frame, then a new one
        for (int i = 0; i < 9; ++i) {
            const Bytes x = seq_bytes(20, static_cast<uint8_t>(i));
            n.feed(x.data(), x.size(), 400, f);
        }
        CHECK(f.size() == 5 && f[4].size() == 160 && n.held() == 20);
        n.reset();
        CHECK(n.held() == 0);
    }
    // esp32 (MTU 176, it never cuts): a full 173 byte notification and the next frame stay apart
    {
        bt::NotifyAssembler n(176);
        CHECK(n.reassembles() && n.chunk() == 173);
        std::vector<Bytes> f;
        const Bytes full = seq_bytes(173, 3), next = seq_bytes(50, 7);
        n.feed(full.data(), full.size(), 0, f);
        CHECK(f.empty());
        n.feed(next.data(), next.size(), 10, f);
        CHECK(f.size() == 2 && f[0] == full && f[1] == next);
    }

    // helpers
    CHECK(bt::address_from_path("/org/bluez/hci0/dev_ec_eb_45_39_28_2D") == kAddrA);
    CHECK(bt::address_from_path("/org/bluez/hci0") == "" && bt::address_from_path("/org/bluez/hci0/dev_12") == "");
    CHECK(bt::device_path("/org/bluez/hci0", "ec:eb:45:39:28:2d") == "/org/bluez/hci0/dev_EC_EB_45_39_28_2D");
    CHECK(bt::valid_address(kAddrB) && !bt::valid_address("D7:C5:07:5B:55") && !bt::valid_address("D7-C5-07-5B-55-EB"));
    bt::BleDevice d;
    d.name = "MeshCore-OSR wio";
    CHECK(bt::is_candidate(d));
    d.name = "Meshtastic_78ec";
    CHECK(!bt::is_candidate(d));
    d.nus = true;
    CHECK(bt::is_candidate(d));
}

/* a short way to drive a link: the fake's events are handled at the next tick */
struct LinkRig {
    bt::FakeBle be;
    bt::BleLink link;
    uint64_t now = 1000;
    std::vector<std::string> logs;
    LinkRig() : link(be) { link.set_log([this](const std::string &l) { logs.push_back(l); }); }
    void step(uint64_t ms = 20, int times = 1)
    {
        for (int i = 0; i < times; ++i) {
            now += ms;
            link.tick(now);
        }
    }
    bt::BleState state() const { return link.status().state; }
};

static void test_ble_link_states()
{
    using S = bt::BleState;
    using F = bt::BleFail;

    // no Bluetooth at all (no libsystemd / no bus): Failed at once, with the reason
    {
        LinkRig r;
        r.be.start_ok = false;
        r.link.connect(kAddrA, r.now);
        CHECK(r.state() == S::Failed && r.link.status().fail == F::NoBluetooth && r.link.status().detail == "no system D-Bus");
        std::string why;
        CHECK(!r.link.start_scan(r.now, &why) && why == "no system D-Bus");
    }
    // BlueZ answers without an adapter
    {
        LinkRig r;
        r.be.ready_ok = false;
        r.link.connect(kAddrA, r.now);
        CHECK(r.state() == S::Connecting);
        r.step();
        CHECK(r.state() == S::Failed && r.link.status().fail == F::NoBluetooth && r.link.status().detail == "no Bluetooth adapter");
    }
    // the adapter is off
    {
        LinkRig r;
        r.be.powered = false;
        r.link.connect(kAddrA, r.now);
        r.step();
        CHECK(r.state() == S::Failed && r.link.status().fail == F::AdapterOff);
        CHECK(std::string(bt::fail_text(F::AdapterOff)) == "Bluetooth is off");
    }
    // an address BlueZ does not know: a search scan, then pairing once it is seen
    {
        LinkRig r;
        r.link.connect(kAddrA, r.now);
        r.step();
        CHECK(r.state() == S::Scanning && r.be.called("scan on") && r.link.status().scanning);
        r.be.appear(kAddrB, "MeshCore-OSR wio");             // another board: not ours
        r.step();
        CHECK(r.state() == S::Scanning);
        r.be.appear(kAddrA, "MeshCore-OSR T1000");
        r.step();
        CHECK(r.state() == S::Pairing && r.be.called("scan off") && r.be.called(std::string("pair ") + kAddrA));
        CHECK(r.link.status().name == "MeshCore-OSR T1000" && !r.link.status().scanning);
    }
    // never seen: NotFound after the search time, the scan stopped, nothing retried
    {
        LinkRig r;
        r.link.connect(kAddrA, r.now);
        r.step();
        r.step(1000, 15);
        CHECK(r.state() == S::Failed && r.link.status().fail == F::NotFound && r.be.called("scan off"));
        r.step(1000, 120);
        CHECK(r.state() == S::Failed && r.be.count("scan on") == 1);
    }
    // a bad address
    {
        LinkRig r;
        r.link.connect("EC:EB:45", r.now);
        CHECK(r.state() == S::Failed && r.link.status().fail == F::NotFound && r.be.calls.empty());
    }
}

static void test_ble_pairing()
{
    using S = bt::BleState;
    using F = bt::BleFail;

    // the PIN path: Pair -> the agent asks -> the user types it -> paired -> services -> notify -> Connected
    {
        LinkRig r;
        r.be.auto_answer = true;
        r.be.add_device(kAddrA, "MeshCore-OSR T1000");
        r.link.connect(kAddrA, r.now);
        r.step();
        CHECK(r.state() == S::Pairing && r.be.called(std::string("pair ") + kAddrA));
        r.step();
        CHECK(r.state() == S::Pairing && r.link.status().pin_needed);
        r.step(1000, 30);                                      // the user takes 30 s: still waiting, nothing sent
        CHECK(r.state() == S::Pairing && r.link.status().pin_needed && r.be.count("passkey") == 0);
        r.link.submit_pin(123456);
        CHECK(!r.link.status().pin_needed && r.be.called("passkey 123456"));
        r.link.submit_pin(123456);                             // a second submit does nothing
        CHECK(r.be.count("passkey") == 1);
        r.step();
        CHECK(r.state() == S::Connecting && r.be.called(std::string("gatt ") + kAddrA));
        r.step();
        CHECK(r.state() == S::Connected && r.link.status().mtu == 247 && r.link.connected());
        CHECK(r.be.count("connect") == 0);                     // Pair connected it already
        CHECK(r.link.stats().connects == 1);
    }
    // a wrong PIN: Failed(PairingFailed), and no retry by itself (the user must try again)
    {
        LinkRig r;
        r.be.auto_answer = true;
        r.be.add_device(kAddrA, "MeshCore-OSR T1000");
        r.link.connect(kAddrA, r.now);
        r.step(20, 2);
        r.link.submit_pin(111111);
        r.step();
        CHECK(r.state() == S::Failed && r.link.status().fail == F::PairingFailed);
        CHECK(r.link.status().detail.find("AuthenticationFailed") != std::string::npos);
        r.step(1000, 200);
        CHECK(r.state() == S::Failed && r.be.count("pair") == 1);
        // trying again with the right PIN works
        r.link.connect(kAddrA, r.now);
        r.step(20, 2);
        r.link.submit_pin(123456);
        r.step(20, 3);
        CHECK(r.state() == S::Connected);
    }
    // the user cancels the PIN box
    {
        LinkRig r;
        r.be.add_device(kAddrA, "MeshCore-OSR T1000");
        r.link.connect(kAddrA, r.now);
        r.be.result(bt::BleEvent::Kind::PasskeyRequest, kAddrA, true);
        r.step();
        CHECK(r.link.status().pin_needed);
        r.link.cancel_pairing();
        CHECK(r.state() == S::Failed && r.link.status().fail == F::PairingCancelled);
        CHECK(r.be.called("passkey cancel") && r.be.called(std::string("cancel ") + kAddrA));
    }
    // BlueZ stops waiting (Agent1.Cancel), then Pair fails as cancelled
    {
        LinkRig r;
        r.be.add_device(kAddrA, "MeshCore-OSR T1000");
        r.link.connect(kAddrA, r.now);
        r.be.result(bt::BleEvent::Kind::PasskeyRequest, kAddrA, true);
        r.step();
        r.be.result(bt::BleEvent::Kind::PairingCancelled, kAddrA, true);
        r.step();
        CHECK(r.state() == S::Pairing && !r.link.status().pin_needed);
        r.be.result(bt::BleEvent::Kind::PairResult, kAddrA, false, "org.bluez.Error.AuthenticationCanceled: Authentication Canceled");
        r.step();
        CHECK(r.state() == S::Failed && r.link.status().fail == F::PairingCancelled);
    }
    // nobody answers: the pairing gives up after pair_ms
    {
        LinkRig r;
        r.be.add_device(kAddrA, "MeshCore-OSR T1000");
        r.link.connect(kAddrA, r.now);
        r.step(1000, 96);
        CHECK(r.state() == S::Failed && r.link.status().fail == F::PairingFailed && r.be.called(std::string("cancel ") + kAddrA));
    }
    // a passkey request for another device is refused
    {
        LinkRig r;
        r.be.add_device(kAddrA, "MeshCore-OSR T1000");
        r.link.connect(kAddrA, r.now);
        r.be.result(bt::BleEvent::Kind::PasskeyRequest, kAddrB, true);
        r.step();
        CHECK(!r.link.status().pin_needed && r.be.called("passkey cancel"));
    }
    // the board is out of reach while pairing: retried later, the PIN is asked again then
    {
        LinkRig r;
        r.be.add_device(kAddrA, "MeshCore-OSR T1000");
        r.link.connect(kAddrA, r.now);
        r.be.result(bt::BleEvent::Kind::PairResult, kAddrA, false, "org.bluez.Error.ConnectionAttemptFailed: Page Timeout");
        r.step();
        CHECK(r.state() == S::Reconnecting && r.link.status().attempt == 1);
        r.step(1000, 2);
        CHECK(r.state() == S::Pairing && r.be.count("pair") == 2);
    }
    // not a companion: no Nordic UART service
    {
        LinkRig r;
        r.be.auto_answer = true;
        r.be.nus = false;
        r.be.add_device(kAddrA, "Some LE thing", true);
        r.link.connect(kAddrA, r.now);
        r.step(20, 4);
        CHECK(r.state() == S::Failed && r.link.status().fail == F::NotCompanion && r.be.called(std::string("disconnect ") + kAddrA));
    }
}

static void test_ble_reconnect()
{
    using S = bt::BleState;
    using F = bt::BleFail;
    using K = bt::BleEvent::Kind;

    // a bonded board: Connect (no pairing), then lost and found again with a backoff
    LinkRig r;
    r.be.auto_answer = true;
    r.be.add_device(kAddrA, "MeshCore-OSR T1000", true);
    r.link.connect(kAddrA, r.now);
    r.step(20, 3);
    CHECK(r.state() == S::Connected && r.be.count("pair") == 0 && r.be.count("connect") == 1);
    const uint32_t gen = r.link.generation();
    r.be.set_connected(kAddrA, false, false);                  // the board walked away
    r.step();
    CHECK(r.state() == S::Reconnecting && r.link.generation() == gen + 1 && r.link.status().attempt == 1);
    CHECK(r.link.status().retry_at_ms == r.now + 2000);
    r.step(1000, 1);
    CHECK(r.be.count("connect") == 1);
    r.step(1000, 1);
    r.step(20, 3);
    CHECK(r.state() == S::Connected && r.be.count("connect") == 2 && r.link.status().attempt == 0);
    CHECK(r.link.stats().connects == 2);

    // the notify socket hangs up: lost too
    r.be.result(K::NotifyClosed, kAddrA, false);
    r.step();
    CHECK(r.state() == S::Reconnecting && r.link.generation() == gen + 2);

    // backoff 2, 4, 8, 16, 32, 60, 60 s while the board stays away
    r.be.auto_answer = false;
    r.step(2000, 1);
    std::vector<uint64_t> delays;
    for (int i = 0; i < 7; ++i) {
        CHECK(r.state() == S::Connecting);
        r.be.result(K::ConnectResult, kAddrA, false, "org.bluez.Error.Failed: le-connection-abort-by-local");
        r.step();
        CHECK(r.state() == S::Reconnecting);
        delays.push_back(r.link.status().retry_at_ms - r.now);
        r.now = r.link.status().retry_at_ms - 20;
        r.step();
    }
    CHECK((delays == std::vector<uint64_t>{4000, 8000, 16000, 32000, 60000, 60000, 60000}));   // the first was 2000 above
    // a connect that never answers times out into the same loop
    r.step(1000, 36);
    CHECK(r.state() == S::Reconnecting);

    // the board was re-flashed: the stored keys are refused twice -> BondLost, no more tries
    {
        LinkRig b;
        b.be.add_device(kAddrA, "MeshCore-OSR T1000", true);
        b.link.connect(kAddrA, b.now);
        b.step();
        b.be.result(K::ConnectResult, kAddrA, false, "org.bluez.Error.Failed: Authentication Failed (0x05)");
        b.step();
        CHECK(b.state() == S::Reconnecting);
        b.step(1000, 3);
        b.be.result(K::ConnectResult, kAddrA, false, "org.bluez.Error.Failed: Authentication Failed (0x05)");
        b.step();
        CHECK(b.state() == S::Failed && b.link.status().fail == F::BondLost);
        b.step(1000, 100);
        CHECK(b.be.count("connect") == 2);
    }
    // disconnect(): Idle, the bond stays, Device1.Disconnect sent, the generation moves
    {
        LinkRig d;
        d.be.auto_answer = true;
        d.be.add_device(kAddrA, "MeshCore-OSR T1000", true);
        d.link.connect(kAddrA, d.now);
        d.step(20, 3);
        const uint32_t g = d.link.generation();
        d.link.disconnect();
        CHECK(d.state() == S::Idle && d.be.called(std::string("disconnect ") + kAddrA) && d.link.generation() == g + 1);
        CHECK(d.be.devs[kAddrA].paired);
        d.step(1000, 100);
        CHECK(d.state() == S::Idle && d.be.count("connect") == 1);
    }
    // Bluetooth switched off while connected
    {
        LinkRig o;
        o.be.auto_answer = true;
        o.be.add_device(kAddrA, "MeshCore-OSR T1000", true);
        o.link.connect(kAddrA, o.now);
        o.step(20, 3);
        o.be.powered = false;
        o.be.result(K::AdapterPowered, "", false);
        o.step();
        CHECK(o.state() == S::Reconnecting);
        o.step(1000, 3);
        CHECK(o.state() == S::Reconnecting && o.link.status().detail == "Bluetooth is off" && o.be.count("connect") == 1);
        o.be.powered = true;
        o.step(1000, 8);
        CHECK(o.state() == S::Connected);
    }
}

static void test_ble_data()
{
    using S = bt::BleState;
    using K = bt::BleEvent::Kind;

    LinkRig r;
    r.be.add_device(kAddrA, "MeshCore-OSR T1000", true);
    r.be.mtu = 23;                                             // a small MTU: the Bluefruit cutting
    r.be.auto_answer = true;
    r.link.connect(kAddrA, r.now);
    r.step(20, 3);
    CHECK(r.state() == S::Connected && r.link.status().mtu == 23);
    r.be.auto_answer = false;                                  // the write answers come from the test now

    // one write at a time; a frame longer than MTU - 3 still goes as ONE write
    const Bytes q = build_device_query(), contact = make_contact(resp::kContact, 0x30, "Alice", 1, 1790000000);
    CHECK(r.link.send(q) && r.link.send(contact));
    CHECK(r.be.writes.size() == 1 && r.be.writes[0] == q);
    r.be.result(K::WriteResult, kAddrA, true);
    r.step();
    CHECK(r.be.writes.size() == 2 && r.be.writes[1] == contact && r.link.stats().long_writes == 1);
    CHECK(!r.link.send(seq_bytes(177, 0)) && r.link.stats().refused == 1);
    CHECK(!r.link.send(Bytes{}));
    r.be.result(K::WriteResult, kAddrA, false, "org.bluez.Error.Failed: Operation failed with ATT error: 0x0e");
    r.step();
    CHECK(r.link.stats().write_errors == 1 && r.state() == S::Connected);

    // notifications cut at 20 bytes come back as frames
    for (const Bytes &x : cut(contact, 20)) r.be.notify(x);
    r.be.notify({0x83});
    r.step();
    Bytes f;
    CHECK(r.link.receive(f) && f == contact);
    CHECK(r.link.receive(f) && f == Bytes{0x83});
    CHECK(!r.link.receive(f));
    CHECK(r.link.stats().frames_in == 2 && r.link.stats().joined == 1);

    // a write that is never answered: after 10 s the link is dropped (meshcore_py does the same)
    CHECK(r.link.send(q));
    r.step(1000, 10);
    CHECK(r.state() == S::Reconnecting && r.link.status().detail.find("no answer") != std::string::npos);
    CHECK(!r.link.send(q));

    // 32 frames wait at most
    LinkRig w;
    w.be.add_device(kAddrA, "x", true);
    w.be.auto_answer = true;
    w.link.connect(kAddrA, w.now);
    w.step(20, 3);
    w.be.auto_answer = false;
    int queued = 0;
    for (int i = 0; i < 40; ++i) queued += w.link.send(q);
    CHECK(queued == 33);                                       // one in flight + 32 waiting

    // the device list: MeshCore boards only, paired first, then the strongest
    LinkRig l;
    l.be.add_device("11:11:11:11:11:11", "MeshCore-far", false, -90);
    l.be.add_device("22:22:22:22:22:22", "MeshCore-near", false, -40);
    l.be.add_device("33:33:33:33:33:33", "MeshCore-mine", true, -80);
    l.be.add_device("44:44:44:44:44:44", "Meshtastic_1eec", false, -30);
    CHECK(l.link.start_scan(l.now) && l.be.called("scan on"));
    const std::vector<bt::BleDevice> c = l.link.candidates();
    CHECK(c.size() == 3 && c[0].name == "MeshCore-mine" && c[1].name == "MeshCore-near" && c[2].name == "MeshCore-far");
    l.step(1000, 11);
    CHECK(l.be.called("scan off") && !l.link.status().scanning);
}

/* ------------------------------------------------------------------ the transport and the Client over Bluetooth */

/* FakeBle with the scripted companion behind it: each write is a whole command; each answer frame comes back as one
 * notification, or cut in MTU - 3 pieces (cut_at). */
class PeerBle : public bt::FakeBle {
public:
    FakePeer peer;
    size_t cut_at = 0;
    PeerBle()
    {
        auto_answer = true;
        peer.open();
    }
    void write(const Bytes &frame) override
    {
        bt::FakeBle::write(frame);
        const Bytes f = encode_frame(frame);
        peer.write(f.data(), f.size());
    }
    void poll(std::vector<bt::BleEvent> &out) override
    {
        uint8_t buf[512];
        long n;
        while ((n = peer.read(buf, sizeof(buf))) > 0) {
            std::vector<Bytes> frames;
            parser_.feed(buf, static_cast<size_t>(n), frames);
            for (const Bytes &fr : frames) {
                if (cut_at) for (const Bytes &x : cut(fr, cut_at)) notify(x);
                else notify(fr);
            }
        }
        bt::FakeBle::poll(out);
    }

private:
    FrameParser parser_;
};

static void test_ble_transport()
{
    // open() follows the link; read() gives serial frames; a loss is -1 once; then Ok again after the reconnect
    auto owned = std::make_unique<bt::FakeBle>();
    bt::FakeBle &be = *owned;
    be.auto_answer = true;
    be.add_device(kAddrA, "MeshCore-OSR T1000", true);
    uint64_t now = 5000;
    bt::BleTransport t(std::move(owned), kAddrA, "", [&now] { return now; });
    CHECK(t.kind() == "Bluetooth LE" && t.info().path == std::string("ble:") + kAddrA && !t.is_open());
    CHECK(t.open() == OpenStatus::NotFound && !t.is_open());
    CHECK(t.last_error().rfind("MeshCore-OSR T1000: ", 0) == 0);      // "<name>: <step>" while on its way
    for (int i = 0; i < 5 && t.open() != OpenStatus::Ok; ++i) now += 20;
    CHECK(t.is_open() && t.info().product == "MeshCore-OSR T1000");

    const Bytes q = build_device_query();
    const Bytes wire = encode_frame(q);
    CHECK(t.write(wire.data(), 2) && t.write(wire.data() + 2, wire.size() - 2));   // split by the caller: still one write
    CHECK(be.writes.size() == 1 && be.writes[0] == q);

    be.notify(Bytes{resp::kOk});
    be.notify(make_self_info("Deck Zero"));
    uint8_t buf[512];
    long n = t.read(buf, 2);                                   // a small buffer: the rest comes with the next read
    CHECK(n == 2);
    Bytes got(buf, buf + n);
    n = t.read(buf, sizeof(buf));
    CHECK(n > 0);
    got.insert(got.end(), buf, buf + n);
    FrameParser p;
    std::vector<Bytes> frames;
    p.feed(got.data(), got.size(), frames);
    CHECK(frames.size() == 2 && frames[0] == Bytes{resp::kOk} && frames[1] == make_self_info("Deck Zero"));
    CHECK(t.read(buf, sizeof(buf)) == 0);

    be.set_connected(kAddrA, false, false);
    CHECK(t.read(buf, sizeof(buf)) == -1 && t.last_error().find("lost") != std::string::npos);
    CHECK(!t.write(wire.data(), wire.size()));
    t.close();
    CHECK(!t.is_open() && t.open() == OpenStatus::NotFound);
    now += 2000;
    for (int i = 0; i < 5 && t.open() != OpenStatus::Ok; ++i) now += 20;
    CHECK(t.is_open() && be.count("connect") == 2);

    // Failed (the user must act): Error with the reason
    auto o2 = std::make_unique<bt::FakeBle>();
    o2->start_ok = false;
    bt::BleTransport t2(std::move(o2), kAddrB, "", [&now] { return now; });
    CHECK(t2.open() == OpenStatus::Error && t2.last_error().find("not available") != std::string::npos);

    // shutdown: the link goes, the backend stops
    t.shutdown();
    CHECK(!t.is_open() && be.called(std::string("disconnect ") + kAddrA) && be.called("stop"));
}

static void ble_client_run(int mtu, size_t cut_at, const char *name)
{
    set_time_source(fake_time);
    const std::string dir = tmpdir(name);
    Model m;
    Store st(dir);
    attach_b(st, m);
    auto owned = std::make_unique<PeerBle>();
    PeerBle &be = *owned;
    be.mtu = mtu;
    be.cut_at = cut_at;
    be.add_device(kAddrA, "MeshCore-OSR T1000");               // not paired yet: the PIN path first
    bt::BleTransport t(std::move(owned), kAddrA, "", [] { return g_now; });
    Client c(t, m);
    c.set_deck_clock([] { return DeckClock::Synced; }, [] {});
    set_time_offset(0);
    be.peer.messages.push_back(make_chan_msg_v3(0, "Zed: over the air"));

    bool asked = false;
    for (int i = 0; i < 1500 && c.link() != Link::Ready; ++i) {
        g_now += 20;
        t.poll();
        if (t.link().status().pin_needed) {
            asked = true;
            t.link().submit_pin(123456);
        }
        c.poll(g_now);
    }
    CHECK(asked);
    CHECK(c.link() == Link::Ready);
    for (int i = 0; i < 150; ++i) {                            // the first sync after the handshake
        g_now += 20;
        t.poll();
        c.poll(g_now);
    }
    CHECK(m.device() && m.device()->model == "Seeed Xiao-nrf52" && m.self() && m.self()->name == "Deck Zero");
    CHECK(m.contact_count() == 2 && m.message_count() == 1);
    // every GATT write is one bare command, in the order the Client sent them
    CHECK(!be.writes.empty() && be.writes.size() == be.peer.commands.size());
    CHECK(be.writes[0] == build_device_query());
    bool same = be.writes.size() == be.peer.commands.size();
    for (size_t i = 0; same && i < be.writes.size(); ++i) same = be.writes[i] == be.peer.commands[i];
    CHECK(same);
    if (cut_at) CHECK(t.link().stats().joined > 0);

    // the board goes away and comes back: the Client sees a loss, then the handshake again
    be.set_connected(kAddrA, false, false);
    for (int i = 0; i < 20; ++i) {
        g_now += 20;
        t.poll();
        c.poll(g_now);
    }
    CHECK(c.link() == Link::Searching);
    for (int i = 0; i < 1500 && c.link() != Link::Ready; ++i) {
        g_now += 20;
        t.poll();
        c.poll(g_now);
    }
    CHECK(c.link() == Link::Ready && be.count("pair") == 1 && be.count("connect") == 1);
    t.shutdown();
}

static void test_ble_client()
{
    ble_client_run(247, 0, "ble-client");                      // nRF52: MTU 247, one notification per frame
    ble_client_run(23, 20, "ble-client-mtu23");                // no MTU exchange: every frame cut in 20 byte pieces
}

static void test_ble()
{
    test_ble_framing();
    test_ble_link_states();
    test_ble_pairing();
    test_ble_reconnect();
    test_ble_data();
    test_ble_transport();
    test_ble_client();
}
