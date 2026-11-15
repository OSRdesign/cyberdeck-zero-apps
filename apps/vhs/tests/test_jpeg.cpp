/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the photo file (brief 7): the Annex K tables inserted into a frame that has none (and the frame then decodes
 * with TurboJPEG to the same pixels as with its own tables), a frame with tables left as it is, the EXIF APP1 written
 * and parsed back (Make, Model, Orientation, Software, the dates only with a synchronised clock, the pixel size), the
 * APP0 (JFIF) segment removed and the APP1 right after SOI.
 */
#include "check.hpp"

#include "camera/fake_camera.hpp"
#include "jpeg/dht.hpp"
#include "jpeg/exif.hpp"
#include "jpeg/turbojpeg.hpp"

#include <cstring>

using namespace vhs;

namespace {

std::vector<std::uint8_t> test_jpeg(int w, int h)
{
    std::vector<std::uint8_t> card, jpeg;
    fake_test_card(w, h, 1, 6, card);
    JpegEncoder enc;
    if (enc.ok()) enc.encode_bgrx(card.data(), w, h, 85, jpeg);
    return jpeg;
}

/* the frame without its DHT segments (as an MJPEG camera may send it) */
std::vector<std::uint8_t> strip_dht(const std::vector<std::uint8_t> &j)
{
    std::vector<std::uint8_t> out(j.begin(), j.begin() + 2);
    std::size_t i = 2;
    while (i + 4 <= j.size()) {
        const std::uint8_t m = j[i + 1];
        if (m == 0xDA) {
            out.insert(out.end(), j.begin() + static_cast<std::ptrdiff_t>(i), j.end());
            break;
        }
        const std::size_t len = (static_cast<std::size_t>(j[i + 2]) << 8) | j[i + 3];
        if (m != 0xC4) out.insert(out.end(), j.begin() + static_cast<std::ptrdiff_t>(i), j.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
        i += 2 + len;
    }
    return out;
}

std::vector<std::uint8_t> decode(const std::vector<std::uint8_t> &j, int w, int h)
{
    JpegDecoder dec;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    if (!dec.ok() || dec.decode_bgrx(j.data(), j.size(), 1, px.data(), w * 4, false) != 0) return {};
    return px;
}

void annex_k()
{
    const std::vector<std::uint8_t> &t = annex_k_dht();
    CHECK_EQ(t.size(), static_cast<std::size_t>(420)); /* 4 + (17 + 12) x 2 + (17 + 162) x 2 */
    CHECK_EQ(static_cast<int>(t[0]), 0xFF);
    CHECK_EQ(static_cast<int>(t[1]), 0xC4);
    CHECK_EQ((t[2] << 8) | t[3], 418);
    CHECK_EQ(static_cast<int>(t[4]), 0x00); /* DC 0 */
    /* not a JPEG */
    std::vector<std::uint8_t> out;
    const std::uint8_t junk[] = {1, 2, 3, 4, 5, 6};
    CHECK(!ensure_dht(junk, sizeof junk, out, nullptr));
    const std::uint8_t no_sos[] = {0xFF, 0xD8, 0xFF, 0xD9};
    CHECK(!ensure_dht(no_sos, sizeof no_sos, out, nullptr));
    if (!TurboJpeg::get().can_encode()) {
        SKIP("DHT round trip: no libturbojpeg.so.0");
        return;
    }
    const std::vector<std::uint8_t> j = test_jpeg(320, 240);
    CHECK(!j.empty());
    /* a frame with its tables: unchanged */
    bool ins = true;
    CHECK(ensure_dht(j.data(), j.size(), out, &ins));
    CHECK(!ins);
    CHECK(out == j);
    /* without: the Annex K tables go in before SOS, and it decodes to the very same pixels (libjpeg-turbo encodes with
     * these typical tables, so a wrong byte in them would show) */
    const std::vector<std::uint8_t> bare = strip_dht(j);
    int w = 0, h = 0;
    bool dht = true;
    CHECK(jpeg_peek(bare.data(), bare.size(), w, h, dht));
    CHECK(!dht);
    /* (libjpeg-turbo itself falls back to these tables when decoding a bare frame; a file must carry them) */
    CHECK(ensure_dht(bare.data(), bare.size(), out, &ins));
    CHECK(ins);
    CHECK_EQ(out.size(), bare.size() + 420);
    CHECK(jpeg_peek(out.data(), out.size(), w, h, dht));
    CHECK(dht);
    /* the tables themselves: libjpeg-turbo's encoder writes the same four tables, one DHT segment each */
    std::vector<std::uint8_t> theirs;
    for (std::size_t i = 2; i + 4 <= j.size() && j[i + 1] != 0xDA;) {
        const std::size_t len = (static_cast<std::size_t>(j[i + 2]) << 8) | j[i + 3];
        if (j[i + 1] == 0xC4) theirs.insert(theirs.end(), j.begin() + static_cast<std::ptrdiff_t>(i + 4), j.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
        i += 2 + len;
    }
    CHECK(theirs == std::vector<std::uint8_t>(t.begin() + 4, t.end()));
    const std::vector<std::uint8_t> a = decode(j, 320, 240), b = decode(out, 320, 240);
    CHECK(!a.empty());
    CHECK(a == b);
}

void exif_round_trip()
{
    ExifFields f;
    f.make = "Ingenic Semiconductor Co.,Ltd";
    f.model = "UGREEN FineCam 1080P CM969";
    f.software = kSoftware;
    f.datetime = "2026:11:14 18:47:10";
    f.width = 1920;
    f.height = 1080;
    const std::vector<std::uint8_t> app1 = exif_app1(f);
    CHECK_EQ(static_cast<int>(app1[1]), 0xE1);
    CHECK(std::memcmp(app1.data() + 4, "Exif\0\0II*\0", 10) == 0);
    /* in a minimal JPEG: SOI, APP1, EOI */
    std::vector<std::uint8_t> j = {0xFF, 0xD8};
    j.insert(j.end(), app1.begin(), app1.end());
    j.push_back(0xFF);
    j.push_back(0xD9);
    ExifFields r;
    CHECK(exif_parse(j.data(), j.size(), r));
    CHECK_EQ(r.make, f.make);
    CHECK_EQ(r.model, f.model);
    CHECK_EQ(r.software, std::string("VHS 0.1.0"));
    CHECK(r.has_orientation);
    CHECK_EQ(r.orientation, 1);
    CHECK_EQ(r.datetime, std::string("2026:11:14 18:47:10"));
    CHECK_EQ(r.datetime_original, std::string("2026:11:14 18:47:10"));
    CHECK(r.has_exif_ifd);
    CHECK_EQ(r.width, 1920u);
    CHECK_EQ(r.height, 1080u);
    /* the clock not synchronised: no DateTime, no DateTimeOriginal */
    f.datetime.clear();
    std::vector<std::uint8_t> k = {0xFF, 0xD8};
    const std::vector<std::uint8_t> nodate = exif_app1(f);
    k.insert(k.end(), nodate.begin(), nodate.end());
    CHECK(exif_parse(k.data(), k.size(), r));
    CHECK(r.datetime.empty());
    CHECK(r.datetime_original.empty());
    CHECK_EQ(r.width, 1920u);
    CHECK_EQ(r.model, f.model);
    /* no Make / Model known: left out; short strings inline */
    f.make.clear();
    f.model = "C27";
    std::vector<std::uint8_t> m = {0xFF, 0xD8};
    const std::vector<std::uint8_t> s = exif_app1(f);
    m.insert(m.end(), s.begin(), s.end());
    CHECK(exif_parse(m.data(), m.size(), r));
    CHECK(r.make.empty());
    CHECK_EQ(r.model, std::string("C27"));
    /* no EXIF */
    const std::uint8_t plain[] = {0xFF, 0xD8, 0xFF, 0xD9};
    CHECK(!exif_parse(plain, sizeof plain, r));
    CHECK_EQ(exif_datetime(0, true), std::string("1970:01:01 00:00:00"));
    CHECK_EQ(exif_datetime(1794682030, true), std::string("2026:11:14 18:47:10"));
}

void photo_file()
{
    if (!TurboJpeg::get().can_encode()) {
        SKIP("photo file: no libturbojpeg.so.0");
        return;
    }
    const std::vector<std::uint8_t> j = test_jpeg(640, 360);
    /* TurboJPEG writes a JFIF APP0: the photo drops it and puts the EXIF APP1 right after SOI */
    std::vector<std::uint8_t> mk = jpeg_markers(j.data(), j.size());
    CHECK(mk.size() >= 3 && mk[1] == 0xE0);
    ExifFields f;
    f.make = "Maker";
    f.model = "Cam";
    f.datetime = "2026:11:14 18:47:10";
    std::vector<std::uint8_t> out;
    std::string err;
    PhotoFacts facts;
    CHECK(make_photo(j.data(), j.size(), f, out, &err, &facts));
    CHECK(facts.app0_removed);
    CHECK(!facts.dht_inserted);
    CHECK_EQ(facts.width, 640);
    CHECK_EQ(facts.height, 360);
    mk = jpeg_markers(out.data(), out.size());
    CHECK(mk.size() >= 3 && mk[0] == 0xD8 && mk[1] == 0xE1);
    bool e0 = false;
    for (std::uint8_t m : mk) e0 |= m == 0xE0;
    CHECK(!e0);
    ExifFields r;
    CHECK(exif_parse(out.data(), out.size(), r));
    CHECK_EQ(r.width, 640u); /* from the frame's SOF */
    CHECK_EQ(r.height, 360u);
    CHECK_EQ(r.make, std::string("Maker"));
    /* the picture is the camera's, unchanged: the same pixels */
    CHECK(decode(out, 640, 360) == decode(j, 640, 360));
    /* a frame without tables: they go in, and an EXIF APP1 it carried is replaced */
    const std::vector<std::uint8_t> bare = strip_dht(out);
    std::vector<std::uint8_t> again;
    CHECK(make_photo(bare.data(), bare.size(), f, again, &err, &facts));
    CHECK(facts.dht_inserted);
    int app1 = 0;
    for (std::uint8_t m : jpeg_markers(again.data(), again.size())) app1 += m == 0xE1;
    CHECK_EQ(app1, 1);
    CHECK(decode(again, 640, 360) == decode(j, 640, 360));
    /* not a JPEG */
    const std::uint8_t junk[] = {0, 1, 2, 3};
    CHECK(!make_photo(junk, sizeof junk, f, out, &err, nullptr));
    CHECK(!err.empty());
}

} // namespace

void test_jpeg()
{
    annex_k();
    exif_round_trip();
    photo_file();
}
