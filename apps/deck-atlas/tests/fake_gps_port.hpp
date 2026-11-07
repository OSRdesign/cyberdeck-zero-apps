/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: a fake serial-port layer for the GPS link (gps_port.hpp). Devices come and go, talk at one speed
 * (at any other speed the port delivers line noise), can be busy or denied, fail, and are fed bytes by the test. It
 * has no send call, like the interface it fakes.
 */
#pragma once

#include "gps_port.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

struct FakeDevice {
    atlas::PortInfo info;
    int baud = 9600;                          /* the speed the receiver talks at */
    bool present = true;                      /* in the list and openable */
    bool gone = false;                        /* an open port reads an error (unplugged) */
    atlas::PortErr open_err = atlas::PortErr::None;
    std::deque<std::uint8_t> data;            /* what the receiver sends, at its own speed */
    int opens = 0;
    std::vector<long> open_times;             /* the test's clock when it was opened (set by FakeGpsPorts::now) */
};

class FakeGpsPorts : public atlas::GpsPortSys {
public:
    std::vector<FakeDevice> devs;
    std::vector<std::string> events;          /* "open /dev/ttyACM0", "config /dev/ttyACM0 9600", "close /dev/ttyACM0" */
    long now = 0;                             /* the test sets it before a tick (for FakeDevice::open_times) */
    int list_calls = 0, open_calls = 0;
    std::size_t bytes_read = 0;

    FakeDevice &add(const std::string &tty, int baud = 9600, std::uint16_t vid = 0, const std::string &by_id = "")
    {
        FakeDevice d;
        d.info.tty = tty;
        d.info.path = "/dev/" + tty;
        d.info.vid = vid;
        d.info.by_id = by_id;
        d.baud = baud;
        devs.push_back(d);
        return devs.back();
    }
    void push(std::size_t dev, const std::string &text)
    {
        for (char c : text) devs[dev].data.push_back(static_cast<std::uint8_t>(c));
    }

    std::vector<atlas::PortInfo> list_ports() override
    {
        ++list_calls;
        std::vector<atlas::PortInfo> out;
        for (const FakeDevice &d : devs)
            if (d.present) out.push_back(d.info);
        return out;
    }
    int open(const std::string &path, atlas::PortErr &err) override
    {
        ++open_calls;
        err = atlas::PortErr::None;
        for (std::size_t i = 0; i < devs.size(); ++i) {
            FakeDevice &d = devs[i];
            if (d.info.path != path) continue;
            if (!d.present) {
                err = atlas::PortErr::Gone;
                return -1;
            }
            if (d.open_err != atlas::PortErr::None) {
                err = d.open_err;
                return -1;
            }
            ++d.opens;
            d.open_times.push_back(now);
            events.push_back("open " + path);
            cur_baud_[100 + static_cast<int>(i)] = 0;
            return 100 + static_cast<int>(i);
        }
        err = atlas::PortErr::Gone;
        return -1;
    }
    bool configure(int h, int baud) override
    {
        cur_baud_[h] = baud;
        events.push_back("config " + devs[static_cast<std::size_t>(h - 100)].info.path + " " + std::to_string(baud));
        return true;
    }
    void discard_input(int) override {}
    long read(int h, std::uint8_t *buf, std::size_t n) override
    {
        FakeDevice &d = devs[static_cast<std::size_t>(h - 100)];
        if (!d.present || d.gone) return -1;
        if (cur_baud_[h] != d.baud) { /* the wrong speed: line noise (no '$', some binary) */
            const std::size_t k = std::min<std::size_t>(n, 24);
            for (std::size_t i = 0; i < k; ++i) buf[i] = static_cast<std::uint8_t>(0x80 + (i * 37 + noise_++) % 100);
            bytes_read += k;
            return static_cast<long>(k);
        }
        const std::size_t k = std::min(n, d.data.size());
        for (std::size_t i = 0; i < k; ++i) {
            buf[i] = d.data.front();
            d.data.pop_front();
        }
        bytes_read += k;
        return static_cast<long>(k);
    }
    void close(int h) override { events.push_back("close " + devs[static_cast<std::size_t>(h - 100)].info.path); }

    int count(const std::string &prefix) const
    {
        int n = 0;
        for (const std::string &e : events)
            if (e.compare(0, prefix.size(), prefix) == 0) ++n;
        return n;
    }

private:
    std::map<int, int> cur_baud_;
    unsigned noise_ = 0;
};
