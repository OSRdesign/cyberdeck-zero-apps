/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares, the launcher's helper (see share_helper.hpp).
 */
#include "share_helper.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace vinyl {
namespace shares {

namespace {

bool printable(const std::string &s) {
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7e) return false;
    return true;
}

bool all_of(const std::string &s, const char *extra) {
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        if (c && std::strchr(extra, static_cast<char>(c))) continue;
        return false;
    }
    return true;
}

bool is_alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_ws(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}

bool contains(const std::string &s, const char *t) { return s.find(t) != std::string::npos; }

/* the reason of an "other" for the screen: printable, at most 80 characters, no full stop of its own */
std::string short_reason(const std::string &r) {
    std::string s;
    for (unsigned char c : r) s += (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : ' ';
    s = trim(s);
    if (s.size() > 80) s = s.substr(0, 77) + "...";
    while (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

}  // namespace

/* ------------------------------------------------------------------------------------------------ allow-lists */

bool valid_name(const std::string &n) {
    return printable(n) && !n.empty() && n.size() <= 32 && n[0] != '-' && all_of(n, "_-");
}

bool valid_ipv4(const std::string &h) {
    int parts = 0;
    size_t i = 0;
    while (true) {
        size_t j = i;
        while (j < h.size() && h[j] >= '0' && h[j] <= '9') ++j;
        const size_t len = j - i;
        if (len == 0 || len > 3) return false;
        if (len > 1 && h[i] == '0') return false; /* no leading zeros */
        if (std::atoi(h.substr(i, len).c_str()) > 255) return false;
        ++parts;
        if (j == h.size()) break;
        if (h[j] != '.') return false;
        i = j + 1;
    }
    return parts == 4;
}

bool valid_host(const std::string &h) {
    if (!printable(h) || h.empty() || h.size() > 253) return false;
    if (all_of(h, ".") && h.find_first_not_of("0123456789.") == std::string::npos) return valid_ipv4(h); /* digits and dots: an address */
    size_t i = 0;
    while (i <= h.size()) {
        size_t j = h.find('.', i);
        if (j == std::string::npos) j = h.size();
        const std::string label = h.substr(i, j - i);
        if (label.empty() || label.size() > 63 || !all_of(label, "-") || !is_alnum(label.front()) || !is_alnum(label.back())) return false;
        i = j + 1;
        if (j == h.size()) break;
    }
    return true;
}

bool valid_share(const std::string &s) {
    return printable(s) && !s.empty() && s.size() <= 80 && s != "." && s.find("..") == std::string::npos && all_of(s, "_.$-");
}

bool valid_subfolder(const std::string &p) {
    if (!printable(p) || p.empty() || p.size() > 200) return false;
    if (p.find("..") != std::string::npos || p.front() == '/' || p.back() == '/' || p.find("//") != std::string::npos) return false;
    size_t i = 0;
    while (true) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || !all_of(seg, "_.$@+=-")) return false;
        if (j == p.size()) break;
        i = j + 1;
    }
    return true;
}

bool valid_user(const std::string &u) {
    return printable(u) && !u.empty() && u.size() <= 64 && u[0] != '-' && all_of(u, "_.@-");
}

