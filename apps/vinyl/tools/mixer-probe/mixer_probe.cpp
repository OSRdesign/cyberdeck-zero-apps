/*
 * SPDX-License-Identifier: MIT
 *
 * vinyl-mixer-probe: what Vinyl sees and does on a card's playback mixer, for the board probes (the Pi 4B, 2026-11-13:
 * "i'm at 5% and it's like 80 to 100% perceived"). Nothing of it runs in Vinyl itself.
 *
 *   vinyl-mixer-probe [CARD] [--set PCT] [--selem-raw PCT] [--selem-db PCT] [--loop N] [--keep]
 *
 *   CARD           the control device, as Vinyl names it: hw:CARD=Headphones (default: "default")
 *   (no option)    lists the card's mixer controls as Vinyl reads them (src/core/alsa, after its dB fix) and as the
 *                  simple-mixer layer of alsa-lib (amixer's view) reports them, Vinyl's choice and its level
 *   --set PCT      sets the level whose percent is nearest PCT with Vinyl's own code path (write_mixer_level: the
 *                  output's own levels, linear in dB over at most its top 60 dB), then reads the control back fresh both
 *                  ways (PCT 0: mute)
 *   --selem-raw PCT  sets the raw value Vinyl computes for that level with snd_mixer_selem_set_playback_volume_all
 *   --selem-db PCT   sets that level's dB with snd_mixer_selem_set_playback_dB_all
 *   --loop N       after a set, reads N more times, 250 ms apart (does the driver move it?)
 *   --keep         leaves the last value set; without it the control's value from before the probe is put back at the end
 *
 * libasound.so.2 is loaded at run time, as in Vinyl. Exit 0, 1 on a usage error, 2 when the card or its mixer is missing.
 */
#include "alsa/alsa_api.hpp"
#include "volume/output_volume.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vinyl;

namespace {

/* the simple-mixer API of alsa-lib (opaque handles; the channel enum as int: 0 = front left = mono) */
struct Selem {
    void *h = nullptr;
    int (*open)(void **, int) = nullptr;
    int (*attach)(void *, const char *) = nullptr;
    int (*reg)(void *, void *, void **) = nullptr;
    int (*load)(void *) = nullptr;
    int (*close)(void *) = nullptr;
    void *(*first)(void *) = nullptr;
    void *(*next)(void *) = nullptr;
    const char *(*name)(void *) = nullptr;
    unsigned (*index)(void *) = nullptr;
    int (*has_pvol)(void *) = nullptr;
    int (*has_psw)(void *) = nullptr;
    int (*is_pmono)(void *) = nullptr;
    int (*pvol_range)(void *, long *, long *) = nullptr;
    int (*pdb_range)(void *, long *, long *) = nullptr;
    int (*get_pvol)(void *, int, long *) = nullptr;
    int (*get_pdb)(void *, int, long *) = nullptr;
    int (*get_psw)(void *, int, int *) = nullptr;
    int (*set_pvol_all)(void *, long) = nullptr;
    int (*set_pdb_all)(void *, long, int) = nullptr;
    const char *(*strerror)(int) = nullptr;

