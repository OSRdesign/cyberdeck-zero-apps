/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the app's files (see store.hpp).
 */
#include "store.hpp"

#include "fsutil.hpp"
#include "json.hpp"

#include <cstdlib>

namespace klondike {

static const char kAppId[] = "klondike-solitaire";
constexpr int kSettingsVersion = 1;

static std::string home_dir()
{
    const char *h = std::getenv("HOME");
    return std::string(h && *h ? h : "/tmp");
}

std::string Store::default_data_dir()
{
    if (const char *x = std::getenv("KLONDIKE_DATA"); x && *x) return x;
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/" + kAppId;
    return home_dir() + "/.local/share/" + kAppId;
}

std::string Store::default_config_dir()
{
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/" + kAppId;
    return home_dir() + "/.config/" + kAppId;
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

Store::Load Store::load_rules(Rules &out)
{
    out = Rules();
    const std::string path = config_dir_ + "/settings.json";
    std::string text, why;
    if (!path_exists(path)) return Load::Missing;
    if (!read_file(path, text)) return bad(path, "cannot be read");
    json::Value root;
    if (!json::parse(text, root, &why)) return bad(path, "not JSON: " + why);
    const json::Value *ver = root.get("version");
    if (!ver || !ver->is_int() || ver->as_int() < 1 || ver->as_int() > kSettingsVersion) return bad(path, "bad version");
    const json::Value *rules = root.get("rules");
    if (!rules || rules->kind != json::Value::Kind::String || !Rules::parse(rules->text, out)) {
        out = Rules();
        return bad(path, "bad rules");
    }
    return Load::Ok;
}

bool Store::save_rules(const Rules &rules)
{
    const std::string text = "{\n  \"version\": " + std::to_string(kSettingsVersion) +
                             ",\n  \"rules\": " + json::quote(rules.serialize()) + "\n}\n";
    return write(config_dir_, "settings.json", text);
}

} // namespace klondike
