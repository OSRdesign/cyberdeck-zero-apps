/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests: the entity model (registry lists, rooms, a / c / r changes).
 */
#include "test_util.hpp"

#include "json.hpp"
#include "model.hpp"

#include <cmath>

using namespace ha;

namespace {

json::Value J(const std::string &text)
{
    json::Value v;
    if (!json::parse(text, v)) std::fprintf(stderr, "bad test JSON: %s\n", text.c_str());
    return v;
}

} // namespace

void test_model_group()
{
    CHECK(domain_of("light.x") == Domain::Light);
    CHECK(domain_of("binary_sensor.x") == Domain::BinarySensor);
    CHECK(domain_of("media_player.tv") == Domain::Other);
    CHECK(domain_of("nodot") == Domain::Other);
    CHECK(row_action(Domain::Light) == RowAction::Toggle);
    CHECK(row_action(Domain::InputBoolean) == RowAction::Toggle);
    CHECK(row_action(Domain::Scene) == RowAction::Run);
    CHECK(row_action(Domain::Lock) == RowAction::Confirm);
    CHECK(row_action(Domain::Cover) == RowAction::Open);
    CHECK(row_action(Domain::Climate) == RowAction::Info);

    Model m;
    CHECK(m.set_areas(J("[{\"area_id\":\"kitchen\",\"name\":\"Kitchen\"},{\"area_id\":\"bed\",\"name\":\"bedroom\"},"
                        "{\"area_id\":\"empty\",\"name\":\"Attic\"},{\"name\":\"no id\"}]")));
    CHECK(m.area_count() == 3);
    CHECK(m.set_devices(J("[{\"id\":\"d1\",\"area_id\":\"kitchen\",\"name\":\"Fridge\",\"name_by_user\":null},"
                          "{\"id\":\"d2\",\"area_id\":null,\"name\":\"Hue\",\"name_by_user\":\"Bulb\"}]")));
    CHECK(m.set_entities(J("{\"entity_categories\":{\"0\":\"config\"},\"entities\":["
                           "{\"ei\":\"light.k\",\"en\":\"Kitchen light\",\"ai\":\"kitchen\"},"
                           "{\"ei\":\"sensor.fridge\",\"en\":\"Temperature\",\"di\":\"d1\",\"hn\":true,\"dp\":1},"
                           "{\"ei\":\"switch.b\",\"en\":\"Blanket\",\"ai\":\"bed\"},"
                           "{\"ei\":\"light.hue\",\"di\":\"d2\"},"
                           "{\"ei\":\"sensor.diag\",\"en\":\"Uptime\",\"ai\":\"kitchen\",\"ec\":1},"
                           "{\"ei\":\"light.hidden\",\"en\":\"Hidden\",\"ai\":\"kitchen\",\"hb\":true},"
                           "{\"ei\":\"media_player.tv\",\"en\":\"TV\",\"ai\":\"kitchen\"},"
                           "{\"ei\":\"light.gone_area\",\"en\":\"Old\",\"ai\":\"deleted_area\"},"
                           "{\"en\":\"no id\"}]}")));
    CHECK(m.registry_size() == 7); /* media_player and the entry without an id dropped */
    CHECK_EQ(m.area_of("sensor.fridge"), std::string("kitchen")); /* from its device */
    CHECK_EQ(m.display_name("sensor.fridge"), std::string("Fridge Temperature"));
    CHECK_EQ(m.display_name("light.hue"), std::string("Bulb"));
    CHECK_EQ(m.display_name("light.not_known_at_all"), std::string("not known at all"));
    CHECK(m.entry("sensor.fridge")->precision == 1);
    CHECK(m.entry("sensor.diag")->hidden);
    CHECK(m.entry("light.hidden")->hidden);

    const auto rooms = m.rooms();
    CHECK(rooms.size() == 4); /* Attic, bedroom, Kitchen sorted case-insensitively, then No room */
    CHECK_EQ(rooms[0].name, std::string("Attic"));
    CHECK(rooms[0].count == 0);
    CHECK_EQ(rooms[1].name, std::string("bedroom"));
    CHECK(rooms[1].count == 1);
    CHECK_EQ(rooms[2].name, std::string("Kitchen"));
    CHECK(rooms[2].count == 2); /* light.k and sensor.fridge (via device); hidden and diagnostic not counted */
    CHECK_EQ(rooms[3].name, std::string(kNoRoomName));
    CHECK(rooms[3].id.empty());
    CHECK(rooms[3].count == 2); /* light.hue (device without area) and light.gone_area (deleted area) */
    const auto kitchen = m.entities_in("kitchen");
    CHECK(kitchen.size() == 2);
    CHECK_EQ(kitchen[0], std::string("sensor.fridge")); /* "fridge temperature" < "kitchen light" */
    CHECK_EQ(kitchen[1], std::string("light.k"));
    CHECK(m.entities_in("").size() == 2);
    CHECK_EQ(m.area_name(""), std::string(kNoRoomName));

    /* subscribe_entities: snapshot, changes, removal */
    std::set<std::string> ids;
    CHECK(m.apply_event(J("{\"a\":{\"light.k\":{\"s\":\"on\",\"a\":{\"friendly_name\":\"Kitchen ceiling\",\"brightness\":128,"
                          "\"supported_color_modes\":[\"brightness\"],\"effect_list\":[\"a\",\"b\"]},\"c\":\"01X\",\"lc\":1762000000.5},"
                          "\"sensor.fridge\":{\"s\":\"4.12\",\"a\":{\"unit_of_measurement\":\"\\u00b0C\"},\"c\":{\"id\":\"01Y\"},\"lc\":1762000001}}}"),
                        &ids));
    CHECK(ids.size() == 2);
    CHECK(m.state_count() == 2);
    CHECK_EQ(m.display_name("light.k"), std::string("Kitchen ceiling"));
    CHECK_EQ(m.display_name("light.k", false), std::string("Kitchen light"));
    CHECK(m.state("light.k")->dimmable);
    CHECK(m.state("light.k")->brightness == 128);
    CHECK(std::fabs(m.state("light.k")->last_changed - 1762000000.5) < 1e-6);
    /* a change: state and brightness removed, an unknown entity ignored */
    ids.clear();
    CHECK(m.apply_event(J("{\"c\":{\"light.k\":{\"+\":{\"s\":\"off\",\"lc\":1762000100},\"-\":{\"a\":[\"brightness\"]}},"
                          "\"light.unknown\":{\"+\":{\"s\":\"on\"}}}}"),
                        &ids));
    CHECK(ids.size() == 1);
    CHECK_EQ(m.state("light.k")->state, std::string("off"));
    CHECK(m.state("light.k")->brightness == -1);
    CHECK_EQ(m.state("light.k")->friendly_name, std::string("Kitchen ceiling")); /* kept */
    CHECK(!m.state("light.unknown"));
    /* an attribute change only */
    CHECK(m.apply_event(J("{\"c\":{\"light.k\":{\"+\":{\"a\":{\"brightness\":20},\"s\":\"on\"}}}}")));
    CHECK(m.state("light.k")->brightness == 20);
    /* a diff without "+" content still counts as a change of that entity */
    CHECK(m.apply_event(J("{\"c\":{\"light.k\":{}}}")));
    /* removal */
    CHECK(m.apply_event(J("{\"r\":[\"sensor.fridge\",\"never.there\"]}")));
    CHECK(!m.state("sensor.fridge"));
    CHECK(!m.apply_event(J("{\"r\":[\"never.there\"]}")));
    /* keep_only after a subscription change */
    m.apply_event(J("{\"a\":{\"switch.b\":{\"s\":\"on\",\"a\":{}}}}"));
    m.keep_only({"switch.b"});
    CHECK(m.state_count() == 1 && m.state("switch.b"));
    CHECK(!m.set_entities(J("[]")));
}