    bool load_lib() {
        h = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!h) return false;
        bool ok = true;
        auto sym = [&](auto &fn, const char *n) {
            void *p = ::dlsym(h, n);
            if (!p) {
                std::printf("  libasound lacks %s\n", n);
                ok = false;
            }
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
        };
        sym(open, "snd_mixer_open");
        sym(attach, "snd_mixer_attach");
        sym(reg, "snd_mixer_selem_register");
        sym(load, "snd_mixer_load");
        sym(close, "snd_mixer_close");
        sym(first, "snd_mixer_first_elem");
        sym(next, "snd_mixer_elem_next");
        sym(name, "snd_mixer_selem_get_name");
        sym(index, "snd_mixer_selem_get_index");
        sym(has_pvol, "snd_mixer_selem_has_playback_volume");
        sym(has_psw, "snd_mixer_selem_has_playback_switch");
        sym(is_pmono, "snd_mixer_selem_is_playback_mono");
        sym(pvol_range, "snd_mixer_selem_get_playback_volume_range");
        sym(pdb_range, "snd_mixer_selem_get_playback_dB_range");
        sym(get_pvol, "snd_mixer_selem_get_playback_volume");
        sym(get_pdb, "snd_mixer_selem_get_playback_dB");
        sym(get_psw, "snd_mixer_selem_get_playback_switch");
        sym(set_pvol_all, "snd_mixer_selem_set_playback_volume_all");
        sym(set_pdb_all, "snd_mixer_selem_set_playback_dB_all");
        sym(strerror, "snd_strerror");
        return ok;
    }

    const char *err(int r) const { return strerror ? strerror(r) : "error"; }

    /* a fresh simple-mixer handle on the card (closed by the caller) */
    void *open_card(const std::string &card) {
        void *m = nullptr;
        int r = open(&m, 0);
        if (r < 0) {
            std::printf("  snd_mixer_open: %s\n", err(r));
            return nullptr;
        }
        if ((r = attach(m, card.c_str())) < 0 || (r = reg(m, nullptr, nullptr)) < 0 || (r = load(m)) < 0) {
            std::printf("  snd_mixer on %s: %s\n", card.c_str(), err(r));
            close(m);
            return nullptr;
        }
        return m;
    }

    void *find(void *m, const std::string &n) {
        for (void *e = first(m); e; e = next(e))
            if (n == name(e) && index(e) == 0) return e;
        return nullptr;
    }
};

std::string db_text(long cdb) {
    char b[32];
    if (cdb <= kAlsaDbGainMute) return "mute (" + std::to_string(cdb) + ")";
    std::snprintf(b, sizeof b, "%.2f dB", cdb / 100.0);
    return b;
}

/* the simple-mixer view of every playback element */
void list_selem(Selem &s, const std::string &card) {
    void *m = s.open_card(card);
    if (!m) return;
    std::printf("simple-mixer elements (amixer's view):\n");
    for (void *e = s.first(m); e; e = s.next(e)) {
        if (!s.has_pvol(e)) continue;
        long mn = 0, mx = 0, dmn = 0, dmx = 0, v = 0, d = 0;
        int sw = -1;
        s.pvol_range(e, &mn, &mx);
        const int rd = s.pdb_range(e, &dmn, &dmx);
        s.get_pvol(e, 0, &v);
        const int rg = s.get_pdb(e, 0, &d);
        if (s.has_psw(e)) s.get_psw(e, 0, &sw);
        std::printf("  '%s',%u%s raw %ld..%ld", s.name(e), s.index(e), s.is_pmono(e) ? " mono" : "", mn, mx);
        if (rd >= 0) std::printf(", dB range %s .. %s", db_text(dmn).c_str(), db_text(dmx).c_str());
        else std::printf(", no dB range (%s)", s.err(rd));
        std::printf("; now raw %ld", v);
        if (rg >= 0) std::printf(" = %s", db_text(d).c_str());
        if (sw >= 0) std::printf(", switch %s", sw ? "on" : "off");
        std::printf("\n");
    }
    s.close(m);
}

/* one fresh read both ways */
void read_back(AlsaApi &a, Selem &s, bool have_selem, const std::string &card, const MixerChoice &c, const std::string &selem_name,
               const char *tag) {
    long raw = 0;
    const int r = a.mixer_get(card, c.volume, raw);
    std::printf("  %-10s vinyl read: ", tag);
    if (r < 0) std::printf("failed (%s)", alsa_error_text(r).c_str());
    else {
        const int lv = mixer_level_for_raw(c, raw);
        std::printf("raw=%ld (level %d/%d, %d %%%s)", raw, lv, c.model.n, c.model.pct_of(lv),
                    c.has_db ? (", " + db_text(mixer_cdb_for(c, raw))).c_str() : "");
    }
    if (have_selem) {
        if (void *m = s.open_card(card)) {
            if (void *e = s.find(m, selem_name)) {
                long v = 0, d = 0;
                s.get_pvol(e, 0, &v);
                const int rg = s.get_pdb(e, 0, &d);
                std::printf("; simple-mixer read: raw=%ld%s", v, rg >= 0 ? (" = " + db_text(d)).c_str() : "");
            }
            s.close(m);
        }
    }
    std::printf("\n");
}

