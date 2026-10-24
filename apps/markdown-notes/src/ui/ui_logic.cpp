/*
 * SPDX-License-Identifier: MIT
 */
#include "ui_logic.hpp"

#include "dates.hpp"
#include "fsutil.hpp"
#include "fuzzy.hpp"
#include "links.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

using namespace mdnotes;

namespace notesui {

const char *const kSep = "  \xC2\xB7  ";
const char *const kUnsavedDot = " \xE2\x97\x8F";

const char *page_key(PageId p)
{
    switch (p) {
    case PageId::Home: return "home";
    case PageId::Folder: return "folder";
    case PageId::Recent: return "recent";
    case PageId::Open: return "open";
    case PageId::NewNote: return "new";
    case PageId::Preview: return "preview";
    case PageId::Editor: return "editor";
    case PageId::Settings: return "settings";
    case PageId::Help: return "help";
    }
    return "?";
}

/* ------------------------------------------------------------------ dialogs */

bool Choice::step(int delta)
{
    if (labels.empty()) return false;
    const int next = std::clamp(index + delta, 0, static_cast<int>(labels.size()) - 1);
    if (next == index) return false;
    index = next;
    return true;
}

const std::string &Choice::label() const
{
    static const std::string empty;
    if (index < 0 || index >= static_cast<int>(labels.size())) return empty;
    return labels[static_cast<std::size_t>(index)];
}

std::string Choice::detail() const
{
    if (index < 0 || index >= static_cast<int>(details.size())) return "";
    return details[static_cast<std::size_t>(index)];
}

std::string Choice::position() const
{
    return std::to_string(index + 1) + " / " + std::to_string(labels.size());
}

ChoiceResult choice_key(const KeyEvent &e, Choice &c)
{
    switch (e.key) {
    case Key::Left:
    case Key::Up: return c.step(-1) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Right:
    case Key::Down: return c.step(1) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Home: return c.step(-1000) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::End: return c.step(1000) ? ChoiceResult::Moved : ChoiceResult::None;
    case Key::Enter: return e.repeat ? ChoiceResult::None : ChoiceResult::Accept;
    case Key::Esc: return e.repeat ? ChoiceResult::None : ChoiceResult::Cancel;
    default: return ChoiceResult::None;
    }
}

ConfirmResult confirm_key(const KeyEvent &e, int &focus)
{
    switch (e.key) {
    case Key::Left:
    case Key::Up:
        focus = 0;
        return ConfirmResult::Moved;
    case Key::Right:
    case Key::Down:
        focus = 1;
        return ConfirmResult::Moved;
    case Key::Tab:
    case Key::BackTab:
        focus = 1 - focus;
        return ConfirmResult::Moved;
    case Key::Enter: return e.repeat ? ConfirmResult::None : (focus == 1 ? ConfirmResult::Yes : ConfirmResult::No);
    case Key::Esc: return e.repeat ? ConfirmResult::None : ConfirmResult::No;
    case Key::Char: {
        if (e.ctrl || e.alt || e.repeat || e.text.size() != 1) return ConfirmResult::None;
        const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0])));
        if (ch == 'y') return ConfirmResult::Yes;
        if (ch == 'n') return ConfirmResult::No;
        return ConfirmResult::None;
    }
    default: return ConfirmResult::None;
    }
}

MenuResult menu_key(const KeyEvent &e, int &selected, int count)
{
    if (e.key == Key::Esc) return e.repeat ? MenuResult::None : MenuResult::Cancel;
    if (count <= 0) return MenuResult::None;
    int n = selected;
    switch (e.key) {
    case Key::Up:
    case Key::Left: n = std::clamp(selected - 1, 0, count - 1); break;
    case Key::Down:
    case Key::Right: n = std::clamp(selected + 1, 0, count - 1); break;
    case Key::Home: n = 0; break;
    case Key::End: n = count - 1; break;
    case Key::Enter: return e.repeat ? MenuResult::None : MenuResult::Accept;
    default: return MenuResult::None;
    }
    const bool moved = n != selected;
    selected = n;
    return moved ? MenuResult::Moved : MenuResult::None;
}

void HoldTracker::begin(const std::string &target, int x, int y, std::uint64_t now)
{
    active_ = true;
    swallow_ = false;
    target_ = target;
    x_ = x;
    y_ = y;
    start_ = now;
}

