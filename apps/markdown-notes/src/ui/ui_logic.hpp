/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: the UI-independent logic of the pages (RSS Reader's ui_logic pattern). The rows of the lists and the
 * lines under the titles, the words for a refused name or a failure, the menus, the choices of Settings and the text
 * sizes, the preview's layout decisions (heading sizes, list markers, the ring's targets), the editor's text offsets
 * (lv_textarea counts characters, the core counts bytes), the key rules of the dialogs and the 3 s hold. No LVGL, no
 * drawing: unit tested on the PC (tests/unit/test_ui.cpp). The LVGL side is notes_app*.cpp and page_*.cpp.
 */
#pragma once

#include "keys.hpp"

#include "markdown.hpp"
#include "names.hpp"
#include "note_file.hpp"
#include "settings.hpp"
#include "vault.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace notesui {

/* ------------------------------------------------------------------ the pages */

enum class PageId { Home, Folder, Recent, Open, NewNote, Preview, Editor, Settings, Help };
const char *page_key(PageId p);              /* "home", "folder", ... (the scripted tests' state line) */

extern const char *const kSep;               /* "  ·  ": the separator of the grey lines */
extern const char *const kUnsavedDot;        /* " ●" after an unsaved note's name */

/* ------------------------------------------------------------------ dialogs (RSS Reader's rules) */

struct Choice {
    std::string title;
    std::vector<std::string> labels;
    std::vector<std::string> details;       /* optional second line per label */
    std::string note;
    int index = 0;
    bool step(int delta);                   /* clamps at both ends; true when the index moved */
    bool can_prev() const { return index > 0; }
    bool can_next() const { return index + 1 < static_cast<int>(labels.size()); }
    const std::string &label() const;
    std::string detail() const;
    std::string position() const;           /* "2 / 3" */
};
enum class ChoiceResult { None, Moved, Accept, Cancel };
/* Left / Right (also Up / Down) step, Enter accepts, Esc cancels. A repeat of Enter or Esc is ignored. */
ChoiceResult choice_key(const KeyEvent &e, Choice &c);

/* The two-button box. focus: 0 = the left (No / Cancel) button, 1 = the right (Yes). Y and N answer at once, Left /
 * Right / Tab move, Enter takes the focus, Esc is No. */
enum class ConfirmResult { None, Moved, Yes, No };
ConfirmResult confirm_key(const KeyEvent &e, int &focus);

/* A menu: Up / Down (Left / Right) move, Home / End, Enter picks, Esc cancels. */
enum class MenuResult { None, Moved, Accept, Cancel };
MenuResult menu_key(const KeyEvent &e, int &selected, int count);

/* A touch held on a row for 3 s opens its options (RSS Reader's and Mesh Hop's rule). The cue appears after kShowMs so
 * a tap or a scroll never flashes it; a move of more than kSlop pixels, a release or a lost press cancels. */
class HoldTracker {
public:
    static constexpr std::uint64_t kShowMs = 500, kFireMs = 3000;
    static constexpr int kSlop = 16;
    void begin(const std::string &target, int x, int y, std::uint64_t now);
    void move(int x, int y);
    void cancel();
    bool active() const { return active_; }
    const std::string &target() const { return target_; }
    bool visible(std::uint64_t now) const { return active_ && now - start_ >= kShowMs; }
    double progress(std::uint64_t now) const;       /* 0..1 over kFireMs */
    /* True exactly once when the hold has lasted kFireMs; the click of the release is then to be swallowed. */
    bool poll(std::uint64_t now);
    bool take_swallow();

private:
    bool active_ = false, swallow_ = false;
    std::string target_;
    int x_ = 0, y_ = 0;
    std::uint64_t start_ = 0;
};

/* Up / Down / Home / End / Page Up / Page Down over `count` rows (a page = `page` rows); rows for which `skip` is true
 * (section headings) are stepped over. True when the key was one of these (the selection may not have moved). */
bool list_move(const KeyEvent &e, int &sel, int count, int page, const std::function<bool(int)> &skip = nullptr);

