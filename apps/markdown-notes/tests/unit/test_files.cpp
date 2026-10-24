// Host tests: loading and saving notes (byte-identical, caps), the atomic save and its failures, the changed-on-disk
// check, Keep both, new notes, the trash, the settings.
#include "test_common.hpp"

#include "dates.hpp"
#include "note_file.hpp"
#include "settings.hpp"
#include "trash.hpp"
#include "vault.hpp"

#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>

using namespace mdnotes;
using namespace notes_test;

namespace {
constexpr std::time_t kNow = 1792339200;   // 2026-10-18 16:00 UTC

bool exists(const std::string &p) { return stat_path(p, false).exists; }
} // namespace

void test_load_save()
{
    TempDir tmp;
    const std::string p = tmp / "note.md";
    const std::vector<std::string> files = {
        "# Plain\n\ntext\n",
        "\xEF\xBB\xBF" "# BOM\r\n\r\n- [ ] a\r\n",
        "- a\n\t- b\n",
        "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80",
        "",
    };
    for (const std::string &raw : files) {
        CHECK(put(p, raw));
        LoadedNote n = load_note(p);
        CHECK(n.status == LoadStatus::Ok && n.editable() && n.readable());
        CHECK(n.stamp.exists && n.stamp.size == static_cast<std::int64_t>(raw.size()));
        // the editor's round trip, saved untouched: the same bytes on disk
        const std::string edit = to_edit(n.body.text, n.body.fmt);
        const SaveResult r = save_note(p, from_edit(edit, n.body.fmt), n.body.fmt, n.stamp);
        CHECK(r.status == SaveStatus::Ok && !r.recreated);
        CHECK(get(p) == raw);
        CHECK(!exists(temp_path_for(p)));
    }
    // an edit in a BOM + CRLF note keeps both
    CHECK(put(p, "\xEF\xBB\xBF" "a\r\n"));
    LoadedNote n = load_note(p);
    CHECK(save_note(p, n.body.text + "b\n", n.body.fmt, n.stamp).status == SaveStatus::Ok);
    CHECK_EQ(get(p), std::string("\xEF\xBB\xBF" "a\r\nb\r\n"));
    // caps
    CHECK(put(p, std::string(kEditCap, 'a')));
    CHECK(load_note(p).status == LoadStatus::Ok);
    CHECK(put(p, std::string(kEditCap + 1, 'a')));
    n = load_note(p);
    CHECK(n.status == LoadStatus::ReadOnlyLarge && n.readable() && !n.editable() && n.body.text.size() == kEditCap + 1);
    CHECK(put(p, std::string(kReadCap, 'a')));
    CHECK(load_note(p).status == LoadStatus::ReadOnlyLarge);
    CHECK(put(p, std::string(kReadCap + 1, 'a')));
    n = load_note(p);
    CHECK(n.status == LoadStatus::TooBig && !n.readable() && n.body.text.empty());
    // not UTF-8: readable, never editable
    CHECK(put(p, "caf\xE9 latin-1\n"));
    n = load_note(p);
    CHECK(n.status == LoadStatus::ReadOnlyNotUtf8 && n.readable() && !n.editable());
    CHECK(load_note(tmp / "missing.md").status == LoadStatus::Missing);
    make_dirs(tmp / "dir.md");
    n = load_note(tmp / "dir.md");
    CHECK(n.status == LoadStatus::Error && n.err == EISDIR);
}

