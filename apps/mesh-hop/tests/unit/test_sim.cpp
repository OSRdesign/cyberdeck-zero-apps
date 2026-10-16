// Integration test: the real SerialTransport + Client + Model + Store against tools/meshcore_sim.py over a pty.
// MESHHOP_SIM = path of the simulator script. Takes about 15 seconds (real time).
#include "client.hpp"
#include "clock_policy.hpp"
#include "features.hpp"
#include "serial_transport.hpp"
#include "store.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace meshzero;

static int g_fail = 0, g_checks = 0;
#define CHECK(...)                                                   \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(__VA_ARGS__)) {                                        \
            ++g_fail;                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
        }                                                            \
    } while (0)

static uint64_t mono()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <typename Pred> static bool wait_for(Client &c, Pred p, int timeout_ms)
{
    const uint64_t end = mono() + static_cast<uint64_t>(timeout_ms);
    while (mono() < end) {
        c.poll(mono());
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main()
{
    const char *sim = std::getenv("MESHHOP_SIM");
    if (!sim) { std::printf("MESHHOP_SIM not set\n"); return 2; }
    const std::string base = "/tmp/meshzero-sim-test-" + std::to_string(::getpid());
    const std::string link = base + ".port";
    const std::string data = base + ".data";

    int to_sim[2];
    if (::pipe(to_sim) != 0) return 2;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(to_sim[0], 0);
        ::close(to_sim[1]);
        ::execlp("python3", "python3", sim, "--link", link.c_str(), "--ack-delay", "0.3", "--echo", "--heard-back", "2", "--heard-back-direct", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(to_sim[0]);
    auto say = [&](const std::string &line) { const std::string l = line + "\n"; if (::write(to_sim[1], l.data(), l.size()) < 0) {} };

    // wait for the port to exist
    for (int i = 0; i < 100 && ::access(link.c_str(), F_OK) != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(::access(link.c_str(), F_OK) == 0);

    Model model;
    Store store(data);
    store.attach(model);
    SerialTransport serial(data, link);
    Client client(serial, model);
    client.set_deck_clock([] { return DeckClock::Synced; }, [] {});

    CHECK(wait_for(client, [&] { return client.link() == Link::Ready; }, 6000));
    CHECK(model.device() && model.device()->model == "Seeed Xiao-nrf52" && model.device()->version == "v1.15.0-dee3e26");
    CHECK(wait_for(client, [&] { return model.contact_count() == 4 && model.message_count() >= 2 && model.find_channel(39) != nullptr; }, 8000));
    CHECK(model.find_channel(0) && model.find_channel(0)->name == "Public" && model.find_channel(1)->name == "#test");
    CHECK(model.unread_total() == 2);
    CHECK(client.junk_bytes() == 0 || client.junk_bytes() > 0);

    CHECK(wait_for(client, [&] { return model.clock().source == ClockSource::DeckNtp; }, 4000));

    // a direct message: sent, acknowledged, answered by the simulator's echo
    const ContactRec *alice = nullptr;
    for (const ContactRec *r : model.contacts_sorted())
        if (r->c.name == "Alice") alice = r;
    CHECK(alice != nullptr);
    const uint32_t seq = client.send_direct(alice->key_hex(), "ping from the deck");
    CHECK(seq != 0);
    CHECK(wait_for(client, [&] { return model.find_message(seq)->state == MsgState::Delivered; }, 3000));
    CHECK(wait_for(client, [&] { return model.messages(Model::conv_direct(alice->prefix())).size() >= 3; }, 5000));
    CHECK(model.messages(Model::conv_direct(alice->prefix())).back()->text == "got: ping from the deck");

    // channel message + echo; the mesh repeats it back over 2 paths (+ one copy heard straight from the sender, not counted), seen in the
    // radio log: "heard back by 2 repeaters" (phase 3). The packet log keeps what it captured.
    client.packet_log().start();
    const uint32_t cseq = client.send_channel(0, "hello mesh Ã©tÃ©");
    CHECK(wait_for(client, [&] { return model.find_message(cseq)->state == MsgState::Sent; }, 2000));
    CHECK(wait_for(client, [&] { return model.messages("c:0").size() >= 3; }, 4000));
    CHECK(wait_for(client, [&] { return model.find_message(cseq)->heard_back == 2; }, 5000));
    bool logged_grp = false;
    for (size_t i = 0; i < client.packet_log().size(); ++i) logged_grp = logged_grp || client.packet_log().at(i).pkt.is_group_text();
    CHECK(logged_grp && client.echoes_open() == 1);
    client.packet_log().stop();

    // typed stdin command: a message arrives on its own
    const size_t before = model.message_count();
    say("msg unsolicited text");
    CHECK(wait_for(client, [&] { return model.message_count() > before; }, 3000));

    // settings round trip
    RadioSettings e = model.radio_settings();
    e.freq_mhz = 868.1;
    e.sf = 9;
    e.name = "DeckZero";
    CHECK(client.save_settings(e));
    CHECK(wait_for(client, [&] { return model.self() && model.self()->name == "DeckZero" && model.self()->sf == 9; }, 3000));
    CHECK(model.self()->freq_khz == 868100);
    CHECK(client.send_advert(false));
    CHECK(wait_for(client, [&] { return client.notice().text.find("zero-hop") != std::string::npos; }, 2000));

    // hashtag channel through the real serial path: slot 2 is the first free one, then removed again
    CHECK(client.add_hashtag_channel("#eu868"));
    CHECK(wait_for(client, [&] { return model.find_channel(2) && model.find_channel(2)->name == "#eu868"; }, 3000));
    CHECK(model.find_channel(2)->app_added);
    CHECK(client.remove_channel(2));
    CHECK(wait_for(client, [&] { return model.find_channel(2) && model.find_channel(2)->empty; }, 3000));

    // ---- phase 2 (0.2.0) over the real serial path
    // path hash size: the simulator reports mode 1 (2 byte hashes) like the user's board
    CHECK(model.device() && model.device()->fw_ver == 11 && model.device()->path_hash_mode == 1);
    CHECK(client.set_path_hash_mode(2));
    CHECK(wait_for(client, [&] { return model.device()->path_hash_mode == 2; }, 3000));
    CHECK(client.set_path_hash_mode(1));
    CHECK(wait_for(client, [&] { return model.device()->path_hash_mode == 1; }, 3000));

    // statistics: three sub types
    CHECK(client.refresh_stats());
    CHECK(wait_for(client, [&] { return !client.stats_busy() && model.stats().core && model.stats().radio && model.stats().packets; }, 4000));
    CHECK(model.stats().core->battery_mv == 3960 && model.stats().radio->noise_floor == -112 && model.stats().radio->last_rssi == -95 &&
          model.stats().packets->has_errors && model.stats().packets->recv_errors == 3 && model.stats().packets->flood_tx == 4);

    // auto-add filter and manual add mode
    CHECK(wait_for(client, [&] { return model.caps().autoadd == Cap::Yes; }, 2000) && model.caps().autoadd_config.config == 0x02);
    CHECK(client.set_autoadd_flags(0x06));
    CHECK(wait_for(client, [&] { return model.caps().autoadd_config.config == 0x06; }, 3000));

    // repeat: the board lists where it may repeat; 869.525 MHz is inside 863-870 MHz
    CHECK(model.caps().repeat_freqs == Cap::Yes && repeat_state(&*model.device(), model.caps(), model.self()->freq_mhz()).available);
    CHECK(client.set_client_repeat(true));
    CHECK(wait_for(client, [&] { return model.device()->repeat; }, 3000));
    CHECK(client.set_client_repeat(false));
    CHECK(wait_for(client, [&] { return !model.device()->repeat; }, 3000));

    // the zero-hop discover: Alice and the repeater are contacts, Newbie and Ridge are strangers
    CHECK(client.start_discover());
    CHECK(wait_for(client, [&] { return client.discover().found == 4; }, 5000));
    CHECK(model.nearby_count() >= 4 && model.nearby_count() <= 5);        // the four that answered (+ a known contact whose advert push came earlier)
    const NearbyRec *newbie = nullptr;
    for (const NearbyRec *n : model.nearby_sorted())
        if (n->type == 1 && !model.find_contact(n->key_hex)) newbie = n;
    CHECK(newbie && newbie->full_key && newbie->snr == 8.0 && newbie->rssi == -61);
    const std::string newbie_key = newbie ? newbie->key_hex : "";
    CHECK(wait_for(client, [&] { return !client.discover().active; }, 12000));
    CHECK(client.add_nearby(newbie_key));
    CHECK(wait_for(client, [&] { return model.find_contact(newbie_key) != nullptr; }, 3000));
    CHECK(model.contact_count() == 5 && model.find_contact(newbie_key)->c.type == 1);

    // manual add: a new advert is pending, not a contact; the log gives its SNR
    CHECK(client.set_manual_add(true));
    CHECK(wait_for(client, [&] { return model.self() && model.self()->manual_add_contacts; }, 3000));
    say("newnode");
    CHECK(wait_for(client, [&] { return model.pending_count() == 1; }, 3000));
    CHECK(model.contact_count() == 5);
    std::string pending_key;
    for (const NearbyRec *n : model.nearby_sorted())
        if (n->pending) { pending_key = n->key_hex; CHECK(n->has_snr && n->hops == 1 && n->hash_size == 2 && n->name == "Ridge repeater" && n->type == 2); }
    CHECK(client.add_nearby(pending_key));
    CHECK(wait_for(client, [&] { return model.find_contact(pending_key) != nullptr; }, 3000));
    CHECK(model.pending_count() == 0 && model.contact_count() == 6);
    CHECK(client.set_manual_add(false));
    CHECK(wait_for(client, [&] { return model.self() && !model.self()->manual_add_contacts; }, 3000));

    // bulk delete of two contacts, one by one
    std::vector<PubKey> doomed;
    for (const ContactRec *r : model.contacts_sorted())
        if (r->c.name == "Bob" || r->c.name == "Weather room") doomed.push_back(r->c.key);
    CHECK(doomed.size() == 2 && client.remove_contacts(doomed));
    CHECK(wait_for(client, [&] { return !client.bulk().active && client.bulk().done == 2; }, 5000));
    CHECK(model.contact_count() == 4);
    CHECK(client.refresh_contacts());
    CHECK(wait_for(client, [&] { return model.contact_count() == 4; }, 3000));                        // the board agrees with the model

    // reboot: the port goes away and comes back, the data is still there
    CHECK(client.reboot());
    CHECK(wait_for(client, [&] { return client.link() == Link::Searching; }, 4000));
    CHECK(wait_for(client, [&] { return client.link() == Link::Ready; }, 10000));
    CHECK(wait_for(client, [&] { return model.contact_count() == 4 && model.find_channel(39) != nullptr; }, 6000));

    // ---- 0.2.2: another board on the same cable ("swap" unplugs, changes the board and plugs it back)
    const std::string key_a = model.board_key();
    const size_t msgs_a = model.message_count();
    const int unread_a = model.unread_total();
    CHECK(model.board_known() && key_a == to_hex(model.self()->key) && msgs_a > 0 && model.contact_count() == 4);
    const uint32_t epoch_a = model.board_epoch();
    say("swap");
    CHECK(wait_for(client, [&] { return client.link() == Link::Searching; }, 4000));
    CHECK(model.board_key() == key_a && model.message_count() == msgs_a);                    // unplugged: the last board's data is what is shown
    CHECK(wait_for(client, [&] { return client.link() == Link::Ready && model.board_key() != key_a; }, 10000));
    CHECK(model.board_epoch() == epoch_a + 1 && model.self_name() == "SimNodeB");
    CHECK(wait_for(client, [&] { return model.contact_count() == 3 && model.message_count() >= 2 && model.find_channel(1) && model.find_channel(1)->name == "#boardb"; }, 8000));
    const std::string key_b = model.board_key();
    CHECK(model.unread_total() == 2 && model.messages("c:1").size() == 1 && model.messages("c:1")[0]->text == "B's own channel");  // B's own messages only
    for (const ContactRec *r : model.contacts_sorted()) CHECK(r->c.name != "Alice" && r->c.name != "Bob");
    CHECK(model.find_channel(39) == nullptr || model.find_channel(39)->empty);              // A's channel 39 (the simulator's own test slot) is not B's
    CHECK(model.advert_schedule().interval_hours == 0 && model.groups().groups().empty() && model.muted().empty());
    const uint32_t bseq = client.send_channel(1, "hello from the deck on B");
    CHECK(bseq != 0 && wait_for(client, [&] { return model.find_message(bseq)->state == MsgState::Sent; }, 3000));
    const size_t msgs_b = model.message_count();
    say("swap");                                                                               // back to the first board
    CHECK(wait_for(client, [&] { return client.link() == Link::Searching; }, 4000));
    CHECK(wait_for(client, [&] { return client.link() == Link::Ready && model.board_key() == key_a; }, 10000));
    CHECK(model.board_epoch() == epoch_a + 2);
    CHECK(wait_for(client, [&] { return model.contact_count() == 4 && model.find_channel(39) != nullptr; }, 8000));
    CHECK(model.message_count() >= msgs_a && model.unread_total() >= unread_a && model.self_name() == "DeckZero");       // its history is back (plus what arrived since)
    CHECK(model.find_message(seq) != nullptr && model.find_message(seq)->text == "ping from the deck");
    {
        bool leaked = false;
        for (const Message &m : model.all_messages())
            if (m.text == "hello from the deck on B") leaked = true;
        CHECK(!leaked);                                                                        // what was written on B is not in A's history
    }

    // factory reset: the board answers, restarts with a new identity; the app shows the new (empty) board, the old identity keeps its folder
    const size_t msgs = model.message_count();
    CHECK(client.factory_reset());
    CHECK(wait_for(client, [&] { return client.reset_phase() == Client::ResetPhase::Waiting && client.notice().text.find("Board reset") == 0; }, 3000));
    CHECK(wait_for(client, [&] { return client.link() == Link::Searching; }, 4000));
    CHECK(wait_for(client, [&] { return client.link() == Link::Ready; }, 10000));
    CHECK(wait_for(client, [&] { return client.reset_phase() == Client::ResetPhase::Done; }, 4000));
    CHECK(wait_for(client, [&] { return model.find_channel(0) && model.find_channel(0)->empty && model.find_channel(39) != nullptr; }, 6000));
    CHECK(model.board_key() != key_a && model.board_key() != key_b && model.contact_count() == 0 && model.message_count() == 0 && model.unread_total() == 0);
    CHECK(client.notice().text.find("Board reset") == 0 && client.notice().ok);
    CHECK(::access((data + "/boards/" + key_a.substr(0, 12) + "/messages.jsonl").c_str(), F_OK) == 0);       // the old identity's history is still on the deck
    CHECK(store.other_boards().size() == 2 && store.board_id() == model.board_key().substr(0, 12));
    CHECK(msgs > 0);
    say("swap");                                                                               // board B again: its history was kept
    CHECK(wait_for(client, [&] { return client.link() == Link::Searching; }, 4000));
    CHECK(wait_for(client, [&] { return client.link() == Link::Ready && model.board_key() == key_b; }, 10000));
    CHECK(wait_for(client, [&] { return model.contact_count() == 3; }, 6000));
    {
        bool back = false;
        for (const Message &m : model.all_messages())
            if (m.text == "hello from the deck on B") back = true;
        CHECK(model.message_count() >= msgs_b && back);
    }

    // unplug / replug the same board: nothing changes
    const uint32_t epoch_b = model.board_epoch();
    say("unplug");
    CHECK(wait_for(client, [&] { return client.link() == Link::Searching; }, 4000));
    say("plug");
    CHECK(wait_for(client, [&] { return client.link() == Link::Ready; }, 8000));
    CHECK(model.board_key() == key_b && model.board_epoch() == epoch_b);

    // history survives a restart, per board
    const size_t count = model.message_count();
    {
        Model m2;
        Store s2(data);
        s2.attach(m2);
        CHECK(!m2.board_known() && m2.message_count() == 0 && m2.conversations().empty());        // nothing before a board says who it is
        PubKey kb2 = model.self()->key;
        m2.select_board(kb2);                                // the same board: its saved history is loaded
        CHECK(m2.message_count() == count && m2.contact_count() == 3);
    }

    say("quit");
    ::close(to_sim[1]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    std::string cmd = "rm -rf " + data + " " + link;
    if (std::system(cmd.c_str()) != 0) {}
    std::printf("sim: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
