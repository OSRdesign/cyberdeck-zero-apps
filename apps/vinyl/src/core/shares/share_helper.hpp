/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares, the launcher's helper (0.8.0; the launcher's docs/APP-CONTRACT.md "Network shares (SMB)" and its
 * SHARE-HELPER-INTERFACE.md): add and remove a share of a server from the player (owner decisions 2026-11-22: D1 the root
 * step behind the toolkit's sudo password prompt at every Add and Remove, D3 guest by default with an optional user and
 * password, D4/D7 SMB only, D9 a removed share takes its folder, its library rows and its queued tracks away).
 *
 *   helper     /usr/libexec/cyberdeckzero/cyberdeckzero-share, argv only (never a shell): `version` -> "ok 1", `list` ->
 *              "<name> <host> <share> [<subfolder>] <state>" lines (a space inside share or folder is written as the 4 characters \040) then "ok" (both as the user, no password), `add --name N
 *              --host H --share S [--subfolder P] [--user U --credfile F]` -> "ok path=/mnt/cyberdeck/N", `remove --name N` ->
 *              "ok" (both through sudo). A failure is "err <word> [reason]". The reply is the LAST non-empty line (sudo
 *              may print its own lines first).
 *   checks     the helper's own allow-lists (it checks again): a form is refused here before any prompt shows
 *   credfile   the password never goes on a command line nor on stdin (stdin may hold the sudo password): with a user, Vinyl
 *              writes /run/user/<uid>/cyberdeckzero-share/<random>.cred itself (folder 0700, file 0600, O_EXCL, never
 *              following a link) with "username=U\npassword=P\n", passes its path, and deletes it after the helper ran,
 *              whatever the outcome (the helper deletes it too). The password is never logged, never in an error text.
 *   messages   each reply word and each result of the sudo prompt as a short line for a 320 px wide notice
 *
 * The tests run a fake helper script in place of the real one: the path is a field of the callers' configuration
 * (ShareConfig::helper in share_exec.hpp); the app always uses kHelperPath. LVGL-free, no sudo here (ui/ owns the prompt).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vinyl {
namespace shares {

constexpr const char *kHelperPath = "/usr/libexec/cyberdeckzero/cyberdeckzero-share";
constexpr const char *kMountRoot = "/mnt/cyberdeck"; /* where the helper mounts: kMountRoot/<name> */
constexpr int kProtocol = 1;                         /* `version` answers "ok 1" */

/* ---- the helper's allow-lists (SHARE-HELPER-INTERFACE.md "Grammar"; the helper checks them all again) */
bool valid_name(const std::string &n);      /* 1-32 of A-Za-z0-9_- , not a leading '-' */
bool valid_ipv4(const std::string &h);      /* dotted quad, no leading zeros */
bool valid_host(const std::string &h);      /* IPv4, or a host name: labels A-Za-z0-9- (not at a label's ends), dots, <= 253 */
bool valid_share(const std::string &s);     /* 1-80 of A-Za-z0-9_.$- and single inner spaces (no leading,
                                               trailing or double space), not ".", no ".." */
bool valid_subfolder(const std::string &p); /* relative segments of A-Za-z0-9_.$@+=- (single inner spaces
                                               too) joined by one '/', no "..", no "." segment, no leading or trailing '/', <= 200 */
bool valid_user(const std::string &u);      /* 1-64 of A-Za-z0-9_.@- , not a leading '-' */
bool valid_password(const std::string &p);  /* 1-256 bytes, no control character */

/* A name the helper takes, made of `wanted` (the share name, or what was typed in NAME IN VINYL): every character outside
 * A-Za-z0-9_- becomes '_', leading '-' and '_' go, at most 32; "share" when nothing is left. A name in `taken` gets "_2",
 * "_3" ... (within 32). */
std::string make_name(const std::string &wanted, const std::vector<std::string> &taken);

/* ---- the Add form */
struct AddForm {
    std::string host, share, subfolder, name; /* name: NAME IN VINYL, "" for the default */
    bool guest = true;
    std::string user, password; /* only with !guest */
};
enum class Field { None, Host, Share, Subfolder, Name, User, Password };
struct FormCheck {
    bool ok = false;
    Field field = Field::None; /* the field to fix */
    std::string message;       /* "Type the server's IP address or name." */
};
/* Tidies the form in place (spaces trimmed; a pasted "//host/share/sub", "\\host\share", "smb://host/share" (%20 decoded there) in SERVER split
 * into the three fields; the folder's leading and trailing '/' dropped, "/" meaning the whole share; the name made by
 * make_name, `taken` being the names of the shares there are) and checks it with the helper's rules. A user name typed
 * that the helper refuses, a password that is empty or too long: refused here, before the prompt. */
FormCheck check_form(AddForm &f, const std::vector<std::string> &taken);

/* ---- argv (no password ever) */
std::vector<std::string> version_argv(const std::string &helper);
std::vector<std::string> list_argv(const std::string &helper);
std::vector<std::string> add_argv(const std::string &helper, const AddForm &f, const std::string &credfile);
std::vector<std::string> remove_argv(const std::string &helper, const std::string &name);

/* ---- replies */
struct Reply {
    bool ok = false;
    std::string word;   /* err: usage exists nomodule nohelper unreachable notfound denied timeout other ("" for ok;
                           a word Vinyl does not know reads "other", an empty or unreadable reply "noreply") */
    std::string reason; /* the rest of the err line (for the log; "other" shows it) */
    std::string rest;   /* ok: what follows "ok " ("path=/mnt/cyberdeck/x", "1") */
    std::string path;   /* add: the path= value */
};
/* the last non-empty line of the output (sudo's lines may come first) */
std::string last_line(const std::string &output);
Reply parse_reply(const std::string &output);

struct ShareEntry {
    std::string name, host, share, subfolder;
    std::string state; /* mounted idle failed inactive */
    std::string path() const { return std::string(kMountRoot) + "/" + name; }
    std::string where() const; /* "192.168.68.142 / home-share" (" / Music" for a folder in it) */
};
/* `list`'s output: true with its rows when the last line is exactly "ok"; a row that does not read is skipped */
bool parse_list(const std::string &output, std::vector<ShareEntry> &out);

/* ---- what the user is told */
/* the result of the toolkit's sudo prompt and command (cp0_sudo_result_t, mapped by ui/) */
enum class SudoResult { Success, AuthFailed, ExecFailed, Cancelled, TimedOut, NoPrompt };
struct RunResult {
    SudoResult result = SudoResult::ExecFailed;
    int exit_code = -1;
    std::string output; /* stdout and stderr of the command (sudo's lines included) */
};
struct Outcome {
    bool ok = false;
    std::string path;    /* add: where the share is now */
    std::string word;    /* the helper's word, or "sudo-auth", "sudo-cancel", "sudo-timeout", "sudo-denied", "noprompt" */
    std::string message; /* the line shown (a notice, the form's status line) */
    std::string log;     /* the journal's line (never a password) */
};
/* add (with_user: a user was given, which tells "denied" apart) and remove */
/* spaced: the typed share or folder holds a space (an `err usage` then means a launcher <= 0.10.9) */
Outcome add_outcome(const RunResult &r, bool with_user, const std::string &name, bool spaced = false);
Outcome remove_outcome(const RunResult &r, const std::string &name);
/* the line for one helper word ("denied" with or without a user; "other" with its reason) */
std::string word_message(const std::string &word, const std::string &reason, bool with_user, bool removing);

/* ---- the credentials file */
struct CredFile {
    bool ok = false;
    std::string path;  /* <runtime>/cyberdeckzero-share/<random>.cred */
    std::string error; /* why not (no password in it) */
};
/* `runtime` is /run/user/<uid> (the helper takes no other place; the tests give a folder of their own). The folder is
 * made 0700 (an existing one must be a real folder of ours: it is set 0700 again); the file is created O_EXCL|O_NOFOLLOW,
 * 0600, with the two lines. Nothing existing is ever overwritten. */
CredFile write_credfile(const std::string &runtime, const std::string &user, const std::string &password);
/* deletes that file if it is still there (only a path write_credfile made: under <x>/cyberdeckzero-share/, ending .cred) */
void remove_credfile(const std::string &path);
/* /run/user/<uid> of this process */
std::string runtime_dir();
/* overwrites a string's bytes before it goes (the password's copies) */
void wipe(std::string &s);

}  // namespace shares
}  // namespace vinyl
