/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares, running the helper (share_helper.hpp):
 *
 *   ShareLister  the user-side questions (`version`, then `list`: no root, no password) on a thread of its own, with a time
 *                limit; the UI reads the last answer (snapshot) and never waits. "Is the helper there?" is access(X_OK)
 *                and `version` answering "ok 1"; a launcher older than the helper has no such file (NoHelper).
 *   ShareOps     Add and Remove through a root runner the caller gives (the app: the toolkit's sudo prompt, ui/; the
 *                tests: exec_root_runner, which runs the fake helper directly): the credentials file written before the
 *                run and deleted after it whatever happened, the reply turned into an Outcome. One operation at a time.
 *
 * The helper's path is ShareConfig::helper: kHelperPath in the app, a fake script in the tests (tests/fake-share-helper.sh);
 * nothing reads it from the environment. LVGL-free.
 */
#pragma once

#include "share_helper.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vinyl {
namespace shares {

struct ShareConfig {
    std::string helper = kHelperPath;
    std::string runtime;                   /* "" : /run/user/<uid> (the only place the helper takes a credentials file from) */
    std::string mount_root = kMountRoot;   /* the scenes use a folder of the demonstration world */
};

/* Runs argv (no shell; argv[0] a path or a name in PATH), stdin /dev/null, stdout and stderr read together until it ends or
 * timeout_ms passes (then killed: 124). 127: it could not be run. The output is capped at 64 KB. */
int run_capture(const std::vector<std::string> &argv, int timeout_ms, std::string &out);

enum class Availability { Unknown, Ok, NoHelper, OldHelper };
const char *availability_name(Availability a);

struct ShareSnapshot {
    Availability avail = Availability::Unknown;
    std::vector<ShareEntry> shares;
    bool listed = false;  /* `list` answered (its rows are real) */
    uint64_t gen = 0;     /* changes with every answer */
};

class ShareLister {
public:
    /* run: how a command is run (default run_capture; the scenes answer from data) */
    using Run = std::function<int(const std::vector<std::string> &argv, int timeout_ms, std::string &out)>;
    explicit ShareLister(std::string helper, Run run = Run());
    ~ShareLister();
    ShareLister(const ShareLister &) = delete;
    ShareLister &operator=(const ShareLister &) = delete;

    void refresh();                 /* asks again (at once; never blocks) */
    ShareSnapshot snapshot() const;
    void wait_idle();               /* the tests */

private:
    void loop();
    std::string helper_;
    Run run_;
    bool custom_ = false;
    mutable std::mutex mu_;
    std::condition_variable cv_, idle_cv_;
    bool want_ = false, busy_ = false, quit_ = false;
    ShareSnapshot snap_;
    std::thread th_;
};

/* the root runner: runs argv as root (the app: sudo through the toolkit's prompt) and calls done once, on any thread */
using RootRun = std::function<void(const std::vector<std::string> &argv, std::function<void(const RunResult &)> done)>;
/* the tests' root runner: argv run directly (no sudo) on a thread of its own; exit 0 Success, 124 TimedOut, else ExecFailed */
RootRun exec_root_runner(int timeout_ms);

class ShareOps {
public:
    ShareOps(ShareConfig cfg, RootRun run);
    bool busy() const { return busy_->load(); }
    /* `f` passed check_form. done(outcome) runs once, on the runner's thread; the password's copies are wiped before the run. */
    void add(AddForm f, std::function<void(const Outcome &)> done);
    void remove(const std::string &name, std::function<void(const Outcome &)> done);
    /* the last argv run (the tests: never a password in it) */
    std::vector<std::string> last_argv() const;

private:
    ShareConfig cfg_;
    RootRun run_;
    std::shared_ptr<std::atomic<bool>> busy_ = std::make_shared<std::atomic<bool>>(false);
    mutable std::mutex mu_;
    std::vector<std::string> last_argv_;
};

}  // namespace shares
}  // namespace vinyl
