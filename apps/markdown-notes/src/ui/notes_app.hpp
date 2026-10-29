/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: the app (report 037 section 4, the approved mock-ups). One pane at every screen size: a page fills the
 * screen under the shared header (cp0::ui::Page: the title, the Esc button, the status strip) and a grey line under it
 * (what the page shows, or a short notice). The header holds nothing else. RSS Reader's structure: the keys come from
 * the platform (stage.hpp, the key bridge), the lists are RowList (only the rows in view are widgets), the dialogs are
 * cp0::ui::Dialog.
 *
 *   Home          Open or create... (Ctrl+P), All notes, Recent, Today's note (Ctrl+D), New note (Ctrl+N), Settings; a
 *                 short Esc (or the header's Esc) quits
 *   Folder        New note here, FOLDERS, NOTES (newest first); a note's options (Delete, Note info) by a 3 s hold or
 *                 Ctrl+O
 *   Recent        the 20 notes edited last (options as Folder)
 *   Open          a field and the ranked matches, 'Create "query"' first when no note has that name
 *   New note      a field for the name, Create (Enter)
 *   Preview       the note drawn from md4c's model (page_preview.cpp): tap / Tab+Enter a task box ticks it, a link
 *                 follows (a missing note is created), a web address shows in a box; Edit, More (Note info, Delete)
 *   Editor        the note's text in an lv_textarea (page_editor.cpp): autosave 1.5 s after the last edit, the
 *                 changed-on-disk check, Ctrl+S / E / T / Enter, Tab; Preview, More (Tick task, Insert date and time,
 *                 Note info, Delete)
 *   Settings      Vault folder, Text size, Open notes in, Version
 *
 * The vault is one walk in memory (core vault.hpp), walked again when a list page shows after a file changed. A note
 * is read from disk each time its page shows. Everything is sized from the layout service's tokens.
 */
#pragma once

#include "row_list.hpp"
#include "stage.hpp"
#include "ui_logic.hpp"

#include "markdown.hpp"
#include "note_file.hpp"
#include "settings.hpp"
#include "vault.hpp"

#include "cp0_ui_widgets.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace notesui {

constexpr const char *kAppTitle = "Markdown Notes";   /* the app's name: the notice, the logs, Settings */
constexpr const char *kHomeTitle = "Notes";           /* the Home page's header (longer titles are cut at 640x480) */
constexpr int kMinW = 640;   /* = X-MinSize of the .desktop file */
constexpr int kMinH = 480;
constexpr int kPortraitW = 480;   /* = X-Portrait of the .desktop file (PDA mode: the smallest portrait canvas) */
constexpr int kPortraitH = 640;

/* One line on stderr, "[markdown-notes] ..." (the launcher's journal). */
void log_line(const std::string &s);

class App {
public:
    explicit App(Platform &plat);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    void tick();
    void on_key(const KeyEvent &e);

    /* for the scripted tests */
    PageId page() const { return stack_.back().page; }
    std::string debug_state() const;
    const std::string &vault_root() const { return index_.root; }
    lv_obj_t *task_box(int n) const;                 /* the n-th task box of the preview (0 first), NULL */
    lv_obj_t *preview_scroller() const { return pv_; }

    /* LVGL callbacks */
    void row_clicked(RowList *list, int index);
    void row_pressed(RowList *list, int what, int index, int x, int y);
    void button_clicked(int tag);
    void popup_pick(int index);
    void preview_clicked(int x, int y);
    /* A finger on the preview or on the editor's text (what: 0 down, 1 moved, 2 up or lost): the 3 s hold that opens the
     * note's options; a plain tap on the editor's text places the cursor (editor_tapped), a hold never does. */
    void note_pressed(lv_obj_t *scroller, int what, int x, int y);
    void editor_tapped(int x, int y);
    void preview_touched();                 /* the ring gives way to the touch */

private:
    struct Geo {
        int w = 0, x = 0, cw = 0, lx = 0, lw = 0, inset = 0, s = 0, m = 0, h = 0;
        int row = 0, two_line = 0, key = 0;
    };
    Geo geo() const;

    /* A page of the stack: what it shows (the folder of a Folder page, the note of a Preview / Editor). */
    struct Nav {
        PageId page = PageId::Home;
        std::string folder;
        std::string note;             /* rel */
        int sel = 0;                  /* the list's selection when it was left */
        int scroll = -1;              /* the preview's scroll when it was left (-1: top) */
        bool typing = false;          /* the editor was being typed into when it was left */
    };

    /* The note of the Preview / Editor on top. */
    struct Note {
        std::string rel, path, name;
        mdnotes::LoadedNote file;
        bool dirty = false;           /* the editor's text differs from the file */
        std::uint64_t edit_ms = 0;    /* the last edit (the autosave runs kAutosaveMs after it) */
        std::uint64_t retry_ms = 0;   /* a failed save is tried again after this */
        bool open() const { return !rel.empty(); }
    };

    struct Popup {
        enum Kind { None, Choice, Confirm, Menu, Info } kind = None;
        std::string title, body, small;
        notesui::Choice choice;
        std::vector<std::string> items, values;
        std::vector<Mark> marks;
        int focus = 0;
        std::string yes_text = "Yes", no_text = "No";
        bool danger = false;
        bool yn_keys = true;          /* Y and N answer a question (not the changed-on-disk one: it may pop up mid-typing) */
        std::function<void(int)> on_accept;
        std::function<void()> on_yes, on_no;
        std::uint64_t opened_ms = 0;
    };

    /* ---- construction (notes_app.cpp) */
    void load_fonts();
    /* FreeType from the launcher's fonts with DejaVu Sans and Noto CJK behind it (any script in a note), else the
     * builtin Montserrat. italic: FreeType's synthetic oblique (there is no italic face). */
    const lv_font_t *font(const char *face, int px, bool italic = false);
    void build_shell();
    lv_obj_t *make_page(PageId id);
    lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *f, std::uint32_t color, int x, int y, int w, int h);
    lv_obj_t *make_button(lv_obj_t *parent, const char *text, int tag, bool primary);
    lv_obj_t *make_text_field(lv_obj_t *parent, int tag, std::size_t max_len, int h, const char *placeholder);
    void style_field(lv_obj_t *ta, bool editing);
    RowList::Config list_config(int max_cells) const;
    void make_list(RowList &list, lv_obj_t *parent, int y, int h, int max_cells);
    Row build_row(const ListItem &it) const;
    std::vector<Row> build_rows(const std::vector<ListItem> &items) const;
    void layout_pages();                   /* after the room under the shell changed (the on-screen keyboard) */

    /* ---- the vault */
    void open_vault();
    void rewalk();                         /* a new walk (after a file was made, deleted or saved) */
    std::string vault_shown() const;

    /* ---- navigation and the shell */
    void push(Nav n);
    void replace_top(Nav n);               /* Preview <-> Editor, New note -> Editor */
    void go_up();
    void back(bool from_header);           /* Esc of a keyboard (false) or the header's Esc (true) */
    void header_back();
    void enter_page();                     /* the page on top shows (again): its data read anew */
    void leave_page();                     /* the page on top goes: what it holds is saved first */
    /* What leaving the editor needs first (a save, perhaps the changed-on-disk question), then `then`. */
    void with_saved_note(std::function<void()> then);
    void show_page();
    void update_title();
    void set_caption(const std::string &text);
    void notice(const std::string &text, bool ok = true);
    void render();
    void invalidate() { dirty_ = true; }
    void post(std::function<void()> fn);
    void hold_tick(std::uint64_t now);
    bool tap_after_hold();                 /* the release of a hold (done, or given up after its cue) is not a tap */
    void open_options();                   /* Ctrl+O, a 3 s hold: the options of the selected row / of the note shown */
    void note_options();                   /* the options box of the preview / editor (no bars: owner decision) */
    void note_action(NoteAction a);
    void preview_to_editor();              /* Ctrl+E, Edit */
    void editor_to_preview();              /* Ctrl+E, Preview (saved first) */
    void delete_this_note();               /* Ctrl+Del, Delete note (asked first) */
    void open_help();                      /* Ctrl+H, Markdown help: over the preview or the editor, which stay as they are */
    bool keyboard_present();               /* a physical keyboard there (input.state): the captions' hint */
    bool global_key(const KeyEvent &e);    /* Ctrl+P, Ctrl+N, Ctrl+D anywhere a field does not take them */
    void lifecycle_tick();

    /* ---- the on-screen keyboard (the shared rule: only while no physical keyboard is there) */
    /* The field being typed into holds a request for the keyboard (ui_logic osk_step): asked again when the service
     * lost it, and at every tap on the field; the rule alone decides whether it shows. */
    void sync_osk();
    lv_obj_t *osk_object(OskField f) const;
    OskField osk_field_of(lv_obj_t *obj) const;
    void release_osk();
    void on_osk_inset(int inset);
    bool field_key(lv_obj_t *ta, const KeyEvent &e, std::size_t max_len);   /* typing into a one-line field */

    /* ---- Home, Folder, Recent, Settings (page_lists.cpp) */
    void build_home();
    void render_home();
    void key_home(const KeyEvent &e);
    void home_activate(const std::string &id);
    void build_folder();
    void render_folder();
    void build_recent();
    void render_recent();
    void key_list(RowList &list, const KeyEvent &e);   /* Folder, Recent */
    void list_activate(RowList &list, int index);
    void note_options(const std::string &rel);
    void note_options_action(const std::string &rel, NoteAction a);
    void build_settings();
    void render_settings();
    void key_settings(const KeyEvent &e);
    void settings_activate(int index);
    void save_settings();
    void place_hint(lv_obj_t *hint, RowList &list, int top);

    /* ---- Open or create, New note (page_open.cpp) */
    void build_open();
    void render_open();
    void layout_open();
    void key_open(const KeyEvent &e);
    void refresh_open_rows();
    void open_activate(int index);
    void build_new();
    void render_new();
    void layout_new();
    void key_new(const KeyEvent &e);
    void new_create();

    /* ---- the notes: open, create, delete (notes_app.cpp) */
    enum class OpenMode { Default, Preview, Editor };
    /* Reads the note and shows it (push, or replace the top page): false when it cannot be read (a notice says why). */
    bool open_note(const std::string &rel, OpenMode mode, bool replace = false, const std::string &heading = std::string());
    bool load_note_into(const std::string &rel);   /* note_ from the disk */
    void create_and_edit(const std::string &rel, bool replace);
    void open_today();
    /* "Delete this note?" Yes: before() (the editor saves its text), the move to .trash, after(). */
    void ask_delete(const std::string &rel, std::function<void()> after, std::function<void()> before = nullptr);
    void show_note_info();
    void follow_link(const LinkPlan &plan);

    /* ---- the preview (page_preview.cpp) */
    void build_preview();
    void layout_preview();
    void render_preview_content(bool keep_scroll);
    void key_preview(const KeyEvent &e);
    void preview_scroll(int dy);
    void preview_ring(int dir);
    void preview_act(int target);           /* the ringed / tapped target: tick its box, follow its link */
    void toggle_task(int block);
    void update_ring();
    void scroll_to_heading(const std::string &heading);
    int preview_px() const;

    /* ---- the editor (page_editor.cpp) */
    void build_editor();
    void layout_editor();
    void editor_load();                     /* the textarea from note_ */
    void key_editor(const KeyEvent &e);
    void editor_insert(const std::string &text);
    /* Typed text waits for the next tick (or the next key that is not typing): one textarea update for every key that
     * came in meanwhile. The textarea's work per update grows with the note (a full relayout), so a slow board falls
     * behind less when keys come faster than it draws. */
    void editor_queue(const std::string &text);
    void editor_flush();
    void editor_changed();
    void editor_tick(std::uint64_t now);
    enum class SaveWhy { Auto, Now, Leave, Quit };
    /* Saves the editor's text. true: saved (or nothing to save); false: not saved (a conflict asked, or an error).
     * then: run once the note is safe (after the conflict's answer too). */
    bool editor_save(SaveWhy why, std::function<void()> then = nullptr);
    void resolve_conflict(bool keep_mine, std::function<void()> then);
    void editor_tick_task();
    void editor_ctrl_enter();
    std::string editor_text() const;        /* the body (tabs back from the glyph) */
    std::size_t editor_cursor_byte() const; /* in the textarea's text */
    int editor_px() const;

    /* ---- dialogs (notes_app.cpp, RSS Reader's) */
    void open_choice(notesui::Choice c, std::function<void(int)> on_accept);
    void open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes = "Yes",
                      const std::string &no = "No", bool danger = false, std::function<void()> on_no = nullptr);
    void open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick,
                   const std::vector<std::string> &values = {}, const std::vector<Mark> &marks = {});
    void open_info(const std::string &title, const std::string &body, const std::string &small = "");
    void popup_begin(Popup p);
    void popup_build();
    void popup_refresh();
    void popup_key(const KeyEvent &e);
    void popup_click(int tag);
    void popup_accept();
    void popup_cancel();
    void popup_no();
    void close_popup();
    lv_obj_t *dialog_label(const std::string &text, bool small, std::uint32_t color);
    lv_obj_t *dialog_row(const std::string &text, const std::string &value, Mark mark, int index);
    lv_obj_t *dialog_button(std::size_t index) const;
    bool popup_open() const { return pop_.kind != Popup::None; }

    /* ---- state */
    Platform &plat_;
    std::string home_;
    std::string settings_path_;
    mdnotes::Settings settings_;
    bool vault_from_env_ = false;
    mdnotes::VaultOpen vault_;
    mdnotes::VaultIndex index_;
    bool index_stale_ = false;
    std::vector<Nav> stack_{Nav{}};
    Note note_;
    bool dirty_ = true;
    std::uint64_t start_ms_ = 0, last_render_ms_ = 0;
    std::string notice_text_;
    bool notice_ok_ = true;
    std::uint64_t notice_ms_ = 0;
    std::vector<std::function<void()>> posted_;
    std::string title_key_;
    bool was_hidden_ = false;
    bool help_return_ = false;             /* the page now shown again was under the help page: not read again */
    std::string last_save_;                /* what the last save did (the state line of the tests) */

    /* fonts */
    const lv_font_t *f_row_ = nullptr, *f_body_ = nullptr, *f_caption_ = nullptr, *f_value_ = nullptr;
    const lv_font_t *f_field_ = nullptr, *f_title_ = nullptr, *f_button_ = nullptr;
    std::map<std::string, const lv_font_t *> fonts_;
    std::vector<lv_font_t *> owned_fonts_;

    /* the shell */
    cp0_ui_roller_page_t layout_{};
    std::unique_ptr<cp0::ui::Page> shell_;
    lv_obj_t *body_ = nullptr, *cap_ = nullptr;
    int body_y_ = 0, panel_h_ = 0, full_h_ = 0;
    std::map<PageId, lv_obj_t *> pages_;

    /* the 3 s hold */
    HoldTracker hold_;
    RowList *hold_list_ = nullptr;         /* the row held, or nullptr: the note (hold_scroller_) */
    lv_obj_t *hold_scroller_ = nullptr;    /* what must not scroll while the hold lasts */
    int hold_index_ = -1;
    int hold_scroll_y_ = 0;
    std::uint64_t hold_release_ms_ = 0;
    lv_obj_t *hold_box_ = nullptr, *hold_label_ = nullptr, *hold_track_ = nullptr, *hold_bar_ = nullptr;
    int hold_track_w_ = 0;

    /* the on-screen keyboard */
    int osk_inset_ = 0, inset_listener_ = 0;
    lv_obj_t *osk_field_ = nullptr;
    bool osk_tap_ = false;                 /* a field was tapped since the last sync: ask again */

    /* Home */
    RowList home_list_;
    lv_obj_t *home_hint_ = nullptr;
    std::vector<ListItem> home_items_;

    /* Folder, Recent */
    RowList folder_list_, recent_list_;
    lv_obj_t *folder_hint_ = nullptr, *recent_hint_ = nullptr;
    std::vector<ListItem> folder_items_, recent_items_;

    /* Open or create */
    lv_obj_t *open_ta_ = nullptr;
    RowList open_list_;
    std::vector<ListItem> open_items_;
    std::string open_query_;
    bool open_typing_ = false;
    int open_sel_ = 0;

    /* New note */
    lv_obj_t *new_ta_ = nullptr, *new_create_ = nullptr, *new_status_ = nullptr;
    bool new_typing_ = false;
    std::string new_error_;

    /* the preview */
    lv_obj_t *pv_ = nullptr;
    mdnotes::Document doc_;
    std::vector<PreviewTarget> targets_;
    struct TaskBox {
        lv_obj_t *box = nullptr;
        int block = -1;
    };
    std::vector<TaskBox> boxes_;
    struct LinkSpan {
        lv_obj_t *group = nullptr;
        lv_span_t *span = nullptr;
        int link = -1;
        int block = -1;
    };
    std::vector<LinkSpan> spans_;
    std::vector<lv_obj_t *> block_objs_;   /* the outermost object of each block (scrolling to it) */
    lv_obj_t *ring_obj_ = nullptr;         /* the outline drawn round a ringed link */
    int ring_ = -1;
    bool preview_keys_ = false;            /* the last input of the preview was a key: the ring shows */
    int preview_built_px_ = 0;

    /* the editor */
    lv_obj_t *ed_ta_ = nullptr;
    bool kbd_present_ = true;              /* the captions' hint (keyboard_present, looked at once a second) */
    std::uint64_t kbd_check_ms_ = 0;
    bool editor_typing_ = false;
    std::size_t ed_bytes_ = 0;
    std::string ed_pending_;               /* typed, not in the textarea yet (editor_queue) */

    /* Settings */
    RowList settings_list_;
    std::vector<ListItem> settings_items_;
    int settings_sel_ = 1;

    /* the dialog */
    Popup pop_;
    bool pop_dirty_ = false;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::vector<lv_obj_t *> pop_items_;
    lv_obj_t *pop_value_ = nullptr, *pop_detail_ = nullptr, *pop_pos_ = nullptr, *pop_left_ = nullptr, *pop_right_ = nullptr;
};

} // namespace notesui
