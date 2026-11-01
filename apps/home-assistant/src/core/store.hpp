/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the files the app keeps (no LVGL; tests/test_store.cpp). All in
 * ${XDG_CONFIG_HOME:-~/.config}/home-assistant (mode 0700), each written to a temporary file and renamed into place:
 *
 *   settings.json     {"server": "homeassistant.local:8123", "qr_use_ip": false}
 *   favourites.json   {"entities": ["light.kitchen", ...]}   (starred on the deck, in the order they were starred)
 *   names.json        {"names": {"light.kitchen": "Kitchen light"}}   (the favourites' names, for a start offline)
 *   credentials.json  {"server": ..., "client_id": ..., "refresh_token": ...}   mode 0600; never logged or printed.
 *                     Deleted by Sign out (after Home Assistant revoked the token) or when Home Assistant refuses it.
 *
 * As with SSH keys, any program running as the deck's user can read the credentials file: there is no keyring on the
 * boards and the file is not encrypted.
 */
#pragma once

#include "engine.hpp"

#include <map>
#include <string>
#include <vector>

namespace ha {
namespace store {

/* The folder (created 0700 when missing by the writers). HA_CONFIG_DIR overrides it (tests). */
std::string config_dir();

Settings load_settings(const std::string &dir);
bool save_settings(const std::string &dir, const Settings &s);

std::vector<std::string> load_favourites(const std::string &dir);
bool save_favourites(const std::string &dir, const std::vector<std::string> &ids);

/* names.json: {"names": {"light.kitchen": "Kitchen light", ...}}, the favourites' names as last seen */
std::map<std::string, std::string> load_names(const std::string &dir);
bool save_names(const std::string &dir, const std::map<std::string, std::string> &names);

Credentials load_credentials(const std::string &dir);
bool save_credentials(const std::string &dir, const Credentials &c);
bool delete_credentials(const std::string &dir);

/* The file's text written to path.tmp (mode), fsync'd, renamed over path. */
bool write_atomic(const std::string &path, const std::string &text, unsigned mode);
bool read_file(const std::string &path, std::string &out, std::size_t max_bytes = 1 << 20);
bool make_dirs(const std::string &dir, unsigned mode);

} // namespace store
} // namespace ha
