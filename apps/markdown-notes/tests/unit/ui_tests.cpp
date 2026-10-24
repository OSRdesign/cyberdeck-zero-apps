// Host tests of Markdown Notes' UI-independent logic (src/ui: ui_logic, keys, key_bridge): the rows and lines of the pages,
// the words for refused names, the menus, the text sizes, the preview's layout decisions and ring, links, the editor's
// offsets, the dialogs' key rules. No LVGL. Built and run by run_tests.sh after the core tests.
#include "test_common.hpp"

#include "key_bridge.hpp"
#include "keys.hpp"
#include "ui_logic.hpp"

#include "markdown.hpp"
#include "names.hpp"
#include "text_codec.hpp"
#include "vault.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>

using namespace mdnotes;
using namespace notesui;
using namespace notes_test;

int g_fail = 0, g_checks = 0;

namespace {

constexpr std::int64_t kNow = 1792850580;   // 2026-10-24 14:03 UTC

KeyEvent key(Key k)
{
    KeyEvent e;
    e.key = k;
    return e;
}

KeyEvent ch(const std::string &t, bool ctrl = false)
{
    KeyEvent e;
    e.key = Key::Char;
    e.text = t;
    e.ctrl = ctrl;
    return e;
}

const ListItem *find_id(const std::vector<ListItem> &rows, const std::string &id)
{
    for (const ListItem &r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

std::string ids(const std::vector<ListItem> &rows)
{
    std::string s;
    for (const ListItem &r : rows) s += (s.empty() ? "" : " | ") + (r.section ? "[" + r.label + "]" : r.id);
    return s;
}

/* A small vault with fixed mtimes (seconds before kNow). */
void make_vault(const TempDir &v)
{
    put(v / "Deck build log.md", "# Deck\n");
    set_mtime(v / "Deck build log.md", kNow - 7200);
    put(v / "Welcome.md", "# Welcome\n");
    set_mtime(v / "Welcome.md", kNow - 86400 - 60);
    put(v / "Projects/Antenna ideas.md", "x\n");
    set_mtime(v / "Projects/Antenna ideas.md", kNow - 3 * 86400);
    put(v / "Projects/Mesh/Mesh antenna ideas.md", "x\n");
    set_mtime(v / "Projects/Mesh/Mesh antenna ideas.md", kNow - 300);
    put(v / "Journal/2026-10-23.md", "x\n");
    set_mtime(v / "Journal/2026-10-23.md", kNow - 40000);
    put(v / ".trash/Old.md", "x\n");
}

void test_keys()
{
    KeyTranslator t(Layout::US);
    KeyEvent e;
    CHECK(t.feed(30, 1, e) && e.key == Key::Char && e.text == "a");
    CHECK(!t.feed(29, 1, e));                       // ctrl down
    CHECK(t.feed(25, 1, e) && e.ctrl && e.text == "p");
    CHECK(is_ctrl_letter(e, 'p') && is_ctrl_letter(e, 'P') && !is_ctrl_letter(e, 'o'));
    CHECK(t.feed(28, 1, e) && e.key == Key::Enter && e.ctrl);      // Ctrl+Enter
    CHECK(!t.feed(29, 0, e));
    CHECK(t.feed(15, 1, e) && e.key == Key::Tab);
    CHECK(!t.feed(42, 1, e));
    CHECK(t.feed(15, 1, e) && e.key == Key::BackTab);
    CHECK(!t.feed(42, 0, e));
    KeyTranslator fr(Layout::FR);
    CHECK(fr.feed(16, 1, e) && e.text == "a");      // AZERTY
    KeyBridge b;
    // the on-screen keyboard's Ctrl + a letter: the code, Ctrl, no text
    CHECK(b.feed(25, KeyBridge::kPressed, KeyBridge::kModCtrl, 0, "", e) && is_ctrl_letter(e, 'p'));
    CHECK(b.feed(28, KeyBridge::kPressed, KeyBridge::kModCtrl, 0, "\r", e) && e.key == Key::Enter && e.ctrl);
    CHECK(b.feed(0, KeyBridge::kPressed, 0, 0, "\xC3\xA9", e) && e.key == Key::Char && e.text == "\xC3\xA9");
    CHECK(b.feed(15, KeyBridge::kPressed, KeyBridge::kModShift, 0, "", e) && e.key == Key::BackTab);
    CHECK(is_typing(ch("x")) && !is_typing(ch("x", true)));
}

void test_dialog_keys()
{
    Choice c = text_size_choice(TextSize::Medium);
    CHECK_EQ(c.index, 1);
    CHECK(choice_key(key(Key::Right), c) == ChoiceResult::Moved && c.label() == "Large");
    CHECK(choice_key(key(Key::Right), c) == ChoiceResult::None);
    CHECK(choice_key(key(Key::Enter), c) == ChoiceResult::Accept);
    CHECK_EQ(c.position(), std::string("3 / 3"));
    int focus = 0;
    CHECK(confirm_key(key(Key::Right), focus) == ConfirmResult::Moved && focus == 1);
    CHECK(confirm_key(key(Key::Enter), focus) == ConfirmResult::Yes);
    focus = 0;
    CHECK(confirm_key(key(Key::Enter), focus) == ConfirmResult::No);
    CHECK(confirm_key(ch("y"), focus) == ConfirmResult::Yes);
    CHECK(confirm_key(key(Key::Esc), focus) == ConfirmResult::No);
    int sel = 0;
    CHECK(menu_key(key(Key::Down), sel, 4) == MenuResult::Moved && sel == 1);
    CHECK(menu_key(key(Key::End), sel, 4) == MenuResult::Moved && sel == 3);
    CHECK(menu_key(key(Key::Esc), sel, 4) == MenuResult::Cancel);
    HoldTracker h;
    h.begin("note:a.md", 10, 10, 1000);
    CHECK(!h.visible(1400) && h.visible(1600));
    h.move(20, 15);
    CHECK(h.active());
    CHECK(!h.poll(3999) && h.poll(4000) && h.take_swallow() && !h.take_swallow());
    h.begin("x", 0, 0, 0);
    h.move(40, 0);
    CHECK(!h.active());
    // list_move over sections
    std::vector<bool> section = {false, true, false, false, true, false};
    auto skip = [&](int i) { return section[static_cast<std::size_t>(i)]; };
    sel = 0;
    CHECK(list_move(key(Key::Down), sel, 6, 4, skip) && sel == 2);
    sel = 3;
    CHECK(list_move(key(Key::Down), sel, 6, 4, skip) && sel == 5);
    CHECK(list_move(key(Key::Up), sel, 6, 4, skip) && sel == 3);
    CHECK(list_move(key(Key::Home), sel, 6, 4, skip) && sel == 0);
    CHECK(list_move(key(Key::End), sel, 6, 4, skip) && sel == 5);
    CHECK(!list_move(key(Key::Enter), sel, 6, 4, skip));
    const PageColumn pc = page_column(1280, 20, 640, 960);
    CHECK(pc.w == 960 && pc.x == 160);
    CHECK_EQ(page_column(640, 20, 640, 960).w, 600);
}

void test_formatting()
{
    CHECK_EQ(plural(1, "note", "notes"), std::string("1 note"));
    CHECK_EQ(plural(184, "note", "notes"), std::string("184 notes"));
    CHECK_EQ(fmt_size(342), std::string("342 bytes"));
    CHECK_EQ(fmt_size(1), std::string("1 byte"));
    CHECK_EQ(fmt_size(3482), std::string("3.4 KB"));
    CHECK_EQ(fmt_size(131072), std::string("128 KB"));
    CHECK_EQ(fmt_size(1258291), std::string("1.2 MB"));
    CHECK_EQ(tilde_path("/home/osr/Notes", "/home/osr"), std::string("~/Notes"));
    CHECK_EQ(tilde_path("/home/osrx/Notes", "/home/osr"), std::string("/home/osrx/Notes"));
    CHECK_EQ(tilde_path("/srv/notes", "/home/osr"), std::string("/srv/notes"));
    CHECK_EQ(edited_text((kNow - 7200) * 1000000000LL, kNow), std::string("edited 2 h ago"));
    CHECK_EQ(edited_text(0, kNow), std::string(""));
    CHECK_EQ(truncate_text("\xC3\xA9t\xC3\xA9 long", 3), std::string("\xC3\xA9t\xC3\xA9..."));
}

void test_home_rows()
{
    const std::vector<ListItem> rows = home_rows(184, "2026-10-24");
    CHECK_EQ(ids(rows), std::string("open | all | recent | today | new | settings"));
    CHECK_EQ(rows[0].label, std::string("Open or create\xE2\x80\xA6"));
    CHECK_EQ(rows[0].value, std::string("Ctrl+P"));
    CHECK_EQ(rows[1].value, std::string("184"));
    CHECK_EQ(rows[2].value, std::string("last 20"));
    CHECK_EQ(rows[3].label, std::string("Today\xE2\x80\x99s note"));
    CHECK_EQ(rows[3].value, std::string("2026-10-24"));
    CHECK_EQ(rows[4].value, std::string("+"));
    CHECK(rows[4].aff == Affordance::Action);           // New note acts (asks a name)
    for (int i : {0, 1, 2, 3, 5}) CHECK(rows[static_cast<std::size_t>(i)].aff == Affordance::Open);
    CHECK_EQ(home_caption("~/Notes", 184), std::string("Vault: ~/Notes  \xC2\xB7  184 notes"));
    CHECK(home_hint().find("Esc: quit") != std::string::npos && folder_hint().find("Ctrl+O") != std::string::npos);
}

void test_folder_rows()
{
    TempDir v;
    make_vault(v);
    const VaultIndex ix = walk_vault(v.path);
    std::vector<ListItem> root = folder_rows(ix, "", kNow);
    CHECK_EQ(ids(root), std::string("new | [FOLDERS] | folder:Journal | folder:Projects | [NOTES] | note:Deck build log.md | note:Welcome.md"));
    CHECK(root[0].aff == Affordance::Action && root[0].label == "New note here" && root[0].value == "+");
    const ListItem *p = find_id(root, "folder:Projects");
    CHECK(p && p->value == "2" && p->aff == Affordance::Open);    // notes in it and below
    const ListItem *d = find_id(root, "note:Deck build log.md");
    CHECK(d && d->label == "Deck build log" && d->line2 == "edited 2 h ago" && d->aff == Affordance::Open);
    const ListItem *w = find_id(root, "note:Welcome.md");
    CHECK(w && w->line2 == "edited yesterday");
    std::vector<ListItem> proj = folder_rows(ix, "Projects", kNow);
    CHECK_EQ(ids(proj), std::string("new | [FOLDERS] | folder:Projects/Mesh | [NOTES] | note:Projects/Antenna ideas.md"));
    std::vector<ListItem> empty = folder_rows(ix, "Nowhere", kNow);
    CHECK_EQ(ids(empty), std::string("new"));
    CHECK_EQ(folder_caption(12, 2), std::string("12 notes  \xC2\xB7  2 folders"));
    CHECK_EQ(folder_caption(1, 0), std::string("1 note"));
    CHECK_EQ(folder_caption(0, 0), std::string("No notes here yet"));
    const std::vector<std::string> t0 = folder_title("");
    CHECK(t0.size() == 1 && t0[0] == "All notes");
    const std::vector<std::string> t2 = folder_title("Projects/Mesh");
    CHECK(t2.size() == 3 && t2[0] == "Notes" && t2[1] == "Projects" && t2[2] == "Mesh");
    std::vector<ListItem> recent = recent_rows(ix, kNow, 3);
    CHECK_EQ(ids(recent), std::string("note:Projects/Mesh/Mesh antenna ideas.md | note:Deck build log.md | note:Journal/2026-10-23.md"));
    CHECK_EQ(recent[0].line2, std::string("edited 5 min ago  \xC2\xB7  Projects/Mesh"));
    CHECK_EQ(recent_caption(3), std::string("The 3 notes edited last"));
    CHECK_EQ(recent_caption(0), std::string("No notes yet"));
}

void test_names_words()
{
    NameCheck c;
    for (NameError e : {NameError::Empty, NameError::TooLong, NameError::Forbidden, NameError::ControlChar, NameError::LeadingDot,
                        NameError::TrailingSpaceOrDot, NameError::Reserved, NameError::NotUtf8, NameError::TooDeep, NameError::EmptyFolder}) {
        c.error = e;
        c.bad = e == NameError::Forbidden ? ":" : "CON";
        const std::string w = name_error_text(c);
        CHECK(!w.empty() && w.back() == '.');
    }
    c.error = NameError::Forbidden;
    c.bad = "#";
    CHECK(name_error_text(c).find("\"#\"") != std::string::npos);
    c.error = NameError::None;
    CHECK(name_error_text(c).empty());

    TempDir v;
    make_vault(v);
    NewTarget t = new_note_target(v.path, "", "  Field log.md ");
    CHECK(t.ok && t.rel == "Field log.md" && t.name == "Field log");
    t = new_note_target(v.path, "Projects", "Mast");
    CHECK(t.ok && t.rel == "Projects/Mast.md");
    t = new_note_target(v.path, "Projects", "Trips/2026/Alps");          // a path: from the vault root
    CHECK(t.ok && t.rel == "Trips/2026/Alps.md");
    t = new_note_target(v.path, "", "deck BUILD log");                   // taken, ignoring case
    CHECK(!t.ok && t.exists && t.existing_rel == "Deck build log.md");
    t = new_note_target(v.path, "Projects", "antenna ideas");
    CHECK(t.exists && t.existing_rel == "Projects/Antenna ideas.md");
    t = new_note_target(v.path, "", "bad:name");
    CHECK(!t.ok && !t.exists && t.error.find("\":\"") != std::string::npos);
    t = new_note_target(v.path, "", "con");
    CHECK(!t.ok && t.error.find("\"con\"") != std::string::npos);
    t = new_note_target(v.path, "", "a//b");
    CHECK(!t.ok && !t.error.empty());
    t = new_note_target(v.path, "", "   ");
    CHECK(!t.ok && t.error == "Type a name first.");
    t = new_note_target(v.path, "", ".hidden");
    CHECK(!t.ok && t.error.find("dot") != std::string::npos);
}

void test_open_rows()
{
    TempDir v;
    make_vault(v);
    const VaultIndex ix = walk_vault(v.path);
    std::vector<ListItem> r = open_rows(ix, v.path, "mesh ant", kNow);
    CHECK(!r.empty() && r[0].id == "create" && r[0].label == "Create \xE2\x80\x9Cmesh ant\xE2\x80\x9D" && r[0].aff == Affordance::Action);
    CHECK(r[0].line2.empty() && !r[0].warn);
    CHECK(r.size() >= 2 && r[1].id == "note:Projects/Mesh/Mesh antenna ideas.md" && r[1].line2 == "Projects/Mesh");
    r = open_rows(ix, v.path, "welcome", kNow);                        // an exact name: no Create row
    CHECK(!r.empty() && r[0].id == "note:Welcome.md");
    CHECK(find_id(r, "create") == nullptr);
    r = open_rows(ix, v.path, "a:b", kNow);
    CHECK(!r.empty() && r[0].id == "create" && r[0].warn && r[0].line2.find("\":\"") != std::string::npos);
    r = open_rows(ix, v.path, "Trips/Alps", kNow);
    CHECK(!r.empty() && r[0].id == "create" && r[0].line2 == "in Trips");
    r = open_rows(ix, v.path, "", kNow);                               // nothing typed: the recent notes
    CHECK(r.size() == 5 && r[0].id == "note:Projects/Mesh/Mesh antenna ideas.md");
    CHECK_EQ(open_caption(true, 0), std::string("Type a name, Enter opens"));
    CHECK_EQ(open_caption(false, 3), std::string("Type a name, Enter opens"));
    CHECK_EQ(open_caption(false, 0), std::string("No note by that name: Enter creates it"));
}

void test_settings_rows()
{
    CHECK(text_size_from_index(0) == TextSize::Small && text_size_from_index(1) == TextSize::Medium && text_size_from_index(2) == TextSize::Large);
    CHECK_EQ(text_size_label(TextSize::Large), std::string("Large"));
    Choice o = open_in_choice(OpenIn::Editor);
    CHECK(o.index == 1 && o.labels.size() == 2 && o.label() == "Editor" && !o.note.empty());
    CHECK(open_in_from_index(0) == OpenIn::Preview && open_in_from_index(1) == OpenIn::Editor);
    auto id = [](int px) { return px; };
    // the deck's text sizes: caption 18, body 24, heading 28, title 32
    CHECK_EQ(preview_text_px(TextSize::Small, 18, 24, 28, 32, id), 18);
    CHECK_EQ(preview_text_px(TextSize::Medium, 18, 24, 28, 32, id), 24);
    CHECK_EQ(preview_text_px(TextSize::Large, 18, 24, 28, 32, id), 30);
    CHECK_EQ(editor_text_px(TextSize::Small, 18, 24, id), 18);
    CHECK_EQ(editor_text_px(TextSize::Medium, 18, 24, id), 20);
    CHECK_EQ(editor_text_px(TextSize::Large, 18, 24, id), 24);
    std::vector<ListItem> s = settings_rows("~/Notes", false, TextSize::Medium, OpenIn::Preview, "0.1.0");
    CHECK_EQ(ids(s), std::string("[VAULT] | vault | [DISPLAY] | text_size | open_in | [ABOUT] | version"));
    const ListItem *vault = find_id(s, "vault");
    CHECK(vault && vault->label == "Folder" && vault->value == "~/Notes" && vault->aff == Affordance::Info && vault->line2.empty());
    CHECK(find_id(s, "text_size")->aff == Affordance::Edit && find_id(s, "text_size")->value == "Medium");
    CHECK(find_id(s, "open_in")->value == "Preview");
    CHECK(find_id(s, "version")->aff == Affordance::Info && find_id(s, "version")->value == "0.1.0");
    s = settings_rows("/srv/n", true, TextSize::Small, OpenIn::Editor, "0.1.0");
    CHECK_EQ(find_id(s, "vault")->line2, std::string("Set by NOTES_VAULT"));
    CHECK_EQ(settings_caption("0.1.0"), std::string("Markdown Notes 0.1.0"));
}

void test_note_pages()
{
    CHECK_EQ(editor_title("Deck build log", true), std::string("Deck build log \xE2\x97\x8F"));
    CHECK_EQ(editor_title("Deck build log", false), std::string("Deck build log"));
    // the line under the title ends with the way to the options: a hold without a keyboard, Ctrl+O with one
    CHECK_EQ(options_hint(false), std::string("Hold 3 s: options"));
    CHECK_EQ(options_hint(true), std::string("Ctrl+O: options"));
    CHECK_EQ(editor_caption(342, false), std::string("Editor  \xC2\xB7  342 bytes  \xC2\xB7  Hold 3 s: options"));
    CHECK_EQ(editor_caption(342, true), std::string("Editor  \xC2\xB7  342 bytes  \xC2\xB7  Ctrl+O: options"));
    CHECK_EQ(preview_caption(LoadStatus::Ok, (kNow - 7200) * 1000000000LL, kNow, false),
             std::string("Preview  \xC2\xB7  edited 2 h ago  \xC2\xB7  Hold 3 s: options"));
    CHECK_EQ(preview_caption(LoadStatus::ReadOnlyLarge, 0, kNow, true),
             std::string("Preview  \xC2\xB7  read only, over 128 KB  \xC2\xB7  Ctrl+O: options"));
    CHECK_EQ(preview_caption(LoadStatus::ReadOnlyNotUtf8, 0, kNow, false),
             std::string("Preview  \xC2\xB7  read only, not UTF-8  \xC2\xB7  Hold 3 s: options"));
    CHECK_EQ(preview_caption(LoadStatus::Ok, 0, kNow, true), std::string("Preview  \xC2\xB7  Ctrl+O: options"));
    CHECK(readonly_text(LoadStatus::ReadOnlyLarge).find("128 KB") != std::string::npos);
    CHECK(readonly_text(LoadStatus::ReadOnlyNotUtf8).find("UTF-8") != std::string::npos);
    CHECK(readonly_text(LoadStatus::Ok).empty());
    CHECK(load_failure_text(LoadStatus::TooBig, 0).find("1.0 MB") != std::string::npos);
    CHECK(load_failure_text(LoadStatus::Missing, 0).find("gone") != std::string::npos);
    CHECK(load_failure_text(LoadStatus::Error, 13).find("Permission denied") != std::string::npos);
    TextStats st = text_stats("# Title\n\nTwo words\n");
    CHECK(st.lines == 3 && st.words == 4);
    st = text_stats("");
    CHECK(st.lines == 0 && st.words == 0);
    st = text_stats("one");
    CHECK(st.lines == 1 && st.words == 1);
    TextFormat f;
    CHECK_EQ(format_text(f, true), std::string("LF, UTF-8"));
    f.eol = Eol::Crlf;
    f.write_crlf = true;
    f.bom = true;
    CHECK_EQ(format_text(f, true), std::string("CRLF, UTF-8 with BOM"));
    f.eol = Eol::Mixed;
    f.bom = false;
    CHECK_EQ(format_text(f, true), std::string("mixed line ends (saved as CRLF), UTF-8"));
    CHECK_EQ(format_text(TextFormat{}, false), std::string("LF, not UTF-8"));
    const std::string info = note_info_text("Projects/Antenna ideas.md", 3482, (kNow - 7200) * 1000000000LL, kNow, "# A\n\nb c\n", TextFormat{}, true);
    CHECK(info.find("Path: Projects/Antenna ideas.md\n") != std::string::npos);
    CHECK(info.find("Size: 3.4 KB\n") != std::string::npos);
    CHECK(info.find("Edited: 2026-10-24 12:03 (2 h ago)\n") != std::string::npos);
    CHECK(info.find("Lines: 3  \xC2\xB7  Words: 4\n") != std::string::npos);
    CHECK(info.find("Format: LF, UTF-8") != std::string::npos);
    // the menus: the mock-up's More, the preview's, a list's options
    // the options boxes (no bars, owner decision 2026-10-24): every action with its Ctrl key
    const std::vector<NoteAction> ed = editor_actions();
    CHECK(ed.size() == 6);
    if (ed.size() == 6) {
        CHECK_EQ(std::string(note_action_label(ed[0])), std::string("Preview"));
        CHECK_EQ(std::string(note_action_label(ed[1])), std::string("Tick task on this line"));
        CHECK_EQ(std::string(note_action_label(ed[2])), std::string("Insert date and time"));
        CHECK_EQ(std::string(note_action_label(ed[3])), std::string("Note info"));
        CHECK_EQ(std::string(note_action_label(ed[4])), std::string("Markdown help"));
        CHECK_EQ(std::string(note_action_label(ed[5])), std::string("Delete note"));
        CHECK_EQ(std::string(note_action_key(ed[0])), std::string("Ctrl+E"));
        CHECK_EQ(std::string(note_action_key(ed[1])), std::string("Ctrl+Enter"));
        CHECK_EQ(std::string(note_action_key(ed[2])), std::string("Ctrl+T"));
        CHECK_EQ(std::string(note_action_key(ed[3])), std::string("Ctrl+I"));
        CHECK_EQ(std::string(note_action_key(ed[4])), std::string("Ctrl+H"));
        CHECK_EQ(std::string(note_action_key(ed[5])), std::string("Ctrl+Del"));
    }
    const std::vector<NoteAction> pv = preview_actions();
    CHECK(pv.size() == 4 && pv[0] == NoteAction::Edit && pv[1] == NoteAction::NoteInfo && pv[2] == NoteAction::Help &&
          pv[3] == NoteAction::Delete);
    CHECK_EQ(std::string(note_action_label(NoteAction::Edit)), std::string("Edit"));
    CHECK_EQ(std::string(note_action_key(NoteAction::Edit)), std::string("Ctrl+E"));
    for (NoteAction a : {NoteAction::Edit, NoteAction::Preview, NoteAction::TickTask, NoteAction::InsertStamp, NoteAction::NoteInfo,
                         NoteAction::Help, NoteAction::Delete})
        CHECK(std::string(note_action_key(a)).rfind("Ctrl+", 0) == 0 && *note_action_label(a));
    CHECK(note_action_asks(NoteAction::NoteInfo) && note_action_asks(NoteAction::Delete) && !note_action_asks(NoteAction::Edit) &&
          !note_action_asks(NoteAction::TickTask));
    CHECK(list_note_actions().size() == 2 && list_note_actions()[0] == NoteAction::Delete);
    CHECK_EQ(options_title("Deck build log"), std::string("Options for Deck build log"));
    CHECK_EQ(options_body("Deck build log"), std::string(""));
    CHECK_EQ(options_title("A very long note name that goes on"), std::string("Options for A very long note name th..."));
    CHECK_EQ(options_title("CafÃ©"), std::string("Options"));        // not ASCII: the name goes in the body
    CHECK_EQ(options_body("CafÃ©"), std::string("CafÃ©"));
    CHECK(delete_body("Parts list").find("Parts list moves to the .trash folder") == 0);
    CHECK(conflict_body("Field log").find("Field log was changed on disk") == 0);
    CHECK(clock_warning_body("2020-01-01").find("2020-01-01") != std::string::npos);
    CHECK(exists_body("Mast").find("Open it?") != std::string::npos);
    CHECK(!external_link_note().empty());
}

void test_preview_layout()
{
    auto id = [](int px) { return px; };
    CHECK_EQ(heading_px(1, 24, id), 36);
    CHECK_EQ(heading_px(2, 24, id), 31);
    CHECK_EQ(heading_px(3, 24, id), 28);
    CHECK_EQ(heading_px(4, 24, id), 24);
    CHECK_EQ(heading_px(6, 24, id), 24);
    CHECK(heading_gold(2) && !heading_gold(1) && !heading_gold(3));
    CHECK_EQ(display_text("a\tb"), std::string("a    b"));
    CHECK_EQ(display_text("plain"), std::string("plain"));
    CHECK(truncated_text().find("400") != std::string::npos && !empty_note_text().empty());

    const Document d = parse_markdown("---\ntags: [a, b]\n---\n# T\n\n- one [[A]]\n  - two\n    - three\n1. first\n- [ ] task [x](https://e.org)\n- [x] done\n\ntext [[B|b]] and [[B|b]] [c](C.md)\n");
    CHECK(d.blocks.size() >= 7);
    CHECK(d.blocks[0].kind == BlockKind::Properties);
    CHECK_EQ(properties_text(d.blocks[0]), std::string("tags: a, b"));
    // markers: the items' first blocks
    std::vector<std::string> markers;
    for (const Block &b : d.blocks)
        if (b.item) markers.push_back(list_marker(b));
    CHECK(markers.size() == 6);
    if (markers.size() == 6) {
        CHECK_EQ(markers[0], std::string("\xE2\x80\xA2"));
        CHECK_EQ(markers[1], std::string("\xE2\x97\xA6"));
        CHECK_EQ(markers[2], std::string("\xE2\x96\xAA"));
        CHECK_EQ(markers[3], std::string("1."));
        CHECK_EQ(markers[4], std::string(""));            // a task: the box, no marker
        CHECK_EQ(markers[5], std::string(""));
    }
    // the ring: in reading order, boxes then the block's links, a link once per run of it
    const std::vector<PreviewTarget> t = preview_targets(d);
    std::string order;
    for (const PreviewTarget &x : t) order += x.task ? "T" : "L";
    CHECK_EQ(order, std::string("LTLTLLL"));
    CHECK_EQ(ring_next(-1, 1, 5), 0);
    CHECK_EQ(ring_next(-1, -1, 5), 4);
    CHECK_EQ(ring_next(2, 1, 5), 3);
    CHECK_EQ(ring_next(4, 1, 5), 4);
    CHECK_EQ(ring_next(0, -1, 5), 0);
    CHECK_EQ(ring_next(0, 1, 0), -1);
}

void test_links_plan()
{
    TempDir v;
    make_vault(v);
    const VaultIndex ix = walk_vault(v.path);
    LinkPlan p = plan_link(ix, "Welcome.md", LinkKind::Wiki, "deck build log");
    CHECK(p.go == LinkGo::OpenNote && p.rel == "Deck build log.md");
    p = plan_link(ix, "Welcome.md", LinkKind::Wiki, "Antenna ideas#Mast");
    CHECK(p.go == LinkGo::OpenNote && p.rel == "Projects/Antenna ideas.md" && p.heading == "Mast");
    p = plan_link(ix, "Projects/Antenna ideas.md", LinkKind::Wiki, "New idea");
    CHECK(p.go == LinkGo::CreateNote && p.rel == "New idea.md");          // a bare name: at the vault root
    p = plan_link(ix, "Welcome.md", LinkKind::Wiki, "#Lists");
    CHECK(p.go == LinkGo::SameNote && p.heading == "Lists");
    p = plan_link(ix, "Welcome.md", LinkKind::External, "https://example.org");
    CHECK(p.go == LinkGo::External && p.address == "https://example.org");
    p = plan_link(ix, "Welcome.md", LinkKind::File, "pic.png");
    CHECK(p.go == LinkGo::File && link_problem_text(p) == "Not a note: pic.png");
    p = plan_link(ix, "Projects/Antenna ideas.md", LinkKind::Note, "Mesh/Mesh%20antenna%20ideas.md");
    CHECK(p.go == LinkGo::OpenNote && p.rel == "Projects/Mesh/Mesh antenna ideas.md");
    p = plan_link(ix, "Welcome.md", LinkKind::Note, "../../out.md");
    CHECK(p.go == LinkGo::Invalid && !link_problem_text(p).empty());
}

void test_editor_text()
{
    const std::string s = "\xC3\xA9t\xC3\xA9\n\xE2\x87\xA5- [ ] x";       // "été\n⇥- [ ] x"
    CHECK_EQ(char_to_byte(s, 0), static_cast<std::size_t>(0));
    CHECK_EQ(char_to_byte(s, 1), static_cast<std::size_t>(2));
    CHECK_EQ(char_to_byte(s, 3), static_cast<std::size_t>(5));
    CHECK_EQ(char_to_byte(s, 5), static_cast<std::size_t>(9));
    CHECK_EQ(char_to_byte(s, 1000), s.size());
    CHECK_EQ(byte_to_char(s, 5), static_cast<std::size_t>(3));
    CHECK_EQ(byte_to_char(s, 9), static_cast<std::size_t>(5));
    CHECK_EQ(byte_to_char(s, s.size() + 5), byte_to_char(s, s.size()));
    CHECK_EQ(line_begin(s, 0), static_cast<std::size_t>(0));
    CHECK_EQ(line_begin(s, 7), static_cast<std::size_t>(6));
    CHECK_EQ(line_begin(s, 6), static_cast<std::size_t>(6));
    CHECK_EQ(line_end(s, 2), static_cast<std::size_t>(5));
    CHECK_EQ(line_end(s, 7), s.size());
    std::string t = s;
    std::size_t cur = 12;
    CHECK(outdent_line(t, cur, kTabGlyph) && t == "\xC3\xA9t\xC3\xA9\n- [ ] x" && cur == 9);
    CHECK(!outdent_line(t, cur, kTabGlyph));
    t = "a\n      deep";
    cur = 10;
    CHECK(outdent_line(t, cur, kTabGlyph) && t == "a\n  deep" && cur == 6);
    t = "\tx";
    cur = 0;
    CHECK(outdent_line(t, cur, nullptr) && t == "x" && cur == 0);
    // the tab glyph round trip of the editor (core text_codec): a tab typed is a tab on disk
    TextFormat f;
    const std::string body = "\t- a\n";
    const std::string edit = to_edit(body, f);
    CHECK(edit.find(kTabGlyph) == 0);
    CHECK_EQ(from_edit(edit, f), body);
}

/* The on-screen keyboard's request (0.1.1, the deck report): which field asks, and when it asks again. The rule
 * itself (whether the keyboard shows) is the shared policy's; the app only keeps its request alive. */
void test_osk_request()
{
    CHECK(osk_field_for(PageId::Editor, false, false, false, true) == OskField::Editor);
    CHECK(osk_field_for(PageId::Editor, false, true, true, false) == OskField::None);     // the editor not typing
    CHECK(osk_field_for(PageId::Open, false, true, false, true) == OskField::Open);
    CHECK(osk_field_for(PageId::NewNote, false, false, true, false) == OskField::New);
    CHECK(osk_field_for(PageId::Editor, true, false, false, true) == OskField::None);     // a dialog is open
    CHECK(osk_field_for(PageId::Preview, false, true, true, true) == OskField::None);
    CHECK(osk_field_for(PageId::Home, false, true, true, true) == OskField::None);
    CHECK_EQ(std::string(osk_field_key(OskField::Editor)), std::string("editor"));
    CHECK_EQ(std::string(osk_field_key(OskField::None)), std::string("-"));
    const OskField E = OskField::Editor, O = OskField::Open, N = OskField::New, X = OskField::None;
    // the first request, whatever the rule says at that moment
    CHECK(osk_step(E, X, X, false) == OskStep::Request);
    // held by the service (allowed or not: it asks the rule again by itself): nothing to do
    CHECK(osk_step(E, E, E, false) == OskStep::Keep);
    CHECK(osk_step(O, O, O, false) == OskStep::Keep);
    // the service lost it (dropped, or another field took it): asked again
    CHECK(osk_step(E, E, X, false) == OskStep::Request);
    CHECK(osk_step(N, N, O, false) == OskStep::Request);
    // a tap on the field always asks again
    CHECK(osk_step(E, E, E, true) == OskStep::Request);
    CHECK(osk_step(O, O, O, true) == OskStep::Request);
    CHECK(osk_step(N, N, N, true) == OskStep::Request);
    // another field, or none
    CHECK(osk_step(O, E, E, false) == OskStep::Switch);
    CHECK(osk_step(X, E, E, false) == OskStep::Release);
    CHECK(osk_step(X, X, X, true) == OskStep::Keep);

    // Settings > On-screen keyboard: what the rule says and why
    CHECK_EQ(osk_value(true), std::string("allowed: yes"));
    CHECK_EQ(osk_value(false), std::string("allowed: no"));
    OskFacts f;
    f.have_file = true;
    f.now_mono = 4000;
    CHECK_EQ(osk_detail(f, true), std::string("input.state keyboards=0 vkb=auto"));
    f.keyboards = 1;
    CHECK_EQ(osk_detail(f, false), std::string("a keyboard is connected  \xC2\xB7  keyboards=1 vkb=auto"));
    // the deck's file of the report (kb_gone = bt_gone = 3473): asleep until 4073 on the monotonic clock
    f.keyboards = 0;
    f.kb_gone = f.bt_gone = 3473;
    f.now_mono = 3473 + 120;
    CHECK_EQ(osk_detail(f, false), std::string("Bluetooth keyboard asleep 8 min more  \xC2\xB7  keyboards=0 vkb=auto"));
    f.now_mono = 3473 + 590;
    CHECK_EQ(osk_detail(f, false), std::string("Bluetooth keyboard asleep 10 s more  \xC2\xB7  keyboards=0 vkb=auto"));
    f.now_mono = 3473 + 600;
    CHECK_EQ(osk_detail(f, true), std::string("input.state keyboards=0 vkb=auto"));
    f.bt_gone = 0;
    f.kb_gone = 4000;
    f.now_mono = 4001;
    CHECK_EQ(osk_detail(f, false), std::string("a keyboard just went away  \xC2\xB7  keyboards=0 vkb=auto"));
    f.vkb = 1;
    CHECK(osk_detail(f, false).find("Off") != std::string::npos && osk_detail(f, false).find("vkb=off") != std::string::npos);
    f.vkb = 2;
    f.keyboards = 1;
    CHECK_EQ(osk_detail(f, true), std::string("forced on  \xC2\xB7  keyboards=1 vkb=force"));
    OskFacts nf;
    nf.fallback_keyboards = 0;
    CHECK_EQ(osk_detail(nf, true), std::string("no input.state (no launcher): 0 keyboards found"));
    nf.fallback_keyboards = -1;
    CHECK_EQ(osk_detail(nf, true), std::string("no input.state (no launcher): keyboards unknown"));
    // the row of Settings (ABOUT, a read-out)
    const std::vector<ListItem> s = settings_rows("~/Notes", false, TextSize::Medium, OpenIn::Preview, "0.1.1", "allowed: no",
                                                  "a keyboard is connected");
    const ListItem *o = find_id(s, "osk");
    CHECK(o && o->label == "On-screen keyboard" && o->value == "allowed: no" && o->line2 == "a keyboard is connected" &&
          o->aff == Affordance::Info);
    CHECK(s.back().id == "osk");
    CHECK(find_id(settings_rows("~/Notes", false, TextSize::Medium, OpenIn::Preview, "0.1.1"), "osk") == nullptr);
}

/* The Markdown help page (0.1.2): a built-in text the preview draws; every syntax with what is typed and how it looks,
 * the line on what is not supported, every Ctrl key of the app. */
void test_markdown_help()
{
    CHECK_EQ(std::string(note_action_label(NoteAction::Help)), std::string("Markdown help"));
    CHECK_EQ(std::string(note_action_key(NoteAction::Help)), std::string("Ctrl+H"));
    CHECK(note_action_asks(NoteAction::Help));
    CHECK_EQ(std::string(page_key(PageId::Help)), std::string("help"));
    CHECK_EQ(help_caption(), std::string("What you type, then how it looks"));
    const std::string &t = markdown_help_text();
    const Document d = parse_markdown(t);
    CHECK(d.status == 0 && !d.truncated && d.frontmatter_len == 0);   // the properties example is in a code block
    std::vector<std::string> headings;
    int code = 0, tasks = 0, checked = 0, items = 0, quotes = 0, rules = 0;
    for (const Block &b : d.blocks) {
        if (b.kind == BlockKind::Heading) headings.push_back(block_text(b));
        if (b.kind == BlockKind::Code) ++code;
        if (b.kind == BlockKind::Rule) ++rules;
        if (b.task) ++tasks;
        if (b.task && b.checked) ++checked;
        if (b.item) ++items;
        if (b.quote > 0) ++quotes;
    }
    for (const char *h : {"Markdown help", "Headings", "Emphasis", "Code block", "Lists", "Tasks", "Links", "Quote and rule",
                          "Properties", "Not yet", "Keys"})
        CHECK(std::find(headings.begin(), headings.end(), h) != headings.end());
    CHECK(std::find(headings.begin(), headings.end(), "Heading 3") != headings.end());   // an example drawn
    CHECK(code >= 9);              // what is typed, for every syntax
    CHECK(tasks == 2 && checked == 1 && quotes >= 1 && rules == 1);
    CHECK(items >= 4 + 2 + 11);    // the lists' example, the tasks, the keys
    int wiki = 0, external = 0;
    for (const DocLink &l : d.links) {
        if (l.kind == LinkKind::Wiki) ++wiki;
        if (l.kind == LinkKind::External) ++external;
    }
    CHECK(wiki == 3 && external >= 1);
    // the emphasis drawn: bold, italic, bold italic, strike, code runs
    bool bold = false, ital = false, both = false, strike = false, inl = false;
    for (const Block &b : d.blocks)
        for (const Run &r : b.runs) {
            bold |= (r.style & kStyleBold) && !(r.style & kStyleItalic);
            ital |= (r.style & kStyleItalic) && !(r.style & kStyleBold);
            both |= (r.style & kStyleBold) && (r.style & kStyleItalic);
            strike |= (r.style & kStyleStrike) != 0;
            inl |= (r.style & kStyleCode) != 0;
        }
    CHECK(bold && ital && both && strike && inl);
    // the fenced example of a code block keeps its inner fence as text
    bool fence_example = false;
    for (const Block &b : d.blocks)
        if (b.kind == BlockKind::Code && b.code.find("```") != std::string::npos) fence_example = true;
    CHECK(fence_example);
    CHECK(t.find("not supported yet") != std::string::npos && t.find("Tables, images and embeds") != std::string::npos);
    // every Ctrl key of the app, listed once as a key line
    CHECK(app_ctrl_keys().size() == 11);
    for (const std::string &k : app_ctrl_keys()) CHECK(t.find("- " + k + ": ") != std::string::npos);
    for (NoteAction a : {NoteAction::Edit, NoteAction::TickTask, NoteAction::InsertStamp, NoteAction::NoteInfo, NoteAction::Help,
                         NoteAction::Delete})
        CHECK(std::find(app_ctrl_keys().begin(), app_ctrl_keys().end(), std::string(note_action_key(a))) != app_ctrl_keys().end());
}

void test_page_keys()
{
    CHECK_EQ(std::string(page_key(PageId::Home)), std::string("home"));
    CHECK_EQ(std::string(page_key(PageId::Editor)), std::string("editor"));
    CHECK_EQ(std::string(page_key(PageId::NewNote)), std::string("new"));
}

} // namespace

#define RUN(fn)                                                                  \
    do {                                                                         \
        int before_ = g_fail;                                                    \
        fn();                                                                    \
        std::printf("%-30s %s\n", #fn, g_fail == before_ ? "ok" : "FAILED");      \
    } while (0)

int main()
{
    ::setenv("TZ", "UTC", 1);
    ::tzset();
    RUN(test_keys);
    RUN(test_dialog_keys);
    RUN(test_formatting);
    RUN(test_home_rows);
    RUN(test_folder_rows);
    RUN(test_names_words);
    RUN(test_open_rows);
    RUN(test_settings_rows);
    RUN(test_note_pages);
    RUN(test_preview_layout);
    RUN(test_links_plan);
    RUN(test_editor_text);
    RUN(test_osk_request);
    RUN(test_markdown_help);
    RUN(test_page_keys);
    std::printf("ui: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