int usage() {
    std::fprintf(stderr, "usage: vinyl-mixer-probe [CARD] [--set PCT] [--selem-raw PCT] [--selem-db PCT] [--loop N] [--keep]\n"
                         "  CARD: hw:CARD=<id> (aplay -l, /proc/asound/cards), default \"default\"\n");
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    std::string card = "default";
    int set_pct = -1, raw_pct = -1, db_pct = -1, loops = 0;
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto num = [&](int &out) {
            if (i + 1 >= argc) return false;
            out = std::atoi(argv[++i]);
            return true;
        };
        if (a == "--set") {
            if (!num(set_pct)) return usage();
        } else if (a == "--selem-raw") {
            if (!num(raw_pct)) return usage();
        } else if (a == "--selem-db") {
            if (!num(db_pct)) return usage();
        } else if (a == "--loop") {
            if (!num(loops)) return usage();
        } else if (a == "--keep") {
            keep = true;
        } else if (a == "-h" || a == "--help") {
            return usage();
        } else if (!a.empty() && a[0] != '-') {
            card = a;
        } else {
            return usage();
        }
    }
    for (int *p : {&set_pct, &raw_pct, &db_pct})
        if (*p > 100) *p = 100;

    std::unique_ptr<AlsaApi> alsa = make_real_alsa();
    std::string why;
    if (!alsa->available(&why)) {
        std::printf("libasound: %s\n", why.c_str());
        return 2;
    }
    std::printf("card %s\n", card.c_str());
    std::vector<CtlInfo> ctls;
    int r = alsa->mixer_list(card, ctls);
    if (r < 0) {
        std::printf("mixer_list: %s\n", alsa_error_text(r).c_str());
        return 2;
    }
    std::printf("controls as Vinyl reads them (src/core/alsa, after the dB fix):\n");
    for (const CtlInfo &c : ctls) {
        if (c.capture || c.type == CtlInfo::Type::Other) continue;
        std::printf("  '%s' %s x%d", c.name.c_str(), c.type == CtlInfo::Type::Boolean ? "bool" : "int", c.count);
        if (c.type == CtlInfo::Type::Integer) {
            std::printf(" raw %ld..%ld", c.min, c.max);
            if (c.has_db) std::printf(", dB %s .. %s", db_text(c.db_min_cdb).c_str(), db_text(c.db_max_cdb).c_str());
            else std::printf(", no dB data");
            if (c.min_is_mute) std::printf(", min is mute");
        }
        std::printf("%s%s\n", c.readable ? "" : " (not readable)", c.writable ? "" : " (not writable)");
    }

    Selem s;
    const bool have_selem = s.load_lib();
    if (have_selem) list_selem(s, card);

    MixerChoice c;
    if (!choose_playback_mixer(ctls, c)) {
        std::printf("no playback mixer: Vinyl uses its software volume on this card\n");
        return 2;
    }
    std::string selem_name = c.volume;
    if (selem_name.size() > 16 && selem_name.compare(selem_name.size() - 16, 16, " Playback Volume") == 0)
        selem_name.resize(selem_name.size() - 16);
    std::printf("Vinyl's choice: '%s'%s, raw %ld..%ld", c.volume.c_str(), c.sw.empty() ? "" : (" + '" + c.sw + "'").c_str(), c.min,
                c.max);
    if (c.has_db) std::printf(", dB %s .. %s", db_text(c.db_min).c_str(), db_text(c.db_max).c_str());
    else std::printf(", no dB data");
    std::printf("%s\nVinyl's levels: %s\n", c.min_is_mute ? ", min is mute" : "", volume_model_text(c.model).c_str());
    std::string cc;
    MixerLevel lv;
    read_mixer(*alsa, card, cc, c, lv);
    std::printf("Vinyl's level: %d/%d, %d %%%s, raw=%ld%s\n", lv.level, c.model.n, lv.pct, lv.muted ? " (muted)" : "", lv.raw,
                c.has_db ? (", " + db_text(lv.cdb)).c_str() : "");
    std::printf("the levels here:\n");
    for (int i = 1; i <= c.model.n; ++i)
        std::printf("  %2d %3d %% %s raw=%ld\n", i, c.model.pct_of(i), c.has_db ? db_text(lround(c.model.db_of(i) * 100)).c_str() : "",
                    mixer_raw_for_level(c, i));

    const long before = lv.raw;
    bool changed = false;
    auto after = [&](const char *what) {
        changed = true;
        read_back(*alsa, s, have_selem, card, c, selem_name, what);
        for (int i = 0; i < loops; ++i) {
            ::usleep(250000);
            char t[24];
            std::snprintf(t, sizeof t, "+%d ms", (i + 1) * 250);
            read_back(*alsa, s, have_selem, card, c, selem_name, t);
        }
    };
    if (set_pct >= 0) {
        const int level = c.model.from_pct(set_pct);
        const long raw = mixer_raw_for_level(c, level);
        std::printf("--set %d: level %d/%d, Vinyl's write_mixer_level, raw=%ld\n", set_pct, level, c.model.n, raw);
        r = write_mixer_level(*alsa, card, c, level);
        if (r < 0) std::printf("  write_mixer_level: %s\n", alsa_error_text(r).c_str());
        long back = 0;
        const int rr = alsa->mixer_get(card, c.volume, back);
        std::printf("  log line: volume: %s\n",
                    volume_set_log_text(card + " '" + c.volume + "'", c.model, level, raw, rr, back).c_str());
        after("after");
    }
    if ((raw_pct >= 0 || db_pct >= 0) && !have_selem) std::printf("the simple-mixer layer is missing: --selem-* skipped\n");
    if (have_selem && (raw_pct >= 0 || db_pct >= 0)) {
        void *m = s.open_card(card);
        void *e = m ? s.find(m, selem_name) : nullptr;
        if (!e) std::printf("simple-mixer element '%s' not found\n", selem_name.c_str());
        if (e && raw_pct >= 0) {
            const long raw = mixer_raw_for_level(c, std::max(1, c.model.from_pct(raw_pct)));
            r = s.set_pvol_all(e, raw);
            std::printf("--selem-raw %d: snd_mixer_selem_set_playback_volume_all(%ld): %s\n", raw_pct, raw, r < 0 ? s.err(r) : "ok");
            after("after");
        }
        if (e && db_pct >= 0) {
            const long cdb = db_pct == 0 ? kAlsaDbGainMute : lround(c.model.db_of(c.model.from_pct(db_pct)) * 100);
            r = s.set_pdb_all(e, cdb, 0);
            std::printf("--selem-db %d: snd_mixer_selem_set_playback_dB_all(%s, dir 0): %s\n", db_pct, db_text(cdb).c_str(),
                        r < 0 ? s.err(r) : "ok");
            after("after");
        }
        if (m) s.close(m);
    }
    if (changed && !keep) {
        r = alsa->mixer_set(card, c.volume, before);
        std::printf("put back raw=%ld: %s\n", before, r < 0 ? alsa_error_text(r).c_str() : "ok");
        read_back(*alsa, s, have_selem, card, c, selem_name, "restored");
    }
    return 0;
}
