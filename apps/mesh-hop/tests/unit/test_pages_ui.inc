// Phase P3 (the pages that follow the screen): the column rule, the columns of a table, the row kinds of Settings, the
// texts of Contacts and Nearby (src/ui/ui_pages.*). Included by test_ui.cpp.
#include "ui_pages.hpp"

static void test_pages_ui()
{
    set_time_source(fake_time);

    // the column: one SPACE_L margin at 640, two from 80 px more, capped (the five screens of the harness)
    {
        const PageColumn c640 = page_column(640, 20, 640, 960);
        CHECK(c640.x == 20 && c640.w == 600);
        const PageColumn c800 = page_column(800, 20, 640, 960);
        CHECK(c800.x == 40 && c800.w == 720);
        const PageColumn c720 = page_column(720, 24, 640, 960);
        CHECK(c720.x == 48 && c720.w == 624);
        const PageColumn c1024 = page_column(1024, 20, 640, 960);
        CHECK(c1024.x == 40 && c1024.w == 944);
        const PageColumn c1280 = page_column(1280, 24, 640, 960);
        CHECK(c1280.x == 160 && c1280.w == 960);
        const PageColumn c660 = page_column(660, 20, 640, 960);     // halfway: 10 px more margin
        CHECK(c660.x == 30 && c660.w == 600);
    }

    // the columns of a table: the first stays, the others while they fit, a narrow one after a wide one that did not
    {
        CHECK((fit_columns({100, 100, 100}, 10, 320) == std::vector<int>{0, 1, 2}));
        CHECK((fit_columns({100, 100, 100}, 10, 319) == std::vector<int>{0, 1}));
        CHECK((fit_columns({100, 200, 50}, 10, 170) == std::vector<int>{0, 2}));
        CHECK((fit_columns({500, 100}, 10, 300) == std::vector<int>{0}));
        CHECK(fit_columns({}, 10, 300).empty());
    }

    // the row kinds of Settings: read-outs carry nothing, pages a chevron, values a chevron after the value, actions a dot
    {
        CHECK(settings_row_aff(SRowKind::Header) == Aff::None);
        for (SRowKind k : {SRowKind::Info, SRowKind::GpsNotice, SRowKind::StatsNotice, SRowKind::PathHashNotice,
                           SRowKind::AutoAddNotice, SRowKind::ChannelsNotice})
            CHECK(settings_row_aff(k) == Aff::Info);
        for (SRowKind k : {SRowKind::Position, SRowKind::Stats, SRowKind::PacketLog, SRowKind::Channel, SRowKind::AddChannel})
            CHECK(settings_row_aff(k) == Aff::Open);
        for (SRowKind k : {SRowKind::Name, SRowKind::Preset, SRowKind::Freq, SRowKind::Bw, SRowKind::Sf, SRowKind::Cr, SRowKind::Tx,
                           SRowKind::RetryAttempts, SRowKind::ResetAfter, SRowKind::BoardGps, SRowKind::PathHash, SRowKind::Repeat,
                           SRowKind::AdvertEvery, SRowKind::AdvertKind, SRowKind::ManualAdd, SRowKind::AutoAddTypes})
            CHECK(settings_row_aff(k) == Aff::Edit);
        for (SRowKind k : {SRowKind::SyncClock, SRowKind::AddPublic, SRowKind::HistoryAll, SRowKind::HistoryOlder, SRowKind::HistoryForget,
                           SRowKind::HistoryOthers, SRowKind::Reboot, SRowKind::FactoryReset, SRowKind::Undo, SRowKind::Save})
            CHECK(settings_row_aff(k) == Aff::Action);
        // every selectable row (Enter does something) has a marker; the rows the keys skip are read-outs
        for (int i = 0; i <= static_cast<int>(SRowKind::PacketLog); ++i) {
            const SRowKind k = static_cast<SRowKind>(i);
            const Aff a = settings_row_aff(k);
            if (k == SRowKind::Header) continue;
            CHECK(settings_row_selectable(k) == (a == Aff::Open || a == Aff::Edit || a == Aff::Action));
        }
    }

    // a refresh of Settings keeps the highlight on the same row (kind and argument), never back to the top
    {
        SettingsContext ctx;
        ctx.connected = true;
        ctx.have_self = true;
        ctx.board_known = true;
        ctx.channels = {0, 1, 2};
        ctx.channel_admin.available = true;
        const std::vector<SRowSpec> before = settings_layout(ctx);
        int save = -1, ch2 = -1, first = -1;
        for (size_t i = 0; i < before.size(); ++i) {
            if (before[i].kind == SRowKind::Save) save = static_cast<int>(i);
            if (before[i].kind == SRowKind::Channel && before[i].arg == 2) ch2 = static_cast<int>(i);
            if (first < 0 && settings_row_selectable(before[i].kind)) first = static_cast<int>(i);
        }
        CHECK(save > 0 && ch2 > 0 && first >= 0);
        CHECK(keep_selected_row(before, save, before) == save);                 // nothing changed: the same row
        CHECK(keep_selected_row(before, -1, before) == first);                  // nothing selected yet: the first row that acts
        ctx.other_boards = 2;                                                   // a row appears above Save: Save moves down, the highlight with it
        const std::vector<SRowSpec> more = settings_layout(ctx);
        CHECK(more.size() == before.size() + 1 && more[static_cast<size_t>(keep_selected_row(before, save, more))].kind == SRowKind::Save);
        ctx.other_boards = 0;
        ctx.channels = {0, 1};                                                  // the selected channel went: the nearest row that acts, not the top
        const std::vector<SRowSpec> fewer = settings_layout(ctx);
        const int kept = keep_selected_row(before, ch2, fewer);
        CHECK(kept > first && settings_row_selectable(fewer[static_cast<size_t>(kept)].kind) && std::abs(kept - ch2) <= 2);
        const int info = 1;                                                     // a read-out (Board) was selected: the nearest row that acts
        CHECK(before[info].kind == SRowKind::Info && keep_selected_row(before, info, before) == first);
        CHECK(keep_selected_row(before, save, {}) == -1);
    }

    // More > Send advert: the arrow choice of the scope, in the words of the Settings row, starting on the scope used last
    {
        const Choice f = make_send_advert_choice(true), z = make_send_advert_choice(false);
        CHECK(f.title == "Send advert" && f.labels.size() == 2 && f.labels[0] == "Flood (whole mesh)" && f.labels[1] == "Zero-hop (neighbours)");
        CHECK(f.index == 0 && f.initial == 0 && z.index == 1 && z.initial == 1 && f.details.size() == 2 && !f.note.empty());
    }

    // the Sort dialog: the six column titles of 0.3.0, a direction for each
    {
        const auto &keys = sort_menu_keys();
        CHECK(keys.size() == 6 && keys[0] == SortKey::Name && keys[1] == SortKey::Heard && keys[5] == SortKey::Distance);
        CHECK(std::string(sort_field_name(SortKey::Snr)) == "SNR" && std::string(sort_field_name(SortKey::Heard)) == "Heard");
        CHECK(sort_direction_text(SortKey::Heard, false) == "newest first" && sort_direction_text(SortKey::Heard, true) == "oldest first");
        CHECK(sort_direction_text(SortKey::Distance, false) == "nearest first" && sort_direction_text(SortKey::Name, true) == "Z to A");
        ContactView v;
        CHECK(sort_button_text(v) == "Sort: Heard");
        v.tap_column(SortKey::Heard);                     // the active one again: the direction flips, the field stays
        CHECK(v.sort == SortKey::Heard && v.reverse && sort_button_text(v) == "Sort: Heard");
        v.tap_column(SortKey::Snr);
        CHECK(v.sort == SortKey::Snr && !v.reverse && sort_button_text(v) == "Sort: SNR");
    }

    // the line under the title, and the select mode's
    {
        ContactView v;
        CHECK(contact_view_summary(193, 193, v) == "193 contacts  \xC2\xB7  All types  \xC2\xB7  Any time");
        CHECK(contact_view_summary(1, 1, v) == "1 contact  \xC2\xB7  All types  \xC2\xB7  Any time");
        v.filter.type = TypeFilter::Repeater;
        v.filter.age = AgeFilter::Day;
        v.group = "Friends";
        CHECK(contact_view_summary(12, 193, v) == "12 of 193  \xC2\xB7  Repeater  \xC2\xB7  Last 24 h  \xC2\xB7  Group Friends");
        CHECK(std::string(type_filter_label(TypeFilter::All)) == "All types" && std::string(age_filter_label(AgeFilter::Week)) == "Last 7 days");
        CHECK(contact_marked_summary(1, 0) == "Manage contacts: 1 selected");
        CHECK(contact_marked_summary(3, 1) == "Manage contacts: 3 selected (+1 hidden by the filters)");
    }

    // a contact row's second line and its distance
    {
        ContactRow r;
        r.type_name = "Repeater";
        r.heard = g_time - 47 * 60;
        r.hops = 3;
        CHECK(contact_row_caption(r, false) == "Repeater  \xC2\xB7  heard 47 min  \xC2\xB7  3 hops");
        r.has_snr = true;
        r.snr = 5.5;
        CHECK(contact_row_caption(r, false) == "Repeater  \xC2\xB7  heard 47 min  \xC2\xB7  3 hops");
        CHECK(contact_row_caption(r, true) == "Repeater  \xC2\xB7  heard 47 min  \xC2\xB7  3 hops  \xC2\xB7  SNR 5.5 dB");
        r.hops = -1;                                      // unknown: left out
        r.heard = g_time - 20;
        CHECK(contact_row_caption(r, true) == "Repeater  \xC2\xB7  heard < 1 min  \xC2\xB7  SNR 5.5 dB");
        r.hops = 0;
        r.has_snr = false;
        CHECK(contact_row_caption(r, true) == "Repeater  \xC2\xB7  heard < 1 min  \xC2\xB7  direct");
        r.heard = 0;
        r.hops = 1;
        CHECK(contact_row_caption(r, false) == "Repeater  \xC2\xB7  never heard  \xC2\xB7  1 hop");
        CHECK(contact_row_distance(r) == "\xE2\x80\x93");
        r.has_dist = true;
        r.dist_km = 6.24;
        CHECK(contact_row_distance(r) == "6.2 km");
    }

    // Nearby: the line under the title and a row's second line
    {
        CHECK(nearby_summary(5, 2, false) == "5 nodes heard  \xC2\xB7  2 waiting");
        CHECK(nearby_summary(1, 0, false) == "1 node heard");
        CHECK(nearby_summary(3, 0, true) == "3 ignored nodes");
        NearbyRow n;
        n.type_name = "Chat";
        n.detail = "SNR 5.5 dB  direct  12 s ago";
        CHECK(nearby_row_caption(n) == "Chat  \xC2\xB7  SNR 5.5 dB  direct  12 s ago");
    }
}
