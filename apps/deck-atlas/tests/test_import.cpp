/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the import page's model (phases from the engine's progress, the bar, the counts, the rate and the
 * time left, a resumed import, the end states) and the free-space check.
 */
#include "check.hpp"
#include "import_model.hpp"

using namespace atlas;

void test_import()
{
    ImportProgress p;
    p.start(ImportKind::Folder, "tiles", 1000);
    CHECK(p.running());
    CHECK(p.phase() == ImportPhase::Space);
    CHECK_EQ(p.title(), std::string("Checking free space"));
    CHECK_EQ(p.fraction(), 0.0);
    CHECK_EQ(p.name(), std::string("tiles"));

    /* scan: counting (no bar yet) */
    p.update(1, 5000, 0, 0, 0, 1500);
    CHECK(p.phase() == ImportPhase::Scan);
    CHECK_EQ(p.title(), std::string("Counting tiles"));
    CHECK_EQ(p.count_line(), std::string("5,000 tiles found"));
    CHECK_EQ(p.fraction(), 0.0);
    CHECK_EQ(p.rate_line(1500), std::string(""));

    /* convert: 0 .. total, the rate after a second, the time left */
    p.update(3, 0, 10326, 0, 0, 2000);
    CHECK(p.phase() == ImportPhase::Convert);
    CHECK_EQ(p.title(), std::string("Converting tiles"));
    CHECK_EQ(p.count_line(), std::string("0 of 10,326 tiles"));
    p.update(3, 400, 10326, 0, 0, 2500);
    CHECK_EQ(p.rate(2500), 0.0); /* under a second: no rate yet */
    p.update(3, 4000, 10326, 2, 0, 7000);
    CHECK_NEAR(p.fraction(), 4000.0 / 10326.0, 1e-9);
    CHECK_NEAR(p.rate(7000), 800.0, 1e-9);
    CHECK_EQ(p.count_line(), std::string("4,000 of 10,326 tiles"));
    CHECK_EQ(p.rate_line(7000), std::string("800 tiles/s, about 8 s left"));
    CHECK_EQ(p.skipped(), static_cast<std::uint64_t>(2));

    /* check, then done: the convert phase's average stays for the result line */
    p.update(3, 10326, 10326, 2, 0, 14000);
    p.update(4, 0, 0, 2, 0, 14100);
    CHECK(p.phase() == ImportPhase::Check);
    CHECK_EQ(p.fraction(), 1.0);
    CHECK_NEAR(p.convert_rate(), 10326.0 / 12.1, 1e-6);
    p.finish_ok(10326, 289406976ull, 14300);
    CHECK(!p.running());
    CHECK(p.phase() == ImportPhase::Done);
    CHECK_EQ(p.title(), std::string("Imported"));
    CHECK_EQ(p.result_line(), std::string("10,326 tiles, 276 MB, 853 tiles/s, 2 other files left out"));
    p.update(3, 1, 2, 0, 0, 15000); /* late progress after the end is ignored */
    CHECK(p.phase() == ImportPhase::Done);

    /* a resumed conversion: the rate counts only this run's tiles */
    ImportProgress q;
    q.start(ImportKind::Folder, "tiles", 0);
    q.update(3, 4000, 10326, 0, 4000, 1000);
    q.update(3, 6000, 10326, 0, 4000, 3000);
    CHECK_NEAR(q.rate(3000), 1000.0, 1e-9);
    CHECK_EQ(q.count_line(), std::string("6,000 of 10,326 tiles (4,000 from before)"));
    q.finish_cancelled(3500);
    CHECK(q.phase() == ImportPhase::Cancelled);
    CHECK_EQ(q.title(), std::string("Import stopped"));
    CHECK_EQ(q.result_line(), std::string("Stopped. Import the same folder again to go on where it stopped."));

    /* a copied file: bytes */
    ImportProgress c;
    c.start(ImportKind::FileCopy, "paris.mbtiles", 0);
    c.update(2, 120ull << 20, 276ull << 20, 0, 0, 2000);
    CHECK_EQ(c.title(), std::string("Copying the file"));
    CHECK_EQ(c.count_line(), std::string("120 MB of 276 MB"));
    CHECK_EQ(c.rate_line(2000), std::string(""));
    c.update(2, 140ull << 20, 276ull << 20, 0, 0, 4000);
    CHECK_EQ(c.rate_line(4000), std::string("10 MB/s, about 14 s left"));
    c.finish_failed("Not an MBTiles or PMTiles file", 4100);
    CHECK(c.phase() == ImportPhase::Failed);
    CHECK_EQ(c.title(), std::string("Import failed"));
    CHECK_EQ(c.result_line(), std::string("Not an MBTiles or PMTiles file"));
    ImportProgress c2;
    c2.start(ImportKind::FileCopy, "x.pmtiles", 0);
    c2.finish_cancelled(10);
    CHECK_EQ(c2.result_line(), std::string("Stopped. Nothing was added."));

    /* free space */
    const std::uint64_t mb = 1ull << 20;
    CHECK_EQ(space_needed(ImportKind::Folder, 276 * mb), 276 * mb + 276 * mb / 20 + 16 * mb);
    CHECK_EQ(space_needed(ImportKind::FileCopy, 100 * mb), 116 * mb);
    CHECK_EQ(space_needed(ImportKind::FileInPlace, 100 * mb), static_cast<std::uint64_t>(0));
    CHECK_EQ(space_error(100 * mb, 200 * mb), std::string(""));
    CHECK_EQ(space_error(306 * mb, 120 * mb), std::string("Not enough free space: about 306 MB needed, 120 MB free"));
    CHECK_EQ(space_error(0, 0), std::string(""));
}
