/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: add and remove a network share through the launcher's helper (0.8.0, core/shares). The helper is a FAKE
 * script the test writes into a temporary folder and passes as ShareConfig::helper (the app always runs
 * /usr/libexec/cyberdeckzero/cyberdeckzero-share; nothing reads a path from the environment). The fake speaks the real
 * grammar and answers what FAKE_SHARE_REPLY says, after a line of "sudo" noise (the reply is the last line); it logs its
 * argv and what it found in a credentials file (mode, line counts: never the password) to FAKE_SHARE_LOG.
 * Covered: the allow-lists, the form (pasted addresses, defaults, refusals), the argv (no password ever), every reply word
 * and every result of the sudo prompt, the credentials file (0700 folder, 0600 file, O_EXCL, deleted after every run, also
 * when the helper left it), `version` and `list`, the share index and the watch, and the glyph state.
 */
#include "check.hpp"

#include "library/lib_model.hpp"
#include "library/library.hpp"
#include "library/library_ctl.hpp"
#include "library/roots.hpp"
#include "library/schema.hpp"
#include "shares/share_exec.hpp"
#include "shares/share_helper.hpp"
#include "shares/share_watch.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <future>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace vinyl;
using namespace vinyl::shares;

namespace {

const char *kFakeHelper = R"SH(#!/bin/sh
# Vinyl's fake cyberdeckzero-share (tests only): the real helper's grammar, the replies from the environment
log=${FAKE_SHARE_LOG:-/dev/null}
printf 'argv:' >> "$log"; for a in "$@"; do printf ' %s' "$a" >> "$log"; done; printf '\n' >> "$log"
verb=$1; [ $# -ge 1 ] && shift
case $verb in
version) echo "${FAKE_SHARE_VERSION:-ok 1}"; exit "${FAKE_SHARE_VERSION_EXIT:-0}" ;;
list) if [ -n "$FAKE_SHARE_LIST" ] && [ -f "$FAKE_SHARE_LIST" ]; then cat "$FAKE_SHARE_LIST"; fi; echo ok; exit 0 ;;
add|remove) ;;
*) echo "err usage unknown verb"; exit 2 ;;
esac
name= cred= user=
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || { echo "err usage missing value"; exit 2; }
  case $1 in
  --name) name=$2 ;; --host|--share|--subfolder) ;; --user) user=$2 ;; --credfile) cred=$2 ;;
  *) echo "err usage unknown option"; exit 2 ;;
  esac
  shift 2
done
[ -n "$name" ] || { echo "err usage --name is required"; exit 2; }
if [ -n "$cred" ]; then
  if [ -f "$cred" ] && [ ! -L "$cred" ]; then
    mode=$(stat -c %a "$cred"); dmode=$(stat -c %a "$(dirname "$cred")")
    u=$(grep -c "^username=$user\$" "$cred"); p=$(grep -c '^password=.' "$cred"); n=$(wc -l < "$cred")
    echo "cred mode=$mode dir=$dmode user=$u password=$p lines=$n" >> "$log"
    if [ "${FAKE_SHARE_DELETE_CRED:-0}" = 1 ]; then rm -f "$cred"; fi
  else
    echo "cred missing" >> "$log"
  fi
fi
[ -n "$FAKE_SHARE_SLEEP" ] && sleep "$FAKE_SHARE_SLEEP"
echo "[sudo] a line of sudo's before the reply"
reply=${FAKE_SHARE_REPLY:-ok}
if [ "$verb" = add ] && [ "$reply" = ok ]; then reply="ok path=/mnt/cyberdeck/$name"; fi
echo "$reply"
case $reply in ok*) exit 0 ;; "err usage"*) exit 2 ;; *) exit 1 ;; esac
)SH";

std::string make_fake(const std::string &dir) {
    const std::string p = dir + "/cyberdeckzero-share";
    write_file(p, kFakeHelper);
    ::chmod(p.c_str(), 0755);
    return p;
}

Outcome wait_add(ShareOps &ops, const AddForm &f) {
    auto pr = std::make_shared<std::promise<Outcome>>();
    std::future<Outcome> fu = pr->get_future();
    ops.add(f, [pr](const Outcome &o) { pr->set_value(o); });
    if (fu.wait_for(std::chrono::seconds(20)) != std::future_status::ready) return Outcome{false, "", "test-timeout", "", ""};
    return fu.get();
}

Outcome wait_remove(ShareOps &ops, const std::string &name) {
    auto pr = std::make_shared<std::promise<Outcome>>();
    std::future<Outcome> fu = pr->get_future();
    ops.remove(name, [pr](const Outcome &o) { pr->set_value(o); });
    if (fu.wait_for(std::chrono::seconds(20)) != std::future_status::ready) return Outcome{false, "", "test-timeout", "", ""};
    return fu.get();
}

int files_in(const std::string &dir) {
    DIR *d = ::opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    while (dirent *e = ::readdir(d))
        if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n;
}

int mode_of(const std::string &p) {
    struct stat st;
    return ::lstat(p.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 07777) : -1;
}

RunResult rr(SudoResult r, int code, const std::string &out) {
    RunResult x;
    x.result = r;
    x.exit_code = code;
    x.output = out;
    return x;
}

/* ---- the library side: the Music folders rows, the bar, Remove (D9) */

void add_tracks(LibraryWriter &w, int64_t root, const std::string &folder, int n) {
    for (int t = 1; t <= n; ++t) {
        TrackTags tg;
        tg.title = folder + " song " + std::to_string(t);
        tg.artist = "Artist " + folder;
        tg.album = "Album " + folder;
        tg.codec = "FLAC";
        tg.duration_ms = 180000;
        tg.ok = true;
        TrackFile f;
        f.root_id = root;
        f.folder_id = w.folder(root, folder, 1);
        f.name = std::to_string(t) + ".flac";
        f.mtime = 1;
        f.size = 10;
        w.add_track(f, tg);
    }
}