/* ------------------------------------------------------------------ layout */

/* The column the list pages are laid out in (Mesh Hop's page_column): one SPACE_L margin each side on the smallest
 * screen, two once the screen is that much wider, never wider than cap_w. */
struct PageColumn {
    int x = 0, w = 0;
};
PageColumn page_column(int screen_w, int inset, int min_w, int cap_w);

/* ------------------------------------------------------------------ formatting */

std::string plural(long n, const char *one, const char *many);   /* "1 note", "6 notes" */
std::string truncate_text(const std::string &s, std::size_t max_chars);   /* by characters, "..." added when cut */
std::string fmt_size(std::int64_t bytes);         /* "342 bytes", "3.4 KB", "120 KB", "1.2 MB" */
/* path under home shown as "~/...", else as it is. */
std::string tilde_path(const std::string &path, const std::string &home);
/* "edited 2 h ago" (core age_text), "" for an unknown time. */
std::string edited_text(std::int64_t mtime_ns, std::int64_t now_s);

/* ------------------------------------------------------------------ rows */

/* What a row does (the guidelines' row kinds; row_list draws the marker): Open a chevron, Edit the value then a
 * chevron, Action a dot, Info / None nothing (Info: a grey read-out the selection skips). */
enum class Affordance { None, Open, Edit, Action, Info };

struct ListItem {
    std::string id;               /* what the row stands for: "open", "note:<rel>", "folder:<rel>", "new", "create" ... */
    std::string label;            /* the first line */
    std::string line2;            /* a grey second line, "" for a one-line row */
    std::string value;            /* the value at the right */
    bool section = false;         /* a grey heading in capitals, never selected */
    bool warn = false;            /* line2 says what is wrong (the warn tone) */
    Affordance aff = Affordance::None;
};

/* Home: Open or create... (Ctrl+P), All notes (the count), Recent (last 20), Today's note (the date), New note (+),
 * Settings. Ids: open, all, recent, today, new, settings. */
std::vector<ListItem> home_rows(std::size_t notes, const std::string &today);
/* "Vault: ~/Notes  ·  184 notes" */
std::string home_caption(const std::string &vault_shown, std::size_t notes);
/* The bottom line of Home and of the folder pages (the mock-up's key hints). */
std::string home_hint();
std::string home_hint_short();           /* the portrait column (480 px wide) when the whole hint does not fit */
std::string folder_hint();

/* A folder page: "New note here" (Action, +), then FOLDERS (each Open, its notes in it and below) and NOTES (Open,
 * "edited 2 h ago", newest first). Ids: new, folder:<rel>, note:<rel>. */
std::vector<ListItem> folder_rows(const mdnotes::VaultIndex &ix, const std::string &rel, std::int64_t now_s);
/* "12 notes  ·  2 folders", "1 note", "No notes here yet" */
std::string folder_caption(int notes, int folders);
/* The header path of a folder page: {"All notes"} for the vault root, {"Notes", "Projects", "Mesh"} below it (the
 * shared Page shortens a path that does not fit). */
std::vector<std::string> folder_title(const std::string &rel);
/* Recent: the n most recently edited notes, "edited 2 h ago  ·  Projects/Mesh". Ids: note:<rel>. */
std::vector<ListItem> recent_rows(const mdnotes::VaultIndex &ix, std::int64_t now_s, std::size_t n = 20);
std::string recent_caption(std::size_t shown);

/* ------------------------------------------------------------------ names: Open or create, New note */

/* A refused name in words (names.hpp NameError). */
std::string name_error_text(const mdnotes::NameCheck &c);

/* Where a typed name goes: in_folder (a folder page's "New note here"; "" = the vault root) unless the text holds a
 * '/', which makes it a path from the vault root. ok: rel ("Folder/Name.md") may be created; exists: a note of that name
 * (ignoring case) is already in that folder (existing_rel: it). The folder is read now (names.hpp find_existing_ci). */