void test_atomic_save()
{
    TempDir tmp;
    const std::string p = tmp / "n.md";
    CHECK_EQ(temp_path_for("/v/Sub/n.md"), std::string("/v/Sub/.n.md.~deck"));
    CHECK_EQ(temp_path_for("n.md"), std::string(".n.md.~deck"));
    int err = 0;
    CHECK(write_file_atomic(p, "one\n", &err) && err == 0);
    CHECK_EQ(get(p), std::string("one\n"));
    CHECK_EQ(stat_path(p).mode, 0644u);
    // the old file's mode is kept
    ::chmod(p.c_str(), 0600);
    CHECK(write_file_atomic(p, "two\n"));
    CHECK_EQ(stat_path(p).mode, 0600u);
    CHECK_EQ(get(p), std::string("two\n"));
    // a temp file left by a crash does not stop the next save, and goes
    CHECK(put(temp_path_for(p), "junk from a crash"));
    CHECK(write_file_atomic(p, "three\n"));
    CHECK(!exists(temp_path_for(p)));
    std::vector<DirEntry> entries;
    CHECK(list_dir(tmp.path, entries) && entries.size() == 1 && entries[0].name == "n.md");
    // O_EXCL creation
    CHECK(!create_file_exclusive(p, "x", &err) && err == EEXIST);
    CHECK_EQ(get(p), std::string("three\n"));

    // a read-only folder: refused, the old file intact, nothing left behind (root ignores permissions: skipped)
    if (::geteuid() != 0) {
        const std::string ro = tmp / "ro";
        CHECK(make_dirs(ro));
        CHECK(put(ro + "/n.md", "old\n"));
        ::chmod(ro.c_str(), 0555);
        CHECK(!write_file_atomic(ro + "/n.md", "new\n", &err));
        CHECK(err == EACCES);
        CHECK_EQ(get(ro + "/n.md"), std::string("old\n"));
        CHECK(!exists(temp_path_for(ro + "/n.md")));
        DiskStamp st;
        LoadedNote n = load_note(ro + "/n.md");
        st = n.stamp;
        const SaveResult r = save_note(ro + "/n.md", "new\n", n.body.fmt, st);
        CHECK(r.status == SaveStatus::Error && r.err == EACCES);
        CHECK(st.hash == n.stamp.hash);   // the stamp still describes the file on disk
        ::chmod(ro.c_str(), 0755);
    }

    // the disk full while writing (a file size limit, as `ulimit -f`): in a child process, which gets EFBIG
    CHECK(put(p, "small old note\n"));
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl;
        rl.rlim_cur = rl.rlim_max = 4096;
        ::setrlimit(RLIMIT_FSIZE, &rl);
        int e = 0;
        const bool ok = write_file_atomic(p, std::string(64 * 1024, 'x'), &e);
        ::_exit(ok ? 1 : (e == EFBIG ? 0 : 2));
    }
    int status = -1;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK_EQ(get(p), std::string("small old note\n"));
    CHECK(!exists(temp_path_for(p)));
}