bool valid_password(const std::string &p) {
    if (p.empty() || p.size() > 256) return false;
    for (unsigned char c : p)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

std::string make_name(const std::string &wanted, const std::vector<std::string> &taken) {
    std::string n;
    for (unsigned char c : trim(wanted)) {
        if (c >= 0x80) { /* a UTF-8 character: one '_' for all its bytes */
            if ((c & 0xC0) == 0x80) continue;
            n += '_';
            continue;
        }
        n += (is_alnum(static_cast<char>(c)) || c == '-' || c == '_') ? static_cast<char>(c) : '_';
    }
    while (!n.empty() && (n[0] == '-' || n[0] == '_')) n.erase(0, 1);
    if (n.size() > 32) n.resize(32);
    if (n.empty()) n = "share";
    auto used = [&](const std::string &x) { return std::find(taken.begin(), taken.end(), x) != taken.end(); };
    if (!used(n)) return n;
    for (int k = 2; k < 1000; ++k) {
        const std::string suf = "_" + std::to_string(k);
        std::string base = n.substr(0, std::min(n.size(), static_cast<size_t>(32) - suf.size()));
        if (!used(base + suf)) return base + suf;
    }
    return n;
}

/* ------------------------------------------------------------------------------------------------ the form */

FormCheck check_form(AddForm &f, const std::vector<std::string> &taken) {
    FormCheck c;
    f.host = trim(f.host);
    f.share = trim(f.share);
    f.subfolder = trim(f.subfolder);
    f.name = trim(f.name);
    f.user = trim(f.user);
    /* a pasted address in SERVER: //host/share/sub, \\host\share, smb://host/share */
    {
        std::string h = f.host;
        for (char &ch : h)
            if (ch == '\\') ch = '/';
        bool url = false;
        if (h.compare(0, 6, "smb://") == 0 || h.compare(0, 6, "SMB://") == 0) {
            h = h.substr(6);
            url = true;
        } else if (h.compare(0, 2, "//") == 0) {
            h = h.substr(2);
            url = true;
        }
        if (url || h.find('/') != std::string::npos) {
            const size_t s1 = h.find('/');
            std::string host = h.substr(0, s1);
            std::string rest = s1 == std::string::npos ? std::string() : h.substr(s1 + 1);
            while (!rest.empty() && rest.back() == '/') rest.pop_back();
            const size_t s2 = rest.find('/');
            const std::string share = rest.substr(0, s2);
            const std::string sub = s2 == std::string::npos ? std::string() : rest.substr(s2 + 1);
            f.host = host;
            if (f.share.empty()) f.share = share;
            if (f.subfolder.empty()) f.subfolder = sub;
        }
    }
    while (!f.subfolder.empty() && f.subfolder.front() == '/') f.subfolder.erase(0, 1);
    while (!f.subfolder.empty() && f.subfolder.back() == '/') f.subfolder.pop_back();
    auto fail = [&](Field fl, const char *msg) {
        c.ok = false;
        c.field = fl;
        c.message = msg;
        return c;
    };
    if (f.host.empty()) return fail(Field::Host, "Type the server's IP address or name.");
    if (f.host.find(':') != std::string::npos) return fail(Field::Host, "No port or IPv6: type an IPv4 address or a name.");
    if (!valid_host(f.host)) return fail(Field::Host, "That server name or address is not valid.");
    if (f.share.empty()) return fail(Field::Share, "Type the share name.");
    if (f.share.find('/') != std::string::npos) return fail(Field::Share, "Put a folder in FOLDER IN THE SHARE.");
    if (!valid_share(f.share)) return fail(Field::Share, "Share names: letters, digits, _ . $ - (no spaces).");
    if (!f.subfolder.empty() && !valid_subfolder(f.subfolder)) return fail(Field::Subfolder, "Folder: letters, digits, _ . - and / (no spaces).");
    const bool typed = !f.name.empty();
    if (typed) {
        const std::string made = make_name(f.name, {});
        if (std::find(taken.begin(), taken.end(), made) != taken.end()) return fail(Field::Name, "That name is in use. Pick another.");
        f.name = made;
    } else {
        f.name = make_name(f.share, taken);
    }
    if (!valid_name(f.name)) return fail(Field::Name, "Names: letters, digits, _ and - only.");
    if (!f.guest) {
        if (f.user.empty()) return fail(Field::User, "Type the user name, or choose Guest.");
        if (!valid_user(f.user)) return fail(Field::User, "User names: letters, digits, _ . @ - only.");
        if (f.password.empty()) return fail(Field::Password, "Type the password.");
        if (!valid_password(f.password)) return fail(Field::Password, "Passwords: up to 256 characters, no control keys.");
    } else {
        f.user.clear();
        wipe(f.password);
    }
    c.ok = true;
    return c;
}

/* ------------------------------------------------------------------------------------------------ argv */

std::vector<std::string> version_argv(const std::string &helper) { return {helper, "version"}; }
std::vector<std::string> list_argv(const std::string &helper) { return {helper, "list"}; }

std::vector<std::string> add_argv(const std::string &helper, const AddForm &f, const std::string &credfile) {
    std::vector<std::string> a = {helper, "add", "--name", f.name, "--host", f.host, "--share", f.share};
    if (!f.subfolder.empty()) {
        a.push_back("--subfolder");
        a.push_back(f.subfolder);
    }
    if (!f.guest && !f.user.empty()) {
        a.push_back("--user");
        a.push_back(f.user);
        a.push_back("--credfile");
        a.push_back(credfile);
    }
    return a;
}

std::vector<std::string> remove_argv(const std::string &helper, const std::string &name) { return {helper, "remove", "--name", name}; }

/* ------------------------------------------------------------------------------------------------ replies */

std::string last_line(const std::string &output) {
    std::istringstream in(output);
    std::string line, last;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!trim(line).empty()) last = line;
    }
    return trim(last);
}