struct NewTarget {
    bool ok = false;
    std::string rel;              /* "Projects/Antenna ideas.md" */
    std::string name;             /* "Antenna ideas" */
    std::string error;            /* why not, in words */
    bool exists = false;
    std::string existing_rel;
};
NewTarget new_note_target(const std::string &vault_root, const std::string &in_folder, const std::string &typed);

/* The Open or create page: a 'Create "query"' row first (Action, +) when no note has exactly that name (refused: the
 * reason as its second line, warn), then the matches ranked by the core (fuzzy.hpp: the name, the folder as the second
 * line). An empty query lists the recent notes. Ids: create, note:<rel>. */
std::vector<ListItem> open_rows(const mdnotes::VaultIndex &ix, const std::string &vault_root, const std::string &query,
                                std::int64_t now_s, std::size_t limit = 50);
std::string open_caption(bool empty_query, std::size_t matches);

/* ------------------------------------------------------------------ Settings */

Choice text_size_choice(mdnotes::TextSize current);
mdnotes::TextSize text_size_from_index(int index);
std::string text_size_label(mdnotes::TextSize s);              /* "Small", "Medium", "Large" */
Choice open_in_choice(mdnotes::OpenIn current);
mdnotes::OpenIn open_in_from_index(int index);
std::string open_in_label(mdnotes::OpenIn o);                  /* "Preview", "Editor" */
/* The preview's text size (RSS Reader's reading sizes): Small the CAPTION size, Medium BODY, Large halfway between
 * HEADING and TITLE; snap() rounds a size to one the font set has. */
int preview_text_px(mdnotes::TextSize s, int caption_px, int body_px, int heading_px, int title_px, const std::function<int(int)> &snap);
/* The editor's monospace size: Small CAPTION, Medium a third of the way to BODY, Large BODY (a monospace letter is
 * wider: about 50 columns at Medium on the deck). */
int editor_text_px(mdnotes::TextSize s, int caption_px, int body_px, const std::function<int(int)> &snap);
/* What the on-screen keyboard's shared rule says now and why (Settings, the owner's check on a board): the launcher's
 * input.state as the app read it (cdk::osk_policy) and the rule's times (cp0_app_contract.h). */
struct OskFacts {
    bool have_file = false;            /* input.state read; else the app's own probe (started without the launcher) */
    unsigned keyboards = 0;
    int vkb = 0;                       /* cp0_app_vkb_t: 0 auto, 1 off, 2 force */
    std::int64_t now_mono = 0;         /* monotonic s, the clock of kb_gone / bt_gone */
    std::int64_t kb_gone = 0, bt_gone = 0;   /* monotonic s when the last keyboard / Bluetooth keyboard went away, 0 never */
    int asleep_s = 600;                /* CP0_APP_OSK_BT_ASLEEP_S: a Bluetooth keyboard that went away still counts */
    int grace_s = 2;                   /* CP0_APP_OSK_GRACE_S */
    int fallback_keyboards = -1;       /* no file: the keyboards the app found (-1 unknown) */
};
std::string osk_value(bool allowed);                       /* "allowed: yes" / "allowed: no" */
/* "Bluetooth keyboard asleep 8 min more  ·  keyboards=0 vkb=auto", "keyboards=0 vkb=auto" ... */
std::string osk_detail(const OskFacts &f, bool allowed);

/* VAULT: Folder (Info: the path; "Set by NOTES_VAULT" under it when the variable chose it); DISPLAY: Text size, Open
 * notes in (Edit); ABOUT: Version (Info), On-screen keyboard (Info: osk_value, osk_detail under it; no row when
 * osk_value is empty). Ids: vault, text_size, open_in, version, osk. */
std::vector<ListItem> settings_rows(const std::string &vault_shown, bool from_env, mdnotes::TextSize size, mdnotes::OpenIn open_in,
                                    const std::string &version, const std::string &osk_value = std::string(),
                                    const std::string &osk_detail = std::string());
std::string settings_caption(const std::string &version);   /* "Markdown Notes 0.1.0" */

/* ------------------------------------------------------------------ the note pages */