void HoldTracker::move(int x, int y)
{
    if (!active_) return;
    if (std::abs(x - x_) > kSlop || std::abs(y - y_) > kSlop) active_ = false;
}

void HoldTracker::cancel() { active_ = false; }

double HoldTracker::progress(std::uint64_t now) const
{
    if (!active_) return 0;
    const double p = static_cast<double>(now - start_) / static_cast<double>(kFireMs);
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

bool HoldTracker::poll(std::uint64_t now)
{
    if (!active_ || now - start_ < kFireMs) return false;
    active_ = false;
    swallow_ = true;
    return true;
}

bool HoldTracker::take_swallow()
{
    const bool s = swallow_;
    swallow_ = false;
    return s;
}

bool list_move(const KeyEvent &e, int &sel, int count, int page, const std::function<bool(int)> &skip)
{
    int dir = 0, n = sel;
    switch (e.key) {
    case Key::Up: n = sel - 1; dir = -1; break;
    case Key::Down: n = sel + 1; dir = 1; break;
    case Key::Home: n = 0; dir = 1; break;
    case Key::End: n = count - 1; dir = -1; break;
    case Key::PageUp: n = sel - std::max(1, page); dir = -1; break;
    case Key::PageDown: n = sel + std::max(1, page); dir = 1; break;
    default: return false;
    }
    if (count <= 0) return true;
    n = std::clamp(n, 0, count - 1);
    if (skip) {
        int m = n;
        while (m >= 0 && m < count && skip(m)) m += dir;
        if (m < 0 || m >= count) {            /* nothing selectable that way: the nearest one the other way */
            m = n;
            while (m >= 0 && m < count && skip(m)) m -= dir;
        }
        if (m < 0 || m >= count) return true;
        n = m;
    }
    sel = n;
    return true;
}

/* ------------------------------------------------------------------ layout */

PageColumn page_column(int screen_w, int inset, int min_w, int cap_w)
{
    PageColumn c;
    const int margin = screen_w >= min_w + 2 * inset ? 2 * inset : inset;
    c.w = std::max(1, std::min(cap_w, screen_w - 2 * margin));
    c.x = (screen_w - c.w) / 2;
    return c;
}

/* ------------------------------------------------------------------ formatting */

std::string plural(long n, const char *one, const char *many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string truncate_text(const std::string &s, std::size_t max_chars)
{
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
        if (chars == max_chars) return s.substr(0, i) + "...";
        ++chars;
    }
    return s;
}

std::string fmt_size(std::int64_t n)
{
    char b[32];
    if (n < 0) n = 0;
    if (n < 1000) std::snprintf(b, sizeof(b), "%lld %s", static_cast<long long>(n), n == 1 ? "byte" : "bytes");
    else if (n < 10 * 1024) std::snprintf(b, sizeof(b), "%.1f KB", static_cast<double>(n) / 1024.0);
    else if (n < 1000 * 1024) std::snprintf(b, sizeof(b), "%lld KB", static_cast<long long>((n + 512) / 1024));
    else std::snprintf(b, sizeof(b), "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    return b;
}

std::string tilde_path(const std::string &path, const std::string &home)
{
    if (home.empty() || home == "/") return path;
    if (path == home) return "~";
    if (path.size() > home.size() && path.compare(0, home.size(), home) == 0 && path[home.size()] == '/')
        return "~" + path.substr(home.size());
    return path;
}

std::string edited_text(std::int64_t mtime_ns, std::int64_t now_s)
{
    if (mtime_ns <= 0) return "";
    return "edited " + age_text(mtime_ns / 1000000000LL, now_s);
}

/* ------------------------------------------------------------------ rows */

namespace {

ListItem item(const std::string &id, const std::string &label, const std::string &value, Affordance aff,
              const std::string &line2 = std::string())
{
    ListItem r;
    r.id = id;
    r.label = label;
    r.value = value;
    r.aff = aff;
    r.line2 = line2;
    return r;
}

ListItem section(const std::string &title)
{
    ListItem r;
    r.section = true;
    r.label = title;
    return r;
}

} // namespace

std::vector<ListItem> home_rows(std::size_t notes, const std::string &today)
{
    return {
        item("open", "Open or create\xE2\x80\xA6", "Ctrl+P", Affordance::Open),
        item("all", "All notes", std::to_string(notes), Affordance::Open),
        item("recent", "Recent", "last 20", Affordance::Open),
        item("today", "Today\xE2\x80\x99s note", today, Affordance::Open),
        item("new", "New note", "+", Affordance::Action),
        item("settings", "Settings", "", Affordance::Open),
    };
}

std::string home_caption(const std::string &vault_shown, std::size_t notes)
{
    return "Vault: " + vault_shown + kSep + plural(static_cast<long>(notes), "note", "notes");
}

std::string home_hint() { return "Up/Down: choose  Enter: open  Ctrl+D: today  Esc: quit"; }
std::string folder_hint() { return "Enter: open  Ctrl+O: options  Esc: back"; }

std::vector<ListItem> folder_rows(const VaultIndex &ix, const std::string &rel, std::int64_t now_s)
{
    std::vector<ListItem> rows;
    rows.push_back(item("new", "New note here", "+", Affordance::Action));
    const FolderListing l = list_folder(ix, rel);
    if (!l.folders.empty()) {
        rows.push_back(section("FOLDERS"));
        for (int fi : l.folders) {
            const FolderEntry &f = ix.folders[static_cast<std::size_t>(fi)];
            rows.push_back(item("folder:" + f.rel, f.name, std::to_string(f.total), Affordance::Open));
        }
    }
    if (!l.notes.empty()) {
        rows.push_back(section("NOTES"));
        for (int ni : l.notes) {
            const NoteEntry &n = ix.notes[static_cast<std::size_t>(ni)];
            rows.push_back(item("note:" + n.rel, n.name, "", Affordance::Open, edited_text(n.mtime_ns, now_s)));
        }
    }
    return rows;
}

std::string folder_caption(int notes, int folders)
{
    if (notes == 0 && folders == 0) return "No notes here yet";
    std::string s = plural(notes, "note", "notes");
    if (folders > 0) s += kSep + plural(folders, "folder", "folders");
    return s;
}

std::vector<std::string> folder_title(const std::string &rel)
{
    if (rel.empty()) return {"All notes"};
    std::vector<std::string> path{"Notes"};
    std::size_t i = 0;
    for (;;) {
        const std::size_t j = rel.find('/', i);
        path.push_back(rel.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return path;
}

std::vector<ListItem> recent_rows(const VaultIndex &ix, std::int64_t now_s, std::size_t n)
{
    std::vector<ListItem> rows;
    for (int ni : recent_notes(ix, n)) {
        const NoteEntry &e = ix.notes[static_cast<std::size_t>(ni)];
        std::string line2 = edited_text(e.mtime_ns, now_s);
        if (!e.folder.empty()) line2 += kSep + e.folder;
        rows.push_back(item("note:" + e.rel, e.name, "", Affordance::Open, line2));
    }
    return rows;
}

std::string recent_caption(std::size_t shown)
{
    if (shown == 0) return "No notes yet";
    return "The " + plural(static_cast<long>(shown), "note", "notes") + " edited last";
}

/* ------------------------------------------------------------------ names */

std::string name_error_text(const NameCheck &c)
{
    switch (c.error) {
    case NameError::None: return "";
    case NameError::Empty: return "Type a name first.";
    case NameError::TooLong: return "That name is too long (at most " + std::to_string(kNameMaxBytes) + " bytes).";
    case NameError::Forbidden: return "A name cannot hold \"" + c.bad + "\": it breaks links or other systems.";
    case NameError::ControlChar: return "A name cannot hold control characters.";
    case NameError::LeadingDot: return "A name cannot start with a dot: the note would be hidden.";
    case NameError::TrailingSpaceOrDot: return "A name cannot end with a space or a dot.";
    case NameError::Reserved: return "\"" + c.bad + "\" is a name Windows keeps for itself: choose another.";
    case NameError::NotUtf8: return "That name is not valid text.";
    case NameError::TooDeep: return "Too many folders: at most " + std::to_string(kMaxFolderDepth) + ".";
    case NameError::EmptyFolder: return "A folder of that path is empty, \".\" or \"..\".";
    }
    return "That name cannot be used.";
}

NewTarget new_note_target(const std::string &vault_root, const std::string &in_folder, const std::string &typed)
{
    NewTarget t;
    std::string name = clean_typed_name(typed);
    std::string folder = in_folder;
    std::string stem;
    if (name.find('/') != std::string::npos) {           /* a path: from the vault root */
        while (!name.empty() && name[0] == '/') name.erase(0, 1);
        const NameCheck c = check_note_path(name);
        if (!c.ok()) {
            NameCheck shown = c;
            if (shown.error == NameError::Reserved && shown.bad.empty()) shown.bad = name;
            t.error = name_error_text(shown);
            return t;
        }
        folder = dir_name(name);
        stem = base_name(name);
    } else {
        NameCheck c = check_name(name);
        if (!c.ok()) {
            if (c.error == NameError::Reserved) c.bad = name;
            t.error = name_error_text(c);
            return t;
        }
        stem = name;
    }
    const std::string depth_check = path_join(folder, stem);
    const NameCheck d = check_note_path(depth_check);
    if (!d.ok()) {
        t.error = name_error_text(d);
        return t;
    }
    t.name = stem;
    t.rel = path_join(folder, note_file_name(stem));
    const std::string existing = find_existing_ci(path_join(vault_root, folder), note_file_name(stem));
    if (!existing.empty()) {
        t.exists = true;
        t.existing_rel = path_join(folder, existing);
        return t;
    }
    t.ok = true;
    return t;
}

std::vector<ListItem> open_rows(const VaultIndex &ix, const std::string &vault_root, const std::string &query, std::int64_t now_s,
                                std::size_t limit)
{
    std::vector<ListItem> rows;
    const std::string q = clean_typed_name(query);
    if (q.empty()) {
        for (ListItem &r : recent_rows(ix, now_s, 20)) rows.push_back(std::move(r));
        return rows;
    }
    const std::vector<Match> m = rank_notes(ix, q, limit);
    if (!has_exact(m)) {
        const NewTarget t = new_note_target(vault_root, "", q);
        ListItem c = item("create", "Create \xE2\x80\x9C" + q + "\xE2\x80\x9D", "+", Affordance::Action);
        if (!t.ok && !t.exists) {
            c.line2 = t.error;
            c.warn = true;
        } else if (t.ok && t.rel.find('/') != std::string::npos) {
            c.line2 = "in " + dir_name(t.rel);
        }
        rows.push_back(c);
    }
    for (const Match &x : m) {
        const NoteEntry &n = ix.notes[static_cast<std::size_t>(x.note)];
        rows.push_back(item("note:" + n.rel, n.name, "", Affordance::Open, n.folder));
    }
    return rows;
}

std::string open_caption(bool empty_query, std::size_t matches)
{
    if (!empty_query && matches == 0) return "No note by that name: Enter creates it";
    return "Type a name, Enter opens";
}

/* ------------------------------------------------------------------ Settings */

Choice text_size_choice(TextSize current)
{
    Choice c;
    c.title = "Text size";
    c.labels = {"Small", "Medium", "Large"};
    c.details = {"More text on the screen", "The usual size", "Easier to read at arm's length"};
    c.index = current == TextSize::Small ? 0 : current == TextSize::Large ? 2 : 1;
    return c;
}

TextSize text_size_from_index(int index) { return index <= 0 ? TextSize::Small : index >= 2 ? TextSize::Large : TextSize::Medium; }

std::string text_size_label(TextSize s) { return s == TextSize::Small ? "Small" : s == TextSize::Large ? "Large" : "Medium"; }

Choice open_in_choice(OpenIn current)
{
    Choice c;
    c.title = "Open notes in";
    c.labels = {"Preview", "Editor"};
    c.details = {"Read first; Edit to write", "Write at once"};
    c.note = "New notes and today's note always open in the editor.";
    c.index = current == OpenIn::Editor ? 1 : 0;
    return c;
}

OpenIn open_in_from_index(int index) { return index >= 1 ? OpenIn::Editor : OpenIn::Preview; }

std::string open_in_label(OpenIn o) { return o == OpenIn::Editor ? "Editor" : "Preview"; }

int preview_text_px(TextSize s, int caption_px, int body_px, int heading_px, int title_px, const std::function<int(int)> &snap)
{
    switch (s) {
    case TextSize::Small: return snap(caption_px);
    case TextSize::Large: return snap((heading_px + title_px + 1) / 2);
    default: return snap(body_px);
    }
}

int editor_text_px(TextSize s, int caption_px, int body_px, const std::function<int(int)> &snap)
{
    switch (s) {
    case TextSize::Small: return snap(caption_px);
    case TextSize::Large: return snap(body_px);
    default: return snap(caption_px + (body_px - caption_px + 2) / 3);
    }
}

namespace {

std::string span_text(std::int64_t s)
{
    if (s < 60) return std::to_string(s < 1 ? 1 : s) + " s";
    return std::to_string((s + 59) / 60) + " min";
}

} // namespace

std::string osk_value(bool allowed) { return allowed ? "allowed: yes" : "allowed: no"; }

std::string osk_detail(const OskFacts &f, bool allowed)
{
    if (!f.have_file)
        return "no input.state (no launcher): " + (f.fallback_keyboards < 0 ? std::string("keyboards unknown")
                                                                               : plural(f.fallback_keyboards, "keyboard", "keyboards") + " found");
    const char *vkb = f.vkb == 1 ? "off" : f.vkb == 2 ? "force" : "auto";
    const std::string facts = "keyboards=" + std::to_string(f.keyboards) + " vkb=" + vkb;
    std::string why;
    if (f.vkb == 1) why = "the on-screen keyboard is Off in the launcher's Settings";
    else if (f.vkb == 2) why = "forced on";
    else if (f.keyboards > 0) why = "a keyboard is connected";
    else if (f.bt_gone > 0 && f.now_mono - f.bt_gone < f.asleep_s)
        why = "Bluetooth keyboard asleep " + span_text(f.asleep_s - (f.now_mono - f.bt_gone)) + " more";
    else if (f.kb_gone > 0 && f.now_mono - f.kb_gone < f.grace_s) why = "a keyboard just went away";
    if (why.empty() || (allowed && f.vkb != 2)) return "input.state " + facts;
    return why + kSep + facts;
}

std::vector<ListItem> settings_rows(const std::string &vault_shown, bool from_env, TextSize size, OpenIn open_in,
                                    const std::string &version, const std::string &osk_val, const std::string &osk_det)
{
    std::vector<ListItem> rows;
    rows.push_back(section("VAULT"));
    rows.push_back(item("vault", "Folder", vault_shown, Affordance::Info, from_env ? "Set by NOTES_VAULT" : ""));
    rows.push_back(section("DISPLAY"));
    rows.push_back(item("text_size", "Text size", text_size_label(size), Affordance::Edit));
    rows.push_back(item("open_in", "Open notes in", open_in_label(open_in), Affordance::Edit));
    rows.push_back(section("ABOUT"));
    rows.push_back(item("version", "Version", version, Affordance::Info));
    if (!osk_val.empty()) rows.push_back(item("osk", "On-screen keyboard", osk_val, Affordance::Info, osk_det));
    return rows;
}

std::string settings_caption(const std::string &version) { return "Markdown Notes " + version; }

/* ------------------------------------------------------------------ the note pages */

std::string editor_title(const std::string &name, bool unsaved) { return unsaved ? name + kUnsavedDot : name; }

std::string options_hint(bool keyboard) { return keyboard ? "Ctrl+O: options" : "Hold 3 s: options"; }

std::string editor_caption(std::size_t bytes, bool keyboard)
{
    return std::string("Editor") + kSep + fmt_size(static_cast<std::int64_t>(bytes)) + kSep + options_hint(keyboard);
}

std::string preview_caption(LoadStatus st, std::int64_t mtime_ns, std::int64_t now_s, bool keyboard)
{
    std::string s = "Preview";
    if (st == LoadStatus::ReadOnlyLarge) s += kSep + std::string("read only, over ") + fmt_size(static_cast<std::int64_t>(kEditCap));
    else if (st == LoadStatus::ReadOnlyNotUtf8) s += kSep + std::string("read only, not UTF-8");
    else if (const std::string e = edited_text(mtime_ns, now_s); !e.empty()) s += kSep + e;
    return s + kSep + options_hint(keyboard);
}

std::string readonly_text(LoadStatus st)
{
    if (st == LoadStatus::ReadOnlyLarge)
        return "Read only: notes over " + fmt_size(static_cast<std::int64_t>(kEditCap)) + " are not edited here";
    if (st == LoadStatus::ReadOnlyNotUtf8) return "Read only: this file is not UTF-8 text";
    return "";
}

std::string load_failure_text(LoadStatus st, int err)
{
    switch (st) {
    case LoadStatus::TooBig: return "Too large to open (over " + fmt_size(static_cast<std::int64_t>(kReadCap)) + ")";
    case LoadStatus::Missing: return "That note is gone (moved or deleted)";
    case LoadStatus::Error: return std::string("Cannot read it: ") + std::strerror(err);
    default: return "";
    }
}

TextStats text_stats(const std::string &body)
{
    TextStats t;
    if (body.empty()) return t;
    t.lines = 1;
    bool in_word = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\n' && i + 1 < body.size()) ++t.lines;
        const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (!space && !in_word) ++t.words;
        in_word = !space;
    }
    return t;
}

std::string format_text(const TextFormat &fmt, bool utf8)
{
    std::string s = fmt.eol == Eol::Crlf ? "CRLF" : fmt.eol == Eol::Mixed ? std::string("mixed line ends (saved as ") + (fmt.write_crlf ? "CRLF" : "LF") + ")" : "LF";
    s += utf8 ? ", UTF-8" : ", not UTF-8";
    if (fmt.bom) s += " with BOM";
    return s;
}

std::string note_info_text(const std::string &rel, std::int64_t size, std::int64_t mtime_ns, std::int64_t now_s, const std::string &body,
                           const TextFormat &fmt, bool utf8)
{
    const TextStats st = text_stats(body);
    const std::time_t mt = static_cast<std::time_t>(mtime_ns / 1000000000LL);
    std::string s = "Path: " + rel + "\n";
    s += "Size: " + fmt_size(size) + "\n";
    if (mtime_ns > 0) s += "Edited: " + stamp_text(mt) + " (" + age_text(mtime_ns / 1000000000LL, now_s) + ")\n";
    s += "Lines: " + std::to_string(st.lines) + kSep + "Words: " + std::to_string(st.words) + "\n";
    s += "Format: " + format_text(fmt, utf8);
    return s;
}

std::vector<NoteAction> editor_actions()
{
    return {NoteAction::Preview, NoteAction::TickTask, NoteAction::InsertStamp, NoteAction::NoteInfo, NoteAction::Help, NoteAction::Delete};
}
std::vector<NoteAction> preview_actions() { return {NoteAction::Edit, NoteAction::NoteInfo, NoteAction::Help, NoteAction::Delete}; }
std::vector<NoteAction> list_note_actions() { return {NoteAction::Delete, NoteAction::NoteInfo}; }

const char *note_action_key(NoteAction a)
{
    switch (a) {
    case NoteAction::Edit:
    case NoteAction::Preview: return "Ctrl+E";
    case NoteAction::TickTask: return "Ctrl+Enter";
    case NoteAction::InsertStamp: return "Ctrl+T";
    case NoteAction::NoteInfo: return "Ctrl+I";
    case NoteAction::Help: return "Ctrl+H";
    case NoteAction::Delete: return "Ctrl+Del";
    }
    return "";
}

bool note_action_asks(NoteAction a) { return a == NoteAction::NoteInfo || a == NoteAction::Help || a == NoteAction::Delete; }

const std::vector<std::string> &app_ctrl_keys()
{
    static const std::vector<std::string> keys = {"Ctrl+O", "Ctrl+E", "Ctrl+Enter", "Ctrl+T", "Ctrl+S", "Ctrl+P",
                                                  "Ctrl+N", "Ctrl+D", "Ctrl+I", "Ctrl+Del", "Ctrl+H"};
    return keys;
}

std::string help_caption() { return "What you type, then how it looks"; }

/* Short on purpose: one screen per syntax at most on the deck. Code blocks hold what is typed; what follows each one
 * is the same Markdown, drawn. A fence of four backticks holds the three of the code block's example. */
const std::string &markdown_help_text()
{
    static const std::string text = R"MD(# Markdown help

Notes are plain text: the marks stay in the file, the preview draws them.

## Headings

```
# Heading 1
## Heading 2
### Heading 3
```

### Heading 3

## Emphasis

```
**bold**  *italic*  ***bold italic***
~~struck~~  `inline code`
```

**bold**  *italic*  ***bold italic***
~~struck~~  `inline code`

## Code block

````
```
vcgencmd get_throttled
```
````

```
vcgencmd get_throttled
```

## Lists

```
- one
  - nested
1. first
2. second
```

- one
  - nested
1. first
2. second

## Tasks

```
- [ ] to do
- [x] done
```

- [ ] to do
- [x] done

Tap a box in the preview, or Ctrl+Enter on its line in the editor.

## Links

```
[text](https://example.org)
[[Another note]]
[[Another note|shown text]]
[[Another note#Heading]]
```

[text](https://example.org), [[Another note]], [[Another note|shown text]], [[Another note#Heading]]

A link to a note that does not exist yet creates it; a web address shows in a box.

## Quote and rule

```
> A quote

---
```

> A quote

---

## Properties

```
---
tags: a, b
---
```

At the very top of a note: shown muted, kept as written.

## Not yet

Tables, images and embeds are not supported yet in this version: a table shows as plain text, an image or an embed as a link.

## Keys

- Ctrl+O: the options of the note
- Ctrl+E: preview / edit
- Ctrl+Enter: tick the task, or follow the link, under the cursor
- Ctrl+T: insert the date and time
- Ctrl+S: save now (it saves by itself)
- Ctrl+P: open or create a note
- Ctrl+N: new note
- Ctrl+D: today's note
- Ctrl+I: note info
- Ctrl+Del: delete the note
- Ctrl+H: this help
)MD";
    return text;
}

std::string options_title(const std::string &name)
{
    for (unsigned char c : name)
        if (c < 0x20 || c >= 0x7F) return "Options";
    return "Options for " + truncate_text(name, 24);
}

std::string options_body(const std::string &name)
{
    return options_title(name) == "Options" ? name : std::string();
}

const char *note_action_label(NoteAction a)
{
    switch (a) {
    case NoteAction::Edit: return "Edit";
    case NoteAction::Preview: return "Preview";
    case NoteAction::TickTask: return "Tick task on this line";
    case NoteAction::InsertStamp: return "Insert date and time";
    case NoteAction::NoteInfo: return "Note info";
    case NoteAction::Help: return "Markdown help";
    case NoteAction::Delete: return "Delete note";
    }
    return "";
}

std::string delete_body(const std::string &name) { return name + " moves to the .trash folder of the vault."; }

std::string conflict_body(const std::string &name)
{
    return name + " was changed on disk (another device or program) while you edited it. The version you do not keep is "
                  "saved beside it as a copy.";
}

std::string clock_warning_body(const std::string &date)
{
    return "The clock says " + date + " but looks unset (no network time yet). Create the note for that day anyway?";
}

std::string exists_body(const std::string &name) { return name + " is already there. Open it?"; }

std::string external_link_note() { return "There is no web browser on the deck: open it on another device."; }

/* ------------------------------------------------------------------ the preview's layout decisions */

int heading_px(int level, int px, const std::function<int(int)> &snap)
{
    switch (level) {
    case 1: return snap((px * 3 + 1) / 2);
    case 2: return snap((px * 13 + 5) / 10);
    case 3: return snap((px * 23 + 10) / 20);
    default: return snap(px);
    }
}

bool heading_gold(int level) { return level == 2; }

std::string list_marker(const Block &b)
{
    if (!b.item || b.list <= 0 || b.task) return "";
    if (b.ordered) return std::to_string(b.number) + ".";
    if (b.list == 1) return "\xE2\x80\xA2";        /* • */
    if (b.list == 2) return "\xE2\x97\xA6";        /* ◦ */
    return "\xE2\x96\xAA";                          /* ▪ */
}

std::string display_text(const std::string &t)
{
    if (t.find('\t') == std::string::npos) return t;
    std::string out;
    out.reserve(t.size() + 8);
    for (char c : t) {
        if (c == '\t') out += "    ";
        else out += c;
    }
    return out;
}

std::string properties_text(const Block &b)
{
    std::string s;
    for (const Property &p : b.props) {
        if (!s.empty()) s += '\n';
        s += p.key.empty() ? p.value : p.key + ": " + p.value;
    }
    return s;
}

std::string truncated_text()
{
    return "Preview truncated: the rest of this note is not shown here (" + std::to_string(kPreviewMaxBlocks) +
           " blocks at most). Edit shows all of it.";
}

std::string empty_note_text() { return "This note is empty. Edit to write in it."; }

std::vector<PreviewTarget> preview_targets(const Document &d)
{
    std::vector<PreviewTarget> out;
    for (std::size_t bi = 0; bi < d.blocks.size(); ++bi) {
        const Block &b = d.blocks[bi];
        if (b.task && b.task_offset != kNoOffset) out.push_back({static_cast<int>(bi), true, -1});
        int last = -1;
        for (const Run &r : b.runs) {
            if (r.link >= 0 && r.link != last) out.push_back({static_cast<int>(bi), false, r.link});
            last = r.link;
        }
    }
    return out;
}

int ring_next(int cur, int dir, int count)
{
    if (count <= 0) return -1;
    if (cur < 0 || cur >= count) return dir < 0 ? count - 1 : 0;
    return std::clamp(cur + (dir < 0 ? -1 : 1), 0, count - 1);
}

/* ------------------------------------------------------------------ the on-screen keyboard's request */

OskField osk_field_for(PageId page, bool popup, bool open_typing, bool new_typing, bool editor_typing)
{
    if (popup) return OskField::None;
    if (page == PageId::Open && open_typing) return OskField::Open;
    if (page == PageId::NewNote && new_typing) return OskField::New;
    if (page == PageId::Editor && editor_typing) return OskField::Editor;
    return OskField::None;
}

const char *osk_field_key(OskField f)
{
    switch (f) {
    case OskField::Open: return "open";
    case OskField::New: return "new";
    case OskField::Editor: return "editor";
    case OskField::None: break;
    }
    return "-";
}

OskStep osk_step(OskField want, OskField asked, OskField service_owner, bool tapped)
{
    if (want == OskField::None) return asked == OskField::None ? OskStep::Keep : OskStep::Release;
    if (asked != OskField::None && asked != want) return OskStep::Switch;
    if (asked == OskField::None || tapped || service_owner != want) return OskStep::Request;
    return OskStep::Keep;
}

/* ------------------------------------------------------------------ links */

LinkPlan plan_link(const VaultIndex &ix, const std::string &from_rel, LinkKind kind, const std::string &target)
{
    LinkPlan p;
    p.address = target;
    Resolved r;
    switch (kind) {
    case LinkKind::External: p.go = LinkGo::External; return p;
    case LinkKind::File: p.go = LinkGo::File; return p;
    case LinkKind::Wiki: r = resolve_wikilink(ix, from_rel, target, NewNoteFolder::VaultRoot); break;
    case LinkKind::Note: r = resolve_note_href(ix, from_rel, target); break;
    }
    p.heading = r.heading;
    if (r.invalid) {
        p.go = LinkGo::Invalid;
        return p;
    }
    if (r.same_note) {
        p.go = LinkGo::SameNote;
        p.rel = from_rel;
        return p;
    }
    p.rel = r.rel;
    p.go = r.found ? LinkGo::OpenNote : LinkGo::CreateNote;
    return p;
}

std::string link_problem_text(const LinkPlan &p)
{
    switch (p.go) {
    case LinkGo::File: return "Not a note: " + p.address;
    case LinkGo::Invalid: return "That link names no note that can be opened or created";
    default: return "";
    }
}

/* ------------------------------------------------------------------ the editor's text */

std::size_t char_to_byte(const std::string &s, std::size_t chars)
{
    std::size_t i = 0, n = 0;
    while (i < s.size() && n < chars) {
        utf8_next(s, i);
        ++n;
    }
    return i;
}

std::size_t byte_to_char(const std::string &s, std::size_t bytes)
{
    std::size_t i = 0, n = 0;
    const std::size_t end = std::min(bytes, s.size());
    while (i < end) {
        utf8_next(s, i);
        ++n;
    }
    return n;
}

std::size_t line_begin(const std::string &s, std::size_t byte)
{
    if (byte > s.size()) byte = s.size();
    const std::size_t nl = byte == 0 ? std::string::npos : s.rfind('\n', byte - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

std::size_t line_end(const std::string &s, std::size_t byte)
{
    if (byte > s.size()) return s.size();
    const std::size_t nl = s.find('\n', byte);
    return nl == std::string::npos ? s.size() : nl;
}

bool outdent_line(std::string &text, std::size_t &cursor, const char *tab_glyph)
{
    const std::size_t b = line_begin(text, cursor);
    std::size_t cut = 0;
    const std::size_t glyph = tab_glyph ? std::strlen(tab_glyph) : 0;
    if (b < text.size() && text[b] == '\t') cut = 1;
    else if (glyph && text.compare(b, glyph, tab_glyph) == 0) cut = glyph;
    else
        while (cut < 4 && b + cut < text.size() && text[b + cut] == ' ') ++cut;
    if (cut == 0) return false;
    text.erase(b, cut);
    if (cursor >= b + cut) cursor -= cut;
    else if (cursor > b) cursor = b;
    return true;
}

} // namespace notesui
