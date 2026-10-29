/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the app. One pane at every screen size: a page fills the screen under the shared header (title, Esc, the
 * status strip) and a grey line under it (what the page shows, or a short notice). The header holds nothing else; the
 * secondary actions of a row or of the article are in an Options dialog opened by a 3 s hold or Ctrl+O.
 *
 *   Home        Favorites (N new), Browse feeds, Saved articles (count), Add a feed, Settings; Esc asks "Quit RSS Reader?"
 *   Favorites   the Refresh bar (Refresh all / Stop, the status), the pinned feeds (unread dot, "N new", chevron)
 *   Articles    the articles of a feed (unread dot, age, "saved"); options: read / unread, save for offline / remove from
 *               saved, mark all read, refresh, info
 *   Reader      the article's text (headings, paragraphs, lists, quotes) and its images (downloaded on worker threads into
 *               the image cache, decoded off the UI thread: image_decode.hpp); a tap or Enter on an image opens it full
 *               screen (page_viewer.cpp); Left / Right the previous / next article; options: text size, save / remove,
 *               mark as unread, article info
 *   Browse      country > topic > feed, the favorite toggle at the end of a feed's row; a search over the whole catalog
 *   Saved       the saved articles, newest saved first, with their size and the use of the cap; options: remove, info
 *   Add a feed  an address, Find feed, Pin
 *   Settings    text size, refresh when opening, images, storage, clear read history, export / import OPML, about
 *
 * Network and slow work run on worker threads (jobs.hpp); the UI thread only draws and reads the store's small files.
 * Everything is sized from the layout service's tokens (no pixel constant): rows are at least the ROW token (7 mm).
 */
#pragma once

#include "catalog.hpp"
#include "fetch.hpp"
#include "image_cache.hpp"
#include "image_decode.hpp"
#include "jobs.hpp"
#include "refresh.hpp"
#include "row_list.hpp"
#include "stage.hpp"
#include "store.hpp"
#include "ui_logic.hpp"

#include "cp0_ui_widgets.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rssui {

/* What the reader walks through with Left / Right: the articles of a feed (a snapshot of the list, shared: a refresh that
 * replaces the list does not disturb the reader) or the saved articles (loaded one at a time). */
struct ReaderSource {
    bool saved = false;
    std::shared_ptr<const std::vector<rssreader::StoredArticle>> arts;
    std::vector<rssreader::SavedSummary> saved_list;
    std::string feed_id, feed_name;
    int count() const { return saved ? static_cast<int>(saved_list.size()) : arts ? static_cast<int>(arts->size()) : 0; }
};

class App {
public:
    explicit App(Platform &plat);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    void tick();
    void on_key(const KeyEvent &e);

    /* for the scripted tests */
    PageId page() const { return stack_.back(); }
    std::string debug_state() const;
    lv_obj_t *body_root() const { return body_; }
    lv_obj_t *image_tile(int index) const
    {
        return index >= 0 && index < static_cast<int>(rimgs_.size()) ? rimgs_[static_cast<std::size_t>(index)].tile : nullptr;
    }

    /* LVGL callbacks */
    void row_clicked(RowList *list, int index);
    void row_pressed(RowList *list, int what, int index, int x, int y);
    void button_clicked(int tag);
    void popup_pick(int index);
    void reader_pressed(int what, int x, int y);
    void image_tapped(int index);
    void viewer_pressed(int what, int x, int y);

private:
    struct Geo {
        int w = 0, x = 0, cw = 0, lx = 0, lw = 0, inset = 0, s = 0, m = 0, h = 0;
        int row = 0, bar = 0, two_line = 0;
    };
    Geo geo() const;

    /* What a row shows: one line (line2 empty) or two; a value at the right (before the marker); the unread dot. */
    struct RowText {
        std::string id;
        std::string line1, line2, line2_extra;     /* line2_extra: drawn right after line2 in its own colour ("  ·  saved") */
        std::uint32_t c1 = 0xFFFFFF, c2 = 0x9A9A9A, c2_extra = 0x5AAAFF;
        const lv_font_t *f1 = nullptr;             /* default: the list label face */
        std::string right;
        std::uint32_t c_right = 0x9A9A9A;
        int title_lines = 1;                       /* portrait: a title wider than the row wraps over this many lines */
        bool lead_dot = false, indent = false;     /* indent: the text starts after the unread dot's column */
        bool selectable = true, section = false;
        Mark trail = Mark::None;
    };
    Row build_row(const RowText &t) const;
    /* PDA mode: the canvas is taller than wide (X-Portrait). The landscape layouts do not look at it. */
    bool portrait() const { return layout_.h > layout_.w; }

