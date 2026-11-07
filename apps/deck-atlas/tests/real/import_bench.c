/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas, the optional real-tile check (tests/real/run.sh): imports one tile folder (or an MBTiles / PMTiles file,
 * copied) with the engine's own import into a new store, then prints what the import page would show and the numbers
 * of report 054 M8: tiles, skipped files, seconds, tiles per second, the output's size. Never writes to the source.
 *
 *   import_bench <source folder or file> <new store folder>
 *
 * Built by run.sh against the pinned engine's sources (cp0_lvgl src/map without the widget), like the engine's own
 * tests. Exit status 0 when the import succeeded.
 */
#include "cp0_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
    double t0, phase_t0, last_print;
    int phase;
    cp0_map_progress_t last;
    double convert_s;
    unsigned long long convert_done0;
} bench_t;

static const char *phase_name(int p)
{
    switch (p) {
    case CP0_MAP_PHASE_SCAN: return "scan";
    case CP0_MAP_PHASE_COPY: return "copy";
    case CP0_MAP_PHASE_CONVERT: return "convert";
    case CP0_MAP_PHASE_CHECK: return "check";
    default: return "?";
    }
}

static void progress(void *user, const cp0_map_progress_t *p)
{
    bench_t *b = (bench_t *)user;
    const double t = now_s();
    if ((int)p->phase != b->phase) {
        if (b->phase == CP0_MAP_PHASE_CONVERT) b->convert_s = t - b->phase_t0;
        printf("  %-8s at %6.2f s\n", phase_name((int)p->phase), t - b->t0);
        b->phase = (int)p->phase;
        b->phase_t0 = t;
        if (p->phase == CP0_MAP_PHASE_CONVERT) b->convert_done0 = p->done;
    }
    if (t - b->last_print >= 1.0) {
        printf("  %-8s %llu / %llu (skipped %llu, resumed %llu)\n", phase_name((int)p->phase),
               (unsigned long long)p->done, (unsigned long long)p->total, (unsigned long long)p->skipped,
               (unsigned long long)p->resumed);
        b->last_print = t;
    }
    b->last = *p;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <source folder or file> <new store folder>\n", argv[0]);
        return 2;
    }
    char ua[256];
    cp0_map_user_agent(ua, sizeof ua, "deck-atlas", "0.1.0");
    cp0_map_store_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.store_dir = argv[2];
    cfg.user_agent = ua;
    cfg.network = 0;
    cp0_map_store_t *store = NULL;
    int rc = cp0_map_store_open(&cfg, &store);
    if (rc != CP0_MAP_OK) {
        fprintf(stderr, "store: %s\n", cp0_map_strerror(rc));
        return 1;
    }
    bench_t b;
    memset(&b, 0, sizeof b);
    b.t0 = b.phase_t0 = now_s();
    char id[CP0_MAP_ID_MAX] = "";
    cp0_map_import_opts_t o;
    memset(&o, 0, sizeof o);
    struct stat st;
    o.copy = stat(argv[1], &st) == 0 && S_ISREG(st.st_mode);
    o.progress = progress;
    o.user = &b;
    o.out_id = id;
    o.out_id_size = sizeof id;
    rc = cp0_map_store_import_ex(store, argv[1], &o);
    const double total_s = now_s() - b.t0;
    if (b.phase == CP0_MAP_PHASE_CONVERT && b.convert_s == 0) b.convert_s = now_s() - b.phase_t0;
    if (rc != CP0_MAP_OK) {
        fprintf(stderr, "import: %s\n", cp0_map_strerror(rc));
        cp0_map_store_close(store);
        return 1;
    }
    cp0_map_source_info_t info;
    memset(&info, 0, sizeof info);
    cp0_map_store_find(store, id, &info);
    const unsigned long long tiles = b.last.phase == CP0_MAP_PHASE_CHECK ? 0 : (unsigned long long)b.last.done;
    printf("RESULT id=%s path=%s\n", id, info.path);
    printf("RESULT zooms=%d-%d tile_px=%d bytes=%llu (%.1f MB)\n", info.minzoom, info.maxzoom, info.tile_px,
           (unsigned long long)info.bytes, info.bytes / 1048576.0);
    printf("RESULT bounds=%.5f,%.5f,%.5f,%.5f attribution=%s\n", info.bounds.west, info.bounds.south, info.bounds.east,
           info.bounds.north, info.attribution);
    printf("RESULT skipped=%llu total_s=%.2f convert_s=%.2f last_done=%llu\n", (unsigned long long)b.last.skipped,
           total_s, b.convert_s, tiles);
    cp0_map_store_close(store);
    return 0;
}
