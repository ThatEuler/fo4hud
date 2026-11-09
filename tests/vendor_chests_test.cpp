#include "../vendor_chests.h"
#include "../loot_safety.h"
#include <array>
#include <cassert>
#include <cstring>

template<size_t N, class T> void put(std::array<unsigned char, N>& data, size_t off, T v) {
    assert(off + sizeof(v) <= N);
    std::memcpy(data.data() + off, &v, sizeof(v));
}
template<size_t N> uintptr_t ptr(std::array<unsigned char, N>& a) { return uintptr_t(a.data()); }
int main() {
    std::array<unsigned char, 0x108> ref{}, base{};
    std::array<unsigned char, 0x380> file{}, patch{};
    std::array<unsigned char, 0x10> sources{}, files{}, masters{};
    put(sources, 0, ptr(files)); put(sources, 8, uint32_t(1));
    put(files, 0, ptr(file));
    put(ref, 8, ptr(sources)); put(base, 8, ptr(sources));
    put(ref, 0x14, uint32_t(0x1679C7)); put(base, 0x14, uint32_t(0x1679C6));
    put(ref, 0x1A, uint8_t(64)); put(base, 0x1A, uint8_t(0x1F));
    put(ref, 0xE0, ptr(base));
    std::strcpy(reinterpret_cast<char*>(file.data()+0x70), "Fallout4.esm");
    auto read = [](uintptr_t address, auto& v) { if (!address) return false; std::memcpy(&v, reinterpret_cast<void*>(address), sizeof(v)); return true; };
    assert(std::string(fo4hud_vendor_chest_name(ptr(ref), read)) == "Trashcan Carla's vendor chest");
    put(ref, 0x14, uint32_t(0xFF001234));
    assert(std::string(fo4hud_vendor_chest_name(ptr(ref), read)) == "Trashcan Carla's vendor chest");
    put(ref, 0x14, uint32_t(0x1679C7));
    // A winning override resolves the original namespace through its masters.
    put(files, 0, ptr(patch)); put(patch, 0x370, uint8_t(7));
    put(patch, 0x358, uint32_t(1)); put(patch, 0x360, ptr(masters)); put(masters, 0, ptr(file));
    assert(fo4hud_vendor_chest_name(ptr(ref), read));
    // A matching local ID in another plugin or an unrelated base is rejected.
    file[0x70] = 'X'; assert(!fo4hud_vendor_chest_name(ptr(ref), read)); file[0x70] = 'F';
    put(base, 0x14, uint32_t(0x123)); assert(!fo4hud_vendor_chest_name(ptr(ref), read));
    put(base, 0x14, uint32_t(0x1679C6));
    // DLC load-order prefixes are resolved at runtime.
    put(files, 0, ptr(file)); std::strcpy(reinterpret_cast<char*>(file.data()+0x70), "DLCCoast.esm");
    put(file, 0x370, uint8_t(9));
    put(ref, 0x14, uint32_t(0x09005CE0)); put(base, 0x14, uint32_t(0x09005CDE));
    assert(std::string(fo4hud_vendor_chest_name(ptr(ref), read)) == "Allen Lee's vendor chest");
    // ESL identities additionally check their light-plugin index.
    std::strcpy(reinterpret_cast<char*>(file.data()+0x70), "ccsbjfo4003-grenade.esl");
    put(file, 0x370, uint8_t(0xFE)); put(file, 0x372, uint16_t(0x123));
    put(ref, 0x14, uint32_t(0xFE123142)); put(base, 0x14, uint32_t(0xFE123141));
    assert(fo4hud_vendor_chest_name(ptr(ref), read));
    put(file, 0x372, uint16_t(0x124)); assert(!fo4hud_vendor_chest_name(ptr(ref), read));
    // The special permission ignores locks, while preserving quest/activation guards.
    std::array<unsigned char, 0x20> extras{}, node{};
    put(ref, 0x100, ptr(extras)); put(extras, 8, ptr(node));
    for (uint8_t kind : {0x2A, 0xA3}) {
        put(node, 0x12, kind);
        assert(fo4hud_loot_reference_reason(ptr(ref), read));
        assert(!fo4hud_loot_reference_reason(ptr(ref), read, nullptr, true));
    }
    for (uint8_t kind : {0x88, 0x95, 0x52, 0x53, 0x68}) {
        put(node, 0x12, kind);
        assert(fo4hud_loot_reference_reason(ptr(ref), read, nullptr, true));
    }
}
