#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

struct Fo4RadarPoint { float x, y; };
struct Fo4MapInfo {
    float west = 0, north = 0, east = 0, south = 0;
    std::string texture;
    // Texture coordinates of the MNAM corners, calibrated by WorldMapHolder.
    // Some DLC corners extend beyond the image (negative gutters).
    float u_west = 0, v_north = 0, u_east = 1, v_south = 1;
    // Local maps use the rendered camera's NW, NE, SW corners. These can be
    // rotated by the cell's north marker, independently of player heading.
    bool oriented = false;
    std::array<Fo4RadarPoint, 3> corners{};
    float determinant() const {
        return (corners[1].x - corners[0].x) * (corners[2].y - corners[0].y) -
               (corners[1].y - corners[0].y) * (corners[2].x - corners[0].x);
    }
    bool valid() const {
        if (oriented) {
            for (const auto& p : corners)
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
            return !texture.empty() && std::isfinite(determinant()) && std::abs(determinant()) > 1.0f;
        }
        return std::isfinite(west) && std::isfinite(north) &&
               std::isfinite(east) && std::isfinite(south) &&
               east > west && north > south && !texture.empty() &&
               std::isfinite(u_west) && std::isfinite(v_north) &&
               std::isfinite(u_east) && std::isfinite(v_south) &&
               u_east > u_west && v_south > v_north;
    }
    Fo4RadarPoint world_to_uv(float x, float y) const {
        if (oriented) {
            const float dx = x - corners[0].x, dy = y - corners[0].y;
            return {(dx * (corners[2].y - corners[0].y) - dy * (corners[2].x - corners[0].x)) / determinant(),
                    ((corners[1].x - corners[0].x) * dy - (corners[1].y - corners[0].y) * dx) / determinant()};
        }
        return {u_west + (x - west) / (east - west) * (u_east - u_west),
                v_north + (north - y) / (north - south) * (v_south - v_north)};
    }
};

struct Fo4RadarTransform {
    float player_x, player_y, heading, range, radius;
    Fo4RadarPoint project(float world_x, float world_y) const {
        const float x = world_x - player_x, y = world_y - player_y;
        const float s = std::sin(heading), c = std::cos(heading);
        return {(x * c - y * s) * radius / range,
                -(x * s + y * c) * radius / range};
    }
    Fo4RadarPoint unproject(float screen_x, float screen_y) const {
        const float right = screen_x * range / radius;
        const float forward = -screen_y * range / radius;
        const float s = std::sin(heading), c = std::cos(heading);
        return {player_x + right * c + forward * s,
                player_y - right * s + forward * c};
    }
};

struct Fo4MapVertex { float x, y, u, v; };
// Intersect the radar disc with the texture rectangle. Clipping in UV space
// keeps the geometry and UVs affine and avoids smearing edge texels outside
// the actual map. All positions are relative to the radar center.
inline std::vector<Fo4MapVertex> fo4hud_radar_map_polygon(
        const Fo4MapInfo& map, const Fo4RadarTransform& transform) {
    std::vector<Fo4MapVertex> polygon;
    if (!map.valid() || !(transform.range > 0) || !(transform.radius > 0)) return polygon;
    constexpr int segments = 128;
    for (int i = 0; i < segments; ++i) {
        const float angle = i * (6.28318530718f / segments);
        const float x = std::cos(angle) * transform.radius;
        const float y = std::sin(angle) * transform.radius;
        const auto world = transform.unproject(x, y);
        const auto uv = map.world_to_uv(world.x, world.y);
        polygon.push_back({x, y, uv.x, uv.y});
    }
    for (int edge = 0; edge < 4 && !polygon.empty(); ++edge) {
        auto distance = [edge](const Fo4MapVertex& p) {
            switch (edge) {
                case 0: return p.u;
                case 1: return 1.0f - p.u;
                case 2: return p.v;
                default: return 1.0f - p.v;
            }
        };
        std::vector<Fo4MapVertex> clipped;
        Fo4MapVertex previous = polygon.back();
        float previous_distance = distance(previous);
        for (const auto& current : polygon) {
            const float current_distance = distance(current);
            if ((previous_distance >= 0) != (current_distance >= 0)) {
                const float t = previous_distance / (previous_distance - current_distance);
                clipped.push_back({previous.x + t * (current.x - previous.x),
                                   previous.y + t * (current.y - previous.y),
                                   previous.u + t * (current.u - previous.u),
                                   previous.v + t * (current.v - previous.v)});
            }
            if (current_distance >= 0) clipped.push_back(current);
            previous = current;
            previous_distance = current_distance;
        }
        polygon = std::move(clipped);
    }
    return polygon;
}

// Loads a locally extracted map next to libfo4hud.so (assets/maps), or from
// FO4HUD_MAP_DIR when set. The cache contains only pixels, not map calibration.
bool fo4hud_load_map_pixels(const std::string& texture, int& width, int& height,
                            std::vector<unsigned char>& pixels, std::string& error);