void test_disk_check()
{
    TempDir tmp;
    const std::string p = tmp / "Deck log.md";
    CHECK(put(p, "abc\n"));
    set_mtime(p, 1000);
    LoadedNote n = load_note(p);
    DiskStamp st = n.stamp;
    CHECK(check_disk(p, st) == DiskState::Same);
    // touched, same bytes: not a change; the stamp follows the new mtime
    set_mtime(p, 2000);
    CHECK(check_disk(p, st) == DiskState::Same);
    CHECK_EQ(st.mtime_ns, 2000LL * 1000000000LL);
    // the same size and even the same mtime, other bytes (a coarse-mtime file system): the hash sees it
    CHECK(put(p, "abd\n"));
    set_mtime(p, 2000);
    CHECK(check_disk(p, st) == DiskState::Changed);
    // a save is refused then, the other text untouched
    SaveResult r = save_note(p, "mine\n", n.body.fmt, st);
    CHECK(r.status == SaveStatus::Conflict);
    CHECK_EQ(get(p), std::string("abd\n"));
    // Keep both: ours as a copy beside it, then theirs is read again
    const std::string copy = conflict_copy_path(p, kNow - 30 * 60);
    CHECK_EQ(copy, tmp / "Deck log (deck 2026-10-18 1530).md");
    int err = 0;
    DiskStamp copy_stamp;
    CHECK(create_note(copy, "mine\n", n.body.fmt, &copy_stamp, &err));
    CHECK_EQ(get(copy), std::string("mine\n"));
    CHECK_EQ(conflict_copy_path(p, kNow - 30 * 60), tmp / "Deck log (deck 2026-10-18 1530) 2.md");
    n = load_note(p);
    st = n.stamp;
    CHECK_EQ(n.body.text, std::string("abd\n"));
    // a size change
    CHECK(put(p, "longer text\n"));
    CHECK(check_disk(p, st) == DiskState::Changed);
    // force (the user chose to overwrite)
    r = save_note(p, "forced\n", n.body.fmt, st, true);
    CHECK(r.status == SaveStatus::Ok);
    CHECK_EQ(get(p), std::string("forced\n"));
    CHECK(check_disk(p, st) == DiskState::Same);   // our own save is not a change
    // gone: deleted by a sync; a save writes it again
    ::unlink(p.c_str());
    CHECK(check_disk(p, st) == DiskState::Gone);
    r = save_note(p, "back\n", n.body.fmt, st);
    CHECK(r.status == SaveStatus::Ok && r.recreated);
    CHECK_EQ(get(p), std::string("back\n"));
    // its folder gone too
    const std::string deep = tmp / "Sub/n.md";
    CHECK(put(deep, "x\n"));
    LoadedNote dn = load_note(deep);
    remove_tree(tmp / "Sub");
    r = save_note(deep, "y\n", dn.body.fmt, dn.stamp);
    CHECK(r.status == SaveStatus::Ok && r.recreated && get(deep) == "y\n");
    // a stamp of a note never written, and a file appeared there: a conflict, not an overwrite
    DiskStamp none;
    CHECK(put(tmp / "appeared.md", "theirs\n"));
    CHECK(save_note(tmp / "appeared.md", "ours\n", TextFormat(), none).status == SaveStatus::Conflict);
    CHECK(content_hash("a") != content_hash("b") && content_hash("") == 1469598103934665603ULL);
}

void test_create_note()
{
    TempDir tmp;
    int err = 0;
    DiskStamp st;
    const std::string p = tmp / "Projects/Mesh/New idea.md";
    CHECK(create_note(p, "# New idea\n", TextFormat(), &st, &err));
    CHECK_EQ(get(p), std::string("# New idea\n"));
    CHECK(st.exists && st.size == 11);
    CHECK(!create_note(p, "other\n", TextFormat(), nullptr, &err) && err == EEXIST);
    CHECK_EQ(get(p), std::string("# New idea\n"));
    TextFormat crlf;
    crlf.write_crlf = true;
    CHECK(create_note(tmp / "w.md", "a\nb\n", crlf, nullptr, &err));
    CHECK_EQ(get(tmp / "w.md"), std::string("a\r\nb\r\n"));
}

void test_trash()
{
    TempDir v;
    CHECK(put(v / "a.md", "root a\n"));
    CHECK(put(v / "Sub/a.md", "sub a\n"));
    CHECK(put(v / "Sub/b.md", "b\n"));
    TrashResult r = move_to_trash(v.path, "a.md");
    CHECK(r.ok && r.trashed == v / ".trash/a.md");
    CHECK(!exists(v / "a.md"));
    CHECK_EQ(get(v / ".trash/a.md"), std::string("root a\n"));
    r = move_to_trash(v.path, "Sub/a.md");
    CHECK(r.ok && r.trashed == v / ".trash/a 2.md");
    CHECK_EQ(get(v / ".trash/a 2.md"), std::string("sub a\n"));
    r = move_to_trash(v.path, "Sub/missing.md");
    CHECK(!r.ok && r.err == ENOENT);
    r = move_to_trash(v.path, "Sub");
    CHECK(!r.ok && r.err == EISDIR);
    // the trash is a dot folder: out of every list
    const VaultIndex ix = walk_vault(v.path);
    CHECK(ix.notes.size() == 1 && ix.notes[0].rel == "Sub/b.md");
    CHECK_EQ(free_trash_path(v / ".trash", "a.md"), v / ".trash/a 3.md");
    CHECK(put(v / ".trash/x", ""));
    CHECK_EQ(free_trash_path(v / ".trash", "x"), v / ".trash/x 2");
}

