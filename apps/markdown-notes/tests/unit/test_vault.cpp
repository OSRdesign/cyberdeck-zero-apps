// Host tests: the vault walk (dot folders, links, caps), folder listings, recent notes, name rules, the Open page's
// ranking, link resolution.
#include "test_common.hpp"

#include "fuzzy.hpp"
#include "links.hpp"
#include "names.hpp"
#include "vault.hpp"

#include <cerrno>

using namespace mdnotes;
using namespace notes_test;

namespace {

std::vector<std::string> rels(const VaultIndex &ix, const std::vector<int> &idx)
{
    std::vector<std::string> out;
    for (int i : idx) out.push_back(ix.notes[static_cast<std::size_t>(i)].rel);
    return out;
}

std::string join(const std::vector<std::string> &v)
{
    std::string s;
    for (const std::string &x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

const std::string kEte = "\xC3\x89t\xC3\xA9";   // "Été"

/* The walk fixture: notes with fixed mtimes, the things the walk must skip, a folder 13 levels deep. */
void make_vault(const TempDir &v)
{
    put(v / "Home.md", "# Home\n");
    set_mtime(v / "Home.md", 100);
    put(v / "Ideas.MD", "ideas\n");
    set_mtime(v / "Ideas.MD", 200);
    put(v / "readme.txt", "not a note");
    put(v / ".hidden.md", "skipped");
    put(v / ".obsidian/workspace.md", "skipped");
    put(v / ".trash/old.md", "skipped");
    put(v / ".git/x.md", "skipped");
    put(v / "Projects/Mesh/Antenna ideas.md", "a");
    set_mtime(v / "Projects/Mesh/Antenna ideas.md", 300);
    put(v / "Projects/Mesh/Mesh Hop notes.md", "m");
    set_mtime(v / "Projects/Mesh/Mesh Hop notes.md", 400);
    put(v / "Projects/Deck build log.md", "d");
    set_mtime(v / "Projects/Deck build log.md", 500);
    put(v / "Projects/.Deck build log.md.~deck", "a temp file");
    make_dirs(v / "Empty");
    CHECK(::symlink((v / "Projects").c_str(), (v / "link-to-projects").c_str()) == 0);
    CHECK(::symlink((v / "Home.md").c_str(), (v / "linked.md").c_str()) == 0);
    CHECK(::symlink("/tmp", (v / "outside").c_str()) == 0);
    std::string deep = v.path;
    for (int i = 1; i <= 13; ++i) deep += "/d" + std::to_string(i);
    put(deep + "/too deep.md", "x");                 // in d13: not reached
    put(v / "d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/d12/deep enough.md", "x");
    set_mtime(v / "d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/d12/deep enough.md", 50);
}

} // namespace

void test_walk()
{
    TempDir v;
    make_vault(v);
    VaultIndex ix = walk_vault(v.path + "/");
    CHECK(ix.ok);
    CHECK_EQ(ix.root, v.path);
    std::vector<std::string> all;
    for (const NoteEntry &n : ix.notes) all.push_back(n.rel);
    CHECK_EQ(join(all), std::string("Home.md | Ideas.MD | Projects/Deck build log.md | Projects/Mesh/Antenna ideas.md | "
                                    "Projects/Mesh/Mesh Hop notes.md | d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/d12/deep enough.md"));
    CHECK(ix.capped_depth && !ix.capped_notes);
    CHECK_EQ(ix.skipped_links, 3);
    const NoteEntry &ideas = ix.notes[1];
    CHECK(ideas.name == "Ideas" && ideas.folder.empty() && ideas.folded == "ideas" && ideas.mtime_ns == 200LL * 1000000000LL);
    CHECK(ideas.size == 6);
    const int mesh = ix.find_folder("Projects/Mesh");
    CHECK(mesh > 0);
    if (mesh > 0) CHECK(ix.folders[static_cast<std::size_t>(mesh)].notes == 2 && ix.folders[static_cast<std::size_t>(mesh)].depth == 2);
    const int proj = ix.find_folder("Projects");
    CHECK(proj > 0);
    if (proj > 0) {
        const FolderEntry &f = ix.folders[static_cast<std::size_t>(proj)];
        CHECK(f.notes == 1 && f.folders == 1 && f.total == 3 && f.name == "Projects");
    }
    CHECK_EQ(ix.folders[0].total, 6);
    CHECK_EQ(ix.folders[0].notes, 2);
    CHECK_EQ(ix.folders[0].folders, 3);   // Empty, Projects, d1 (no dot folders, no linked folder)
    CHECK(ix.find_folder("Empty") > 0 && ix.find_folder(".obsidian") < 0 && ix.find_folder("link-to-projects") < 0);
    CHECK(ix.find_folder("d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/d12") > 0);
    CHECK(ix.find_folder("d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/d12/d13") < 0);
    CHECK(ix.find_note("Home.md") == 0 && ix.find_note("linked.md") < 0);
    CHECK_EQ(ix.abs_path("Home.md"), v / "Home.md");

    // the note cap: deterministic (name order), flagged
    VaultIndex capped = walk_vault(v.path, WalkLimits{3, 12});
    CHECK(capped.notes.size() == 3 && capped.capped_notes);
    if (capped.notes.size() == 3) CHECK_EQ(capped.notes[2].rel, std::string("Projects/Deck build log.md"));
    VaultIndex shallow = walk_vault(v.path, WalkLimits{5000, 1});
    CHECK(shallow.find_folder("Projects/Mesh") < 0 && shallow.capped_depth && shallow.notes.size() == 3);

    // folder pages and recent notes
    FolderListing root = list_folder(ix, "");
    std::vector<std::string> fnames;
    for (int i : root.folders) fnames.push_back(ix.folders[static_cast<std::size_t>(i)].name);
    CHECK_EQ(join(fnames), std::string("d1 | Empty | Projects"));                // by name, ignoring case
    CHECK_EQ(join(rels(ix, root.notes)), std::string("Ideas.MD | Home.md"));   // newest first
    FolderListing pl = list_folder(ix, "Projects");
    CHECK(pl.folders.size() == 1 && ix.folders[static_cast<std::size_t>(pl.folders[0])].rel == "Projects/Mesh");
    CHECK_EQ(join(rels(ix, pl.notes)), std::string("Projects/Deck build log.md"));
    CHECK_EQ(join(rels(ix, list_folder(ix, "Projects/Mesh").notes)),
             std::string("Projects/Mesh/Mesh Hop notes.md | Projects/Mesh/Antenna ideas.md"));
    CHECK(list_folder(ix, "Nope").notes.empty());
    CHECK_EQ(join(rels(ix, recent_notes(ix, 3))),
             std::string("Projects/Deck build log.md | Projects/Mesh/Mesh Hop notes.md | Projects/Mesh/Antenna ideas.md"));
    CHECK_EQ(recent_notes(ix).size(), std::size_t(6));   // fewer than 20

    // a folder we cannot read is counted, the rest walked; a missing root
    if (::geteuid() != 0) {
        ::chmod((v / "Empty").c_str(), 0);
        VaultIndex u = walk_vault(v.path);
        CHECK(u.ok && u.unreadable == 1 && u.notes.size() == 6);
        ::chmod((v / "Empty").c_str(), 0755);
    }
    VaultIndex none = walk_vault(v / "missing");
    CHECK(!none.ok && none.notes.empty() && none.folders.size() == 1);
}

void test_names()
{
    CHECK_EQ(clean_typed_name("  Idea.md  "), std::string("Idea"));
    CHECK_EQ(clean_typed_name("Plan.MD"), std::string("Plan"));
    CHECK_EQ(clean_typed_name(" a b "), std::string("a b"));
    CHECK_EQ(clean_typed_name("Notes .md"), std::string("Notes"));
    CHECK_EQ(clean_typed_name(".md"), std::string(".md"));
    CHECK(check_name("Antenna ideas").ok());
    CHECK(check_name("\xC3\x9C" "n\xC3\xAF" "c\xC3\xB8" "d\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC (2) - draft, v1.2!").ok());
    CHECK(check_name("").error == NameError::Empty);
    CHECK(check_name(std::string(120, 'a')).ok());
    CHECK(check_name(std::string(121, 'a')).error == NameError::TooLong);
    CHECK(check_name(std::string(39, 'a') + "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" + std::string(73, 'b')).error == NameError::TooLong);   // 121 bytes, 115 letters
    for (const char *bad : {"/", "\\", ":", "*", "?", "\"", "<", ">", "|", "#", "^", "[", "]"}) {
        const NameCheck c = check_name(std::string("a") + bad + "b");
        CHECK(c.error == NameError::Forbidden && c.bad == bad);
    }
    CHECK(check_name("a\tb").error == NameError::ControlChar);
    CHECK(check_name("a\x7f").error == NameError::ControlChar);
    CHECK(check_name(".hidden").error == NameError::LeadingDot);
    CHECK(check_name("trail ").error == NameError::TrailingSpaceOrDot);
    CHECK(check_name("trail.").error == NameError::TrailingSpaceOrDot);
    for (const char *dev : {"CON", "con", "Prn", "aux", "NUL", "COM1", "com9", "LPT1", "lpt9", "nul.txt", "CON .x"})
        CHECK(check_name(dev).error == NameError::Reserved);
    for (const char *ok : {"com0", "console", "COM10", "auxiliary", "lpt"}) CHECK(check_name(ok).ok());
    CHECK(check_name("caf\xC3").error == NameError::NotUtf8);
    CHECK(check_note_path("Projects/Mesh/Idea").ok());
    CHECK(check_note_path("a//b").error == NameError::EmptyFolder);
    CHECK(check_note_path("/a").error == NameError::EmptyFolder);
    CHECK(check_note_path("a/../b").error == NameError::EmptyFolder);
    CHECK(check_note_path("").error == NameError::Empty);
    std::string twelve;
    for (int i = 0; i < 12; ++i) twelve += "f" + std::to_string(i) + "/";
    CHECK(check_note_path(twelve + "n").ok());
    CHECK(check_note_path(twelve + "x/n").error == NameError::TooDeep);
    NameCheck c = check_note_path("a/b:c");
    CHECK(c.error == NameError::Forbidden && c.bad == ":");
    c = check_note_path("a/.git/x");
    CHECK(c.error == NameError::LeadingDot && c.bad == ".git");
    CHECK_EQ(note_file_name("Idea"), std::string("Idea.md"));
    CHECK(is_note_file("a.md") && is_note_file("A.Md") && !is_note_file(".md") && !is_note_file("a.markdown") && !is_note_file("amd"));
    CHECK_EQ(strip_md("Idea.MD"), std::string("Idea"));
    CHECK_EQ(strip_md("Idea.txt"), std::string("Idea.txt"));
    // a collision, ignoring case, read from the folder itself
    TempDir v;
    put(v / "Home.md", "x");
    put(v / (kEte + ".md"), "x");
    CHECK_EQ(find_existing_ci(v.path, "home.MD"), std::string("Home.md"));
    CHECK_EQ(find_existing_ci(v.path, "\xC3\xA9t\xC3\xA9.md"), kEte + ".md");
    CHECK(find_existing_ci(v.path, "nope.md").empty());
    CHECK(find_existing_ci(v / "missing", "a.md").empty());
}

void test_fuzzy()
{
    TempDir v;
    make_vault(v);
    put(v / "Inbox/Meshtastic vs MeshCore.md", "x");
    put(v / (kEte + " notes.md"), "x");
    put(v / "plant care.md", "x");
    const VaultIndex ix = walk_vault(v.path);
    const auto name_of = [&](const Match &x) { return ix.notes[static_cast<std::size_t>(x.note)].name; };
    std::vector<Match> m = rank_notes(ix, "mesh ant");
    CHECK(m.size() == 1 && name_of(m[0]) == "Antenna ideas");   // "mesh" in its folder, "ant" in its name
    m = rank_notes(ix, "mesh");
    CHECK(m.size() == 3);
    if (m.size() == 3) {
        CHECK_EQ(name_of(m[0]), std::string("Mesh Hop notes"));          // a name prefix, the shorter name
        CHECK_EQ(name_of(m[1]), std::string("Meshtastic vs MeshCore"));
        CHECK_EQ(name_of(m[2]), std::string("Antenna ideas"));           // the folder only
    }
    CHECK(!has_exact(m));
    m = rank_notes(ix, " HOME ");
    CHECK(!m.empty() && m[0].exact && has_exact(m) && name_of(m[0]) == "Home");
    m = rank_notes(ix, "dbl");
    CHECK(!m.empty() && name_of(m[0]) == "Deck build log");   // word starts
    CHECK(rank_notes(ix, "").empty() && rank_notes(ix, "   ").empty());
    CHECK(rank_notes(ix, "zzz").empty());
    m = rank_notes(ix, "\xC3\xA9t\xC3\xA9");   // "été": Unicode folding
    CHECK(!m.empty() && name_of(m[0]) == kEte + " notes");
    for (const Match &x : rank_notes(ix, "ete")) CHECK(name_of(x) != kEte + " notes");   // no accent folding
    m = rank_notes(ix, "\xC3\xA9n");            // "én": a subsequence by code points, never by bytes
    CHECK(!m.empty() && name_of(m[0]) == kEte + " notes");
    CHECK(fuzzy_word_score("ant", "antenna ideas") > fuzzy_word_score("ant", "plant care"));
    CHECK(fuzzy_word_score("ide", "antenna ideas") > fuzzy_word_score("ide", "a widely"));
    CHECK(fuzzy_word_score("ai", "antenna ideas") > 0 && fuzzy_word_score("xa", "antenna ideas") < 0);
    CHECK(fuzzy_word_score("\xC3\xA9", "\xC3\xA8") < 0);   // é is not è, byte for byte either
    CHECK(rank_notes(ix, "e", 2).size() == 2);
}

void test_links()
{
    WikiTarget t = parse_wikilink("a#h|b");
    CHECK(t.note == "a" && t.heading == "h" && t.alias == "b");
    t = parse_wikilink(" Note | shown #x ");
    CHECK(t.note == "Note" && t.heading.empty() && t.alias == "shown #x");
    t = parse_wikilink("#Top");
    CHECK(t.note.empty() && t.heading == "Top");

    CHECK(classify_href("https://example.com/x") == LinkKind::External);
    CHECK(classify_href("mailto:a@b.c") == LinkKind::External);
    CHECK(classify_href("www.example.com") == LinkKind::External);
    CHECK(classify_href("me@example.com") == LinkKind::External);
    CHECK(classify_href("file:///etc/x.md") == LinkKind::External);
    CHECK(classify_href("Other%20note.md") == LinkKind::Note);
    CHECK(classify_href("../a/b.MD#h") == LinkKind::Note);
    CHECK(classify_href("#Heading") == LinkKind::Note);
    CHECK(classify_href("image.png") == LinkKind::File);
    CHECK(classify_href("docs/") == LinkKind::File);
    CHECK(classify_href("") == LinkKind::File);
    CHECK_EQ(url_decode("a%20b%zz%2"), std::string("a b%zz%2"));
    CHECK_EQ(url_decode("%C3%A9t%C3%A9"), std::string("\xC3\xA9t\xC3\xA9"));
    std::string n;
    CHECK(normalize_rel("a/./b/../c", n) && n == "a/c");
    CHECK(normalize_rel("a//b/", n) && n == "a/b");
    CHECK(!normalize_rel("../a", n));
    CHECK(!normalize_rel("a/../../b", n));

    TempDir v;
    for (const char *p : {"Home.md", "Ideas.md", "Projects/Plan.md", "Projects/Ideas.md", "Archive/2025/Plan.md",
                          "Archive/Old Plan.md", "Archive/2025/x.md"})
        put(v / p, "x");
    const VaultIndex ix = walk_vault(v.path);
    Resolved r = resolve_wikilink(ix, "Home.md", "Plan");
    CHECK(r.found && r.rel == "Projects/Plan.md");                 // the shallowest of two
    r = resolve_wikilink(ix, "Archive/2025/x.md", "plan");
    CHECK(r.found && r.rel == "Archive/2025/Plan.md");             // the linking note's folder first
    r = resolve_wikilink(ix, "Home.md", "IDEAS#Heading 2|shown");
    CHECK(r.found && r.rel == "Ideas.md" && r.heading == "Heading 2");
    r = resolve_wikilink(ix, "Projects/Plan.md", "Ideas");
    CHECK(r.found && r.rel == "Projects/Ideas.md");
    r = resolve_wikilink(ix, "Home.md", " Home ");
    CHECK(r.found && r.rel == "Home.md");
    r = resolve_wikilink(ix, "Projects/Plan.md", "#Steps");
    CHECK(r.same_note && r.found && r.rel == "Projects/Plan.md" && r.heading == "Steps");
    r = resolve_wikilink(ix, "Home.md", "Projects/Plan");
    CHECK(r.found && r.rel == "Projects/Plan.md");
    r = resolve_wikilink(ix, "Home.md", "projects/plan.md");
    CHECK(r.found && r.rel == "Projects/Plan.md");
    r = resolve_wikilink(ix, "Home.md", "2025/Plan");               // a path tail
    CHECK(r.found && r.rel == "Archive/2025/Plan.md");
    r = resolve_wikilink(ix, "Archive/2025/x.md", "./Plan");
    CHECK(r.found && r.rel == "Archive/2025/Plan.md");
    r = resolve_wikilink(ix, "Archive/2025/x.md", "../Old Plan");
    CHECK(r.found && r.rel == "Archive/Old Plan.md");
    r = resolve_wikilink(ix, "Projects/Plan.md", "/Ideas");          // from the vault root only
    CHECK(r.found && r.rel == "Ideas.md");
    // missing: the path to create
    r = resolve_wikilink(ix, "Projects/Plan.md", "New note#Part");
    CHECK(!r.found && !r.invalid && r.rel == "New note.md" && r.heading == "Part");   // the vault root (Obsidian's default)
    r = resolve_wikilink(ix, "Projects/Plan.md", "New note", NewNoteFolder::SameFolder);
    CHECK(!r.found && r.rel == "Projects/New note.md");
    r = resolve_wikilink(ix, "Home.md", "Inbox/Today");
    CHECK(!r.found && r.rel == "Inbox/Today.md");
    r = resolve_wikilink(ix, "Archive/2025/x.md", "../New");
    CHECK(!r.found && r.rel == "Archive/New.md");
    r = resolve_wikilink(ix, "Home.md", "/Inbox/Later");
    CHECK(!r.found && r.rel == "Inbox/Later.md");
    r = resolve_wikilink(ix, "Home.md", "bad:name");
    CHECK(r.invalid && !r.found);
    r = resolve_wikilink(ix, "Home.md", "../../x");
    CHECK(r.invalid);
    r = resolve_wikilink(ix, "Home.md", ".hidden");
    CHECK(r.invalid);
    r = resolve_wikilink(ix, "Home.md", "/");
    CHECK(r.invalid);

    r = resolve_note_href(ix, "Projects/Plan.md", "Ideas.md");
    CHECK(r.found && r.rel == "Projects/Ideas.md");
    r = resolve_note_href(ix, "Projects/Plan.md", "../Home.md");
    CHECK(r.found && r.rel == "Home.md");
    r = resolve_note_href(ix, "Projects/Plan.md", "/Archive/Old%20Plan.md");
    CHECK(r.found && r.rel == "Archive/Old Plan.md");
    r = resolve_note_href(ix, "Archive/2025/Plan.md", "Old%20Plan.md#Steps");
    CHECK(!r.found && !r.invalid && r.rel == "Archive/2025/Old Plan.md" && r.heading == "Steps");
    r = resolve_note_href(ix, "Projects/Plan.md", "<New idea.md>");
    CHECK(!r.found && r.rel == "Projects/New idea.md");
    r = resolve_note_href(ix, "Projects/Plan.md", "../../../x.md");
    CHECK(r.invalid);
    r = resolve_note_href(ix, "Projects/Plan.md", "bad%3Aname.md");
    CHECK(r.invalid);
    r = resolve_note_href(ix, "Projects/Plan.md", "#Heading");
    CHECK(r.same_note && r.rel == "Projects/Plan.md" && r.heading == "Heading");
    r = resolve_note_href(ix, "Projects/Plan.md", "IDEAS.MD");
    CHECK(r.found && r.rel == "Projects/Ideas.md");

    // the link under the editor's cursor
    const std::string text =
        "See [[Plan#Steps|the plan]] and [docs](Other%20note.md \"t\") end\nnext [[Home]] ![img](pic.png) [x](<a b.md>)";
    LinkAt la = link_at(text, 8);
    CHECK(la.found && la.wiki && la.target == "Plan#Steps|the plan" && la.begin == 4 && la.end == 27);
    CHECK(link_at(text, 27).found);                  // just after "]]"
    la = link_at(text, 34);
    CHECK(la.found && !la.wiki && la.target == "Other%20note.md");
    CHECK(!link_at(text, text.find(" end") + 2).found);
    la = link_at(text, text.find("[[Home") + 3);
    CHECK(la.found && la.wiki && la.target == "Home");
    la = link_at(text, text.find("![img"));
    CHECK(la.found && la.target == "pic.png");
    la = link_at(text, text.find("<a b"));
    CHECK(la.found && la.target == "a b.md");
    CHECK(!link_at(text, 0).found);
    CHECK(!link_at(text, text.size() + 1).found);
}
