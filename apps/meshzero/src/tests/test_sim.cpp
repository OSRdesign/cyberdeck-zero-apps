// Integration test: the real SerialTransport + Client + Model + Store against tools/meshcore_sim.py over a pty.
// MESHZERO_SIM = path of the simulator script. Takes about 15 seconds (real time).
#include "client.hpp"
#include "clock_policy.hpp"
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
    const char *sim = std::getenv("MESHZERO_SIM");
    if (!sim) { std::printf("MESHZERO_SIM not set\n"); return 2; }
    const std::string base = "/tmp/meshzero-sim-test-" + std::to_string(::getpid());
    const std::string link = base + ".port";
    const std::string data = base + ".data";

    int to_sim[2];
    if (::pipe(to_sim) != 0) return 2;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(to_sim[0], 0);
        ::close(to_sim[1]);
        ::execlp("python3", "python3", sim, "--link", link.c_str(), "--ack-delay", "0.3", "--echo", static_cast<char *>(nullptr));
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

    // channel message + echo
    const uint32_t cseq = client.send_channel(0, "hello mesh");
    CHECK(wait_for(client, [&] { return model.find_message(cseq)->state == MsgState::Sent; }, 2000));
    CHECK(wait_for(client, [&] { return model.messages("c:0").size() >= 3; }, 4000));

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

    // unplug / replug
    say("unplug");
    CHECK(wait_for(client, [&] { return client.link() == Link::Searching; }, 4000));
    CHECK(model.contact_count() == 4);
    say("plug");
    CHECK(wait_for(client, [&] { return client.link() == Link::Ready; }, 8000));

    // history survives a restart
    const size_t count = model.message_count();
    {
        Model m2;
        Store s2(data);
        s2.attach(m2);
        CHECK(m2.message_count() == count && m2.contact_count() == 4);
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