void test_settings()
{
    Settings s = parse_settings("# comment\nvault = ~/Work/Notes \ntext_size=large\nopen_in=editor\njunk line\nunknown=1\n");
    CHECK_EQ(s.vault, std::string("~/Work/Notes"));
    CHECK(s.text_size == TextSize::Large && s.open_in == OpenIn::Editor);
    s = parse_settings("");
    CHECK(s.vault == "~/Notes" && s.text_size == TextSize::Medium && s.open_in == OpenIn::Preview);
    s = parse_settings("text_size=huge\nopen_in=web\nvault=\r\n");
    CHECK(s.vault == "~/Notes" && s.text_size == TextSize::Medium && s.open_in == OpenIn::Preview);
    s.vault = "/data/vault";
    s.text_size = TextSize::Small;
    const Settings back = parse_settings(format_settings(s));
    CHECK(back.vault == "/data/vault" && back.text_size == TextSize::Small && back.open_in == OpenIn::Preview);
    CHECK_EQ(std::string(text_size_key(TextSize::Large)), std::string("large"));
    CHECK_EQ(std::string(open_in_key(OpenIn::Editor)), std::string("editor"));

    TempDir tmp;
    const char *old_xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string saved_xdg = old_xdg ? old_xdg : "";
    ::setenv("XDG_CONFIG_HOME", tmp.path.c_str(), 1);
    CHECK_EQ(settings_path("/home/u"), tmp / "markdown-notes/settings.conf");
    ::unsetenv("XDG_CONFIG_HOME");
    CHECK_EQ(settings_path("/home/u"), std::string("/home/u/.config/markdown-notes/settings.conf"));
    if (old_xdg) ::setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
    const std::string sp = tmp / "cfg/markdown-notes/settings.conf";
    CHECK(save_settings(sp, s));
    const Settings loaded = load_settings(sp);
    CHECK(loaded.vault == s.vault && loaded.text_size == s.text_size && loaded.open_in == s.open_in);
    CHECK(load_settings(tmp / "nothing.conf").vault == "~/Notes");

    CHECK_EQ(expand_path("~/Notes", "/home/u", "/x"), std::string("/home/u/Notes"));
    CHECK_EQ(expand_path("~", "/home/u", "/x"), std::string("/home/u"));
    CHECK_EQ(expand_path("Docs/Notes/", "/home/u", "/home/u"), std::string("/home/u/Docs/Notes"));
    CHECK_EQ(expand_path("/abs/vault//", "/home/u", "/x"), std::string("/abs/vault"));
    Settings d;
    CHECK_EQ(resolve_vault(d, nullptr, "/home/u"), std::string("/home/u/Notes"));
    CHECK_EQ(resolve_vault(d, "", "/home/u"), std::string("/home/u/Notes"));
    CHECK_EQ(resolve_vault(d, "/tmp/v", "/home/u"), std::string("/tmp/v"));   // NOTES_VAULT wins
    CHECK_EQ(resolve_vault(d, "rel", "/home/u"), path_join(current_dir(), "rel"));
    d.vault = "Docs/Notes";
    CHECK_EQ(resolve_vault(d, nullptr, "/home/u"), std::string("/home/u/Docs/Notes"));

    // the vault folder: created on first start, then ready; a file in its place; a folder we cannot write
    const std::string vp = tmp / "home/Notes";
    VaultOpen vo = open_vault(vp);
    CHECK(vo.state == VaultState::Created && stat_path(vp).is_dir);
    CHECK(open_vault(vp).state == VaultState::Ready);
    CHECK(put(tmp / "file", "x"));
    vo = open_vault(tmp / "file");
    CHECK(vo.state == VaultState::NotAFolder);
    if (::geteuid() != 0) {
        CHECK(make_dirs(tmp / "locked"));
        ::chmod((tmp / "locked").c_str(), 0555);
        vo = open_vault(tmp / "locked/Notes");
        CHECK(vo.state == VaultState::Unusable && vo.err == EACCES);
        vo = open_vault(tmp / "locked");
        CHECK(vo.state == VaultState::Unusable);
        ::chmod((tmp / "locked").c_str(), 0755);
    }
}