    struct Popup {
        enum Kind { None, Choice, Confirm, Menu, Info, List } kind = None;
        std::string title, body, small;
        rssui::Choice choice;
        std::vector<std::string> items, values;
        std::vector<Mark> marks;
        int focus = 0;
        std::string yes_text = "Yes", no_text = "No";
        bool danger = false;
        std::function<void(int)> on_accept;
        std::function<void()> on_yes;
        std::uint64_t opened_ms = 0;
    };

    /* ---- construction (rss_app.cpp) */
    void load_fonts();
    const lv_font_t *font(const char *face, int px);          /* FreeType with the DejaVu / Noto CJK fallback, else builtin */
    void build_shell();
    lv_obj_t *make_page();
    lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *f, std::uint32_t color, int x, int y, int w, int h);
    lv_obj_t *make_button(lv_obj_t *parent, const char *text, int tag, bool primary);
    lv_obj_t *make_text_field(lv_obj_t *parent, int tag, std::size_t max_len, int h, const char *placeholder);
    void style_field(lv_obj_t *ta, bool editing);
    RowList::Config list_config(int max_cells) const;
    void make_list(RowList &list, lv_obj_t *parent, int y, int h, int max_cells);
    void layout_pages();                   /* after the room under the shell changed (the on-screen keyboard) */

    /* ---- navigation and the shell */
    void open(PageId p);
    void back();                           /* Esc of the keyboard, the header's Esc */
    void header_back();
    void go_up();                          /* the page goes, the one under it shows */
    void ask_quit();
    void show_page();
    void update_title();
    void set_caption(const std::string &text);
    void notice(const std::string &text, bool ok = true);
    void render();
    void invalidate() { dirty_ = true; }
    void post(std::function<void()> fn);
    void hold_tick(std::uint64_t now);
    void open_options();                   /* Ctrl+O, a 3 s hold: the options of the page's selected row / article */

    /* ---- the on-screen keyboard (the shared rule: only while no physical keyboard is there) */
    void sync_osk();
    void release_osk();
    void on_osk_inset(int inset);
    bool field_key(lv_obj_t *ta, const KeyEvent &e, std::size_t max_len);   /* typing into a field; true when used */

    /* ---- Home, Favorites, Articles, Saved (pages_lists.cpp) */
    void build_home();
    void render_home();
    void key_home(const KeyEvent &e);
    void home_activate(int index);
    void build_favorites();
    void render_favorites();
    void key_favorites(const KeyEvent &e);
    void favorite_activate(int index);
    void favorite_options(int index);
    void start_refresh(const std::vector<rssreader::Subscription> &subs, bool all, bool quiet = false);
    void stop_refresh();
    void on_refresh_result(const rssreader::RefreshResult &r, std::size_t done, std::size_t total, bool all);
    void open_articles(const rssreader::Subscription &s);
    void load_articles();
    void build_articles();
    void render_articles();
    void key_articles(const KeyEvent &e);
    void article_activate(int index);
    void article_options(int index);
    void set_article_read(int index, bool read);
    void build_saved();
    void render_saved();
    void key_saved(const KeyEvent &e);
    void saved_activate(int index);
    void saved_row_options(int index);
    /* the first-run hint ("Hold or press Ctrl+O for options") above a list, and the hairline the rows scroll under */
    void place_list(RowList &list, lv_obj_t *hint, lv_obj_t *rule, int top, int bottom);
    bool hint_shown();
    void options_opened();                 /* the hint goes for good (settings.json options_seen) */

    /* ---- Save for offline (pages_lists.cpp) */
    void save_offline(const rssreader::Article &a, const std::string &feed_id, const std::string &feed_name);
    void ask_remove_saved(const std::string &id, const std::string &title, std::function<void()> after);

    /* ---- the reader (page_reader.cpp) */
    void build_reader();
    void open_reader(ReaderSource src, int index);
    void show_article(int index);
    void render_reader_text();
    void key_reader(const KeyEvent &e);
    void reader_scroll(int dy);
    void reader_options();
    int reader_px() const;

    /* ---- the images of the reader (page_reader.cpp) */
    struct ReaderImage {
        ArticleImage src;
        ImageState state = ImageState::Loading;
        std::shared_ptr<DecodedImage> pix;        /* the thumbnail (RGB565), shared with nothing once posted */
        lv_image_dsc_t dsc{};
        lv_obj_t *tile = nullptr;
    };
    void reader_images_stop();             /* cancels the article's image jobs, frees every picture */
    void start_reader_images();
    void on_image(unsigned seq, int index, ImageState st, std::shared_ptr<DecodedImage> pix);
    lv_obj_t *make_image_tile(lv_obj_t *parent, int index);
    void style_image_tile(int index);
    void update_image_focus();
    void focus_image_step(int dir);
    int thumb_max_w() const;
    int thumb_max_h() const;
    /* The bytes of an image without the network: the saved copy of the article (saved_id, file), else the image cache. Any
     * thread. */
    static bool local_image(rssreader::Store *store, rssreader::ImageCache *cache, const std::string &saved_id, const std::string &file,
                            const std::string &url, std::string &bytes, std::string &type);
    /* Downloads an image (fetch_image: 2 MB at most, 15 s) into the image cache. Any thread. */
    static bool fetch_to_cache(rssreader::HttpClient *http, rssreader::ImageCache *cache, const std::string &url, const std::atomic<bool> *cancel,
                               std::string &bytes, std::string &type, rssreader::FetchError &error);

    /* ---- the full-screen image (page_viewer.cpp) */
    void build_viewer();
    void open_viewer(int index);
    void close_viewer();
    void viewer_show(int index);
    void viewer_step(int dir);
    void viewer_key(const KeyEvent &e);
    void viewer_tick(std::uint64_t now);
    bool viewer_open() const { return viewer_idx_ >= 0; }
    std::vector<int> viewer_list() const;  /* the images that can be shown full screen (Ready), in their order */

    /* ---- Browse (page_browse.cpp) */
    void build_browse();
    void render_browse();
    void key_browse(const KeyEvent &e);
    void browse_activate(int index);
    bool browse_back();                    /* one level up, or the search away; false at the countries */
    void toggle_favorite(const rssreader::CatalogFeed *f);
    std::vector<std::string> browse_path() const;

    /* ---- Add a feed (page_add.cpp) */
    void build_add();
    void render_add();
    void key_add(const KeyEvent &e);
    void add_find();
    void add_pin();
    void add_reset();
    void layout_add();

    /* ---- Settings (page_settings.cpp) */
    void build_settings();
    void render_settings();
    void key_settings(const KeyEvent &e);
    void settings_activate(int index);
    void export_opml();
    void choose_import();
    void import_opml(const std::string &path);

    /* ---- dialogs (rss_app.cpp) */
    void open_choice(rssui::Choice c, std::function<void(int)> on_accept);
    void open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes = "Yes",
                      const std::string &no = "Cancel", bool danger = false);
    void open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick,
                   const std::vector<std::string> &values = {}, const std::vector<Mark> &marks = {});
    void open_list(const std::string &title, const std::vector<std::string> &items, int focus, std::function<void(int)> on_pick);
    void open_info(const std::string &title, const std::string &body, const std::string &small = "");
    void popup_begin(Popup p);
    void popup_build();
    void popup_refresh();
    void popup_key(const KeyEvent &e);
    void popup_click(int tag);
    void popup_accept();
    void popup_cancel();
    void close_popup();
    lv_obj_t *dialog_label(const std::string &text, bool small, std::uint32_t color);
    lv_obj_t *dialog_row(const std::string &text, const std::string &value, Mark mark, int index);
    lv_obj_t *dialog_button(std::size_t index) const;
    bool popup_open() const { return pop_.kind != Popup::None; }

    /* ---- state */
    Platform &plat_;
    rssreader::Store store_;
    rssreader::Catalog catalog_;
    bool catalog_ok_ = false;
    std::unique_ptr<rssreader::HttpClient> http_;
    Jobs jobs_;
    std::vector<PageId> stack_{PageId::Home};
    bool dirty_ = true;
    std::uint64_t start_ms_ = 0, last_render_ms_ = 0;
    std::string notice_text_;
    bool notice_ok_ = true;
    std::uint64_t notice_ms_ = 0;
    std::vector<std::function<void()>> posted_;
    std::string title_key_;

    /* fonts: the token sizes of the layout service (Montserrat with DejaVu and Noto CJK behind it for the text of feeds) */
    const lv_font_t *f_row_ = nullptr, *f_row_bold_ = nullptr, *f_body_ = nullptr, *f_caption_ = nullptr, *f_value_ = nullptr;
    const lv_font_t *f_field_ = nullptr, *f_title_ = nullptr;
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
    RowList *hold_list_ = nullptr;
    int hold_index_ = -1;
    int hold_scroll_y_ = 0;
    std::uint64_t hold_release_ms_ = 0;   /* a hold released after its cue showed: the click of that release is not a tap */
    lv_obj_t *hold_box_ = nullptr, *hold_label_ = nullptr, *hold_track_ = nullptr, *hold_bar_ = nullptr;
    int hold_track_w_ = 0;

    /* the on-screen keyboard */
    int osk_inset_ = 0, inset_listener_ = 0;
    lv_obj_t *osk_field_ = nullptr;

    /* Home */
    RowList home_list_;
    int home_sel_ = 0;

    /* Favorites */
    lv_obj_t *fav_bar_ = nullptr, *fav_bar_label_ = nullptr, *fav_bar_value_ = nullptr;
    RowList fav_list_;
    std::vector<rssreader::Subscription> subs_;
    int fav_sel_ = 0;
    std::string fav_sel_id_;
    Jobs::Ptr refresh_job_;
    bool refreshing_ = false, refresh_all_ = false;
    std::size_t refresh_done_ = 0, refresh_total_ = 0;
    std::string refreshing_id_;            /* a single feed's refresh (options, or a feed just pinned) */
    std::vector<std::string> refresh_ids_; /* the feeds of the refresh in progress, in order */
    int refresh_failed_ = 0, refresh_new_ = 0;
    bool refresh_quiet_ = false;          /* a feed just pinned: no notice of its first update */
    std::int64_t last_auto_refresh_ = 0;

    /* Articles */
    RowList art_list_;
    lv_obj_t *art_hint_ = nullptr, *art_rule_ = nullptr, *fav_rule_ = nullptr;
    rssreader::Subscription art_feed_;
    std::shared_ptr<std::vector<rssreader::StoredArticle>> arts_ = std::make_shared<std::vector<rssreader::StoredArticle>>();
    bool arts_loading_ = false;
    int art_sel_ = 0;
    std::string art_sel_id_;
    Jobs::Ptr load_job_;
    unsigned load_seq_ = 0;

    /* Saved */
    RowList saved_list_;
    std::vector<rssreader::SavedSummary> saved_;
    int saved_sel_ = 0;
    lv_obj_t *saved_hint_ = nullptr, *saved_rule_ = nullptr, *saved_footer_ = nullptr;
    int saved_footer_h_ = 0;
    Jobs::Ptr save_job_;
    std::string saving_id_;
    int hint_seen_ = -1;                   /* settings.options_seen, read once (-1: not yet) */

    /* the reader */
    lv_obj_t *reader_ = nullptr;
    ReaderSource reader_src_;
    rssreader::Article reader_art_;        /* the article shown */
    std::string reader_feed_name_, reader_feed_id_;
    int reader_idx_ = -1;
    int reader_built_px_ = 0;
    std::string reader_saved_id_;          /* the article has a saved copy: its images come from there */
    std::map<std::string, std::string> reader_saved_files_;   /* image URL -> file of the saved copy */
    rssreader::ImageCache image_cache_{rssreader::ImageCache::default_dir()};
    std::vector<ReaderImage> rimgs_;
    std::vector<Jobs::Ptr> img_jobs_;
    unsigned img_seq_ = 0;                 /* the article's generation: results for an older one are dropped */
    int img_focus_ = -1;                   /* the image the keys act on (the ring), -1 none */
    bool reader_keys_ = false;             /* the last input of the reader was a key: the ring shows */

    /* the full-screen image */
    lv_obj_t *viewer_ = nullptr, *viewer_img_ = nullptr, *viewer_cap_ = nullptr;
    int viewer_idx_ = -1;                  /* the index in rimgs_ shown, -1 closed */
    std::shared_ptr<DecodedImage> viewer_pix_;   /* the one full-size decode alive */
    lv_image_dsc_t viewer_dsc_{};
    Jobs::Ptr viewer_job_;
    unsigned viewer_seq_ = 0;
    std::uint64_t viewer_cap_ms_ = 0;
    int viewer_px_ = 0, viewer_py_ = 0;
    bool viewer_down_ = false;

    /* Browse */
    enum class BrowseLevel { Countries, Topics, Feeds };
    BrowseLevel blevel_ = BrowseLevel::Countries;
    std::string bcountry_, btopic_;
    lv_obj_t *search_ta_ = nullptr;
    bool search_typing_ = false;
    std::string search_text_;
    RowList browse_list_;
    int browse_sel_ = 0;
    std::vector<const rssreader::CatalogFeed *> bfeeds_;   /* the rows of the feeds level or of the search */
    std::vector<int> bsel_stack_;
    std::set<std::string> pinned_;

    /* Add a feed */
    lv_obj_t *add_ta_ = nullptr, *add_find_ = nullptr, *add_status_ = nullptr, *add_pin_ = nullptr, *add_cancel_ = nullptr;
    bool add_typing_ = true;
    enum class AddState { Idle, Working, Found, Failed, Pinned } add_state_ = AddState::Idle;
    rssreader::FoundFeed add_found_;
    std::string add_message_;
    Jobs::Ptr add_job_;
    int add_focus_ = 0;                    /* 0 Pin, 1 Cancel (keys) */

    /* Settings */
    RowList settings_list_;
    int settings_sel_ = 0;
    std::vector<int> settings_kinds_;

    /* the dialog */
    Popup pop_;
    bool pop_dirty_ = false;
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::vector<lv_obj_t *> pop_items_;
    lv_obj_t *pop_value_ = nullptr, *pop_detail_ = nullptr, *pop_pos_ = nullptr, *pop_left_ = nullptr, *pop_right_ = nullptr;
};

} // namespace rssui