Reply parse_reply(const std::string &output) {
    Reply r;
    const std::string l = last_line(output);
    if (l == "ok" || l.compare(0, 3, "ok ") == 0) {
        r.ok = true;
        r.rest = l.size() > 3 ? trim(l.substr(3)) : std::string();
        for (const std::string &t : split_ws(r.rest))
            if (t.compare(0, 5, "path=") == 0) r.path = t.substr(5);
        return r;
    }
    if (l.compare(0, 4, "err ") == 0) {
        const std::string rest = trim(l.substr(4));
        const size_t sp = rest.find(' ');
        r.word = rest.substr(0, sp);
        r.reason = sp == std::string::npos ? std::string() : trim(rest.substr(sp + 1));
        static const char *const known[] = {"usage", "exists", "nomodule", "nohelper", "unreachable", "notfound", "denied", "timeout", "other"};
        if (std::none_of(std::begin(known), std::end(known), [&](const char *k) { return r.word == k; })) {
            r.reason = r.word + (r.reason.empty() ? "" : " " + r.reason);
            r.word = "other";
        }
        return r;
    }
    r.word = "noreply";
    r.reason = short_reason(l);
    return r;
}

std::string ShareEntry::where() const { return host + " / " + share + (subfolder.empty() ? std::string() : "/" + subfolder); }

bool parse_list(const std::string &output, std::vector<ShareEntry> &out) {
    out.clear();
    std::vector<std::string> lines;
    {
        std::istringstream in(output);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!trim(line).empty()) lines.push_back(trim(line));
        }
    }
    if (lines.empty() || lines.back() != "ok") return false;
    lines.pop_back();
    for (const std::string &l : lines) {
        const std::vector<std::string> t = split_ws(l);
        if (t.size() != 4 && t.size() != 5) continue; /* sudo's or the shell's lines: not ours */
        ShareEntry e;
        e.name = t[0];
        e.host = t[1];
        e.share = t[2];
        if (t.size() == 5) e.subfolder = t[3];
        e.state = t.back();
        if (!valid_name(e.name) || !valid_host(e.host) || !valid_share(e.share) || (!e.subfolder.empty() && !valid_subfolder(e.subfolder))) continue;
        out.push_back(e);
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ messages */

std::string word_message(const std::string &word, const std::string &reason, bool with_user, bool removing) {
    if (word == "exists") return "That name is in use. Pick another name.";
    if (word == "nomodule" || word == "nohelper") return "Needs a launcher update or cifs-utils.";
    if (word == "unreachable") return "The server does not answer. Check its address.";
    if (word == "notfound") return "No share or folder of that name on the server.";
    if (word == "denied") {
        if (reason.find("needs a login") != std::string::npos) return "Check the share name, or sign in with a user.";
        return with_user ? "Refused: check the user name and password." : "Refused as guest: sign in with a user.";
    }
    if (word == "timeout") return "The server took too long. Try again.";
    if (word == "usage") return "The helper refused the request (update Vinyl and the launcher).";
    if (word == "noreply") return std::string(removing ? "Could not remove it" : "Could not add it") + ": the helper did not answer.";
    const std::string r = short_reason(reason);
    if (removing) return r.empty() ? "Could not remove it." : "Could not remove it: " + r + ".";
    return r.empty() ? "Could not add it." : "Could not add it: " + r + ".";
}

namespace {

Outcome sudo_outcome(const RunResult &r, bool removing) {
    Outcome o;
    const char *nothing = "Nothing was changed.";
    switch (r.result) {
    case SudoResult::AuthFailed:
        o.word = "sudo-auth";
        o.message = std::string("Wrong password. ") + nothing;
        break;
    case SudoResult::Cancelled:
        o.word = "sudo-cancel";
        o.message = std::string("Cancelled. ") + nothing;
        break;
    case SudoResult::TimedOut:
        o.word = "sudo-timeout";
        o.message = std::string("No answer in time. ") + nothing;
        break;
    case SudoResult::NoPrompt:
        o.word = "noprompt";
        o.message = "This build of Vinyl cannot ask for the password.";
        break;
    default: break;
    }
    if (!o.word.empty()) o.log = std::string(removing ? "remove" : "add") + ": " + o.word;
    return o;
}

bool sudo_refused(const RunResult &r) {
    return contains(r.output, "is not in the sudoers file") || contains(r.output, "is not allowed to execute") ||
           contains(r.output, "may not run sudo");
}

}  // namespace

Outcome add_outcome(const RunResult &r, bool with_user, const std::string &name) {
    Outcome o = sudo_outcome(r, false);
    if (!o.word.empty()) return o;
    const Reply rp = parse_reply(r.output);
    if (rp.ok && r.result == SudoResult::Success) {
        o.ok = true;
        o.path = rp.path.empty() ? std::string(kMountRoot) + "/" + name : rp.path;
        o.word = "ok";
        o.message = "Added. Scanning " + name + ".";
        o.log = "add " + name + ": ok " + o.path;
        return o;
    }
    if (!rp.ok && rp.word == "noreply" && sudo_refused(r)) {
        o.word = "sudo-denied";
        o.message = "This user may not use sudo. Nothing was changed.";
        o.log = "add " + name + ": sudo refused the user";
        return o;
    }
    o.word = rp.ok ? "noreply" : rp.word;
    o.message = word_message(o.word, rp.reason, with_user, false);
    o.log = "add " + name + ": err " + o.word + (rp.reason.empty() ? "" : " (" + short_reason(rp.reason) + ")") + ", exit " + std::to_string(r.exit_code);
    return o;
}

Outcome remove_outcome(const RunResult &r, const std::string &name) {
    Outcome o = sudo_outcome(r, true);
    if (!o.word.empty()) return o;
    const Reply rp = parse_reply(r.output);
    if (rp.ok && r.result == SudoResult::Success) {
        o.ok = true;
        o.path = std::string(kMountRoot) + "/" + name;
        o.word = "ok";
        o.message = name + " removed. No file on the server was touched.";
        o.log = "remove " + name + ": ok";
        return o;
    }
    if (!rp.ok && rp.word == "noreply" && sudo_refused(r)) {
        o.word = "sudo-denied";
        o.message = "This user may not use sudo. Nothing was changed.";
        o.log = "remove " + name + ": sudo refused the user";
        return o;
    }
    o.word = rp.ok ? "noreply" : rp.word;
    o.message = word_message(o.word, rp.reason, false, true);
    o.log = "remove " + name + ": err " + o.word + (rp.reason.empty() ? "" : " (" + short_reason(rp.reason) + ")") + ", exit " + std::to_string(r.exit_code);
    return o;
}

/* ------------------------------------------------------------------------------------------------ credentials file */

void wipe(std::string &s) {
    if (!s.empty()) {
        volatile char *p = &s[0];
        for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    }
    s.clear();
}

std::string runtime_dir() { return "/run/user/" + std::to_string(static_cast<unsigned long>(::getuid())); }

namespace {

std::string random_token(size_t n) {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string out;
    unsigned char buf[64];
    size_t got = 0;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        while (got < n) {
            const ssize_t k = ::read(fd, buf + got, std::min(sizeof buf, n) - got);
            if (k <= 0) break;
            got += static_cast<size_t>(k);
        }
        ::close(fd);
    }
    uint64_t x = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (static_cast<uint64_t>(::getpid()) << 32);
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const unsigned v = (i < got ? buf[i] : 0u) ^ static_cast<unsigned>(x);
        out += kChars[v % (sizeof kChars - 1)];
    }
    return out;
}

}  // namespace

