#include "../dialogue_affinity.cpp"
#include "../dialogue_affinity_overlay.h"
#include <cassert>
#include <fstream>

bool fo4hud_player_in_snapshot_cell(const Fo4EntitySnapshot&) { return true; }
bool fo4hud_read_player_position(const Fo4EntitySnapshot&, float out[3]) { std::fill(out,out+3,0); return true; }
bool fo4hud_actor_in_player_space(const Fo4EntitySnapshot&, const Fo4Entity&) { return true; }
bool fo4hud_read_actor_life_state(const Fo4Entity&, uint8_t& out) { out=0; return true; }

template<class T> void put(void* p, size_t off, T v) { std::memcpy(static_cast<char*>(p)+off,&v,sizeof(v)); }
uintptr_t address(const void* p) { return reinterpret_cast<uintptr_t>(p); }
unsigned calls=0;
uintptr_t __attribute__((ms_abi)) game_display(uintptr_t info,uint8_t option,uintptr_t npc,uintptr_t quest) {
    ++calls; assert(info && option<4 && !npc && quest==123); return 987;
}
struct GfxNode {
    std::unordered_map<std::string,GfxNode*> children;
    double x=540,y=450,width=100,height=30;
};
bool __attribute__((ms_abi)) gfx_get(uintptr_t interface,uint64_t data,const char* name,GfxValue* out,bool display) {
    assert(interface==1 && display);
    auto* node=reinterpret_cast<GfxNode*>(data);
    if (!std::strcmp(name,"visible")) { out->type=2;out->data=1;return true; }
    double number=0;
    if (!std::strcmp(name,"alpha")) number=1;
    else if (!std::strcmp(name,"x")) number=node->x;
    else if (!std::strcmp(name,"y")) number=node->y;
    else if (!std::strcmp(name,"width")) number=node->width;
    else if (!std::strcmp(name,"height")) number=node->height;
    else {
        auto it=node->children.find(name);
        if (it==node->children.end()) return false;
        out->interface=1;out->type=10;out->data=address(it->second);return true;
    }
    out->type=5;std::memcpy(&out->data,&number,8);return true;
}
bool __attribute__((ms_abi)) gfx_invoke(uintptr_t interface,uint64_t data,GfxValue* out,const char* method,const GfxValue* args,uint64_t count,uint8_t display) {
    assert(interface==1 && display && !std::strcmp(method,"getBounds") && count==1 && args[0].object());
    out->interface=1;out->type=10;out->data=data;return true;
}
void* __attribute__((ms_abi)) movie_rect(uintptr_t,MovieRect* out) { *out={-220,0,1500,720};return out; }
void __attribute__((ms_abi)) movie_viewport(uintptr_t,MovieViewport* out) { *out={3440,1440,0,0,3440,1440}; }
uintptr_t __attribute__((ms_abi)) game_advance(uintptr_t,float delta,uintptr_t time) {
    assert(delta==.25f && time==321);return 654;
}
struct Form {
    char bytes[0x160]{}, file[0x400]{}, source[16]{};
    uintptr_t entries[1]{};
    Form(uint32_t id, uint8_t type=79) {
        std::strcpy(file+0x70,"Fallout4.esm"); entries[0]=address(file);
        put(source,0,address(entries)); put(source,8,uint32_t(1));
        put(bytes,8,address(source));put(bytes,0x14,id);put(bytes,0x1A,type);
    }
    uintptr_t ptr() const { return address(bytes); }
};
int main() {
    char tmp[]="/tmp/fo4hud-dialogue-test-XXXXXX"; assert(mkdtemp(tmp));
    const auto directory=std::filesystem::path(tmp);
    setenv("XDG_STATE_HOME",tmp,1);
    const auto evidence=directory/"source.txt", database=directory/"index.json";
    std::ofstream(evidence)<<"fixture";
    struct stat st{}; assert(!stat(evidence.c_str(),&st));
    Json root=object(), sources=object(), metadata=object(), rows=object(), row=object(), observers=object();
    set(metadata.get(),"size",int64_t(st.st_size));
    set(metadata.get(),"mtime_ns",int64_t(st.st_mtim.tv_sec)*1000000000+st.st_mtim.tv_nsec);
    set(sources.get(),evidence.c_str(),metadata.release()); set(root.get(),"sources",sources.release());
    set(root.get(),"version",int64_t(1)); set(row.get(),"last","fallout4.esm");
    Json reactions(json_tokener_parse("[{\"companion\":\"Cait\",\"reaction\":\"loves\",\"conditional\":false}]"));
    set(row.get(),"reactions",reactions.release());set(rows.get(),"fallout4.esm|1069d6",row.release());
    set(root.get(),"records",rows.release());set(observers.get(),"fallout4.esm|079305","Cait");
    set(root.get(),"companions",observers.release());
    assert(json_object_to_file(database.c_str(),root.get())==0);
    setenv("FO4HUD_DIALOGUE_AFFINITY_INDEX",database.c_str(),1);
    auto* image=static_cast<char*>(mmap(nullptr,0x3300000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(image!=MAP_FAILED); const auto base=address(image);
    // Exercise the patched native CALL with the real Microsoft x64 ABI,
    // including its shadow space, and verify the original return value.
    const uint8_t before[]={0x48,0x83,0xEC,0x28}, after[]={0x48,0x83,0xC4,0x28,0xC3};
    std::memcpy(image+kCall-4,before,sizeof(before));std::memcpy(image+kCall,kOriginalCall,5);
    std::memcpy(image+kCall+5,after,sizeof(after));
    const uint8_t stub[]={0x48,0xB8,0,0,0,0,0,0,0,0,0xFF,0xE0};
    std::memcpy(image+kDisplay,stub,sizeof(stub));put(image,kDisplay+2,address(reinterpret_cast<void*>(game_display)));
    for (auto [rva,fn]:std::initializer_list<std::pair<uintptr_t,uintptr_t>>{
        {kAdvance,address(reinterpret_cast<void*>(game_advance))},
        {0x1AC8AE0,address(reinterpret_cast<void*>(gfx_get))},
        {0x1AD02A0,address(reinterpret_cast<void*>(gfx_invoke))}}) {
        std::memcpy(image+rva,stub,sizeof(stub));put(image,rva+2,fn);
    }
    put(image,kAdvanceSlot,base+kAdvance);
    alignas(8) char player[0x300]{}, manager[0x80]{}, ui[0x220]{}, menu[0x180]{};
    uintptr_t menus[]={address(menu)};
    alignas(8) char extras[0x20]{}, scene_extra[0x30]{}, scene[0x100]{};
    put(player,0x2D0,uint32_t(8));put(player,0x100,address(extras));put(extras,8,address(scene_extra));
    put(scene_extra,0x12,uint8_t(146));put(scene_extra,0x18,address(scene));
    put(scene,0x1A,uint8_t(126));put(scene,0xCC,uint32_t(7));
    put(image,0x32DD370,address(player));put(player,0xB8,uintptr_t(42));
    put(image,0x30E6E50,address(manager));put(manager,0x14,uint32_t(456));put(manager,0x55,uint8_t(1));
    put(image,0x30E8930,address(ui));put(ui,0,base+0x271DAF8);put(ui,0x190,address(menus));put(ui,0x1A0,uint32_t(1));
    put(menu,0,base+0x25AD118);put(menu,0x58,uint32_t(0x40));put(menu,0x164,uint8_t(1));
    assert(fo4hud_dialogue_affinity_install(base));
    Form known(0x1069D6), unknown(0x123456), cait(0x79305,65);
    auto invoke=reinterpret_cast<Display>(base+kCall-4);
    assert(invoke(known.ptr(),0,0,123)==987);
    assert(invoke(unknown.ptr(),1,0,123)==987);
    assert(invoke(known.ptr(),2,0,123)==987);
    assert(invoke(unknown.ptr(),3,0,123)==987);
    assert(calls==4);
    Fo4EntitySnapshot snapshot; snapshot.player_address=address(player);snapshot.cell_address=42;
    Fo4Entity observer;observer.address=cait.ptr();snapshot.actors={observer};
    auto state=fo4hud_read_dialogue_affinity(snapshot);
    assert(state.visible && state.nearby_companions==std::vector<std::string>{"Cait"});
    assert(state.choices[0].reactions.size()==1 && state.choices[0].reactions[0].reaction=="loves");
    assert(state.choices[1].reactions.empty()); // Unknown must never mean neutral.
    GfxNode root_node,stage_node,fade_node,animator_node,holder_node,button_node;
    root_node.children={{"stage",&stage_node},{"FadeHolder_mc",&fade_node}};
    for (auto name:{"Positive","Negative","Neutral","Question"}) fade_node.children[name]=&animator_node;
    animator_node.children["Holder"]=&holder_node;holder_node.children["Button_mc"]=&button_node;
    put(menu,0x20,uintptr_t(1));put(menu,0x28,uint32_t(10));put(menu,0x30,address(&root_node));
    uintptr_t movie_table[0x19]{}, movie=address(movie_table);
    movie_table[0x12]=address(reinterpret_cast<void*>(movie_rect));
    movie_table[0x0D]=address(reinterpret_cast<void*>(movie_viewport));
    put(menu,0x40,address(&movie));put(menu,0x165,uint8_t(1));
    auto advance=reinterpret_cast<Advance>(value<uintptr_t>(base+kAdvanceSlot));
    assert(advance(address(menu),.25f,321)==654);
    state=fo4hud_read_dialogue_affinity(snapshot);
    assert(state.bounds[2].valid && std::abs(state.bounds[2].right-.5f)<.00001f);
    // New dialogue capture invalidates old geometry until the UI samples it.
    invoke(unknown.ptr(),3,0,123);
    assert(!fo4hud_read_dialogue_affinity(snapshot).bounds[2].valid);
    assert(advance(address(menu),.25f,321)==654);
    layout.sampled-=std::chrono::seconds(1);
    assert(!fo4hud_read_dialogue_affinity(snapshot).bounds[2].valid);
    put(menu,0x165,uint8_t(0));
    assert(advance(address(menu),.25f,321)==654);
    assert(!fo4hud_read_dialogue_affinity(snapshot).bounds[2].valid);
    put(menu,0x165,uint8_t(1));
    // Live Abbot refusal: INFO 0003DE41 keeps only UFO4P in its source-file
    // list, while its runtime FormID still belongs to Fallout4.esm. Resolve
    // the original namespace through the winning patch's master table.
    Form patched(0x3DE41);
    alignas(8) char master_file[0x400]{};
    std::strcpy(master_file+0x70,"Fallout4.esm");
    uintptr_t patch_masters[]={address(master_file)};
    std::strcpy(patched.file+0x70,"unofficial fallout 4 patch.esp");
    put(patched.file,0x370,uint8_t(8));
    put(patched.file,0x358,uint32_t(1));put(patched.file,0x360,address(patch_masters));
    auto identity=form_key(patched.ptr());
    assert(identity.first=="fallout4.esm|03de41" && identity.second=="unofficial fallout 4 patch.esp");
    affinity_records[identity.first]={identity.second,{{"Cait","dislikes",false}}};
    std::vector<Fo4DialogueReaction> patched_effects;add_reactions(patched.ptr(),patched_effects);
    assert(patched_effects.size()==1 && patched_effects[0].reaction=="dislikes");
    // Also preserve light-plugin namespaces when an override supplies a form.
    Form light(0xFE007800);std::strcpy(light.file+0x70,"Example.esl");
    put(light.file,0x370,uint8_t(0xFE));put(light.file,0x372,uint16_t(7));
    assert(form_key(light.ptr()).first=="example.esl|000800");
    // Reject a new winning override absent from the generated index.
    std::strcpy(known.file+0x70,"OtherMod.esp");
    std::vector<Fo4DialogueReaction> effects;add_reactions(known.ptr(),effects);assert(effects.empty());
    std::strcpy(known.file+0x70,"Fallout4.esm");
    put(unknown.bytes,0x30,known.ptr());add_reactions(unknown.ptr(),effects);assert(effects.size()==1);
    put(known.bytes,0x30,unknown.ptr());effects.clear();add_reactions(known.ptr(),effects);assert(effects.size()==1); // Shared cycle bounded.
    put(known.bytes,0x30,uintptr_t(0));put(unknown.bytes,0x30,uintptr_t(0));
    put(manager,0x55,uint8_t(0));assert(!fo4hud_read_dialogue_affinity(snapshot).visible);
    put(manager,0x55,uint8_t(1));put(manager,0x14,uint32_t(999));assert(!fo4hud_read_dialogue_affinity(snapshot).visible);
    put(manager,0x14,uint32_t(456));
    put(scene,0xCC,uint32_t(8));assert(!fo4hud_read_dialogue_affinity(snapshot).visible);
    put(scene,0xCC,uint32_t(7));
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    for (ImVec2 size : {ImVec2(640,480),ImVec2(1920,1080),ImVec2(3440,1440)}) {
        io.DisplaySize=size;io.DeltaTime=1.f/60;
        state.bounds={{{true,.46f,.72f,.54f,.75f},{true,.55f,.66f,.63f,.69f},
                       {true,.35f,.66f,.45f,.69f},{true,.46f,.60f,.54f,.63f}}};
        for (unsigned i=0;i<4;++i) {
            auto rect=fo4hud_dialogue_label_rect(i,state.bounds[i],{70,18},size,6,{7,4});
            assert(rect.min.x>=0 && rect.min.y>=0 && rect.max.x<=size.x && rect.max.y<=size.y);
            if (i==0) assert(rect.min.y>state.bounds[i].bottom*size.y);
            if (i==1) assert(rect.min.x>state.bounds[i].right*size.x);
            if (i==2) assert(rect.max.x<state.bounds[i].left*size.x);
            if (i==3) assert(rect.max.y<state.bounds[i].top*size.y);
        }
        ImGui::NewFrame();fo4hud_draw_dialogue_affinity(state,size);ImGui::Render();
        assert(ImGui::GetDrawData()->TotalVtxCount>0 && !io.WantCaptureMouse && !io.WantCaptureKeyboard);
        for (auto& b:state.bounds) b.valid=false;
        ImGui::NewFrame();fo4hud_draw_dialogue_affinity(state,size);ImGui::Render();
        assert(ImGui::GetDrawData()->TotalVtxCount==0);
    }
    // Ultrawide visible frame starts left of the authored stage, and a
    // viewport can be inset within its render buffer.
    MovieRect frame{-220,0,1500,720}; MovieViewport viewport{3440,1440,0,0,3440,1440};
    auto bounds=screen_bounds(540,450,100,30,frame,viewport);
    assert(bounds.valid && std::abs(bounds.right-.5f)<.00001f && std::abs(bounds.top-.625f)<.00001f);
    viewport.left=100;viewport.width=3240;
    bounds=screen_bounds(540,450,100,30,frame,viewport);
    assert(bounds.valid && std::abs(bounds.right-.5f)<.00001f);
    assert(!screen_bounds(0,0,-1,30,frame,viewport).valid);
    assert(!screen_bounds(NAN,0,100,30,frame,viewport).valid);
    ImGui::DestroyContext();
    fo4hud_dialogue_affinity_uninstall();assert(!std::memcmp(image+kCall,kOriginalCall,5));
    assert(!fo4hud_read_dialogue_affinity(snapshot).visible);
    // Fallout rewrites Plugins.txt on exit. Formatting, comments and filename
    // case must not invalidate data, but active-plugin changes must.
    auto plugin_file=directory/"Plugins.txt";
    Json plugin_metadata=object();
    Json active_plugins(json_tokener_parse("[\"example.esp\"]"));
    set(plugin_metadata.get(),"active_plugins",active_plugins.release());
    set(member(root.get(),"sources"),plugin_file.c_str(),plugin_metadata.release());
    assert(json_object_to_file(database.c_str(),root.get())==0);
    std::ofstream(plugin_file)<<"# Before exit\n*Example.esp\n";
    assert(load_index(database));
    std::ofstream(plugin_file)<<"# Rewritten by game\r\n*EXAMPLE.ESP  \r\n";
    assert(load_index(database));
    std::ofstream(plugin_file)<<"*Different.esp\n";
    assert(!load_index(database));
    std::ofstream(plugin_file)<<"*Example.esp\n";
    assert(load_index(database));
    std::ofstream(evidence,std::ios::app)<<" changed";
    assert(!load_index(database) && affinity_records.empty());
    munmap(image,0x3300000);std::filesystem::remove_all(directory);
}