std::string editor_title(const std::string &name, bool unsaved);   /* "Deck build log ●" while unsaved */
/* The line under the title of the note pages ends with how to reach the options: "Hold 3 s: options" without a physical
 * keyboard (the touch way: there is no bar), "Ctrl+O: options" with one. */
std::string options_hint(bool keyboard);
std::string editor_caption(std::size_t bytes, bool keyboard);        /* "Editor  ·  342 bytes  ·  Hold 3 s: options" */
/* "Preview  ·  edited 2 h ago  ·  Hold 3 s: options"; read-only notes say why: "Preview  ·  read only, over 128 KB  ·  ...",
 * "... not UTF-8 ...". */
std::string preview_caption(mdnotes::LoadStatus st, std::int64_t mtime_ns, std::int64_t now_s, bool keyboard);
/* Edit on a read-only note. */
std::string readonly_text(mdnotes::LoadStatus st);
/* A note that cannot be opened: "Too large to open (over 1 MB)", "That note is gone", "Cannot read it: <reason>". */
std::string load_failure_text(mdnotes::LoadStatus st, int err);

struct TextStats {
    std::size_t lines = 0, words = 0;
};
TextStats text_stats(const std::string &body);
/* "LF, UTF-8", "CRLF, UTF-8 with BOM", "mixed line ends (saved as LF), UTF-8". */
std::string format_text(const mdnotes::TextFormat &fmt, bool utf8);
/* The Note info box: path, size, edited, lines, words, format. */
std::string note_info_text(const std::string &rel, std::int64_t size, std::int64_t mtime_ns, std::int64_t now_s,
                           const std::string &body, const mdnotes::TextFormat &fmt, bool utf8);

/* The options box of a note (a 3 s hold on the preview or on the editor's text, or Ctrl+O; owner decision 2026-10-24:
 * no bars), and a note's options in a list (a 3 s hold on its row, Ctrl+O). Every action has its Ctrl key, shown grey
 * at the right of its row. */
enum class NoteAction { Edit, Preview, TickTask, InsertStamp, NoteInfo, Help, Delete };
/* Preview, Tick task on this line, Insert date and time, Note info, Markdown help, Delete note */
std::vector<NoteAction> editor_actions();
std::vector<NoteAction> preview_actions();   /* Edit, Note info, Markdown help, Delete note */
std::vector<NoteAction> list_note_actions(); /* Delete note, Note info */
const char *note_action_label(NoteAction a);
const char *note_action_key(NoteAction a);   /* "Ctrl+E", "Ctrl+Enter", "Ctrl+T", "Ctrl+I", "Ctrl+H", "Ctrl+Del" */
bool note_action_asks(NoteAction a);         /* it opens a box or a page (Note info, Markdown help, Delete note): the chevron */

/* The Markdown help page (0.1.2, Ctrl+H or the options box): a built-in Markdown text drawn by the preview, read only.
 * For each syntax what you type (in a code block) and how it looks; a line on what is not supported yet; the keys. */
const std::string &markdown_help_text();
std::string help_caption();                  /* the line under its title */
/* Every Ctrl key of the app, as the help page lists them ("Ctrl+O", "Ctrl+E", ...). */
const std::vector<std::string> &app_ctrl_keys();
/* The box's title: "Options for Deck build log" (the name cut at 24 characters); "Options" when the name is not plain
 * ASCII (the toolkit draws dialog titles in its builtin face), the name then going in options_body(). */
std::string options_title(const std::string &name);
std::string options_body(const std::string &name);

/* The dialogs' texts (their titles stay ASCII: the toolkit's face; names go in the bodies). */
std::string delete_body(const std::string &name);         /* "Antenna ideas moves to the .trash folder of the vault." */
std::string conflict_body(const std::string &name);
std::string clock_warning_body(const std::string &date);  /* Today's note while the clock looks unset */
std::string exists_body(const std::string &name);         /* a New note whose name is taken */
std::string external_link_note();                          /* under the address of a web link */

/* ------------------------------------------------------------------ the preview's layout decisions */

