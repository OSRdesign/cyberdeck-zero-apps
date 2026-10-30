// Host tests: the package files (.desktop, app.json, the icon, the copyright file, md4c's licence) and the AppSpec
// constants they must agree with. No LVGL: the sources are read as text.
#include "test_common.hpp"

#include <cstring>

using namespace notes_test;

namespace {

bool has(const std::string &s, const std::string &what) { return s.find(what) != std::string::npos; }

} // namespace

void test_package()
{
    const std::string app = APP_DIR;
    const std::string desktop = get(app + "/root/usr/share/APPLaunch/applications/markdown-notes.desktop");
    // the tile's name: one line under the icon (0.1.1; "Markdown Notes" wrapped onto two over the badge); the
    // registry title stays "Markdown Notes" (app.json), as System Monitor's tile says "Monitor"
    CHECK(has(desktop, "[Desktop Entry]\nName=Notes\n"));
    CHECK(has(desktop, "\nExec=/usr/share/APPLaunch/bin/markdown-notes\n"));
    CHECK(has(desktop, "\nIcon=share/images/markdown-notes.png\n"));
    CHECK(has(desktop, "\nX-Fullscreen=true\n"));
    CHECK(has(desktop, "\nX-MinSize=640x480\n"));
    CHECK(has(desktop, "\nX-Background=freeze\n"));        // multitasking: stopped while hidden (report 037 section 2.4)
    CHECK(!has(desktop, "Terminal=true"));
    // X-MinSize = AppSpec's minimum (notes_app.hpp kMinW / kMinH, used by app_spec.cpp)
    const std::string spec = get(app + "/src/ui/notes_app.hpp");
    CHECK(has(spec, "constexpr int kMinW = 640;") && has(spec, "constexpr int kMinH = 480;"));
    CHECK(has(spec, "constexpr int kPortraitW = 480;") && has(spec, "constexpr int kPortraitH = 640;"));
    CHECK(has(spec, "kAppTitle = \"Markdown Notes\""));
    // the Home page's header: "Notes" (the shared header cuts "Markdown Notes" at 640x480; tests/scenes/shell.scene)
    CHECK(has(spec, "kHomeTitle = \"Notes\""));
    const std::string spec_cpp = get(app + "/src/ui/app_spec.cpp");
    CHECK(has(spec_cpp, "s.id = \"markdown-notes\";"));

    const std::string json = get(app + "/app.json");
    CHECK(has(json, "\"share_code\": \"markdown-notes\""));
    CHECK(has(json, "\"package\": \"markdown-notes\""));
    CHECK(has(json, "\"title\": \"Markdown Notes\""));
    CHECK(has(json, "\"version\": \"0.2.1\""));
    CHECK(has(json, "\"categories\": [\"Productivity\"]"));
    CHECK(has(json, "\"license\": \"MIT\""));
    CHECK(has(json, "\"depends\": \"libfreetype6, libinput10, libxkbcommon0\""));
    CHECK(has(json, "\"arch\": [\"arm64\", \"amd64\"]"));
    CHECK(has(json, "\"permissions\": {}"));
    CHECK(has(json, "\"summary\": \"") && has(json, "\"description\": \""));
    CHECK(!has(json, "libc6"));

    // the icon: a 100x100 PNG, the same file in the package
    const std::string icon = get(app + "/icon.png");
    CHECK(icon.size() > 24 && icon.compare(1, 3, "PNG") == 0);
    const auto be32 = [&](std::size_t at) {
        return (static_cast<unsigned>(static_cast<unsigned char>(icon[at])) << 24) |
               (static_cast<unsigned>(static_cast<unsigned char>(icon[at + 1])) << 16) |
               (static_cast<unsigned>(static_cast<unsigned char>(icon[at + 2])) << 8) |
               static_cast<unsigned>(static_cast<unsigned char>(icon[at + 3]));
    };
    if (icon.size() > 24) CHECK(be32(16) == 100 && be32(20) == 100);
    CHECK(get(app + "/root/usr/share/APPLaunch/share/images/markdown-notes.png") == icon);

    // licences: ours and md4c's, credited in the copyright file and the README
    const std::string copyright = get(app + "/root/usr/share/doc/markdown-notes/copyright");
    CHECK(has(copyright, "Files: usr/share/APPLaunch/bin/markdown-notes"));
    CHECK(has(copyright, "MD4C v0.6.0") && has(copyright, "Martin Mitas"));
    const std::string md4c_licence = get(app + "/third_party/md4c/LICENSE.md");
    CHECK(has(md4c_licence, "The MIT License") && has(md4c_licence, "Martin Mit"));
    CHECK(get(app + "/root/usr/share/doc/markdown-notes/LICENSE.md4c") == md4c_licence);
    const std::string readme = get(app + "/README.md");
    CHECK(has(readme, "md4c") && has(readme, "MIT"));
    // the README's sections (guidelines section 8): the sync how-to, the keys table, the compatibility note
    CHECK(has(readme, "## Sync") && has(readme, "Syncthing") && has(readme, "rsync") && has(readme, "git"));
    CHECK(has(readme, "## Keys and touch") && has(readme, "Ctrl+P") && has(readme, "Ctrl+Enter"));
    CHECK(has(readme, "## Files the app keeps") && has(readme, "## Known limits") && has(readme, "## Multitasking"));
    CHECK(has(readme, "independent project"));
    // the board protocol and the lag test's note
    const std::string board = get(app + "/tests/BOARD-TESTS.md");
    CHECK(has(board, "big-note.sh") && has(board, "Pi 3A+") && has(board, "pgrep -x markdown-notes"));
    CHECK(has(get(app + "/tests/board/big-note.sh"), "Big note"));
    // the vendored parser is v0.6.0, unchanged
    const std::string md4c_h = get(app + "/third_party/md4c/md4c.h");
    CHECK(has(md4c_h, "MD_FLAG_WIKILINKS") && has(md4c_h, "task_mark_offset"));
}
