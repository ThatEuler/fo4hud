#include "radar_map.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <zlib.h>

bool fo4hud_load_map_pixels(const std::string& texture, int& width, int& height,
                            std::vector<unsigned char>& pixels, std::string& error) {
    width = height = 0;
    pixels.clear();
    std::string name;
    for (unsigned char c : texture) {
        if (c == '/' || c == '\\') name += '_';
        else if (c >= 'A' && c <= 'Z') name += char(c + ('a' - 'A'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-' || c == '.') name += char(c);
        else { error = "Unsupported map texture path"; return false; }
    }
    if (name.empty() || name.size() > 240) { error = "Invalid map texture path"; return false; }
    std::filesystem::path directory;
    if (const char* override_path = std::getenv("FO4HUD_MAP_DIR")) directory = override_path;
    else {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void*>(fo4hud_load_map_pixels), &info) || !info.dli_fname) {
            error = "Cannot locate map assets"; return false;
        }
        directory = std::filesystem::path(info.dli_fname).parent_path() / "assets/maps";
    }
    std::ifstream file(directory / (name + ".fmap"), std::ios::binary | std::ios::ate);
    if (!file) { error = "Map texture has not been extracted"; return false; }
    const auto length = file.tellg();
    if (length < 12 || length > 64 * 1024 * 1024) { error = "Invalid map file size"; return false; }
    file.seekg(0);
    std::vector<unsigned char> data(static_cast<size_t>(length));
    if (!file.read(reinterpret_cast<char*>(data.data()), data.size()) ||
        std::memcmp(data.data(), "FMAP", 4) != 0) {
        error = "Invalid map file"; return false;
    }
    uint32_t w = 0, h = 0;
    std::memcpy(&w, data.data() + 4, 4);
    std::memcpy(&h, data.data() + 8, 4);
    if (!w || !h || w > 4096 || h > 4096) { error = "Invalid map dimensions"; return false; }
    pixels.resize(size_t(w) * h * 4);
    uLongf unpacked = pixels.size();
    if (uncompress(pixels.data(), &unpacked, data.data() + 12, data.size() - 12) != Z_OK ||
        unpacked != pixels.size()) {
        pixels.clear(); error = "Cannot decompress map pixels"; return false;
    }
    width = int(w); height = int(h);
    error.clear();
    return true;
}
