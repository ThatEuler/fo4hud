#include "dialogue_affinity.h"
#include "control_memory.h"
#include "logging.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>

namespace {
using namespace fo4control;
using Display = uintptr_t (__attribute__((ms_abi)) *)(uintptr_t, uint8_t, uintptr_t, uintptr_t);
constexpr uintptr_t kCall = 0x416466, kDisplay = 0x4168E0;
constexpr uint8_t kOriginalCall[] = {0xE8, 0x75, 0x04, 0, 0};
std::mutex lifecycle, state_mutex;
std::atomic<uintptr_t> image_base{0};
std::atomic<bool> enabled{false};
uint8_t* island = nullptr;
Display original = nullptr;
using Advance = uintptr_t (__attribute__((ms_abi)) *)(uintptr_t, float, uintptr_t);
constexpr uintptr_t kAdvanceSlot = 0x25AD118 + 32, kAdvance = 0x10441B0;
Advance original_advance = nullptr;
uint64_t batch_generation = 0;
struct Layout {
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point sampled{};
    std::array<Fo4DialogueChoiceBounds, 4> bounds;
} layout;
struct Record { std::string last; std::vector<Fo4DialogueReaction> reactions; };
std::unordered_map<std::string, Record> affinity_records;
std::unordered_map<std::string, std::string> companions;
std::string index_status = "Affinity data unavailable";
struct Batch {
    uintptr_t player = 0, cell = 0, quest = 0, scene = 0;
    uint32_t phase = 0;
    uint32_t target = 0;
    uint8_t received = 0;
    std::array<Fo4DialogueChoiceAffinity, 4> choices;
} batch;

template<class T> T value(uintptr_t address) {
    T out{};
    if (!read_value(address, out)) throw std::runtime_error("Dialogue memory unavailable");
    return out;
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
std::string filename(uintptr_t file) {
    char bytes[260]{};
    if (!file || !read(file + 0x70, bytes, sizeof(bytes))) return {};
    auto end = static_cast<const char*>(std::memchr(bytes, 0, sizeof(bytes)));
    if (!end || end == bytes) return {};
    std::string name = lower(std::string(bytes, size_t(end - bytes)));
    if (name.find_first_of("/\\\r\n") != std::string::npos) return {};
    return name;
}
std::string owner_file(uintptr_t file, uint32_t id) {
    // TESForm's source-file array may retain only the winning override.
    // Resolve the runtime prefix through that TESFile's master table instead
    // of mistaking the patch filename for the original FormID namespace.
    std::vector<uintptr_t> pending{file}, visited;
    while (!pending.empty() && visited.size() < 256) {
        auto current = pending.back(); pending.pop_back();
        if (!current || std::find(visited.begin(), visited.end(), current) != visited.end()) continue;
        visited.push_back(current);
        const auto prefix = value<uint8_t>(current+0x370);
        if (prefix == id>>24 && (prefix != 0xFE || value<uint16_t>(current+0x372) == ((id>>12)&0xFFF)))
            return filename(current);
        const auto count = value<uint32_t>(current+0x358);
        const auto masters = value<uintptr_t>(current+0x360);
        if (count > 256 || (count && !masters)) return {};
        for (uint32_t i=0; i<count; ++i) pending.push_back(value<uintptr_t>(masters+8*i));
    }
    return {};
}
// TESForm source-file array verified against live 1.11.240 records. The last
// file supplies the winning override; the FormID prefix identifies the owner.
std::pair<std::string, std::string> form_key(uintptr_t form) {
    if (!form) return {};
    auto files = value<uintptr_t>(form + 8);
    if (!files) return {};
    auto entries = value<uintptr_t>(files);
    auto count = value<uint32_t>(files + 8);
    if (!entries || count < 1 || count > 256) return {};
    auto id = value<uint32_t>(form + 0x14);
    auto winning = value<uintptr_t>(entries + 8 * (count - 1));
    auto first = owner_file(winning, id);
    auto last = filename(winning);
    if (first.empty() || last.empty()) return {};
    char local[16];
    std::snprintf(local, sizeof(local), "|%06x", id & ((id >> 24) == 0xFE ? 0xFFF : 0xFFFFFF));
    return {first + local, last};
}

std::pair<uintptr_t, uint32_t> scene_context(uintptr_t player) {
    // PlayerCharacter::GetCurrentScene C5C370 -> 4FEBC0 -> 285330:
    // actor flag +2D0 bit 3, ExtraScene (type 146), scene pointer +18.
    if (!(value<uint32_t>(player+0x2D0) & 8)) return {};
    auto extras = value<uintptr_t>(player+0x100);
    if (!extras) return {};
    auto node = value<uintptr_t>(extras+8);
    for (unsigned i=0; node && i<256; ++i) {
        if (value<uint8_t>(node+0x12) == 146) {
            auto scene = value<uintptr_t>(node+0x18);
            if (scene && value<uint8_t>(scene+0x1A) == 126) // SCEN
                return {scene, value<uint32_t>(scene+0xCC)};
            return {};
        }
        auto next = value<uintptr_t>(node+8);
        if (next == node) break;
        node = next;
    }
    return {};
}

bool load_index(const std::filesystem::path& path) {
    try {
        if (std::filesystem::file_size(path) > 8*1024*1024) throw std::runtime_error("Affinity index too large");
        Json root(json_object_from_file(path.c_str()));
        if (!root || integer(root.get(), "version", 0) != 1) throw std::runtime_error("Invalid affinity index");
        auto* sources = member(root.get(), "sources");
        if (!sources || !json_object_is_type(sources, json_type_object) || !json_object_object_length(sources))
            throw std::runtime_error("Affinity index has no source manifest");
        json_object_object_foreach(sources, name, metadata) {
            struct stat st{};
            if (stat(name, &st)) throw std::runtime_error("Affinity source unavailable");
            if (auto* expected = member(metadata, "active_plugins")) {
                if (!json_object_is_type(expected, json_type_array)) throw std::runtime_error("Invalid active plugin manifest");
                std::ifstream file(name); if (!file) throw std::runtime_error("Active plugin list unavailable");
                std::vector<std::string> active;
                std::string line;
                while (std::getline(file, line)) {
                    if (line.compare(0,3,"\xEF\xBB\xBF")==0) line.erase(0,3);
                    if (line.empty() || line[0]!='*') continue;
                    auto first=line.find_first_not_of(" \t",1), end=line.find_last_not_of(" \t\r\n");
                    if (first!=std::string::npos && end>=first) active.push_back(lower(line.substr(first,end-first+1)));
                }
                if (active.size()!=json_object_array_length(expected)) throw std::runtime_error("Active plugins changed; refresh affinity data");
                for (size_t i=0; i<active.size(); ++i) {
                    auto* item=json_object_array_get_idx(expected,i);
                    if (!json_object_is_type(item,json_type_string) || active[i]!=json_object_get_string(item))
                        throw std::runtime_error("Active plugins changed; refresh affinity data");
                }
                continue;
            }
            if (st.st_size != integer(metadata, "size", -1) ||
                int64_t(st.st_mtim.tv_sec)*1000000000 + st.st_mtim.tv_nsec != integer(metadata, "mtime_ns", -1))
                throw std::runtime_error("Affinity data is stale; regenerate the dialogue affinity index");
        }
        auto* records = member(root.get(), "records");
        auto* observers = member(root.get(), "companions");
        if (!records || !observers || !json_object_is_type(records, json_type_object) ||
            !json_object_is_type(observers, json_type_object)) throw std::runtime_error("Invalid affinity records");
        std::unordered_map<std::string, Record> next;
        json_object_object_foreach(records, key, row) {
            Record record; record.last = string(row, "last");
            auto* reactions = member(row, "reactions");
            if (!reactions || !json_object_is_type(reactions, json_type_array) || json_object_array_length(reactions) > 128)
                throw std::runtime_error("Invalid affinity reactions");
            for (size_t i = 0; i < json_object_array_length(reactions); ++i) {
                auto* reaction = json_object_array_get_idx(reactions, i);
                auto name = string(reaction, "companion"), effect = string(reaction, "reaction");
                if (name.empty() || name.size() > 80 || (effect != "likes" && effect != "loves" && effect != "dislikes" && effect != "hates"))
                    throw std::runtime_error("Invalid affinity reaction");
                record.reactions.push_back({name, effect, json_object_get_boolean(member(reaction, "conditional")) != 0});
            }
            next.emplace(key, std::move(record));
        }
        std::unordered_map<std::string, std::string> next_companions;
        json_object_object_foreach(observers, observer_key, observer_name) {
            if (!json_object_is_type(observer_name, json_type_string)) throw std::runtime_error("Invalid companion index");
            next_companions.emplace(observer_key, json_object_get_string(observer_name));
        }
        affinity_records = std::move(next); companions = std::move(next_companions); index_status.clear();
        fo4hud_log("dialogue", "loaded affinity predictions for %zu INFO records and %zu companions", affinity_records.size(), companions.size());
        return true;
    } catch (const std::exception& e) {
        affinity_records.clear(); companions.clear(); index_status = e.what();
        fo4hud_log("dialogue", "%s", e.what()); return false;
    }
}

void add_reactions(uintptr_t info, std::vector<Fo4DialogueReaction>& out) {
    // Shared responses may carry the effect instead of the original INFO.
    std::array<uintptr_t, 8> seen{};
    for (unsigned i = 0; info && i < seen.size(); ++i) {
        if (std::find(seen.begin(), seen.begin()+i, info) != seen.begin()+i) break;
        seen[i] = info;
        if (value<uint8_t>(info + 0x1A) != 79) break; // INFO
        const auto key = form_key(info);
        auto row = affinity_records.find(key.first);
        if (row != affinity_records.end() && row->second.last == key.second) {
            for (const auto& effect : row->second.reactions) {
                auto same = [&](const auto& v) { return v.companion == effect.companion && v.reaction == effect.reaction && v.conditional == effect.conditional; };
                if (std::none_of(out.begin(), out.end(), same)) out.push_back(effect);
            }
        }
        info = value<uintptr_t>(info + 0x30);
    }
}

void capture(uintptr_t player_info, uint8_t option, uintptr_t npc_info, uintptr_t quest) {
    if (option >= 4 || !enabled.load(std::memory_order_acquire)) return;
    auto image = image_base.load();
    auto player = value<uintptr_t>(image + 0x32DD370);
    auto manager = value<uintptr_t>(image + 0x30E6E50);
    if (!player || !manager) return;
    const auto target = value<uint32_t>(manager + 0x14);
    const auto cell = value<uintptr_t>(player + 0xB8);
    const auto scene = scene_context(player);
    if (!scene.first) return;
    Fo4DialogueChoiceAffinity choice;
    choice.present = player_info != 0;
    add_reactions(player_info, choice.reactions);
    add_reactions(npc_info, choice.reactions);
    std::lock_guard<std::mutex> lock(state_mutex);
    if (option == 0 || batch.quest != quest || batch.player != player || batch.cell != cell || batch.target != target ||
        batch.scene != scene.first || batch.phase != scene.second) {
        batch = {}; batch.quest = quest; batch.player = player; batch.cell = cell; batch.target = target;
        batch.scene = scene.first; batch.phase = scene.second;
    }
    batch.choices[option] = std::move(choice); batch.received |= 1u << option;
    ++batch_generation;
}
uintptr_t __attribute__((ms_abi)) display_hook(uintptr_t player_info, uint8_t option, uintptr_t npc_info, uintptr_t quest) {
    const auto result = original(player_info, option, npc_info, quest);
    try { capture(player_info, option, npc_info, quest); } catch (...) { std::lock_guard<std::mutex> lock(state_mutex); batch = {}; }
    return result;
}

bool patch_call(int32_t displacement) {
    const uintptr_t call = image_base.load() + kCall;
    const size_t page = sysconf(_SC_PAGESIZE);
    void* start = reinterpret_cast<void*>(call & ~(uintptr_t(page)-1));
    if (mprotect(start, page, PROT_READ|PROT_WRITE|PROT_EXEC)) return false;
    std::memcpy(reinterpret_cast<void*>(call+1), &displacement, sizeof(displacement));
    __builtin___clear_cache(reinterpret_cast<char*>(call), reinterpret_cast<char*>(call+5));
    return mprotect(start, page, PROT_READ|PROT_EXEC) == 0;
}
bool awaiting_input(uintptr_t image) {
    auto manager = value<uintptr_t>(image + 0x30E6E50);
    if (!manager || value<uint8_t>(manager + 0x55) != 1) return false;
    auto ui = value<uintptr_t>(image + 0x30E8930);
    if (!ui || value<uintptr_t>(ui) != image + 0x271DAF8) return false;
    auto menus = value<uintptr_t>(ui + 0x190);
    auto count = value<uint32_t>(ui + 0x1A0);
    if (!menus || count > 64) return false;
    for (uint32_t i=0; i<count; ++i) {
        auto menu = value<uintptr_t>(menus + i*8);
        if (menu && value<uintptr_t>(menu) == image + 0x25AD118 &&
            (value<uint32_t>(menu + 0x58) & 0x40) && value<uint8_t>(menu + 0x164) == 1) return true;
    }
    return false;
}

// GFx values must be acquired, used and released on the movie's own thread.
// These bindings and the Movie vtable slots are verified for 1.11.240.
struct GfxValue {
    uintptr_t interface = 0;
    uint32_t type = 0, padding = 0;
    uint64_t data = 0, unused = 0;
    GfxValue() = default;
    GfxValue(const GfxValue&) = delete;
    GfxValue& operator=(const GfxValue&) = delete;
    ~GfxValue() {
        if ((type & 64) && interface) {
            using Release = void (__attribute__((ms_abi)) *)(uintptr_t, GfxValue*, uint64_t);
            reinterpret_cast<Release>(image_base.load()+0x1AD6BB0)(interface, this, data);
        }
    }
    bool object() const { auto t=type & 0x8F; return interface && (t==8 || t==9 || t==10); }
    bool get(const char* name, GfxValue& out) const {
        if (!object()) return false;
        using Get = bool (__attribute__((ms_abi)) *)(uintptr_t,uint64_t,const char*,GfxValue*,bool);
        return reinterpret_cast<Get>(image_base.load()+0x1AC8AE0)(interface,data,name,&out,(type&0x8F)==10);
    }
    double number() const {
        if ((type&0x8F)==3) return int32_t(data);
        if ((type&0x8F)==4) return uint32_t(data);
        double n=0;
        if ((type&0x8F)!=5) throw std::runtime_error("Invalid GFx coordinate");
        std::memcpy(&n,&data,8);
        if (!std::isfinite(n)) throw std::runtime_error("Invalid GFx coordinate");
        return n;
    }
    double number(const char* name) const {
        GfxValue out;
        if (!get(name,out)) throw std::runtime_error("GFx coordinate unavailable");
        return out.number();
    }
    bool shown() const {
        GfxValue visible;
        return get("visible",visible) && (visible.type&0x8F)==2 && (visible.data&255) && number("alpha")>0.01;
    }
};
static_assert(sizeof(GfxValue)==32);
struct MovieRect { float left, top, right, bottom; };
struct MovieViewport {
    int32_t buffer_width, buffer_height, left, top, width, height;
    int32_t scissor[4]; uint32_t flags; float scale, aspect;
};
Fo4DialogueChoiceBounds screen_bounds(double x, double y, double w, double h,
                                      const MovieRect& frame, const MovieViewport& viewport) {
    Fo4DialogueChoiceBounds out;
    const double fw=double(frame.right)-frame.left, fh=double(frame.bottom)-frame.top;
    if (!std::isfinite(fw) || !std::isfinite(fh) || fw<=0 || fh<=0 ||
        viewport.buffer_width<=0 || viewport.buffer_height<=0 || viewport.width<=0 || viewport.height<=0 ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0) return out;
    auto sx=[&](double v){return float((viewport.left+(v-frame.left)/fw*viewport.width)/viewport.buffer_width);};
    auto sy=[&](double v){return float((viewport.top+(v-frame.top)/fh*viewport.height)/viewport.buffer_height);};
    out.left=sx(x); out.top=sy(y); out.right=sx(x+w); out.bottom=sy(y+h);
    out.valid=out.left>=0 && out.top>=0 && out.right<=1 && out.bottom<=1 && out.left<out.right && out.top<out.bottom;
    return out;
}
void sample_layout(uintptr_t menu) {
    Layout next;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        next.generation=batch_generation;
    }
    const auto image=image_base.load();
    if (awaiting_input(image) && value<uint8_t>(menu+0x165)==1) {
        auto movie=value<uintptr_t>(menu+0x40);
        if (!movie) return;
        auto table=value<uintptr_t>(movie);
        alignas(16) MovieRect frame{}; MovieViewport viewport{};
        using Rect = void* (__attribute__((ms_abi)) *)(uintptr_t,MovieRect*);
        using Viewport = void (__attribute__((ms_abi)) *)(uintptr_t,MovieViewport*);
        reinterpret_cast<Rect>(value<uintptr_t>(table+0x12*8))(movie,&frame);
        reinterpret_cast<Viewport>(value<uintptr_t>(table+0x0D*8))(movie,&viewport);
        // Borrow the menu's root value without changing its reference count.
        const auto& root=*reinterpret_cast<const GfxValue*>(menu+0x20);
        GfxValue stage, fade;
        if (root.shown() && root.get("stage",stage) && root.get("FadeHolder_mc",fade) && fade.shown()) {
            constexpr const char* names[]={"Positive","Negative","Neutral","Question"};
            for (unsigned i=0;i<4;++i) {
                GfxValue animator, holder, button, bounds;
                if (!fade.get(names[i],animator) || !animator.shown() || !animator.get("Holder",holder) ||
                    !holder.shown() || !holder.get("Button_mc",button) || !button.shown()) continue;
                using Invoke = bool (__attribute__((ms_abi)) *)(uintptr_t,uint64_t,GfxValue*,const char*,const GfxValue*,uint64_t,uint8_t);
                if (!reinterpret_cast<Invoke>(image+0x1AD02A0)(button.interface,button.data,&bounds,"getBounds",&stage,1,(button.type&0x8F)==10)) continue;
                next.bounds[i]=screen_bounds(bounds.number("x"),bounds.number("y"),bounds.number("width"),bounds.number("height"),frame,viewport);
            }
        }
    }
    next.sampled=std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(state_mutex);
    if (next.generation==batch_generation) layout=next;
}
uintptr_t __attribute__((ms_abi)) advance_hook(uintptr_t menu,float delta,uintptr_t time) {
    auto result=original_advance(menu,delta,time);
    if (enabled.load(std::memory_order_acquire)) {
        try { sample_layout(menu); }
        catch (...) { std::lock_guard<std::mutex> lock(state_mutex); layout={}; }
    }
    return result;
}
bool exchange_advance(uintptr_t expected,uintptr_t replacement) {
    const auto slot=image_base.load()+kAdvanceSlot;
    const size_t page=sysconf(_SC_PAGESIZE);
    void* start=reinterpret_cast<void*>(slot&~uintptr_t(page-1));
    if (mprotect(start,page,PROT_READ|PROT_WRITE)) return false;
    bool okay=__atomic_compare_exchange_n(reinterpret_cast<uintptr_t*>(slot),&expected,replacement,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    return mprotect(start,page,PROT_READ)==0 && okay;
}
}

bool fo4hud_dialogue_affinity_install(uintptr_t image) {
    std::lock_guard<std::mutex> lock(lifecycle);
    if (enabled.load()) return image_base.load() == image;
    if (!image) return false;
    uint8_t bytes[5]{};
    if (!read(image+kCall, bytes, sizeof(bytes)) || std::memcmp(bytes, kOriginalCall, sizeof(bytes))) return false;
    uintptr_t advance=0;
    if (!read_value(image+kAdvanceSlot,advance) || advance!=image+kAdvance) return false;
    image_base.store(image); original = reinterpret_cast<Display>(image + kDisplay);
    Dl_info module{};
    if (!dladdr(reinterpret_cast<void*>(fo4hud_dialogue_affinity_install), &module) || !module.dli_fname) return false;
    auto path = std::filesystem::path(module.dli_fname).parent_path()/"assets/dialogue_affinity.json";
    if (auto* custom = std::getenv("FO4HUD_DIALOGUE_AFFINITY_INDEX")) path = custom;
    load_index(path);
    const size_t page = sysconf(_SC_PAGESIZE);
    if (!island) {
        auto try_page = [&](uintptr_t candidate) {
            if (island) return;
            candidate &= ~(uintptr_t(page)-1);
            int64_t delta = int64_t(candidate) - int64_t(image+kCall+5);
            if (delta < INT32_MIN || delta > INT32_MAX) return;
            void* p = mmap(reinterpret_cast<void*>(candidate), page, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) island = static_cast<uint8_t*>(p);
        };
        for (int i=0; i<64 && !island; ++i) { try_page(image+0x425A000+i*page); try_page(image-(i+1)*page); }
        for (int i=0; i<96 && !island; ++i) try_page(image+0x30000000+uintptr_t(i)*0x1000000);
        if (!island) return false;
        const uint8_t stub[] = {0x48,0xB8,0,0,0,0,0,0,0,0,0xFF,0xE0};
        const auto destination = reinterpret_cast<uintptr_t>(display_hook);
        std::memcpy(island, stub, sizeof(stub)); std::memcpy(island+2, &destination, 8);
        if (mprotect(island, page, PROT_READ|PROT_EXEC)) return false;
    }
    int64_t displacement = int64_t(reinterpret_cast<uintptr_t>(island)) - int64_t(image+kCall+5);
    if (displacement < INT32_MIN || displacement > INT32_MAX || !patch_call(int32_t(displacement))) return false;
    original_advance=reinterpret_cast<Advance>(advance);
    if (!exchange_advance(advance,reinterpret_cast<uintptr_t>(advance_hook))) { patch_call(0x475); return false; }
    enabled.store(true, std::memory_order_release);
    fo4hud_log("dialogue", "dialogue affinity capture installed for Fallout 4 1.11.240");
    return true;
}
void fo4hud_dialogue_affinity_uninstall() {
    std::lock_guard<std::mutex> lock(lifecycle);
    enabled.store(false, std::memory_order_release);
    auto image = image_base.load();
    if (image && original_advance) exchange_advance(reinterpret_cast<uintptr_t>(advance_hook),reinterpret_cast<uintptr_t>(original_advance));
    int32_t displacement = 0;
    if (image && island && read_value(image+kCall+1, displacement) &&
        image+kCall+5+displacement == reinterpret_cast<uintptr_t>(island)) patch_call(0x475);
    // Retain the island for a thread that already fetched the CALL.
    std::lock_guard<std::mutex> state(state_mutex); batch = {}; layout = {}; ++batch_generation;
}

Fo4DialogueAffinity fo4hud_read_dialogue_affinity(const Fo4EntitySnapshot& snapshot) {
    Fo4DialogueAffinity result;
    try {
        auto image = image_base.load();
        if (!enabled.load() || !image || !awaiting_input(image) || !fo4hud_player_in_snapshot_cell(snapshot)) return result;
        if (!index_status.empty()) { result.visible = true; result.status = index_status; return result; }
        float player[3]{};
        if (!fo4hud_read_player_position(snapshot, player)) return result;
        for (const auto& actor : snapshot.actors) {
            if (!fo4hud_actor_in_player_space(snapshot, actor)) continue;
            uint8_t life = 0; float position[3]{};
            if (!fo4hud_read_actor_life_state(actor, life) || life == 1 || life == 2 ||
                !read(actor.address+0xD0, position, sizeof(position))) continue;
            double distance = 0;
            for (unsigned i=0; i<3; ++i) distance += (double(position[i])-player[i])*(double(position[i])-player[i]);
            if (!std::isfinite(distance) || distance > 2500.0*2500.0) continue;
            auto found = companions.find(form_key(actor.address).first);
            if (found != companions.end()) result.nearby_companions.push_back(found->second);
        }
        if (result.nearby_companions.empty()) return result;
        auto manager = value<uintptr_t>(image + 0x30E6E50);
        auto scene = scene_context(snapshot.player_address);
        std::lock_guard<std::mutex> lock(state_mutex);
        if (batch.received != 15 || batch.player != snapshot.player_address || batch.cell != snapshot.cell_address ||
            !scene.first || batch.scene != scene.first || batch.phase != scene.second ||
            batch.target != value<uint32_t>(manager + 0x14)) return result;
        result.visible = true; result.choices = batch.choices;
        if (layout.generation==batch_generation && std::chrono::steady_clock::now()-layout.sampled<std::chrono::milliseconds(300))
            result.bounds=layout.bounds;
    } catch (...) { return {}; }
    return result;
}