int tracks_in(const std::string &db_path) {
    Db db;
    std::string why;
    if (!open_library_db(db, db_path, 416, &why)) return -1;
    Library lib(db);
    return static_cast<int>(lib.counts().tracks);
}

void library_test(const std::string &dir) {
    using namespace vinyl::ui;
    std::string why;
    if (!real_sqlite_lib(&why)) {
        SKIP(("libsqlite3.so.0 not installed on the build box: " + why).c_str());
        return;
    }
    const std::string dbp = dir + "/library.db";
    const std::string share = "/mnt/cyberdeck/home-share", sub = share + "/Albums";
    {
        Db db;
        CHECK(open_library_db(db, dbp, 416, &why));
        LibraryWriter w(db);
        Transaction tx(db);
        add_tracks(w, w.root("/music", "Music"), "local", 3);
        add_tracks(w, w.root(share, "home-share"), "net", 4);
        add_tracks(w, w.root(sub, "Albums"), "deep", 2); /* a folder the user added inside the share */
        add_tracks(w, w.root("/mnt/cyberdeck/home-share2", "home-share2"), "other", 1); /* a name that starts the same */
        CHECK(tx.commit());
    }
    CHECK_EQ(tracks_in(dbp), 10);

    /* apply_shares on its own */
    {
        std::vector<RootInfo> roots(3);
        roots[0].path = "/music";
        roots[0].tracks = 3;
        roots[1].path = share;
        roots[1].tracks = 4;
        roots[2].path = "/mnt/nas";
        roots[2].tracks = 7;
        roots[2].network = true;
        std::vector<libm::Row> rows = libm::folder_rows(roots, "/home/pi");
        libm::ShareInfo si;
        si.known = si.available = true;
        si.shares.push_back({"home-share", "192.168.68.142 / home-share", share, false});
        si.shares.push_back({"office-nas", "192.168.68.20 / music", "/mnt/cyberdeck/office-nas", true});
        libm::apply_shares(rows, roots, si);
        CHECK_EQ(rows.size(), static_cast<size_t>(5));
        CHECK(rows[0].glyph == libm::Row::Glyph::None);
        CHECK_EQ(rows[1].text, std::string("home-share"));
        CHECK_EQ(rows[1].sub, std::string("192.168.68.142 / home-share"));
        CHECK_EQ(rows[1].value, std::string("4 tracks"));
        CHECK(rows[1].glyph == libm::Row::Glyph::Share);
        CHECK(rows[1].mark == libm::Row::Mark::Chevron);
        CHECK_EQ(rows[1].key, std::string("share:home-share"));
        CHECK(rows[2].glyph == libm::Row::Glyph::Share); /* the system's own mount: the glyph, its path kept */
        CHECK_EQ(rows[2].text, std::string("/mnt/nas"));
        CHECK_EQ(rows[3].text, std::string("office-nas")); /* no root yet: a row all the same */
        CHECK(rows[3].glyph == libm::Row::Glyph::ShareDown);
        CHECK(rows[3].sub_amber);
        CHECK_EQ(rows[4].key, std::string("addshare"));
        CHECK(rows[4].glyph == libm::Row::Glyph::Add);
        CHECK(rows[4].accent && !rows[4].dimmed);
        /* not answering: "Not answering: N tracks kept", amber, no count at the right */
        si.down_roots = {share, "/mnt/nas"};
        rows = libm::folder_rows(roots, "/home/pi");
        libm::apply_shares(rows, roots, si);
        CHECK(rows[1].glyph == libm::Row::Glyph::ShareDown);
        CHECK_EQ(rows[1].sub, std::string("Not answering: 4 tracks kept"));
        CHECK(rows[1].sub_amber);
        CHECK(rows[1].value.empty());
        CHECK(rows[2].glyph == libm::Row::Glyph::ShareDown);
        CHECK_EQ(rows[2].sub, std::string("Not answering: 7 tracks kept"));
        /* the helper is not there: the Add row says so, dimmed */
        libm::ShareInfo none;
        none.known = true;
        rows = libm::folder_rows(roots, "/home/pi");
        libm::apply_shares(rows, roots, none);
        CHECK(rows.back().dimmed);
        CHECK(!rows.back().sub.empty());
    }

    struct Rec {
        std::vector<std::string> notices, folders, browsed;
        std::vector<std::vector<std::string>> folder_calls;
        std::vector<std::pair<std::string, std::string>> removed;
        std::vector<std::string> confirms;
        int adds = 0;
        libm::ShareInfo si;
    } rec;
    rec.folders = {sub, "/elsewhere"};
    rec.si.known = rec.si.available = true;
    rec.si.shares.push_back({"home-share", "192.168.68.142 / home-share", share, false});
    rec.si.gen = 1;
    LibHost h;
    h.playing_path = [] { return std::string(); };
    h.notice = [&rec](Notice::Kind, const std::string &t) { rec.notices.push_back(t); };
    h.show = [](Page) {};
    h.wanted = [] { return std::vector<RootSpec>(); };
    h.folders = [&rec] { return rec.folders; };
    h.set_folders = [&rec](const std::vector<std::string> &f) {
        rec.folder_calls.push_back(f);
        rec.folders = f;
    };
    h.mini = [] { return MiniView(); };
    h.scroll = [](Page, int) {};
    h.scroll_y = [] { return 0; };
    h.ask_confirm = [&rec](const std::string &t, const std::string &b, const std::string &ok) { rec.confirms.push_back(t + "|" + b + "|" + ok); };
    h.shares = [&rec] { return rec.si; };
    h.add_share = [&rec] { ++rec.adds; };
    h.remove_share = [&rec](const std::string &n, const std::string &r) { rec.removed.push_back({n, r}); };
    h.browse_folder = [&rec](const std::string &f) { rec.browsed.push_back(f); };
    LibConfig cfg;
    cfg.db_path = dbp;
    cfg.ram_mb = 416;
    cfg.home = "/home/pi";
    cfg.demo = true;
    uint64_t now = 1000;
    LibraryCtl c(h, cfg);
    auto pump = [&](int rounds) {
        for (int i = 0; i < rounds; ++i) {
            c.wait_idle();
            now += 30;
            c.tick(now);
        }
    };
    c.enter_settings();
    pump(3);
    c.row(Page::LibSettings, 0);
    pump(3);
    CHECK_EQ(c.page(), Page::LibFolders);
    int share_row = -1, add_row = -1;
    for (size_t i = 0; i < c.rows().size(); ++i) {
        if (c.rows()[i].key == "share:home-share") share_row = static_cast<int>(i);
        if (c.rows()[i].key == "addshare") add_row = static_cast<int>(i);
    }
    CHECK(share_row >= 0);
    CHECK_EQ(add_row, static_cast<int>(c.rows().size()) - 1);
    LibView v = c.view();
    CHECK_EQ(v.heading, "MUSIC FOLDERS, " + std::to_string(c.rows().size() - 1));
    CHECK(!v.share_bar);
    CHECK(!v.buttons.empty());
    CHECK(v.rows && (*v.rows)[static_cast<size_t>(share_row)].glyph == RowView::Glyph::Share);
    CHECK_EQ((*v.rows)[static_cast<size_t>(share_row)].sub, std::string("192.168.68.142 / home-share"));
    /* a share selected: its bar (name, Rescan, Remove) in place of the buttons; a second tap opens it in the Browser */
    c.row(Page::LibFolders, share_row);
    v = c.view();
    CHECK(v.share_bar);
    CHECK_EQ(v.share_name, std::string("home-share"));
    CHECK(v.buttons.empty());
    CHECK(v.share_lines.size() >= 5);
    CHECK_EQ(v.share_lines[1], std::string("192.168.68.142"));
    CHECK_EQ(v.share_lines[2], std::string("Share: home-share"));
    CHECK_EQ(v.share_lines[3], std::string("Folder: /"));
    CHECK(v.share_lines[5].find("4 tracks") == 0);
    CHECK(rec.browsed.empty());
    c.row(Page::LibFolders, share_row);
    CHECK_EQ(rec.browsed.size(), static_cast<size_t>(1));
    CHECK_EQ(rec.browsed[0], share);
    /* the share's state changes: the page asks again (amber, "Not answering") */
    rec.si.down_roots = {share};
    rec.si.gen = 2;
    pump(1);
    CHECK(c.rows()[static_cast<size_t>(share_row)].glyph == libm::Row::Glyph::ShareDown);
    CHECK(c.view().share_down);
    /* Remove: the question first; No does nothing; Yes hands the share to the host (the sudo prompt, then the helper) */
    c.button(Page::LibFolders, 31);
    CHECK_EQ(rec.confirms.size(), static_cast<size_t>(1));
    CHECK(rec.confirms[0].find("Remove home-share?") == 0);
    CHECK(rec.confirms[0].find("4 tracks") != std::string::npos);
    c.confirm_answer(false);
    CHECK(rec.removed.empty());
    c.button(Page::LibFolders, 31);
    c.confirm_answer(true);
    CHECK_EQ(rec.removed.size(), static_cast<size_t>(1));
    CHECK_EQ(rec.removed[0].first, std::string("home-share"));
    CHECK_EQ(rec.removed[0].second, share);
    /* the helper said ok: the share's root, the root inside it and their rows go; the folder setting inside it goes; the other
     * share whose name starts the same and the local music stay */
    rec.si.shares.clear();
    rec.si.down_roots.clear();
    rec.si.gen = 3;
    c.share_removed(share);
    pump(4);
    CHECK_EQ(tracks_in(dbp), 4); /* 3 local + 1 of home-share2 */
    CHECK_EQ(rec.folder_calls.size(), static_cast<size_t>(1));
    CHECK_EQ(rec.folders.size(), static_cast<size_t>(1));
    CHECK_EQ(rec.folders[0], std::string("/elsewhere"));
    for (const libm::Row &r : c.rows()) CHECK(r.key != "share:home-share");
    /* Add: the form (the host's); the Add row of a launcher without the helper says why and opens nothing */
    for (size_t i = 0; i < c.rows().size(); ++i)
        if (c.rows()[i].key == "addshare") add_row = static_cast<int>(i);
    c.row(Page::LibFolders, add_row);
    CHECK_EQ(rec.adds, 1);
    rec.si.available = false;
    rec.si.gen = 4;
    pump(1);
    for (size_t i = 0; i < c.rows().size(); ++i)
        if (c.rows()[i].key == "addshare") add_row = static_cast<int>(i);
    CHECK(c.rows()[static_cast<size_t>(add_row)].dimmed);
    c.row(Page::LibFolders, add_row);
    CHECK_EQ(rec.adds, 1);
    CHECK(rec.notices.back().find("launcher update") != std::string::npos);
    /* a share added: its root is made (the scenes write it; a board syncs the mounts) and it is scanned */
    rec.si.available = true;
    rec.si.shares.push_back({"new-share", "nas / music", "/mnt/cyberdeck/new-share", false});
    rec.si.gen = 5;
    c.share_added("/mnt/cyberdeck/new-share");
    pump(4);
    bool found = false;
    for (const libm::Row &r : c.rows()) found = found || (r.key == "share:new-share" && r.index >= 0);
    CHECK(found);
    if (c.scan_thread()) c.scan_thread()->wait();
    /* remove_roots_under on its own: nothing for "/" or "", the count otherwise */
    {
        Db db;
        CHECK(open_library_db(db, dbp, 416, &why));
        CHECK_EQ(remove_roots_under(db, ""), 0);
        CHECK_EQ(remove_roots_under(db, "/"), 0);
        CHECK_EQ(remove_roots_under(db, "/mnt/cyberdeck/nothing"), 0);
        CHECK_EQ(remove_roots_under(db, "/mnt/cyberdeck/home-share2"), 1);
    }
    CHECK_EQ(tracks_in(dbp), 3);
}

}  // namespace