CredFile write_credfile(const std::string &runtime, const std::string &user, const std::string &password) {
    CredFile cf;
    if (!valid_user(user) || !valid_password(password)) {
        cf.error = "bad user name or password";
        return cf;
    }
    const std::string dir = runtime + "/cyberdeckzero-share";
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        cf.error = std::string("cannot make ") + dir + ": " + std::strerror(errno);
        return cf;
    }
    struct stat st;
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) {
        cf.error = dir + " is not a folder of this user";
        return cf;
    }
    if ((st.st_mode & 07777) != 0700 && ::chmod(dir.c_str(), 0700) != 0) {
        cf.error = std::string("cannot set ") + dir + " to 0700";
        return cf;
    }
    std::string body = "username=" + user + "\npassword=" + password + "\n";
    for (int tries = 0; tries < 8 && !cf.ok; ++tries) {
        const std::string path = dir + "/" + random_token(24) + ".cred";
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            cf.error = std::string("cannot create the credentials file: ") + std::strerror(errno);
            break;
        }
        bool good = ::fchmod(fd, 0600) == 0;
        size_t off = 0;
        while (good && off < body.size()) {
            const ssize_t k = ::write(fd, body.data() + off, body.size() - off);
            if (k <= 0) good = false;
            else off += static_cast<size_t>(k);
        }
        ::close(fd);
        if (!good) {
            ::unlink(path.c_str());
            cf.error = "cannot write the credentials file";
            break;
        }
        cf.ok = true;
        cf.path = path;
    }
    if (!cf.ok && cf.error.empty()) cf.error = "no free name for the credentials file";
    wipe(body);
    return cf;
}

void remove_credfile(const std::string &path) {
    static const std::string mid = "/cyberdeckzero-share/";
    const size_t at = path.rfind(mid);
    if (path.empty() || at == std::string::npos || path.find('/', at + mid.size()) != std::string::npos || path.size() < 5 ||
        path.compare(path.size() - 5, 5, ".cred") != 0)
        return;
    struct stat st;
    if (::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) ::unlink(path.c_str());
}

}  // namespace shares
}  // namespace vinyl
