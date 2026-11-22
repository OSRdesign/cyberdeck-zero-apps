/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares, running the helper (see share_exec.hpp).
 */
#include "share_exec.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vinyl {
namespace shares {

int run_capture(const std::vector<std::string> &argv, int timeout_ms, std::string &out) {
    out.clear();
    if (argv.empty() || argv[0].empty()) return 127;
    int po[2];
    if (::pipe(po) != 0) return 127;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(po[0]);
        ::close(po[1]);
        return 127;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(po[1], 1);
        ::dup2(po[1], 2);
        ::close(po[0]);
        ::close(po[1]);
        const int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) ::dup2(null, 0);
        ::setenv("LC_ALL", "C", 1);
        std::vector<char *> a;
        for (const std::string &s : argv) a.push_back(const_cast<char *>(s.c_str()));
        a.push_back(nullptr);
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    ::close(po[1]);
    const auto t0 = std::chrono::steady_clock::now();
    bool open = true, killed = false;
    char buf[1024];
    while (open) {
        const int left = timeout_ms - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (left <= 0) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            killed = true;
            break;
        }
        pollfd fd{po[0], POLLIN, 0};
        const int n = ::poll(&fd, 1, std::min(left, 200));
        if (n < 0) break;
        if (!(fd.revents & (POLLIN | POLLHUP))) continue;
        const ssize_t k = ::read(po[0], buf, sizeof buf);
        if (k > 0) {
            if (out.size() < 65536) out.append(buf, static_cast<size_t>(k));
        } else {
            open = false;
        }
    }
    ::close(po[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    if (killed) return 124;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

const char *availability_name(Availability a) {
    switch (a) {
    case Availability::Unknown: return "unknown";
    case Availability::Ok: return "ok";
    case Availability::NoHelper: return "no helper";
    case Availability::OldHelper: return "old helper";
    }
    return "?";
}

/* ------------------------------------------------------------------------------------------------ ShareLister */

ShareLister::ShareLister(std::string helper, Run run) : helper_(std::move(helper)), run_(std::move(run)), custom_(static_cast<bool>(run_)) {
    if (!run_) run_ = [](const std::vector<std::string> &a, int t, std::string &o) { return run_capture(a, t, o); };
    th_ = std::thread([this] { loop(); });
}

ShareLister::~ShareLister() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void ShareLister::refresh() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        want_ = true;
    }
    cv_.notify_all();
}

ShareSnapshot ShareLister::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void ShareLister::wait_idle() {
    std::unique_lock<std::mutex> lk(mu_);
    idle_cv_.wait(lk, [this] { return !want_ && !busy_; });
}

void ShareLister::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [this] { return quit_ || want_; });
        if (quit_) break;
        want_ = false;
        busy_ = true;
        lk.unlock();
        ShareSnapshot s;
        /* a launcher older than the helper has no such file (a runner given by the caller answers for itself) */
        const bool there = custom_ || helper_.find('/') == std::string::npos || ::access(helper_.c_str(), X_OK) == 0;
        std::string out;
        if (!there) {
            s.avail = Availability::NoHelper;
        } else {
            const int st = run_(version_argv(helper_), 5000, out);
            const Reply r = parse_reply(out);
            if (st != 0 || !r.ok) s.avail = Availability::NoHelper;
            else s.avail = std::atoi(r.rest.c_str()) >= kProtocol ? Availability::Ok : Availability::OldHelper;
        }
        if (s.avail == Availability::Ok) {
            const int st = run_(list_argv(helper_), 5000, out);
            s.listed = st == 0 && parse_list(out, s.shares);
            if (!s.listed) std::fprintf(stderr, "[vinyl] shares: `list` did not answer (exit %d)\n", st);
        }
        lk.lock();
        s.gen = snap_.gen + 1;
        if (s.avail != snap_.avail) std::fprintf(stderr, "[vinyl] shares: helper %s\n", availability_name(s.avail));
        snap_ = s;
        busy_ = false;
        idle_cv_.notify_all();
    }
    busy_ = false;
    idle_cv_.notify_all();
}

/* ------------------------------------------------------------------------------------------------ root runners */

RootRun exec_root_runner(int timeout_ms) {
    return [timeout_ms](const std::vector<std::string> &argv, std::function<void(const RunResult &)> done) {
        std::thread([argv, done, timeout_ms] {
            RunResult r;
            r.exit_code = run_capture(argv, timeout_ms, r.output);
            r.result = r.exit_code == 0 ? SudoResult::Success : r.exit_code == 124 ? SudoResult::TimedOut : SudoResult::ExecFailed;
            if (done) done(r);
        }).detach();
    };
}

/* ------------------------------------------------------------------------------------------------ ShareOps */

ShareOps::ShareOps(ShareConfig cfg, RootRun run) : cfg_(std::move(cfg)), run_(std::move(run)) {
    if (cfg_.runtime.empty()) cfg_.runtime = runtime_dir();
}

std::vector<std::string> ShareOps::last_argv() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_argv_;
}

void ShareOps::add(AddForm f, std::function<void(const Outcome &)> done) {
    Outcome refuse;
    if (busy_->exchange(true)) {
        wipe(f.password);
        refuse.word = "busy";
        refuse.message = "An add or remove is running already.";
        if (done) done(refuse);
        return;
    }
    const bool with_user = !f.guest && !f.user.empty();
    std::string cred;
    if (with_user) {
        const CredFile cf = write_credfile(cfg_.runtime, f.user, f.password);
        wipe(f.password);
        if (!cf.ok) {
            busy_->store(false);
            refuse.word = "credfile";
            refuse.message = "Could not prepare the sign-in. Nothing was changed.";
            refuse.log = "add " + f.name + ": " + cf.error;
            if (done) done(refuse);
            return;
        }
        cred = cf.path;
    }
    wipe(f.password);
    const std::vector<std::string> argv = add_argv(cfg_.helper, f, cred);
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_argv_ = argv;
    }
    const std::string name = f.name;
    const bool spaced = f.share.find(' ') != std::string::npos || f.subfolder.find(' ') != std::string::npos;
    auto busy = busy_;
    if (!run_) {
        remove_credfile(cred);
        busy->store(false);
        RunResult r;
        r.result = SudoResult::NoPrompt;
        if (done) done(add_outcome(r, with_user, name, spaced));
        return;
    }
    run_(argv, [cred, with_user, name, spaced, done, busy](const RunResult &r) {
        remove_credfile(cred); /* the helper deletes it too; never left behind, whatever happened */
        const Outcome o = add_outcome(r, with_user, name, spaced);
        busy->store(false);
        if (done) done(o);
    });
}

void ShareOps::remove(const std::string &name, std::function<void(const Outcome &)> done) {
    if (busy_->exchange(true)) {
        Outcome refuse;
        refuse.word = "busy";
        refuse.message = "An add or remove is running already.";
        if (done) done(refuse);
        return;
    }
    const std::vector<std::string> argv = remove_argv(cfg_.helper, name);
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_argv_ = argv;
    }
    auto busy = busy_;
    if (!run_) {
        busy->store(false);
        RunResult r;
        r.result = SudoResult::NoPrompt;
        if (done) done(remove_outcome(r, name));
        return;
    }
    run_(argv, [name, done, busy](const RunResult &r) {
        const Outcome o = remove_outcome(r, name);
        busy->store(false);
        if (done) done(o);
    });
}

}  // namespace shares
}  // namespace vinyl
