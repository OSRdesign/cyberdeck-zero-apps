/*
 * SPDX-License-Identifier: MIT
 *
 * The template app's screen: one cp0::ui::Page (header title, the shared status strip, the on-screen Esc) and three
 * bodies swapped under it, all laid out by cp0_lvgl's layout service (tokens only, no geometry here):
 *   root   a Roller: Device info (Nav), Name (Nav, its value in green), Example switch (Toggle), Reset (Nav, opens a
 *          Dialog), Run a command (Busy while it runs, driven by async_run), Screen (Info)
 *   info   an InfoTable of what the app learnt from the launcher contract and the layout service
 *   form   a TextField (the app-side on-screen keyboard shows under the shared rule when no keyboard is there)
 * Esc / Left / the header's Esc go one level up; at the root they end the app (cdk::request_quit).
 * LVGL thread only.
 */
#pragma once

#include "template_core.hpp"

#include "async_run.hpp"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tpl {

class TemplateApp {
public:
    explicit TemplateApp(lv_obj_t *screen);
    ~TemplateApp();
    TemplateApp(const TemplateApp &) = delete;
    TemplateApp &operator=(const TemplateApp &) = delete;

private:
    enum Row { kInfo = 0, kName, kSwitch, kReset, kCommand, kScreen };

    static void posted_cb(void *data);
    /* Runs fn after the current LVGL event: pages are never deleted inside their own callbacks. */
    void post(std::function<void()> fn);

    std::vector<cp0::ui::RowSpec> root_rows() const;
    cp0::ui::RowSpec command_row() const;
    Facts facts() const;
    void focus(lv_obj_t *obj);
    void show_title();

    void activate(int row);
    void back();
    void open_info();
    void open_form();
    void submit_name(const std::string &text);
    void set_form_status(const std::string &text);
    void open_dialog();
    void close_dialog(bool reset);
    void close_view();
    void start_command();
    void refresh_command();

    cp0_ui_roller_page_t layout_{};
    Nav nav_;
    std::unique_ptr<cp0::ui::Page> page_;
    std::unique_ptr<cp0::ui::Roller> roller_;
    std::unique_ptr<cp0::ui::InfoTable> info_;
    std::unique_ptr<cp0::ui::TextField> field_;
    lv_obj_t *form_status_ = nullptr; /* the form's error line, right under the field (the launcher's SSH form rule) */
    std::unique_ptr<cp0::ui::Dialog> dialog_;
    std::vector<std::function<void()>> posted_;
    bool post_armed_ = false;

    std::string name_ = "Deck";
    bool switch_on_ = false;
    RunState run_;
    cdk::async_run::JobId job_ = 0;
    cp0::ui::RowKind command_kind_ = cp0::ui::RowKind::Nav; /* the kind the command row has in the roller now */
};

} // namespace tpl
