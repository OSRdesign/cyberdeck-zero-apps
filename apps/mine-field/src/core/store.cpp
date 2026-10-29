/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the app's files (see store.hpp).
 */
#include "store.hpp"

#include "fsutil.hpp"

#include <cstdlib>

namespace minefield {

static const char kAppDir[] = "mine-field";

static std::string home_dir()
{
    const char *h = std::getenv("HOME");
    return std::string(h && *h ? h : "/tmp");
}

std::string Store::default_data_dir()
{
    if (const char *x = std::getenv("MINEFIELD_DATA"); x && *x) return x;
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/" + kAppDir;
    return home_dir() + "/.local/share/" + kAppDir;
}

std::string Store::default_config_dir()
{
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/" + kAppDir;
    return home_dir() + "/.config/" + kAppDir;
}

Store::Store() : data_dir_(default_data_dir()), config_dir_(default_config_dir()) {}

Store::Store(std::string data_dir, std::string config_dir)
    : data_dir_(std::move(data_dir)), config_dir_(std::move(config_dir))
{
}

Store::Load Store::bad(const std::string &path, const std::string &why)
{
    last_error_ = path + ": " + why + " (kept as " + path + ".bad)";
    rename_path(path, path + ".bad");
    return Load::Bad;
}

bool Store::write(const std::string &dir, const std::string &name, const std::string &data)
{
    if (!make_dirs(dir)) {
        last_error_ = dir + ": cannot create the folder";
        return false;
    }
    if (!write_file_atomic(dir + "/" + name, data)) {
        last_error_ = dir + "/" + name + ": cannot write";
        return false;
    }
    return true;
}

Store::Load Store::load_stats(Stats &out)
{
    out = Stats();
    const std::string path = data_dir_ + "/stats.json";
    std::string text, why;
    if (!path_exists(path)) return Load::Missing;
    if (!read_file(path, text)) return bad(path, "cannot be read");
    if (!Stats::from_json(text, out, &why)) {
        out = Stats();
        return bad(path, why);
    }
    return Load::Ok;
}

bool Store::save_stats(const Stats &stats)
{
    return write(data_dir_, "stats.json", stats.to_json());
}

Store::Load Store::load_game(Game &out)
{
    const std::string path = data_dir_ + "/game.json";
    std::string text, why;
    if (!path_exists(path)) return Load::Missing;
    if (!read_file(path, text)) return bad(path, "cannot be read");
    if (!Game::load(text, out, &why)) return bad(path, why);
    return Load::Ok;
}

bool Store::save_game(const Game &game)
{
    return write(data_dir_, "game.json", game.save());
}

bool Store::clear_game()
{
    return remove_file(data_dir_ + "/game.json");
}

Store::Load Store::load_settings(Settings &out)
{
    out = Settings();
    const std::string path = config_dir_ + "/settings.json";
    std::string text, why;
    if (!path_exists(path)) return Load::Missing;
    if (!read_file(path, text)) return bad(path, "cannot be read");
    if (!Settings::from_json(text, out, &why)) {
        out = Settings();
        return bad(path, why);
    }
    return Load::Ok;
}

bool Store::save_settings(const Settings &settings)
{
    return write(config_dir_, "settings.json", settings.to_json());
}

} // namespace minefield