/* Heading sizes from the reading size: h1 x1.5, h2 x1.3, h3 x1.15, h4-h6 the reading size (all bold); h2 in gold, as
 * the mock-up. */
int heading_px(int level, int px, const std::function<int(int)> &snap);
bool heading_gold(int level);
/* The marker of a list item's first block: "•" depth 1, "◦" depth 2, "▪" deeper; "3." numbered. "" for no item. */
std::string list_marker(const mdnotes::Block &b);
/* What LVGL draws for a text: a tab as four spaces (LVGL draws nothing for '\t'). */
std::string display_text(const std::string &t);
/* The "key: value" lines of the frontmatter. */
std::string properties_text(const mdnotes::Block &b);
std::string truncated_text();                              /* the muted line under a cut preview */
std::string empty_note_text();

/* What Tab / Shift+Tab ring over, in reading order: each task box, then each link of each block (runs of the same link
 * once). */
struct PreviewTarget {
    int block = -1;
    bool task = false;
    int link = -1;                /* !task: the index into Document::links */
};
std::vector<PreviewTarget> preview_targets(const mdnotes::Document &d);
/* The next ring position: from none (-1) dir 1 gives the first, dir -1 the last; it stops at both ends. */
int ring_next(int cur, int dir, int count);

/* ------------------------------------------------------------------ the on-screen keyboard's request */

/* Which field asks for the on-screen keyboard (the shared rule then decides whether it shows; it is never shown by
 * hand): the Open page's field or New note's while they are typed into, the editor's text while typing; none under a
 * dialog. */
enum class OskField { None, Open, New, Editor };
OskField osk_field_for(PageId page, bool popup, bool open_typing, bool new_typing, bool editor_typing);
const char *osk_field_key(OskField f);       /* "open", "new", "editor", "-" (the tests' state line) */

/* What syncing the request does, from the field wanted, the field the app asked for last and the field the keyboard
 * service holds now (cp0::ui::osk::owner()):
 *   Keep     the wanted field holds the request: nothing (the service asks the rule again every 250 ms, so a request
 *            denied while a keyboard was there shows the keyboard when the rule allows it)
 *   Request  ask for the wanted field: nothing asked yet, the service no longer holds it (dropped, taken by another),
 *            or the field was tapped (a tap always asks again)
 *   Switch   give the old field back, ask for the new one
 *   Release  nothing wants it any more */
enum class OskStep { Keep, Request, Switch, Release };
OskStep osk_step(OskField want, OskField asked, OskField service_owner, bool tapped);

/* ------------------------------------------------------------------ links */

enum class LinkGo { OpenNote, CreateNote, SameNote, External, File, Invalid };
struct LinkPlan {
    LinkGo go = LinkGo::Invalid;
    std::string rel;              /* OpenNote / CreateNote: the note */
    std::string heading;          /* the part after '#' */
    std::string address;          /* External / File / Invalid: the target as written */
};
/* What following a link does: a wikilink or a relative .md link to a note opens it (missing: creates it, at the vault
 * root for a bare [[Name]]); a web address shows in a box; a local file that is not a note is named. */
LinkPlan plan_link(const mdnotes::VaultIndex &ix, const std::string &from_rel, mdnotes::LinkKind kind, const std::string &target);
std::string link_problem_text(const LinkPlan &p);

/* ------------------------------------------------------------------ the editor's text */

/* lv_textarea counts characters (code points), the core counts bytes. */
std::size_t char_to_byte(const std::string &s, std::size_t chars);
std::size_t byte_to_char(const std::string &s, std::size_t bytes);
std::size_t line_begin(const std::string &s, std::size_t byte);   /* the byte after the '\n' before it (0) */
std::size_t line_end(const std::string &s, std::size_t byte);     /* the '\n' at or after it (size) */
/* Shift+Tab: one indent step off the start of the cursor's line: a tab (or the tab glyph), else up to 4 spaces. The
 * cursor (a byte offset) follows. False when the line has no indent. */
bool outdent_line(std::string &text, std::size_t &cursor, const char *tab_glyph);

} // namespace notesui