void test_shares() {
    /* ---- the allow-lists: the helper's own rules */
    for (const char *n : {"home-share", "a", "home_share", "_x", "NAS2", "abcdefghijabcdefghijabcdefghij12"}) CHECK(valid_name(n));
    for (const char *n : {"", "-x", "home share", "a.b", "a/b", "x$", "abcdefghijabcdefghijabcdefghij123", "caf\xc3\xa9"}) CHECK(!valid_name(n));
    for (const char *h : {"192.168.68.142", "0.0.0.0", "255.255.255.255", "nas", "nas.local", "zima-os.home.arpa", "a1"}) CHECK(valid_host(h));
    for (const char *h : {"", "192.168.068.142", "256.1.1.1", "1.2.3", "1.2.3.4.5", "nas_box", "-nas", "nas-", "nas..local", ".nas",
                          "nas.local.", "fe80::1", "nas:445", "1.2.3.4:445", "nas box"})
        CHECK(!valid_host(h));
    CHECK(!valid_host(std::string(254, 'a')));
    for (const char *s : {"home-share", "Music", "music$", "a.b", "x_y-z"}) CHECK(valid_share(s));
    for (const char *s : {"", ".", "..", "a..b", "a b", "a/b", "a\\b", "a,b", "caf\xc3\xa9"}) CHECK(!valid_share(s));
    CHECK(!valid_share(std::string(81, 'a')));
    for (const char *p : {"Music", "Music/Jazz", "a.b/c$/d@e+f=g-h", ".hidden"}) CHECK(valid_subfolder(p));
    for (const char *p : {"", "/Music", "Music/", "a//b", "..", "a/../b", ".", "./a", "a/.", "a/./b", "a b", "a,b"}) CHECK(!valid_subfolder(p));
    for (const char *u : {"pi", "music.reader", "user@nas", "_svc", "a-b"}) CHECK(valid_user(u));
    for (const char *u : {"", "-x", "a b", "a/b", "a:b"}) CHECK(!valid_user(u));
    CHECK(valid_password("s3cr3t, with a space"));
    CHECK(valid_password(std::string(256, 'x')));
    CHECK(!valid_password(""));
    CHECK(!valid_password(std::string(257, 'x')));
    CHECK(!valid_password("tab\there"));
    CHECK(!valid_password("line\nbreak"));
    CHECK(!valid_password(std::string("del\x7f")));

    /* ---- the names Vinyl makes */
    CHECK_EQ(make_name("home-share", {}), std::string("home-share"));
    CHECK_EQ(make_name("My NAS!", {}), std::string("My_NAS_"));
    CHECK_EQ(make_name("-x", {}), std::string("x"));
    CHECK_EQ(make_name("music$", {}), std::string("music_"));
    CHECK_EQ(make_name("", {}), std::string("share"));
    CHECK_EQ(make_name("caf\xc3\xa9", {}), std::string("caf_"));
    CHECK_EQ(make_name("home-share", {"home-share"}), std::string("home-share_2"));
    CHECK_EQ(make_name("home-share", {"home-share", "home-share_2"}), std::string("home-share_3"));
    CHECK_EQ(make_name(std::string(40, 'a'), {}).size(), static_cast<size_t>(32));
    CHECK_EQ(make_name(std::string(32, 'a'), {std::string(32, 'a')}), std::string(30, 'a') + "_2");
    for (const char *w : {"x y", "-", "...", "a/b", "\xe4\xb8\xad\xe6\x96\x87"}) CHECK(valid_name(make_name(w, {})));

    /* ---- the form */
    {
        AddForm f;
        f.host = "  //192.168.68.142/home-share/Music/Jazz/ ";
        FormCheck c = check_form(f, {});
        CHECK(c.ok);
        CHECK_EQ(f.host, std::string("192.168.68.142"));
        CHECK_EQ(f.share, std::string("home-share"));
        CHECK_EQ(f.subfolder, std::string("Music/Jazz"));
        CHECK_EQ(f.name, std::string("home-share"));
        CHECK(f.guest);
    }
    {
        AddForm f;
        f.host = "smb://nas.local/music";
        CHECK(check_form(f, {"music"}).ok);
        CHECK_EQ(f.host, std::string("nas.local"));
        CHECK_EQ(f.share, std::string("music"));
        CHECK_EQ(f.name, std::string("music_2")); /* the default name is made free */
    }
    {
        AddForm f;
        f.host = "\\\\NAS\\Public";
        CHECK(check_form(f, {}).ok);
        CHECK_EQ(f.host, std::string("NAS"));
        CHECK_EQ(f.share, std::string("Public"));
    }
    {
        AddForm f;
        f.host = "nas";
        f.share = "music";
        f.subfolder = "/";
        CHECK(check_form(f, {}).ok);
        CHECK_EQ(f.subfolder, std::string("")); /* "/": the whole share */
    }
    auto refused = [](AddForm f, Field want) {
        const FormCheck c = check_form(f, {"taken"});
        return !c.ok && c.field == want && !c.message.empty() && c.message.size() <= 60;
    };
    {
        AddForm f;
        CHECK(refused(f, Field::Host));
        f.host = "nas:445";
        CHECK(refused(f, Field::Host));
        f.host = "192.168.1.300";
        CHECK(refused(f, Field::Host));
        f.host = "nas";
        CHECK(refused(f, Field::Share));
        f.share = "my music";
        CHECK(refused(f, Field::Share));
        f.share = "music";
        f.subfolder = "a/../b";
        CHECK(refused(f, Field::Subfolder));
        f.subfolder = "";
        f.name = "taken";
        CHECK(refused(f, Field::Name));
        f.name = "Living room";
        CHECK(check_form(f, {"taken"}).ok); /* a typed name is made into one the helper takes */
        f.name = "";
        f.guest = false;
        CHECK(refused(f, Field::User));
        f.user = "bad user";
        CHECK(refused(f, Field::User));
        f.user = "reader";
        CHECK(refused(f, Field::Password));
        f.password = "tab\tin it";
        CHECK(refused(f, Field::Password));
        f.password = "fine pass, 1";
        CHECK(check_form(f, {}).ok);
    }
    {
        AddForm f;
        f.host = "nas";
        f.share = "music";
        f.user = "left over";
        f.password = "not used";
        CHECK(check_form(f, {}).ok); /* guest: the user fields are dropped, the password wiped */
        CHECK(f.user.empty());
        CHECK(f.password.empty());
    }

    /* ---- argv: never a password */
    {
        AddForm f;
        f.host = "192.168.68.142";
        f.share = "home-share";
        f.name = "home-share";
        const std::vector<std::string> g = add_argv("/h", f, "");
        CHECK_EQ(g.size(), static_cast<size_t>(8));
        CHECK(std::find(g.begin(), g.end(), "--user") == g.end());
        CHECK(std::find(g.begin(), g.end(), "--credfile") == g.end());
        f.subfolder = "Music";
        f.guest = false;
        f.user = "reader";
        f.password = "hunter2 secret";
        const std::vector<std::string> u = add_argv("/h", f, "/run/user/1000/cyberdeckzero-share/AbCdEfGh12.cred");
        CHECK_EQ(u.size(), static_cast<size_t>(14));
        CHECK_EQ(u[1], std::string("add"));
        CHECK_EQ(u[8], std::string("--subfolder"));
        CHECK_EQ(u[10], std::string("--user"));
        CHECK_EQ(u[12], std::string("--credfile"));
        for (const std::string &a : u) CHECK(a.find("hunter2") == std::string::npos);
        CHECK_EQ(remove_argv("/h", "x").size(), static_cast<size_t>(4));
        CHECK_EQ(version_argv("/h")[1], std::string("version"));
        CHECK_EQ(list_argv("/h")[1], std::string("list"));
    }

    /* ---- replies: the last non-empty line */
    {
        Reply r = parse_reply("[sudo] password for pi:\nSorry, try again.\nok path=/mnt/cyberdeck/home-share\n\n");
        CHECK(r.ok);
        CHECK_EQ(r.path, std::string("/mnt/cyberdeck/home-share"));
        r = parse_reply("ok 1\n");
        CHECK(r.ok);
        CHECK_EQ(r.rest, std::string("1"));
        r = parse_reply("err denied wrong share name, or it needs a login\n");
        CHECK(!r.ok);
        CHECK_EQ(r.word, std::string("denied"));
        CHECK_EQ(r.reason, std::string("wrong share name, or it needs a login"));
        r = parse_reply("err timeout\r\n");
        CHECK_EQ(r.word, std::string("timeout"));
        CHECK(r.reason.empty());
        r = parse_reply("err surprise something new");
        CHECK_EQ(r.word, std::string("other"));
        CHECK_EQ(r.reason, std::string("surprise something new"));
        r = parse_reply("");
        CHECK(!r.ok);
        CHECK_EQ(r.word, std::string("noreply"));
        r = parse_reply("Segmentation fault\n");
        CHECK_EQ(r.word, std::string("noreply"));
        CHECK_EQ(last_line("a\n  b  \n\n \n"), std::string("b"));
    }
    {
        std::vector<ShareEntry> l;
        CHECK(parse_list("home-share 192.168.68.142 home-share mounted\noffice nas.local music Jazz/Live idle\nnoise\n-bad name x y z\nok\n", l));
        CHECK_EQ(l.size(), static_cast<size_t>(2));
        CHECK_EQ(l[0].name, std::string("home-share"));
        CHECK_EQ(l[0].state, std::string("mounted"));
        CHECK_EQ(l[0].path(), std::string("/mnt/cyberdeck/home-share"));
        CHECK_EQ(l[0].where(), std::string("192.168.68.142 / home-share"));
        CHECK_EQ(l[1].subfolder, std::string("Jazz/Live"));
        CHECK_EQ(l[1].state, std::string("idle"));
        CHECK_EQ(l[1].where(), std::string("nas.local / music/Jazz/Live"));
        CHECK(parse_list("ok\n", l));
        CHECK(l.empty());
        CHECK(!parse_list("home-share 1.2.3.4 s mounted\n", l)); /* no closing ok */
        CHECK(!parse_list("", l));
    }

    /* ---- the lines shown: every word, every sudo result, short enough for 320 px */
    {
        for (const char *w : {"usage", "exists", "nomodule", "nohelper", "unreachable", "notfound", "denied", "timeout", "other", "noreply"}) {
            const std::string m = word_message(w, "", false, false);
            CHECK(!m.empty());
            CHECK(m.size() <= 70);
        }
        CHECK_EQ(word_message("nohelper", "", false, false), word_message("nomodule", "", false, false));
        CHECK(word_message("nohelper", "", false, false).find("cifs-utils") != std::string::npos);
        CHECK(word_message("denied", "", false, false) != word_message("denied", "", true, false)); /* guest or user */
        CHECK(word_message("denied", "wrong share name, or it needs a login", false, false).find("share name") != std::string::npos);
        CHECK(word_message("other", "mount error(22): Invalid argument", false, false).find("Invalid argument") != std::string::npos);
        CHECK(word_message("other", std::string(300, 'x'), false, false).size() <= 100);
        CHECK(word_message("other", "", false, true).find("remove") != std::string::npos);

        Outcome o = add_outcome(rr(SudoResult::Success, 0, "ok path=/mnt/cyberdeck/x\n"), false, "x");
        CHECK(o.ok);
        CHECK_EQ(o.path, std::string("/mnt/cyberdeck/x"));
        o = add_outcome(rr(SudoResult::ExecFailed, 1, "[sudo] noise\nerr unreachable could not connect\n"), false, "x");
        CHECK(!o.ok);
        CHECK_EQ(o.word, std::string("unreachable"));
        CHECK(o.log.find("could not connect") != std::string::npos);
        o = add_outcome(rr(SudoResult::AuthFailed, 1, "Sorry, try again.\n"), true, "x");
        CHECK_EQ(o.word, std::string("sudo-auth"));
        CHECK(o.message.find("Wrong password") != std::string::npos);
        o = add_outcome(rr(SudoResult::Cancelled, -125, ""), true, "x");
        CHECK_EQ(o.word, std::string("sudo-cancel"));
        o = add_outcome(rr(SudoResult::TimedOut, -110, ""), true, "x");
        CHECK_EQ(o.word, std::string("sudo-timeout"));
        o = add_outcome(rr(SudoResult::NoPrompt, -1, ""), true, "x");
        CHECK_EQ(o.word, std::string("noprompt"));
        o = add_outcome(rr(SudoResult::ExecFailed, 1, "pi is not in the sudoers file.\n"), false, "x");
        CHECK_EQ(o.word, std::string("sudo-denied"));
        o = add_outcome(rr(SudoResult::Success, 0, "ok 1\n"), false, "x"); /* an ok without a path: the path the helper always uses */
        CHECK(o.ok);
        CHECK_EQ(o.path, std::string("/mnt/cyberdeck/x"));
        o = remove_outcome(rr(SudoResult::Success, 0, "ok\n"), "x");
        CHECK(o.ok);
        o = remove_outcome(rr(SudoResult::ExecFailed, 1, "err other that unit was not made by the share helper\n"), "x");
        CHECK(!o.ok);
        CHECK(o.message.find("remove") != std::string::npos);
        o = remove_outcome(rr(SudoResult::Cancelled, 0, ""), "x");
        CHECK_EQ(o.word, std::string("sudo-cancel"));
    }

    const std::string dir = make_temp_dir("shares");
    const std::string runtime = dir + "/run-user";
    ::mkdir(runtime.c_str(), 0700);
    const std::string secret = "Hunter2, a pass phrase";

    /* ---- the credentials file */
    {
        const CredFile a = write_credfile(runtime, "reader", secret);
        CHECK(a.ok);
        CHECK_EQ(mode_of(runtime + "/cyberdeckzero-share"), 0700);
        CHECK_EQ(mode_of(a.path), 0600);
        CHECK_EQ(read_file(a.path), "username=reader\npassword=" + secret + "\n");
        const std::string name = a.path.substr(a.path.rfind('/') + 1);
        CHECK(name.size() >= 8 + 5 && name.size() <= 64 + 5); /* <8-64 of A-Za-z0-9_->.cred: the helper's rule */
        CHECK(name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") == std::string::npos);
        const CredFile b = write_credfile(runtime, "reader", secret);
        CHECK(b.ok);
        CHECK(a.path != b.path);
        CHECK_EQ(read_file(a.path), "username=reader\npassword=" + secret + "\n"); /* untouched */
        remove_credfile(a.path);
        remove_credfile(b.path);
        CHECK_EQ(mode_of(a.path), -1);
        CHECK_EQ(files_in(runtime + "/cyberdeckzero-share"), 0);
        /* only a credentials file of that folder is ever deleted */
        write_file(dir + "/keep.cred", "x");
        write_file(runtime + "/cyberdeckzero-share/keep.txt", "x");
        remove_credfile(dir + "/keep.cred");
        remove_credfile(runtime + "/cyberdeckzero-share/keep.txt");
        remove_credfile("");
        CHECK_EQ(read_file(dir + "/keep.cred"), std::string("x"));
        CHECK_EQ(read_file(runtime + "/cyberdeckzero-share/keep.txt"), std::string("x"));
        ::unlink((runtime + "/cyberdeckzero-share/keep.txt").c_str());
        /* a folder with loose rights is set 0700 again; a link in its place is refused */
        ::chmod((runtime + "/cyberdeckzero-share").c_str(), 0755);
        const CredFile c = write_credfile(runtime, "reader", secret);
        CHECK(c.ok);
        CHECK_EQ(mode_of(runtime + "/cyberdeckzero-share"), 0700);
        remove_credfile(c.path);
        const std::string other = dir + "/run-other";
        ::mkdir(other.c_str(), 0700);
        ::mkdir((dir + "/elsewhere").c_str(), 0700);
        CHECK(::symlink((dir + "/elsewhere").c_str(), (other + "/cyberdeckzero-share").c_str()) == 0);
        const CredFile d = write_credfile(other, "reader", secret);
        CHECK(!d.ok);
        CHECK(d.error.find(secret) == std::string::npos);
        CHECK_EQ(files_in(dir + "/elsewhere"), 0);
        CHECK(!write_credfile(runtime, "bad user", secret).ok);
        CHECK(!write_credfile(runtime, "reader", "").ok);
    }

    /* ---- Add and Remove with the fake helper (run directly: the sudo prompt is ui/'s) */
    const std::string helper = make_fake(dir);
    const std::string log = dir + "/helper.log";
    ::setenv("FAKE_SHARE_LOG", log.c_str(), 1);
    ShareConfig cfg;
    cfg.helper = helper;
    cfg.runtime = runtime;
    {
        ShareOps ops(cfg, exec_root_runner(10000));
        AddForm g;
        g.host = "192.168.68.142";
        g.share = "home-share";
        CHECK(check_form(g, {}).ok);
        ::setenv("FAKE_SHARE_REPLY", "ok", 1);
        Outcome o = wait_add(ops, g);
        CHECK(o.ok);
        CHECK_EQ(o.path, std::string("/mnt/cyberdeck/home-share"));
        CHECK(read_file(log).find("argv: add --name home-share --host 192.168.68.142 --share home-share\n") != std::string::npos);
        CHECK(!ops.busy());

        /* with a user: the credentials file is there while the helper runs (0600, folder 0700, the two lines), gone after */
        AddForm u = g;
        u.guest = false;
        u.user = "reader";
        u.password = secret;
        u.name = "office";
        CHECK(check_form(u, {"home-share"}).ok);
        ::setenv("FAKE_SHARE_DELETE_CRED", "0", 1); /* the helper leaves it: Vinyl deletes it */
        o = wait_add(ops, u);
        CHECK(o.ok);
        const std::string l = read_file(log);
        CHECK(l.find("cred mode=600 dir=700 user=1 password=1 lines=2") != std::string::npos);
        CHECK(l.find("--user reader --credfile " + runtime + "/cyberdeckzero-share/") != std::string::npos);
        CHECK(l.find(secret) == std::string::npos);
        CHECK(l.find("Hunter2") == std::string::npos);
        CHECK_EQ(files_in(runtime + "/cyberdeckzero-share"), 0);
        for (const std::string &a : ops.last_argv()) CHECK(a.find("Hunter2") == std::string::npos);
        CHECK(o.message.find("Hunter2") == std::string::npos && o.log.find("Hunter2") == std::string::npos);
        ::setenv("FAKE_SHARE_DELETE_CRED", "1", 1); /* the helper deleted it: nothing left either */
        o = wait_add(ops, u);
        CHECK(o.ok);
        CHECK_EQ(files_in(runtime + "/cyberdeckzero-share"), 0);

        /* every word: the outcome, the line, the file gone also after a failure */
        struct W {
            const char *reply, *word;
        };
        const W words[] = {{"err usage bad --host", "usage"}, {"err exists a share with that name is there already", "exists"},
                           {"err nomodule cifs filesystem not supported", "nomodule"}, {"err nohelper mount.cifs is not installed", "nohelper"},
                           {"err unreachable could not connect", "unreachable"}, {"err notfound", "notfound"},
                           {"err denied", "denied"}, {"err denied wrong share name, or it needs a login", "denied"},
                           {"err timeout", "timeout"}, {"err other mount error(22): Invalid argument", "other"}, {"err whatever new", "other"}};
        for (const W &w : words) {
            ::setenv("FAKE_SHARE_REPLY", w.reply, 1);
            o = wait_add(ops, u);
            CHECK(!o.ok);
            CHECK_EQ(o.word, std::string(w.word));
            CHECK(!o.message.empty());
            CHECK(o.message.find("Hunter2") == std::string::npos);
            CHECK_EQ(files_in(runtime + "/cyberdeckzero-share"), 0);
            o = wait_add(ops, g); /* guest: the "denied" line is the guest's */
            CHECK_EQ(o.word, std::string(w.word));
        }
        ::setenv("FAKE_SHARE_REPLY", "err denied", 1);
        CHECK(wait_add(ops, u).message != wait_add(ops, g).message);
        CHECK(read_file(log).find("Hunter2") == std::string::npos);

        /* one at a time */
        ::setenv("FAKE_SHARE_REPLY", "ok", 1);
        ::setenv("FAKE_SHARE_SLEEP", "1", 1);
        auto pr = std::make_shared<std::promise<Outcome>>();
        auto fu = pr->get_future();
        ops.add(g, [pr](const Outcome &x) { pr->set_value(x); });
        CHECK(ops.busy());
        const Outcome second = wait_add(ops, g);
        CHECK_EQ(second.word, std::string("busy"));
        CHECK(fu.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
        CHECK(fu.get().ok);
        ::unsetenv("FAKE_SHARE_SLEEP");

        /* remove: ok (also when not there: the helper is idempotent), a refusal */
        ::setenv("FAKE_SHARE_REPLY", "ok", 1);
        o = wait_remove(ops, "home-share");
        CHECK(o.ok);
        CHECK(read_file(log).find("argv: remove --name home-share\n") != std::string::npos);
        ::setenv("FAKE_SHARE_REPLY", "err other that unit was not made by the share helper", 1);
        o = wait_remove(ops, "home-share");
        CHECK(!o.ok);
        CHECK_EQ(o.word, std::string("other"));
    }
    {
        /* a time limit on the run: the runner's TimedOut */
        ::setenv("FAKE_SHARE_REPLY", "ok", 1);
        ::setenv("FAKE_SHARE_SLEEP", "3", 1);
        ShareOps ops(cfg, exec_root_runner(500));
        AddForm g;
        g.host = "nas";
        g.share = "music";
        CHECK(check_form(g, {}).ok);
        const Outcome o = wait_add(ops, g);
        CHECK(!o.ok);
        CHECK_EQ(o.word, std::string("sudo-timeout"));
        ::unsetenv("FAKE_SHARE_SLEEP");
        /* no runner (a build without the prompt): said so, nothing run */
        ShareOps none(cfg, RootRun());
        CHECK_EQ(wait_add(none, g).word, std::string("noprompt"));
    }

    /* ---- version and list (as the user) */
    {
        const std::string listing = dir + "/list.txt";
        write_file(listing, "home-share 192.168.68.142 home-share mounted\noffice-nas 192.168.68.20 music Albums failed\n");
        ::setenv("FAKE_SHARE_LIST", listing.c_str(), 1);
        ShareLister ls(helper);
        CHECK(ls.snapshot().avail == Availability::Unknown);
        ls.refresh();
        ls.wait_idle();
        ShareSnapshot s = ls.snapshot();
        CHECK(s.avail == Availability::Ok);
        CHECK(s.listed);
        CHECK_EQ(s.shares.size(), static_cast<size_t>(2));
        CHECK_EQ(s.shares[1].name, std::string("office-nas"));
        CHECK_EQ(s.shares[1].state, std::string("failed"));
        const uint64_t g1 = s.gen;
        ::setenv("FAKE_SHARE_VERSION", "ok 0", 1);
        ls.refresh();
        ls.wait_idle();
        CHECK(ls.snapshot().avail == Availability::OldHelper);
        CHECK(ls.snapshot().gen > g1);
        ::setenv("FAKE_SHARE_VERSION", "err usage", 1);
        ::setenv("FAKE_SHARE_VERSION_EXIT", "2", 1);
        ls.refresh();
        ls.wait_idle();
        CHECK(ls.snapshot().avail == Availability::NoHelper);
        CHECK(ls.snapshot().shares.empty());
        ::unsetenv("FAKE_SHARE_VERSION");
        ::unsetenv("FAKE_SHARE_VERSION_EXIT");
        ShareLister missing(dir + "/no-such-helper");
        missing.refresh();
        missing.wait_idle();
        CHECK(missing.snapshot().avail == Availability::NoHelper);
        /* a lister that answers from data (the scenes) */
        ShareLister data("/fake", [](const std::vector<std::string> &a, int, std::string &out) {
            out = a[1] == "version" ? "ok 1\n" : "x 1.2.3.4 s idle\nok\n";
            return 0;
        });
        data.refresh();
        data.wait_idle();
        CHECK(data.snapshot().avail == Availability::Ok);
        CHECK_EQ(data.snapshot().shares.size(), static_cast<size_t>(1));
        ::unsetenv("FAKE_SHARE_LIST");
    }
    ::unsetenv("FAKE_SHARE_REPLY");
    ::unsetenv("FAKE_SHARE_LOG");
    ::unsetenv("FAKE_SHARE_DELETE_CRED");
    {
        std::string out;
        CHECK_EQ(run_capture({"/bin/sh", "-c", "echo a; echo b >&2; exit 3"}, 5000, out), 3);
        CHECK(out.find('a') != std::string::npos && out.find('b') != std::string::npos);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_EQ(run_capture({"/bin/sh", "-c", "sleep 5"}, 300, out), 124);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
        CHECK_EQ(run_capture({dir + "/nothing-here"}, 1000, out), 127);
    }

    /* ---- which paths are on a share: the helper's root, the system's network mounts */
    {
        ShareIndex idx;
        std::vector<ShareEntry> listed(1);
        listed[0].name = "home-share";
        listed[0].host = "192.168.68.142";
        listed[0].share = "home-share";
        idx.update("/dev/root / ext4 rw 0 0\n//nas/m /mnt/nas cifs rw 0 0\nsystemd-1 /mnt/cyberdeck/home-share autofs rw 0 0\n"
                   "/dev/sda1 /media/pi/USB vfat rw 0 0\nnas:/x /srv/nfs nfs4 rw 0 0\n",
                   "/mnt/cyberdeck", listed);
        CHECK_EQ(idx.root_of("/mnt/cyberdeck/home-share/Album/1.flac"), std::string("/mnt/cyberdeck/home-share"));
        CHECK_EQ(idx.root_of("/mnt/cyberdeck/gone/a.mp3"), std::string("/mnt/cyberdeck/gone")); /* under the root even when not listed */
        CHECK_EQ(idx.root_of("/mnt/nas/a.mp3"), std::string("/mnt/nas"));
        CHECK_EQ(idx.root_of("/srv/nfs/a/b.mp3"), std::string("/srv/nfs"));
        CHECK_EQ(idx.root_of("/mnt/nasty/a.mp3"), std::string(""));
        CHECK_EQ(idx.root_of("/media/pi/USB/a.mp3"), std::string(""));
        CHECK_EQ(idx.root_of("/home/pi/Music/a.mp3"), std::string(""));
        CHECK_EQ(idx.root_of(""), std::string(""));
        CHECK_EQ(idx.name_of("/mnt/cyberdeck/home-share"), std::string("home-share"));
        CHECK_EQ(idx.name_of("/mnt/nas"), std::string("nas"));
        CHECK(idx.entry_of("/mnt/cyberdeck/home-share") != nullptr);
        CHECK(idx.entry_of("/mnt/nas") == nullptr);
        const std::vector<std::string> rs = idx.roots();
        CHECK(std::find(rs.begin(), rs.end(), "/mnt/cyberdeck/home-share") != rs.end());
        CHECK(std::find(rs.begin(), rs.end(), "/mnt/nas") != rs.end());
        CHECK(std::find(rs.begin(), rs.end(), "/srv/nfs") != rs.end());
        CHECK_EQ(std::count(rs.begin(), rs.end(), std::string("/mnt/cyberdeck/home-share")), static_cast<long>(1));
        /* the glyph */
        CHECK(glyph_for(idx, ShareState::Unknown, "/home/pi/Music/a.mp3") == Glyph::None);
        CHECK(glyph_for(idx, ShareState::Down, "/home/pi/Music/a.mp3") == Glyph::None);
        CHECK(glyph_for(idx, ShareState::Unknown, "/mnt/nas/a.mp3") == Glyph::Share); /* not asked yet: teal */
        CHECK(glyph_for(idx, ShareState::Ok, "/mnt/nas/a.mp3") == Glyph::Share);
        CHECK(glyph_for(idx, ShareState::Down, "/mnt/cyberdeck/home-share/a.mp3") == Glyph::ShareDown);
        ShareIndex demo;
        demo.update("", "/tmp/demo/mnt/cyberdeck/", {});
        CHECK_EQ(demo.root_of("/tmp/demo/mnt/cyberdeck/x/a.mp3"), std::string("/tmp/demo/mnt/cyberdeck/x"));
    }

    /* ---- the watch: a probe on its own thread, states and their generation */
    {
        std::atomic<bool> up{true};
        std::atomic<int> asked{0};
        ShareWatch::Config wc;
        wc.ok_every_ms = 60;
        wc.down_every_ms = 40;
        wc.tick_ms = 10;
        wc.probe = [&](const std::string &root) {
            ++asked;
            return root == "/a" ? up.load() : false;
        };
        ShareWatch w(wc);
        CHECK(w.state("/a") == ShareState::Unknown);
        const uint64_t g0 = w.gen();
        w.set_roots({"/a", "/b"});
        auto wait_for = [&](const std::string &r, ShareState st) {
            for (int i = 0; i < 300 && w.state(r) != st; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return w.state(r) == st;
        };
        CHECK(wait_for("/a", ShareState::Ok));
        CHECK(wait_for("/b", ShareState::Down));
        CHECK(w.gen() > g0);
        const uint64_t g1 = w.gen();
        up = false;
        CHECK(wait_for("/a", ShareState::Down));
        CHECK(w.gen() > g1);
        up = true;
        CHECK(wait_for("/a", ShareState::Ok));
        w.set_roots({"/a"});
        CHECK(w.state("/b") == ShareState::Unknown); /* forgotten */
        w.set_state("/c", ShareState::Down);
        CHECK(w.state("/c") == ShareState::Down);
        const int before = asked.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        CHECK(asked.load() > before); /* "/a" keeps being asked */
        w.stop();
        w.stop(); /* twice is fine */
    }
    library_test(dir);
    rm_tree(dir);
}
