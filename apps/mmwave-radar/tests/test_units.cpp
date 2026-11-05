/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the units helper. The core keeps mm and cm/s; only units.hpp converts.
 */
#include "check.hpp"

#include "frame.hpp"
#include "units.hpp"

using namespace mmw;

void test_units()
{
    CHECK_EQ(mm_to_m(2340), 2.34);
    CHECK_EQ(cm_s_to_m_s(-51), -0.51);
    CHECK(cm_s_to_km_h(100) > 3.59 && cm_s_to_km_h(100) < 3.61);
    CHECK_EQ(format_distance(2340, UnitMode::MetresKmh), "2.34");
    CHECK_EQ(format_distance(2340, UnitMode::MmCms), "2340");
    CHECK_EQ(format_distance(0, UnitMode::MetresKmh), "0.00");
    CHECK_EQ(format_distance_unit(1883, UnitMode::MetresKmh), "1.88 m");
    CHECK_EQ(format_distance_unit(1883, UnitMode::MmCms), "1883 mm");
    CHECK_EQ(format_speed(-51, UnitMode::MetresKmh), "-1.8");
    CHECK_EQ(format_speed(-51, UnitMode::MmCms), "-51");
    CHECK_EQ(format_speed(17, UnitMode::MetresKmh), "0.6");
    CHECK_EQ(format_speed(0, UnitMode::MetresKmh), "0.0");
    CHECK_EQ(format_speed(1, UnitMode::MetresKmh), "0.0");
    CHECK_EQ(format_speed(-1, UnitMode::MetresKmh), "0.0"); /* never "-0.0" */
    CHECK_EQ(format_speed_unit(-136, UnitMode::MetresKmh), "-4.9 km/h");
    CHECK_EQ(format_speed_unit(34, UnitMode::MmCms), "34 cm/s");
    CHECK_EQ(std::string(distance_unit(UnitMode::MetresKmh)), "m");
    CHECK_EQ(std::string(speed_unit(UnitMode::MetresKmh)), "km/h");
    CHECK(unit_mode_from_name(unit_mode_name(UnitMode::MmCms)) == UnitMode::MmCms);
    CHECK(unit_mode_from_name(unit_mode_name(UnitMode::MetresKmh)) == UnitMode::MetresKmh);
    CHECK(unit_mode_from_name("nonsense") == UnitMode::MetresKmh);

    /* the core itself stays in the sensor's units: a decoded target is plain mm and cm/s, no conversion applied */
    const std::uint8_t d[30] = {0xAA, 0xFF, 0x03, 0x00, 0x0E, 0x03, 0xB1, 0x86, 0x10, 0x00, 0x68, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,
                                0,    0,    0,    0,    0,    0,    0,    0,    0x55, 0xCC};
    Frame f;
    CHECK(decode_frame(d, f));
    CHECK_EQ(f.target[0].y_mm, 1713);
    CHECK_EQ(f.target[0].speed_cm_s, -16);
}
