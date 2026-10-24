/*
 * SPDX-License-Identifier: MIT
 *
 * The repeater and room passwords the deck remembers (report 036 section 4.4): ONE store, meshcore-cli's own file, so the Repeater
 * page and the CLI's `login` read and write the same password. The file is
 *     <data>/cli/.config/meshcore/<64 hex digits of the node's key, lower case>.pass
 * where <data> is Mesh Hop's data folder (Store::default_dir()) and <data>/cli is the HOME the bundled meshcore-cli runs with
 * (root/opt/mesh-hop/cli/mccli). Its content is the password alone on one line with no line end: meshcore-cli writes it with
 * f.write(password) and reads it back with f.readline().strip() (meshcore_cli.py, `login` and `forget_password`).
 *
 * Writes go to a temporary file in the same folder (mode 0600) that is renamed into place; the folders cli/, cli/.config/ and
 * cli/.config/meshcore/ are created 0700 (and set to 0700, as mccli does). The password is never logged and never kept here.
 * No LVGL; unit tested (test_passwords in test_core.cpp).
 */

#pragma once

#include <optional>
#include <string>

namespace meshzero {

class PasswordStore {
public:
    /* data_dir: Mesh Hop's data folder (tests: a temporary folder). */
    explicit PasswordStore(std::string data_dir);

    /* <data>/cli/.config/meshcore */
    std::string dir() const;
    /* The key as meshcore-cli names the file (meshcore_py's public_key: 64 hex digits, lower case); "" when key_hex is not 64 hex digits. */
    static std::string normalize_key(const std::string &key_hex);
    /* The file of a key; "" for a bad key. */
    std::string path(const std::string &key_hex) const;

    /* The remembered password of a node: nullopt when there is none (no file, unreadable, a bad key, or a blank first line). Read like
     * meshcore-cli: the first line, without the spaces and line ends around it. */
    std::optional<std::string> read(const std::string &key_hex) const;
    bool remembered(const std::string &key_hex) const { return read(key_hex).has_value(); }

    enum class SaveResult {
        Saved,
        BadKey,          // not 64 hex digits
        Blank,           // an empty password (a guest login): nothing is written
        NotStorable,     // a line end, a NUL, or spaces around it: meshcore-cli would read back another password
        IoError,         // a folder or the file could not be written (the old file, if any, is left as it was)
    };
    SaveResult save(const std::string &key_hex, const std::string &password);
    /* Deletes the file (meshcore-cli's `fp`). True when no file is left (also when there was none); false for a bad key or an error. */
    bool forget(const std::string &key_hex);

private:
    std::string data_dir_;
};

} // namespace meshzero
