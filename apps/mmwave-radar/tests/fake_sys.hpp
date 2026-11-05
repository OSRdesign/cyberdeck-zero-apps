/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: a fake port layer. Ports deliver scripted bytes against a test clock (bytes appear at the
 * line rate of the speed the port is set to, capped by the device's own rate); a port can be held by another app,
 * deny access, send nothing or vanish. The Sys interface has no write call, so there is nothing to count: a test that
 * compiles proves the code under test cannot write.
 */
#pragma once

#include "port_sys.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mmw {

struct FakePort {
    PortMeta meta;
    bool excl_busy = false;       /* open() fails with EBUSY (TIOCEXCL held elsewhere) */
    bool lock_busy = false;       /* the flock is taken */
    bool proc_held = false;       /* /proc shows another process with it open */
    bool denied = false;
    std::map<int, std::string> stream;   /* per speed: the bytes the device sends, cycled */
    std::string wrong_speed = std::string(1, '\0'); /* what any other speed looks like (cycled); "" = nothing */
    double rate_bps = 578.0;      /* the device's own byte rate */
    long vanish_at_ms = -1;       /* from this time on reads fail */
    bool stopped = false;         /* the device stopped sending (the link stays) */
    /* counters */
    int opens = 0, closes = 0, restores = 0;
    std::vector<int> bauds;       /* every speed set, in order */
    /* state */
    int handle = -1;
    int cur_baud = 0;
    long cfg_ms = 0;
    double delivered = 0;
    std::size_t offset = 0;
};

class FakeSys : public Sys {
public:
    long now_ms = 0;
    std::vector<FakePort> ports;
    int next_handle = 10;

    FakePort &add(const std::string &tty, std::uint16_t vid = 0x1a86, std::uint16_t pid = 0x7523)
    {
        FakePort p;
        p.meta.tty = tty;
        p.meta.path = "/dev/" + tty;
        p.meta.vid = vid;
        p.meta.pid = pid;
        ports.push_back(p);
        return ports.back();
    }
    FakePort *by_tty(const std::string &tty)
    {
        for (FakePort &p : ports)
            if (p.meta.tty == tty) return &p;
        return nullptr;
    }
    void unplug(const std::string &tty)
    {
        ports.erase(std::remove_if(ports.begin(), ports.end(), [&](const FakePort &p) { return p.meta.tty == tty; }),
                    ports.end());
    }
    int open_handles() const
    {
        int n = 0;
        for (const FakePort &p : ports) n += p.handle >= 0 ? 1 : 0;
        return n;
    }
    int total_opens() const
    {
        int n = 0;
        for (const FakePort &p : ports) n += p.opens;
        return n;
    }

    std::vector<PortMeta> list_ports() override
    {
        std::vector<PortMeta> v;
        for (const FakePort &p : ports) v.push_back(p.meta);
        return v;
    }
    bool held_by_other(const std::string &path) override
    {
        const FakePort *p = find(path);
        return p && p->proc_held;
    }
    int open(const std::string &path, OpenErr &err) override
    {
        FakePort *p = find(path);
        err = OpenErr::None;
        if (!p) { err = OpenErr::Gone; return -1; }
        if (p->denied) { err = OpenErr::Denied; return -1; }
        if (p->excl_busy || p->lock_busy || p->handle >= 0) { err = OpenErr::Busy; return -1; }
        ++p->opens;
        p->handle = next_handle++;
        p->cur_baud = 0;
        return p->handle;
    }
    bool configure(int h, int baud) override
    {
        FakePort *p = by_handle(h);
        if (!p) return false;
        p->cur_baud = baud;
        p->cfg_ms = now_ms;
        p->delivered = 0;
        p->offset = 0; /* a new speed: the stream starts again from its beginning */
        p->bauds.push_back(baud);
        return true;
    }
    void flush_input(int h) override
    {
        FakePort *p = by_handle(h);
        if (!p) return;
        p->cfg_ms = now_ms;
        p->delivered = 0;
    }
    long read(int h, std::uint8_t *buf, std::size_t n) override
    {
        FakePort *p = by_handle(h);
        if (!p) return -1;
        if (p->vanish_at_ms >= 0 && now_ms >= p->vanish_at_ms) return -1;
        if (p->stopped) return 0;
        const std::string *src = nullptr;
        const auto it = p->stream.find(p->cur_baud);
        if (it != p->stream.end()) src = &it->second;
        else if (!p->wrong_speed.empty()) src = &p->wrong_speed;
        if (!src || src->empty()) return 0;
        const double rate = it != p->stream.end() ? std::min(p->rate_bps, p->cur_baud / 10.0) : p->cur_baud / 10.0;
        const double avail = (now_ms - p->cfg_ms) / 1000.0 * rate - p->delivered;
        const std::size_t k = std::min<std::size_t>(n, avail > 0 ? static_cast<std::size_t>(avail) : 0);
        for (std::size_t i = 0; i < k; ++i) buf[i] = static_cast<std::uint8_t>((*src)[(p->offset + i) % src->size()]);
        p->offset += k;
        p->delivered += static_cast<double>(k);
        return static_cast<long>(k);
    }
    void restore(int h) override
    {
        if (FakePort *p = by_handle(h)) ++p->restores;
    }
    void close(int h) override
    {
        if (FakePort *p = by_handle(h)) {
            ++p->closes;
            p->handle = -1;
        }
    }

private:
    FakePort *find(const std::string &path)
    {
        for (FakePort &p : ports)
            if (p.meta.path == path) return &p;
        return nullptr;
    }
    FakePort *by_handle(int h)
    {
        for (FakePort &p : ports)
            if (p.handle == h && h >= 0) return &p;
        return nullptr;
    }
};

} // namespace mmw
