/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the driver registry (see driver.hpp).
 */
#include "driver.hpp"

#include "ld2410.hpp"
#include "ld2450.hpp"

#include <cstring>

namespace mmw {

namespace {

/* Real drivers first, then the stubs. */
const SensorDriver *const kRegistry[] = {&kDriverLd2450, &kDriverLd2410};

} // namespace

const SensorDriver *const *drivers(std::size_t &count)
{
    count = sizeof(kRegistry) / sizeof(kRegistry[0]);
    return kRegistry;
}

const SensorDriver *find_driver(const char *id)
{
    if (!id) return nullptr;
    for (const SensorDriver *d : kRegistry)
        if (std::strcmp(d->id, id) == 0) return d;
    return nullptr;
}

bool valid_sensor_type(const char *id)
{
    if (!id) return false;
    if (std::strcmp(id, kSensorAuto) == 0) return true;
    const SensorDriver *d = find_driver(id);
    return d && d->supported();
}

const char *next_sensor_type(const char *id)
{
    bool take_next = std::strcmp(id ? id : kSensorAuto, kSensorAuto) == 0;
    for (const SensorDriver *d : kRegistry) {
        if (!d->supported()) continue;
        if (take_next) return d->id;
        if (id && std::strcmp(d->id, id) == 0) take_next = true;
    }
    return kSensorAuto;
}

const char *sensor_type_label(const char *id)
{
    const SensorDriver *d = find_driver(id);
    return d && d->supported() ? d->name : "Auto";
}

} // namespace mmw
