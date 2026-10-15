/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the system and fixture sources (see source.hpp).
 */
#include "source.hpp"

#include "text.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR ""
#endif

namespace nt {
namespace {

constexpr std::size_t kMaxFile = 256 * 1024;

std::string read_file(const std::string &path, bool *ok)
{
    if (ok) *ok = false;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string();
    std::string out;
    char buf[4096];
    bool good = true;
    while (out.size() < kMaxFile) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) good = false; /* sysfs "speed" of a Wi-Fi interface: EINVAL */
        break;
    }
    close(fd);
    if (out.size() > kMaxFile) out.resize(kMaxFile);
    if (ok) *ok = good;
    return good ? out : std::string();
}

std::vector<std::string> list_dir(const std::string &dir)
{
    std::vector<std::string> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    while (const dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
        if (out.size() > 4096) break;
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

int prefix_of(const sockaddr *mask)
{
    if (!mask) return 0;
    const unsigned char *p = nullptr;
    int n = 0;
    if (mask->sa_family == AF_INET) {
        p = reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in *>(mask)->sin_addr);
        n = 4;
    } else if (mask->sa_family == AF_INET6) {
        p = reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in6 *>(mask)->sin6_addr);
        n = 16;
    }
    int bits = 0;
    for (int i = 0; i < n; ++i)
        for (int b = 7; b >= 0; --b)
            if (p[i] & (1u << b)) ++bits;
    return bits;
}

class SystemSource : public Source {
public:
    std::string read(const std::string &path, bool *ok) const override { return read_file(path, ok); }
    bool exists(const std::string &path) const override
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }
    std::vector<std::string> list(const std::string &dir) const override { return list_dir(dir); }
    std::vector<IfAddr> addresses() const override
    {
        std::vector<IfAddr> out;
        ifaddrs *list = nullptr;
        if (getifaddrs(&list) != 0) return out;
        for (const ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || !i->ifa_name) continue;
            const int fam = i->ifa_addr->sa_family;
            if (fam != AF_INET && fam != AF_INET6) continue;
            char buf[INET6_ADDRSTRLEN] = {};
            const void *src = fam == AF_INET
                                  ? static_cast<const void *>(&reinterpret_cast<const sockaddr_in *>(i->ifa_addr)->sin_addr)
                                  : static_cast<const void *>(&reinterpret_cast<const sockaddr_in6 *>(i->ifa_addr)->sin6_addr);
            if (!inet_ntop(fam, src, buf, sizeof(buf))) continue;
            IfAddr a;
            a.iface = i->ifa_name;
            a.family = fam;
            a.address = buf;
            a.prefix = prefix_of(i->ifa_netmask);
            out.push_back(a);
            if (out.size() > 256) break;
        }
        freeifaddrs(list);
        return out;
    }
    std::string describe() const override { return "the system"; }
};

class FixtureSource : public Source {
public:
    explicit FixtureSource(std::string dir) : dir_(std::move(dir)) {}
    std::string read(const std::string &path, bool *ok) const override { return read_file(dir_ + path, ok); }
    bool exists(const std::string &path) const override
    {
        struct stat st;
        return stat((dir_ + path).c_str(), &st) == 0;
    }
    std::vector<std::string> list(const std::string &dir) const override
    {
        std::vector<std::string> out = list_dir(dir_ + dir);
        out.erase(std::remove(out.begin(), out.end(), std::string(".keep")), out.end());
        return out;
    }
    std::vector<IfAddr> addresses() const override { return parse_ifaddrs_fixture(read_file(dir_ + "/ifaddrs", nullptr)); }
    std::string describe() const override { return "fixture " + dir_; }

private:
    std::string dir_;
};

} // namespace

std::vector<IfAddr> parse_ifaddrs_fixture(const std::string &text)
{
    std::vector<IfAddr> out;
    for (const std::string &raw : split_lines(text)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = split_ws(line);
        if (f.size() < 3) continue;
        IfAddr a;
        a.iface = f[0];
        if (f[1] == "inet") a.family = AF_INET;
        else if (f[1] == "inet6") a.family = AF_INET6;
        else continue;
        const std::size_t slash = f[2].find('/');
        a.address = f[2].substr(0, slash);
        if (slash != std::string::npos && !parse_int(f[2].substr(slash + 1), a.prefix)) a.prefix = 0;
        out.push_back(a);
    }
    return out;
}

std::unique_ptr<Source> make_system_source()
{
    return std::make_unique<SystemSource>();
}

std::unique_ptr<Source> make_fixture_source(const std::string &dir)
{
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return std::make_unique<FixtureSource>(d);
}

std::unique_ptr<Source> source_from_env(std::string *described)
{
    const char *env = std::getenv("NET_TOOLBOX_FIXTURE");
    if (!env || !env[0]) {
        if (described) *described = "the system";
        return nullptr;
    }
    std::string dir = env;
    if (dir[0] != '/') dir = std::string(CDK_APP_DIR) + "/tests/fixtures/" + dir;
    if (described) *described = "fixture " + dir;
    return make_fixture_source(dir);
}

} // namespace nt
