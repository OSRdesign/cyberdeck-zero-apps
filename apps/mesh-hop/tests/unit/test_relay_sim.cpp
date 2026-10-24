// Headless flow of the CLI relay (report 033, phase P3): Mesh Hop's real Client + Model + Store connected to tools/meshcore_sim.py,
// Client::start_relay(), then a console (relay_console.py: what meshcore-cli puts on the wire, until P4 bundles the real one) connects
// to 127.0.0.1:<relay port>, logs in to the simulated repeater and room and runs CLI commands. Mesh Hop keeps Chats going during the
// session, keeps the CLI replies out of them, and resumes after it. Run over TCP (meshcore_sim --tcp: a node on Wi-Fi) and over the
// pty (the USB serial path): the relay sits above the transport.
// MESHHOP_SIM = the simulator, MESHHOP_CONSOLE = relay_console.py. About 40 seconds (real time).
// MESHHOP_MESHCLI = the mccli wrapper of a bundle (phase P4, build.sh cli): a smoke run of the real meshcore-cli on the relay
// (smoke_meshcli below; with or without MESHHOP_CONSOLE).
#include "client.hpp"
#include "serial_transport.hpp"
#include "store.hpp"
#include "tcp_transport.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

static void step(Client &c, ITransport &t)
{
    t.poll();
    c.poll(mono());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

template <typename Pred> static bool wait_for(Client &c, ITransport &t, Pred p, int timeout_ms)
{
    const uint64_t end = mono() + static_cast<uint64_t>(timeout_ms);
    while (mono() < end) {
        step(c, t);
        if (p()) return true;
    }
    return false;
}

static int free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    int port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 && ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len) == 0) port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

static bool has_text(const Model &m, const std::string &needle)
{
    for (const Message &msg : m.all_messages())
        if (msg.text.find(needle) != std::string::npos) return true;
    return false;
}

