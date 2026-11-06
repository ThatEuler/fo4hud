#include "../game_entities.h"
#include "../radar_map.h"

#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <zlib.h>

template<class T, size_t N>
void put(std::array<unsigned char, N>& data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

int main() {
    // Actual Commonwealth MNAM bounds, from the game's cell-to-world mapping.
    const Fo4MapInfo commonwealth{-33 * 4096.0f, 25 * 4096.0f, 28 * 4096.0f, -36 * 4096.0f,
                                  "textures/interface/pip-boy/worldmap_d.dds",
                                  55.0f / 2048, 55.0f / 2048, 1990.0f / 2048, 1990.0f / 2048};
    for (float heading : {0.0f, 1.57079632679f, -2.1f}) {
        for (float range : {100.0f, 2000.0f, 100000.0f}) {
            Fo4RadarTransform t{-80000, 90000, heading, range, 200};
            const auto north = t.project(t.player_x, t.player_y + range);
            assert(std::abs(std::hypot(north.x, north.y) - 200) < 0.01f);
            const auto position = t.unproject(north.x, north.y);
            assert(std::abs(position.x - t.player_x) < 0.02f);
            assert(std::abs(position.y - t.player_y - range) < 0.02f);
            const auto polygon = fo4hud_radar_map_polygon(commonwealth, t);
            assert(polygon.size() >= 3);
            for (const auto& vertex : polygon) {
                assert(vertex.u >= -0.00001f && vertex.u <= 1.00001f);
                assert(vertex.v >= -0.00001f && vertex.v <= 1.00001f);
                assert(std::hypot(vertex.x, vertex.y) <= 200.01f);
                const auto marker = t.project(commonwealth.west + (vertex.u - commonwealth.u_west) /
                                                (commonwealth.u_east - commonwealth.u_west) * (commonwealth.east - commonwealth.west),
                                              commonwealth.north - (vertex.v - commonwealth.v_north) /
                                                (commonwealth.v_south - commonwealth.v_north) * (commonwealth.north - commonwealth.south));
                assert(std::abs(marker.x - vertex.x) < 0.1f);
                assert(std::abs(marker.y - vertex.y) < 0.1f);
            }
        }
    }
    // Map edges intersect the disc; a radar entirely outside it stays blank.
    assert(!fo4hud_radar_map_polygon(commonwealth, {commonwealth.west, commonwealth.north, 0.8f, 2000, 200}).empty());
    assert(fo4hud_radar_map_polygon(commonwealth, {1e6, 1e6, 0, 100, 200}).empty());
    const Fo4RadarTransform east_facing{0, 0, 1.57079632679f, 2000, 200};
    assert(std::abs(east_facing.project(2000, 0).y + 200) < 0.01f);

    // Exercise the real live-memory reader, including parent map inheritance.
    std::array<unsigned char, 0xC0> player{};
    std::array<unsigned char, 0xD0> cell{};
    std::array<unsigned char, 0x1C0> world{}, parent{};
    Fo4EntitySnapshot snapshot;
    snapshot.status = "Ready";
    snapshot.player_address = reinterpret_cast<uintptr_t>(player.data());
    snapshot.cell_address = reinterpret_cast<uintptr_t>(cell.data());
    snapshot.worldspace_address = reinterpret_cast<uintptr_t>(world.data());
    put(player, 0xB8, snapshot.cell_address);
    const char path[] = "Interface\\Pip-Boy\\WorldMap_d.dds";
    std::array<unsigned char, 0x80> pooled_path{};
    put(pooled_path, 0x08, uint16_t(2));
    put(pooled_path, 0x10, uint32_t(sizeof(path) - 1));
    std::memcpy(pooled_path.data() + 0x18, path, sizeof(path));
    put(world, 0x38, reinterpret_cast<uintptr_t>(pooled_path.data()));
    const int16_t cells[] = {-33, 25, 28, -36};
    std::memcpy(world.data() + 0x1B4, cells, sizeof(cells));
    Fo4MapInfo map;
    assert(fo4hud_read_world_map(snapshot, map));
    assert(map.west == commonwealth.west && map.north == commonwealth.north &&
           map.east == commonwealth.east && map.south == commonwealth.south &&
           map.texture == commonwealth.texture);
    assert(map.u_west == commonwealth.u_west && map.v_north == commonwealth.v_north &&
           map.u_east == commonwealth.u_east && map.v_south == commonwealth.v_south);
    // Reported roadside position: the Pip-Boy puts it at (674.024,365.685)
    // in the 2048px image, rather than the old uncalibrated (655.173,328.829).
    const auto roadside = fo4hud_radar_map_polygon(map, {-55236.83984375f, 62282.87890625f, 0, 100, 200});
    float pixel_x = 0, pixel_y = 0;
    for (const auto& vertex : roadside) {
        pixel_x += vertex.u * 2048 / roadside.size();
        pixel_y += vertex.v * 2048 / roadside.size();
    }
    assert(std::abs(pixel_x - 674.024f) < 0.02f);
    assert(std::abs(pixel_y - 365.685f) < 0.02f);
    auto set_texture = [&](const char* text) {
        pooled_path.fill(0);
        put(pooled_path, 0x10, uint32_t(std::strlen(text)));
        std::memcpy(pooled_path.data() + 0x18, text, std::strlen(text) + 1);
    };
    set_texture("DLC03\\Interface\\Map\\MapFarHarbor.dds");
    assert(fo4hud_read_world_map(snapshot, map));
    assert(map.u_west == 0.00920067f && map.v_north == 0.02515624f &&
           map.u_east == 1.0f - 0.03374173f && map.v_south == 1.0f - 0.00958931f);
    set_texture("DLC04\\Interface\\Map\\NukaWorldMap.DDS");
    assert(fo4hud_read_world_map(snapshot, map));
    assert(map.u_west == -0.045f && map.v_north == 0.0005f &&
           map.u_east == 1.095f && map.v_south == 1.155f);
    // Negative DLC gutters must still clip geometry at the actual image edge.
    for (const auto& vertex : fo4hud_radar_map_polygon(map, {map.west, map.north, 0.8f, 20000, 200})) {
        assert(vertex.u >= -0.00001f && vertex.u <= 1.00001f);
        assert(vertex.v >= -0.00001f && vertex.v <= 1.00001f);
    }
    set_texture(path);
    put(world, 0x38, uintptr_t(0));
    assert(fo4hud_read_world_map(snapshot, map) && map.texture == commonwealth.texture);
    std::array<unsigned char, 0x18> alias{};
    put(alias, 0x08, uint16_t(0x4000));
    put(alias, 0x10, reinterpret_cast<uintptr_t>(pooled_path.data()));
    put(world, 0x38, reinterpret_cast<uintptr_t>(alias.data()));
    assert(fo4hud_read_world_map(snapshot, map) && map.texture == commonwealth.texture);
    parent = world;
    put(world, 0x188, reinterpret_cast<uintptr_t>(parent.data()));
    put(world, 0x8A, uint8_t(4));
    put(world, 0x38, uintptr_t(0));
    assert(fo4hud_read_world_map(snapshot, map));
    snapshot.cell_is_interior = true;
    assert(!fo4hud_read_world_map(snapshot, map));
    snapshot.cell_is_interior = false;
    put(parent, 0x8A, uint8_t(4));
    put(parent, 0x188, snapshot.worldspace_address);
    assert(!fo4hud_read_world_map(snapshot, map)); // Cyclic parent data.
    put(player, 0xB8, uintptr_t(0));
    assert(!fo4hud_read_world_map(snapshot, map)); // Stale snapshot.

    // Decode exact pixel content and reject damaged cache entries.
    char directory[] = "/tmp/fo4hud-map-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(setenv("FO4HUD_MAP_DIR", directory, 1) == 0);
    const std::vector<unsigned char> expected = {255, 0, 0, 255, 0, 255, 0, 255};
    std::vector<unsigned char> compressed(compressBound(expected.size()));
    uLongf length = compressed.size();
    assert(compress(compressed.data(), &length, expected.data(), expected.size()) == Z_OK);
    const auto file_path = std::filesystem::path(directory) / "textures_test.dds.fmap";
    {
        std::ofstream file(file_path, std::ios::binary);
        const uint32_t dimensions[] = {2, 1};
        file.write("FMAP", 4);
        file.write(reinterpret_cast<const char*>(dimensions), sizeof(dimensions));
        file.write(reinterpret_cast<const char*>(compressed.data()), length);
    }
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    std::string error;
    assert(fo4hud_load_map_pixels("Textures\\Test.DDS", width, height, pixels, error));
    assert(width == 2 && height == 1 && pixels == expected);
    std::filesystem::resize_file(file_path, 13);
    assert(!fo4hud_load_map_pixels("Textures/Test.dds", width, height, pixels, error));
    assert(width == 0 && height == 0 && pixels.empty());
    assert(!fo4hud_load_map_pixels("Textures/missing.dds", width, height, pixels, error));
    std::filesystem::remove_all(directory);
}