static void flow(const char *sim, const char *console, bool tcp)
{
    std::printf("relay flow over %s\n", tcp ? "TCP" : "the serial pty");
    std::fflush(stdout);
    const std::string base = "/tmp/meshhop-relay-test-" + std::to_string(::getpid()) + (tcp ? "-tcp" : "-pty");
    const std::string link = base + ".port", data = base + ".data";
    const int node_port = tcp ? free_port() : 0;

    int to_sim[2];
    if (::pipe(to_sim) != 0) { CHECK(false); return; }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(to_sim[0], 0);
        ::close(to_sim[1]);
        const std::string p = std::to_string(node_port);
        if (tcp) ::execlp("python3", "python3", sim, "--tcp", p.c_str(), "--ack-delay", "0.3", "--login-delay", "0.3", "--cli-delay", "0.4", "--guest-password", "hello", static_cast<char *>(nullptr));
        else ::execlp("python3", "python3", sim, "--link", link.c_str(), "--ack-delay", "0.3", "--login-delay", "0.3", "--cli-delay", "0.4", "--guest-password", "hello", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(to_sim[0]);
    auto say = [&](const std::string &line) { const std::string l = line + "\n"; if (::write(to_sim[1], l.data(), l.size()) < 0) {} };
    if (!tcp)
        for (int i = 0; i < 100 && ::access(link.c_str(), F_OK) != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Model model;
    Store store(data);
    store.attach(model);
    std::unique_ptr<ITransport> transport;
    if (tcp) transport = std::make_unique<TcpTransport>("127.0.0.1", node_port, "sim");
    else transport = std::make_unique<SerialTransport>(data, link);
    Client client(*transport, model);
    client.set_deck_clock([] { return DeckClock::Synced; }, [] {});
    std::vector<std::string> log;
    client.set_log([&](const std::string &s) { log.push_back(s); });
    auto logged = [&](const std::string &needle) {
        for (const std::string &l : log) if (l.find(needle) != std::string::npos) return true;
        return false;
    };

    CHECK(wait_for(client, *transport, [&] { return client.link() == Link::Ready; }, 10000));
    CHECK(wait_for(client, *transport, [&] { return model.contact_count() == 4 && model.message_count() >= 2 && model.find_channel(39) != nullptr && client.queued() == 0; }, 10000));
    CHECK(wait_for(client, *transport, [&] { return model.clock().source == ClockSource::DeckNtp && client.queued() == 0; }, 4000));
    const size_t msgs = model.message_count();
    const int unread = model.unread_total();

    // the session: Mesh Hop opens the relay, a message arrives for the deck, the console starts and runs its script
    const int port = client.start_relay();
    CHECK(port > 0);
    CHECK(wait_for(client, *transport, [&] { return client.relay_ready(); }, 3000));
    const std::string dm = std::string("arrives during the console session ") + (tcp ? "(tcp)" : "(pty)");
    say("msg " + dm);
    const std::string port_text = std::to_string(port);
    const pid_t con = ::fork();
    if (con == 0) {
        ::execlp("python3", "python3", console, "--port", port_text.c_str(), "--expect-dm", dm.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    int status = -1;
    const uint64_t end = mono() + 60000;
    while (mono() < end) {
        step(client, *transport);
        if (::waitpid(con, &status, WNOHANG) == con) break;
    }
    if (!WIFEXITED(status)) { ::kill(con, SIGKILL); ::waitpid(con, &status, 0); }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);                       // every check of the console passed
    CHECK(client.relay_frames_to_node() > 20 && client.relay_frames_to_console() > 20);

    // the console quit: the session ends by itself, Mesh Hop resumes and reads the node again
    CHECK(wait_for(client, *transport, [&] { return !client.relay_active(); }, 6000));
    CHECK(client.relay_sessions_ended() == 1 && client.relay_end_reason() == "the console closed its connection");
    CHECK(client.link() == Link::Ready);
    // Chats kept working: the message the console pulled off the node is in Chats, unread; no CLI reply is
    CHECK(has_text(model, dm) && model.message_count() == msgs + 1 && model.unread_total() == unread + 1);
    for (const char *reply : {"v1.15.0-sim", "Hilltop repeater", "Hill2", "Unknown command", "??: ", "UTC"}) CHECK(!has_text(model, reply));
    CHECK(wait_for(client, *transport, [&] { return logged("flood scope reset: ok") && client.queued() == 0; }, 5000));
    CHECK(logged("console relay: listening on 127.0.0.1:" + port_text) && logged("console relay: session ended (the console closed its connection)"));
    for (const std::string &l : log) CHECK(l.find(dm) == std::string::npos && l.find("password") == std::string::npos);   // no text in the log

    // Mesh Hop sends again
    const ContactRec *alice = nullptr;
    for (const ContactRec *r : model.contacts_sorted())
        if (r->c.name == "Alice") alice = r;
    CHECK(alice != nullptr);
    const uint32_t seq = alice ? client.send_direct(alice->key_hex(), "after the console") : 0;
    CHECK(seq != 0 && wait_for(client, *transport, [&] { return model.find_message(seq)->state == MsgState::Delivered; }, 4000));
    // a second session can start
    CHECK(client.start_relay() > 0);
    client.stop_relay();
    CHECK(!client.relay_active() && client.relay_sessions_ended() == 2);

    say("quit");
    ::close(to_sim[1]);
    ::waitpid(pid, &status, 0);
    std::string cmd = "rm -rf " + data + " " + link;
    if (std::system(cmd.c_str()) != 0) {}
}

// Runs the mccli wrapper (argv[0] = its path) with its output in `out`, stepping Mesh Hop meanwhile; its exit status (-1 on a
// 60 s timeout) and the run time in ms. XDG_DATA_HOME / XDG_CACHE_HOME point into `home`: the wrapper's data and .pyc folders.
static int run_mccli(Client &c, ITransport &t, const std::vector<std::string> &args, const std::string &home, const std::string &out, uint64_t &ms)
{
    const uint64_t t0 = mono();
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("XDG_DATA_HOME", (home + "/data").c_str(), 1);
        ::setenv("XDG_CACHE_HOME", (home + "/cache").c_str(), 1);
        ::unsetenv("MESHHOP_DATA");
        const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), nul = ::open("/dev/null", O_RDONLY);
        ::dup2(nul, 0);
        ::dup2(fd, 1);
        ::dup2(fd, 2);
        std::vector<char *> argv;
        for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    int status = -1;
    const uint64_t end = mono() + 60000;
    while (mono() < end) {
        step(c, t);
        if (::waitpid(pid, &status, WNOHANG) == pid) break;
    }
    ms = mono() - t0;
    if (!WIFEXITED(status)) { ::kill(pid, SIGKILL); ::waitpid(pid, &status, 0); return -1; }
    return WEXITSTATUS(status);
}

static std::string slurp(const std::string &path)
{
    std::string s;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

// Phase P4 smoke (report 033): the bundled, pinned meshcore-cli itself instead of relay_console.py. MESHHOP_MESHCLI = its mccli
// wrapper in the scratch bundle of build.sh cli (hash-checked wheels). `mccli -v` (the first start: every .pyc is compiled), then
// `mccli -c off -t 127.0.0.1 -p <relay port> infos ver` on the relay over TCP; Mesh Hop resumes after it. Also checks the files
// rules: .pyc files in the cache folder only, meshcore-cli's folder 0700 and empty (no history, no <node>.msgs).
static void smoke_meshcli(const char *sim, const char *mccli)
{
    std::printf("meshcore-cli smoke on the relay (%s)\n", mccli);
    std::fflush(stdout);
    const std::string base = "/tmp/meshhop-relay-smoke-" + std::to_string(::getpid());
    const std::string data = base + ".data", home = base + ".home", out = base + ".out";
    if (std::system(("rm -rf " + base + ".* && mkdir -p " + home).c_str()) != 0) {}
    const int node_port = free_port();
    int to_sim[2];
    if (::pipe(to_sim) != 0) { CHECK(false); return; }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(to_sim[0], 0);
        ::close(to_sim[1]);
        const std::string p = std::to_string(node_port);
        ::execlp("python3", "python3", sim, "--tcp", p.c_str(), "--ack-delay", "0.3", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(to_sim[0]);
    auto say = [&](const std::string &line) { const std::string l = line + "\n"; if (::write(to_sim[1], l.data(), l.size()) < 0) {} };

    Model model;
    Store store(data);
    store.attach(model);
    TcpTransport transport("127.0.0.1", node_port, "sim");
    Client client(transport, model);
    client.set_deck_clock([] { return DeckClock::Synced; }, [] {});
    std::vector<std::string> log;
    client.set_log([&](const std::string &s) { log.push_back(s); });
    CHECK(wait_for(client, transport, [&] { return client.link() == Link::Ready; }, 10000));
    CHECK(wait_for(client, transport, [&] { return model.contact_count() == 4 && client.queued() == 0; }, 10000));

    uint64_t ms = 0;
    int rc = run_mccli(client, transport, {mccli, "-v"}, home, out, ms);
    std::string text = slurp(out);
    std::printf("  mccli -v (first start, .pyc compiled): %llu ms: %s", static_cast<unsigned long long>(ms), text.c_str());
    CHECK(rc == 0 && text.rfind("meshcore-cli: command line interface to MeshCore companion radios v", 0) == 0);

    const int port = client.start_relay();
    CHECK(port > 0 && wait_for(client, transport, [&] { return client.relay_ready(); }, 3000));
    const std::string dm = "waits on the node during the meshcore-cli smoke";
    say("msg " + dm);
    rc = run_mccli(client, transport, {mccli, "-c", "off", "-t", "127.0.0.1", "-p", std::to_string(port), "infos", "ver"}, home, out, ms);
    text = slurp(out);
    std::printf("  mccli infos ver on the relay: %llu ms, exit %d, output:\n%s", static_cast<unsigned long long>(ms), rc, text.c_str());
    CHECK(rc == 0);
    CHECK(text.find("\"name\": \"SimNode\"") != std::string::npos);                                // infos: SELF_INFO as JSON
    CHECK(text.find("Version: v1.15.0-dee3e26") != std::string::npos && text.find("Comp. radio protocol: 11") != std::string::npos);   // ver
    CHECK(text.find("Traceback") == std::string::npos && text.find("Error") == std::string::npos);
    CHECK(client.relay_frames_to_node() >= 4 && client.relay_frames_to_console() >= 4);
    std::printf("  relay frames: %d to the node, %d to meshcore-cli\n", static_cast<int>(client.relay_frames_to_node()), static_cast<int>(client.relay_frames_to_console()));

    // meshcore-cli exited: the session ends, Mesh Hop resumes and fetches the message itself
    CHECK(wait_for(client, transport, [&] { return !client.relay_active(); }, 6000));
    CHECK(client.relay_end_reason() == "the console closed its connection" && client.link() == Link::Ready);
    CHECK(wait_for(client, transport, [&] { return has_text(model, dm) && client.queued() == 0; }, 8000));

    // files: meshcore-cli's folder in the data folder, 0700, nothing written in it; .pyc files only in the cache folder
    const std::string cfg = home + "/data/mesh-hop/cli/.config/meshcore";
    struct stat st{};
    CHECK(::stat(cfg.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(std::system(("test -z \"$(ls -A " + cfg + ")\"").c_str()) == 0);
    const std::string bundle = std::string(mccli).substr(0, std::string(mccli).rfind('/'));
    CHECK(std::system(("test -z \"$(find " + bundle + " -name __pycache__)\"").c_str()) == 0);
    CHECK(std::system(("test -n \"$(find " + home + "/cache/mesh-hop/pycache -name '*.pyc')\"").c_str()) == 0);

    say("quit");
    ::close(to_sim[1]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (std::system(("rm -rf " + base + ".*").c_str()) != 0) {}
}

int main()
{
    const char *sim = std::getenv("MESHHOP_SIM");
    const char *console = std::getenv("MESHHOP_CONSOLE");
    const char *meshcli = std::getenv("MESHHOP_MESHCLI");
    if (!sim || (!console && !meshcli)) { std::printf("MESHHOP_SIM and MESHHOP_CONSOLE and/or MESHHOP_MESHCLI not set\n"); return 2; }
    std::signal(SIGPIPE, SIG_IGN);
    if (console) {
        flow(sim, console, true);
        flow(sim, console, false);
    }
    if (meshcli) smoke_meshcli(sim, meshcli);
    std::printf("relay sim: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
